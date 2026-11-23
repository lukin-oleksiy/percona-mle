# Copyright (c) 2026 Percona LLC and/or its affiliates. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free
# Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
# more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
"""The Python client of the end-to-end benchmark (perf/e2e/e2e.py runs it): one measurement.

    e2e_client.py --host H --port P --variant app|jvm|cpython --pipe 1..6 --c C --k K --clients N
                  --queries Q --warm-min A --warm-max B --batch S

Every client is a process with one persistent connection (mysql-connector-python, numpy with one BLAS
thread: OPENBLAS_NUM_THREADS=1, as in MLE workers). Each warms up until the medians of two batches in a
row agree within 3 % (at least warm-min, at most warm-max requests), then all start their measured
requests together. Prints one JSON line (as client/E2E.java).

Variants: app, the logic here in numpy; jvm, a CALL of the LANGUAGE PYTHON procedure (GraalPy, JVM
worker); cpython, of the LANGUAGE CPYTHON one (the CPython worker). Pipes as in E2E.java.
"""
import argparse
import json
import multiprocessing as mp
import statistics
import time

import mysql.connector
import numpy as np
from mysql.connector.constants import ClientFlag

D = 768


def connect(a):
    return mysql.connector.connect(host=a.host, port=a.port, user='app', password='app', database='e2e',
                                   client_flags=[ClientFlag.MULTI_STATEMENTS])


def user_of(j):
    return 1 + (j * 7919) % 10000


def sets(cur, sql, params):
    """Runs a multi-statement request; the rows of each result set."""
    cur.execute(sql, params, map_results=True)
    out = []
    while True:
        if cur.with_rows:
            out.append(cur.fetchall())
        if not cur.nextset():
            break
    return out


def matrix(rows, col):
    return np.frombuffer(b''.join(bytes(r[col]) for r in rows), np.float32).reshape(len(rows), D)


def rerank(ids, category, price, stock, dist, v, cat, k, pipe):
    """The re-ranking of Rerank.java in numpy (the same scores, the same ties)."""
    s = 0.8 * (1 - dist) + 0.1 * (1 - price / 1000) + 0.05 * (stock > 0) + 0.05 * (category == cat)
    if pipe == 1:
        return [int(x) for x in ids[np.lexsort((ids, -s))[:k]]]
    n = len(ids)
    max_sim = np.zeros(n)
    taken = np.zeros(n, dtype=bool)
    picked = []
    for step in range(min(k, n)):
        value = 0.7 * s - (0.3 * max_sim if step else 0)
        value[taken] = -np.inf
        i = int(np.argmax(value))
        taken[i] = True
        picked.append(int(ids[i]))
        sims = v @ v[i]
        max_sim = sims if step == 0 else np.maximum(max_sim, sims)
    return picked


def candidates(c, with_vectors):
    """The candidate query: the query vector parsed once (SET @q; STRING_TO_VECTOR(?) in the select list
    is evaluated per row), one round trip."""
    return ("SET @q = STRING_TO_VECTOR(%s); SELECT id, category, price, stock, DISTANCE(embedding, @q, 'COSINE') "
            "AS dist" + (", embedding" if with_vectors else "") + " FROM items "
            "ORDER BY DISTANCE(embedding, @q, 'COSINE') LIMIT %d" % int(c))


def chatty(cur, uid, c, k):
    """Three dependent round trips: profile and history (one request), candidates, promotions."""
    prof, hist = sets(cur, "SELECT pref_cat, max_price FROM users WHERE id = %s; "
                           "SELECT i.id, i.embedding FROM views v JOIN items i ON i.id = v.item_id "
                           "WHERE v.user_id = %s ORDER BY v.ts DESC LIMIT 10", (uid, uid))
    pref, max_price = int(prof[0][0]), float(prof[0][1])
    u = matrix(hist, 1).astype(np.float64).sum(axis=0)
    u = (u / np.linalg.norm(u)).astype(np.float32)
    rows = sets(cur, candidates(c, False), ('[' + ','.join('%.9g' % x for x in u) + ']',))[-1]
    ids = np.array([r[0] for r in rows], dtype=np.int64)
    sc = (0.8 * (1 - np.array([r[4] for r in rows], dtype=np.float64))
          + 0.05 * (np.array([r[1] for r in rows]) == pref)
          + 0.05 * (np.array([r[2] for r in rows], dtype=np.float64) <= max_price)
          + 0.05 * (np.array([r[3] for r in rows]) > 0))
    sc[np.isin(ids, np.array([r[0] for r in hist], dtype=np.int64))] = -np.inf
    order = np.lexsort((ids, -sc))[:3 * k]
    short, ssc = ids[order], sc[order]
    cur.execute("SELECT item_id, discount FROM promos WHERE item_id IN (" + ','.join(str(int(x)) for x in short)
                + ")")
    disc = {int(r[0]): float(r[1]) for r in cur.fetchall()}
    final = ssc + 0.2 * np.array([disc.get(int(x), 0.0) for x in short])
    return [int(x) for x in short[np.lexsort((short, -final))[:k]]]


def write_back(cur, cat, c, k, vec, client):
    """Read C items of a category, enrich them, write them back in one transaction as one multi-row upsert
    (what executemany builds), then COMMIT: three round trips."""
    cur.execute("SELECT id, embedding FROM items WHERE category = %%s ORDER BY id LIMIT %d" % int(c), (cat,))
    rows = cur.fetchall()
    ids = np.array([r[0] for r in rows], dtype=np.int64)
    v = matrix(rows, 1).astype(np.float64)
    cen = v.mean(axis=0)
    cen /= np.linalg.norm(cen)
    sim = v @ cen
    picked = [int(x) for x in ids[np.lexsort((ids, -sim))[:k]]]
    if vec:
        w = v - cen
        w = (w / np.linalg.norm(w, axis=1, keepdims=True)).astype(np.float32)
    # The vector as binary (the connector sends bytes as a plain string literal, which VECTOR refuses).
    one = "(%s, %s, %s, %s, _binary %s)" if vec else "(%s, %s, %s, %s)"
    params = []
    idl, siml = ids.tolist(), sim.tolist()
    for i in range(len(idl)):
        params += [client, i, idl[i], siml[i]] + ([w[i].tobytes()] if vec else [])
    cur.execute("INSERT INTO enriched" + (" (client, slot, item_id, score, emb)" if vec else
                                          " (client, slot, item_id, score)")
                + " VALUES " + ", ".join([one] * len(idl))
                + " ON DUPLICATE KEY UPDATE item_id = VALUES(item_id), score = VALUES(score)"
                + (", emb = VALUES(emb)" if vec else ""), params)
    cur.execute("COMMIT")
    return picked


def app(cur, a, j, qt, cat, client):
    pipe, c, k = a.pipe, a.c, a.k
    if pipe >= 5:
        return write_back(cur, cat, c, k, pipe == 6, client)
    if pipe == 3:
        return chatty(cur, user_of(j), c, k)
    if pipe == 4:
        cur.execute("SELECT id, embedding FROM items WHERE category = %s", (cat,))
        rows = cur.fetchall()
        ids = np.array([r[0] for r in rows], dtype=np.int64)
        v = matrix(rows, 1).astype(np.float64)
        cen = v.mean(axis=0)
        cen /= np.linalg.norm(cen)
        sim = v @ cen
        return [int(x) for x in list(ids[np.lexsort((ids, -sim))[:k]]) + list(ids[np.lexsort((ids, sim))[:k]])]
    rows = sets(cur, candidates(c, pipe == 2), (qt,))[-1]
    ids = np.array([r[0] for r in rows], dtype=np.int64)
    category = np.array([r[1] for r in rows])
    price = np.array([r[2] for r in rows], dtype=np.float64)
    stock = np.array([r[3] for r in rows])
    dist = np.array([r[4] for r in rows], dtype=np.float64)
    v = matrix(rows, 5) if pipe == 2 else None
    return rerank(ids, category, price, stock, dist, v, cat, k, pipe)


def mle(cur, a, j, qt, cat, client):
    suffix = {'jvm': 'py', 'cpython': 'cpy'}[a.variant]
    pipe, c, k = a.pipe, a.c, a.k
    if pipe <= 2:
        sql, params = "CALL e2e_rerank_%s(STRING_TO_VECTOR(%%s), %%s, %%s, %%s, %%s, @r)", (qt, cat, c, k, pipe)
    elif pipe == 3:
        sql, params = "CALL e2e_chatty_%s(%%s, %%s, %%s, @r)", (user_of(j), c, k)
    elif pipe == 4:
        sql, params = "CALL e2e_bulk_%s(%%s, %%s, @r)", (cat, k)
    else:
        sql, params = "CALL e2e_wb_%s(%%s, %%s, %%s, %%s, %%s, @r)", (cat, c, k, client, 1 if pipe == 6 else 0)
    rows = sets(cur, (sql % suffix) + "; SELECT @r", params)[-1]
    return json.loads(rows[0][0])['ids']


def client(index, a, queries, ready, go, out):
    try:
        conn = connect(a)
        if a.variant != 'app':
            conn.autocommit = True  # the application: the connector's default (off), as before
        cur = conn.cursor()
        run = app if a.variant == 'app' else mle
        nset = len(queries)

        def one(j):
            qt, cat = queries[j]
            return run(cur, a, j, qt, j % 20 if a.pipe >= 4 else cat, index + 1)

        prev, n, converged = None, 0, False
        while n < a.warm_max:
            b = []
            for _ in range(a.batch):
                t0 = time.perf_counter()
                one((index * 37 + 100 + n) % nset)
                b.append(time.perf_counter() - t0)
                n += 1
            m = statistics.median(b)
            if n >= a.warm_min and prev is not None and abs(m - prev) <= 0.03 * min(m, prev):
                converged = True
                break
            prev = m
        ready.put(None)
        go.wait()
        lat, first = [], None
        for i in range(a.queries):
            j = (index * 37 + i) % nset
            t0 = time.perf_counter()
            ids = one(j)
            lat.append(round((time.perf_counter() - t0) * 1e6))
            if i == 0:
                first = ids
        out.put((index, lat, first, n, converged, None))
        conn.close()
    except Exception as e:  # report, do not hang the others
        ready.put(None)
        out.put((index, [], None, 0, False, repr(e)))


def main():
    ap = argparse.ArgumentParser()
    for name in ('host',):
        ap.add_argument('--' + name, required=True)
    for name in ('port', 'pipe', 'c', 'k', 'clients', 'queries', 'warm-min', 'warm-max', 'batch'):
        ap.add_argument('--' + name, type=int, required=True)
    ap.add_argument('--variant', required=True, choices=('app', 'jvm', 'cpython'))
    a = ap.parse_args()
    conn = connect(a)
    cur = conn.cursor()
    cur.execute("SELECT q, cat FROM queries ORDER BY j")
    queries = [(q, int(c)) for q, c in cur.fetchall()]
    conn.close()
    ctx = mp.get_context('fork')
    ready, out, go = ctx.Queue(), ctx.Queue(), ctx.Event()
    procs = [ctx.Process(target=client, args=(i, a, queries, ready, go, out)) for i in range(a.clients)]
    for p in procs:
        p.start()
    for _ in procs:
        ready.get()
    t0 = time.perf_counter()
    go.set()
    res = sorted(out.get() for _ in procs)
    wall = time.perf_counter() - t0
    for p in procs:
        p.join()
    errors = [r[5] for r in res if r[5]]
    if errors:
        raise SystemExit('client failed: ' + errors[0])
    print(json.dumps({'wall_s': wall, 'lat_us': [x for r in res for x in r[1]], 'ids': res[0][2],
                      'warm': [r[3] for r in res], 'converged': [r[4] for r in res],
                      'versions': {'mysql-connector-python': mysql.connector.__version__, 'numpy': np.__version__}}))


if __name__ == '__main__':
    main()
