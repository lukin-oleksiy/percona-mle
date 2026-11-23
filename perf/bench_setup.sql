-- Data and routines of the MLE benchmark (perf/bench.py). Loaded once into its data directory by the
-- mysql client (DELIMITER is the client's). The same work is written once per runtime:
--   sql_   SQL stored programs (the baseline)
--   v8_    LANGUAGE JS: the V8 worker, or js_lang (V8 inside mysqld) when the router maps JS to it
--   gjs_   LANGUAGE JAVASCRIPT: GraalJS on the JVM worker
--   py_    LANGUAGE PYTHON: GraalPy on the JVM worker
--   cpy_   LANGUAGE CPYTHON: the CPython worker
--   java_  LANGUAGE JAVA: the JVM worker
-- SQL from routines runs in procedures (Oracle's rule for JavaScript; kept for all).

CREATE DATABASE bench;
USE bench;
GRANT CREATE_JAVASCRIPT_ROUTINE, CREATE_PYTHON_ROUTINE, CREATE_JAVA_ROUTINE, CREATE_JS_ROUTINE,
  CREATE_CPYTHON_ROUTINE ON *.* TO root@localhost;
SET SESSION cte_max_recursion_depth = 1000000;

CREATE TABLE t AS WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 1000000) SELECT n FROM c;
CREATE TABLE words AS SELECT n, LEFT(SHA2(n, 256), 8) AS a, LEFT(SHA2(n + 1, 256), 8) AS b FROM t WHERE n <= 20000;
CREATE TABLE rows100k (id INT PRIMARY KEY, v VARCHAR(20), x DOUBLE);
INSERT INTO rows100k SELECT n, CONCAT('row', n), n * 0.5 FROM t WHERE n <= 100000;
CREATE TABLE w (id INT PRIMARY KEY, a VARCHAR(20), x DOUBLE);
CREATE TABLE vecs (id BIGINT UNSIGNED PRIMARY KEY, embedding VECTOR(768) NOT NULL);
CREATE TABLE vecs_hnsw (id BIGINT UNSIGNED PRIMARY KEY, embedding VECTOR(768) NOT NULL,
  VECTOR KEY vk (embedding) TYPE hnsw (M = 16));

-- 100,000 random VECTOR(768), 1,000 rows per multi-row INSERT (executeMany).
CREATE PROCEDURE fill_vecs(IN n INT) LANGUAGE CPYTHON AS $$
    import numpy as np
    rng = np.random.default_rng(42)
    for lo in range(0, n, 1000):
        m = rng.random((min(1000, n - lo), 768), dtype=np.float32)
        session.start_transaction()
        session.execute_many('INSERT INTO vecs VALUES (?, ?)', [(lo + i, m[i]) for i in range(len(m))])
        session.commit()
$$;
CALL fill_vecs(100000);
INSERT INTO vecs_hnsw SELECT * FROM vecs;
DROP PROCEDURE fill_vecs;
-- 10,000 normally distributed VECTOR(768) for the vector library (uniform vectors in [0, 1) are all
-- about as similar to each other, and near-ties would order differently in float32 sums).
CREATE TABLE items (id BIGINT UNSIGNED PRIMARY KEY, embedding VECTOR(768) NOT NULL);
CREATE PROCEDURE fill_items(IN n INT) LANGUAGE CPYTHON AS $$
    import numpy as np
    rng = np.random.default_rng(7)
    for lo in range(0, n, 1000):
        m = rng.standard_normal((min(1000, n - lo), 768), dtype=np.float32)
        session.start_transaction()
        session.execute_many('INSERT INTO items VALUES (?, ?)', [(lo + i, m[i]) for i in range(len(m))])
        session.commit()
$$;
CALL fill_items(10000);
DROP PROCEDURE fill_items;

-- 1. The call: a + b.
CREATE FUNCTION sql_add(a INT, b INT) RETURNS INT DETERMINISTIC RETURN a + b;
CREATE FUNCTION v8_add(a INT, b INT) RETURNS INT LANGUAGE JS AS $$ return a + b $$;
CREATE FUNCTION gjs_add(a INT, b INT) RETURNS INT LANGUAGE JAVASCRIPT AS $$ return a + b $$;
CREATE FUNCTION py_add(a INT, b INT) RETURNS INT LANGUAGE PYTHON AS $$ return a + b $$;
CREATE FUNCTION cpy_add(a INT, b INT) RETURNS INT LANGUAGE CPYTHON AS $$ return a + b $$;
CREATE FUNCTION java_add(a INT, b INT) RETURNS INT LANGUAGE JAVA AS $$ return a + b; $$;

-- 2. Per-row logic: Levenshtein distance of two 8-character strings.
DELIMITER //
CREATE FUNCTION sql_lev(a VARCHAR(64), b VARCHAR(64)) RETURNS INT DETERMINISTIC
BEGIN
  DECLARE la, lb, i, j, c INT;
  DECLARE prev, cur VARBINARY(256);
  SET la = CHAR_LENGTH(a), lb = CHAR_LENGTH(b), prev = '', j = 0;
  WHILE j <= lb DO SET prev = CONCAT(prev, CHAR(j)), j = j + 1; END WHILE;
  SET i = 1;
  WHILE i <= la DO
    SET cur = CHAR(i), j = 1;
    WHILE j <= lb DO
      SET c = LEAST(ORD(SUBSTRING(prev, j + 1, 1)) + 1, ORD(SUBSTRING(cur, j, 1)) + 1,
                    ORD(SUBSTRING(prev, j, 1)) + IF(SUBSTRING(a, i, 1) = SUBSTRING(b, j, 1), 0, 1));
      SET cur = CONCAT(cur, CHAR(c)), j = j + 1;
    END WHILE;
    SET prev = cur, i = i + 1;
  END WHILE;
  RETURN ORD(SUBSTRING(prev, lb + 1, 1));
END//
DELIMITER ;
CREATE FUNCTION v8_lev(a VARCHAR(64), b VARCHAR(64)) RETURNS INT LANGUAGE JS AS $$
  let prev = Array.from({ length: b.length + 1 }, (_, j) => j)
  for (let i = 1; i <= a.length; i++) {
    const cur = [i]
    for (let j = 1; j <= b.length; j++)
      cur[j] = Math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1))
    prev = cur
  }
  return prev[b.length]
$$;
CREATE FUNCTION gjs_lev(a VARCHAR(64), b VARCHAR(64)) RETURNS INT LANGUAGE JAVASCRIPT AS $$
  let prev = Array.from({ length: b.length + 1 }, (_, j) => j)
  for (let i = 1; i <= a.length; i++) {
    const cur = [i]
    for (let j = 1; j <= b.length; j++)
      cur[j] = Math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1))
    prev = cur
  }
  return prev[b.length]
$$;
CREATE FUNCTION py_lev(a VARCHAR(64), b VARCHAR(64)) RETURNS INT LANGUAGE PYTHON AS $$
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]
$$;
CREATE FUNCTION cpy_lev(a VARCHAR(64), b VARCHAR(64)) RETURNS INT LANGUAGE CPYTHON AS $$
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]
$$;
CREATE FUNCTION java_lev(a VARCHAR(64), b VARCHAR(64)) RETURNS INT LANGUAGE JAVA AS $$
    int[] prev = new int[b.length() + 1], cur = new int[b.length() + 1];
    for (int j = 0; j <= b.length(); j++) prev[j] = j;
    for (int i = 1; i <= a.length(); i++) {
      cur[0] = i;
      for (int j = 1; j <= b.length(); j++)
        cur[j] = Math.min(Math.min(prev[j] + 1, cur[j - 1] + 1),
                          prev[j - 1] + (a.charAt(i - 1) == b.charAt(j - 1) ? 0 : 1));
      int[] t = prev; prev = cur; cur = t;
    }
    return prev[b.length()];
$$;

-- 3. JavaScript compute, one call: numbers, objects, strings (V8 and GraalJS).
CREATE FUNCTION v8_math(n INT) RETURNS DOUBLE LANGUAGE JS AS $$
  let s = 0; for (let i = 0; i < n; i++) s += Math.sqrt(i) * Math.sin(i); return s $$;
CREATE FUNCTION gjs_math(n INT) RETURNS DOUBLE LANGUAGE JAVASCRIPT AS $$
  let s = 0; for (let i = 0; i < n; i++) s += Math.sqrt(i) * Math.sin(i); return s $$;
CREATE FUNCTION v8_objects(n INT) RETURNS INT LANGUAGE JS AS $$
  const a = []; for (let i = 0; i < n; i++) a.push({ k: (i * 7919) % n, s: 'x' + i }); a.sort((x, y) => x.k - y.k); return a[n - 1].k $$;
CREATE FUNCTION gjs_objects(n INT) RETURNS INT LANGUAGE JAVASCRIPT AS $$
  const a = []; for (let i = 0; i < n; i++) a.push({ k: (i * 7919) % n, s: 'x' + i }); a.sort((x, y) => x.k - y.k); return a[n - 1].k $$;
CREATE FUNCTION v8_strbuild(n INT) RETURNS INT LANGUAGE JS AS $$
  let parts = []; for (let i = 0; i < n; i++) parts.push(JSON.stringify({ i, v: [i, i * 2] })); return JSON.parse('[' + parts.join(',') + ']').length $$;
CREATE FUNCTION gjs_strbuild(n INT) RETURNS INT LANGUAGE JAVASCRIPT AS $$
  let parts = []; for (let i = 0; i < n; i++) parts.push(JSON.stringify({ i, v: [i, i * 2] })); return JSON.parse('[' + parts.join(',') + ']').length $$;

-- 4. SQL from routines: read 100,000 rows one by one.
DELIMITER //
CREATE PROCEDURE sql_rows(OUT s DOUBLE)
BEGIN
  DECLARE done INT DEFAULT 0; DECLARE a INT; DECLARE b VARCHAR(20); DECLARE c DOUBLE;
  DECLARE cur CURSOR FOR SELECT id, v, x FROM rows100k;
  DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = 1;
  SET s = 0; OPEN cur;
  l: LOOP FETCH cur INTO a, b, c; IF done THEN LEAVE l; END IF; SET s = s + c; END LOOP;
  CLOSE cur;
END//
DELIMITER ;
CREATE PROCEDURE v8_rows(OUT s DOUBLE) LANGUAGE JS AS $$
  const r = session.runSql('SELECT id, v, x FROM rows100k'); let t = 0
  for (let row = r.fetchOne(); row; row = r.fetchOne()) t += row.x
  s = t
$$;
CREATE PROCEDURE gjs_rows(OUT s DOUBLE) LANGUAGE JAVASCRIPT AS $$
  const r = session.runSql('SELECT id, v, x FROM rows100k'); let t = 0
  for (let row = r.fetchOne(); row; row = r.fetchOne()) t += row.x
  s = t
$$;
CREATE PROCEDURE py_rows(OUT s DOUBLE) LANGUAGE PYTHON AS $$
    t = 0.0
    for row in session.run_sql('SELECT id, v, x FROM rows100k'):
        t += row.x
    s = t
$$;
CREATE PROCEDURE cpy_rows(OUT s DOUBLE) LANGUAGE CPYTHON AS $$
    t = 0.0
    for row in session.run_sql('SELECT id, v, x FROM rows100k'):
        t += row.x
    s = t
$$;
CREATE PROCEDURE java_rows(OUT s DOUBLE) LANGUAGE JAVA AS $$
    double t = 0;
    for (Row row : Mle.session().runSql("SELECT id, v, x FROM rows100k")) t += ((Number) row.get(2)).doubleValue();
    s[0] = t;
$$;

-- 5. SQL from routines: many small statements (a query with a parameter, k times).
CREATE PROCEDURE v8_stmts(IN k INT, OUT n INT) LANGUAGE JS AS $$
  let t = 0
  for (let i = 0; i < k; i++) t += session.runSql('SELECT ? + 1 AS v', [i]).fetchOne().v
  n = t
$$;
CREATE PROCEDURE gjs_stmts(IN k INT, OUT n INT) LANGUAGE JAVASCRIPT AS $$
  let t = 0
  for (let i = 0; i < k; i++) t += session.runSql('SELECT ? + 1 AS v', [i]).fetchOne().v
  n = t
$$;
CREATE PROCEDURE py_stmts(IN k INT, OUT n INT) LANGUAGE PYTHON AS $$
    t = 0
    for i in range(k):
        t += session.run_sql('SELECT ? + 1 AS v', [i]).fetch_one().v
    n = t
$$;
CREATE PROCEDURE cpy_stmts(IN k INT, OUT n INT) LANGUAGE CPYTHON AS $$
    t = 0
    for i in range(k):
        t += session.run_sql('SELECT ? + 1 AS v', [i]).fetch_one().v
    n = t
$$;
CREATE PROCEDURE java_stmts(IN k INT, OUT n INT) LANGUAGE JAVA AS $$
    long t = 0;
    var s = Mle.session();
    for (int i = 0; i < k; i++) t += ((Number) s.runSql("SELECT ? + 1 AS v", i).fetchOne().get(0)).longValue();
    n[0] = (int) t;
$$;

-- 6. SQL from routines: write n rows in one transaction (executeMany).
CREATE PROCEDURE v8_write(IN n INT) LANGUAGE JS AS $$
  session.runSql('TRUNCATE w')
  const rows = []
  for (let i = 0; i < n; i++) rows.push([i, 'row' + i, i * 0.5])
  session.startTransaction()
  session.executeMany('INSERT INTO w VALUES (?, ?, ?)', rows)
  session.commit()
$$;
CREATE PROCEDURE gjs_write(IN n INT) LANGUAGE JAVASCRIPT AS $$
  session.runSql('TRUNCATE w')
  const rows = []
  for (let i = 0; i < n; i++) rows.push([i, 'row' + i, i * 0.5])
  session.startTransaction()
  session.executeMany('INSERT INTO w VALUES (?, ?, ?)', rows)
  session.commit()
$$;
CREATE PROCEDURE py_write(IN n INT) LANGUAGE PYTHON AS $$
    session.run_sql('TRUNCATE w')
    rows = [(i, 'row%d' % i, i * 0.5) for i in range(n)]
    session.start_transaction()
    session.execute_many('INSERT INTO w VALUES (?, ?, ?)', rows)
    session.commit()
$$;
CREATE PROCEDURE cpy_write(IN n INT) LANGUAGE CPYTHON AS $$
    session.run_sql('TRUNCATE w')
    rows = [(i, 'row%d' % i, i * 0.5) for i in range(n)]
    session.start_transaction()
    session.execute_many('INSERT INTO w VALUES (?, ?, ?)', rows)
    session.commit()
$$;
CREATE PROCEDURE java_write(IN n INT) LANGUAGE JAVA AS $$
    var s = Mle.session();
    s.runSql("TRUNCATE w");
    java.util.List<Object[]> rows = new java.util.ArrayList<>();
    for (int i = 0; i < n; i++) rows.add(new Object[] {i, "row" + i, i * 0.5});
    s.startTransaction();
    s.prepare("INSERT INTO w VALUES (?, ?, ?)").executeMany(rows);
    s.commit();
$$;

-- 7. Vectors: the 10 nearest of 100,000 by a scan in the routine (numpy; Java's Vector API).
CREATE PROCEDURE py_topk(IN q VECTOR(768), OUT r TEXT) LANGUAGE PYTHON AS $$
    import numpy as np
    c = session.run_sql('SELECT id, embedding FROM vecs').fetch_columns()
    m = c['embedding']
    d = np.einsum('ij,ij->i', m, m) - 2 * (m @ q)
    k = np.argpartition(d, 10)[:10]
    r = ','.join(str(int(c['id'][i])) for i in k[np.argsort(d[k])])
$$;
CREATE PROCEDURE cpy_topk(IN q VECTOR(768), OUT r TEXT) LANGUAGE CPYTHON AS $$
    import numpy as np
    c = session.run_sql('SELECT id, embedding FROM vecs').fetch_columns()
    m = c['embedding']
    d = np.einsum('ij,ij->i', m, m) - 2 * (m @ q)
    k = np.argpartition(d, 10)[:10]
    r = ','.join(str(int(c['id'][i])) for i in k[np.argsort(d[k])])
$$;
CREATE PROCEDURE java_topk(IN q VECTOR(768), OUT ids TEXT) LANGUAGE JAVA AS $$
    var species = jdk.incubator.vector.FloatVector.SPECIES_PREFERRED;
    long[] best = new long[10];
    double[] bestD = new double[10];
    java.util.Arrays.fill(bestD, Double.MAX_VALUE);
    for (Row r : Mle.session().runSql("SELECT id, embedding FROM vecs")) {
      float[] v = (float[]) r.get(1);
      var acc = jdk.incubator.vector.FloatVector.zero(species);
      int i = 0;
      for (; i < species.loopBound(v.length); i += species.length()) {
        var diff = jdk.incubator.vector.FloatVector.fromArray(species, v, i)
            .sub(jdk.incubator.vector.FloatVector.fromArray(species, q, i));
        acc = diff.fma(diff, acc);
      }
      double d = acc.reduceLanes(jdk.incubator.vector.VectorOperators.ADD);
      for (; i < v.length; i++) d += (v[i] - q[i]) * (v[i] - q[i]);
      if (d < bestD[9]) {
        int k = 9;
        while (k > 0 && bestD[k - 1] > d) { bestD[k] = bestD[k - 1]; best[k] = best[k - 1]; k--; }
        bestD[k] = d; best[k] = ((Number) r.get(0)).longValue();
      }
    }
    StringBuilder sb = new StringBuilder();
    for (long b : best) sb.append(sb.length() > 0 ? "," : "").append(b);
    ids[0] = sb.toString();
$$;
-- A VECTOR(768) argument per row.
CREATE FUNCTION py_first(v VECTOR(768)) RETURNS DOUBLE LANGUAGE PYTHON AS $$ return float(v[0]) $$;
CREATE FUNCTION cpy_first(v VECTOR(768)) RETURNS DOUBLE LANGUAGE CPYTHON AS $$ return float(v[0]) $$;

-- 8. The built-in vector library against numpy: for each of 10,000 vectors, its 10 most similar
-- (cosine). Each returns JSON: seconds to load, seconds to compute, a check sum of the ids, threads.
CREATE PROCEDURE java_related(OUT r TEXT) LANGUAGE JAVA AS $$
    long t0 = System.nanoTime();
    VectorSet s = Vectors.load("SELECT id, embedding FROM items ORDER BY id");
    long t1 = System.nanoTime();
    Matches[] all = s.related(10, Vectors.Metric.COSINE);
    long t2 = System.nanoTime();
    long check = 0;
    for (Matches m : all) for (long id : m.ids()) check += id;
    r[0] = String.format("{\"load_s\": %.4f, \"compute_s\": %.4f, \"check\": %d, \"threads\": %d}",
        (t1 - t0) / 1e9, (t2 - t1) / 1e9, check, Vectors.threads());
$$;
CREATE PROCEDURE gjs_related(OUT r TEXT) LANGUAGE JAVASCRIPT AS $$
  const V = Java.type('com.percona.mle.api.Vectors')
  const t0 = Date.now()
  const s = V.load('SELECT id, embedding FROM items ORDER BY id')
  const t1 = Date.now()
  const all = s.related(10, 'cosine')
  const t2 = Date.now()
  let check = 0
  for (const m of all) for (let i = 0; i < m.size(); i++) check += Number(m.id(i))
  r = JSON.stringify({ load_s: (t1 - t0) / 1000, compute_s: (t2 - t1) / 1000, check, threads: V.threads() })
$$;
CREATE PROCEDURE py_related(OUT r TEXT) LANGUAGE PYTHON AS $$
    import java, json, time
    V = java.type('com.percona.mle.api.Vectors')
    t0 = time.perf_counter()
    s = V.load('SELECT id, embedding FROM items ORDER BY id')
    t1 = time.perf_counter()
    rel = s.related(10, 'cosine')
    t2 = time.perf_counter()
    check = sum(sum(m.ids()) for m in rel)
    r = json.dumps({'load_s': t1 - t0, 'compute_s': t2 - t1, 'check': check, 'threads': V.threads()})
$$;
CREATE PROCEDURE py_related_numpy(OUT r TEXT) LANGUAGE PYTHON AS $$
    import json, os, time
    import numpy as np
    K = 10
    t0 = time.perf_counter()
    c = session.run_sql('SELECT id, embedding FROM items ORDER BY id').fetch_columns()
    ids, m = c['id'], c['embedding']
    t1 = time.perf_counter()
    m = m / np.linalg.norm(m, axis=1, keepdims=True)
    n = m.shape[0]
    best = np.empty((n, K), dtype=np.int64)
    for lo in range(0, n, 1024):
        sc = m[lo:lo + 1024] @ m.T
        b = sc.shape[0]
        sc[np.arange(b), np.arange(lo, lo + b)] = -np.inf
        p = np.argpartition(-sc, K, axis=1)[:, :K]
        o = np.argsort(-np.take_along_axis(sc, p, 1), axis=1)
        best[lo:lo + b] = np.take_along_axis(p, o, 1)
    t2 = time.perf_counter()
    r = json.dumps({'load_s': t1 - t0, 'compute_s': t2 - t1, 'check': int(ids[best].sum()),
                    'threads': os.environ.get('OPENBLAS_NUM_THREADS', '')})
$$;
CREATE PROCEDURE cpy_related_numpy(OUT r TEXT) LANGUAGE CPYTHON AS $$
    import json, os, time
    import numpy as np
    K = 10
    t0 = time.perf_counter()
    c = session.run_sql('SELECT id, embedding FROM items ORDER BY id').fetch_columns()
    ids, m = c['id'], c['embedding']
    t1 = time.perf_counter()
    m = m / np.linalg.norm(m, axis=1, keepdims=True)
    n = m.shape[0]
    best = np.empty((n, K), dtype=np.int64)
    for lo in range(0, n, 1024):
        sc = m[lo:lo + 1024] @ m.T
        b = sc.shape[0]
        sc[np.arange(b), np.arange(lo, lo + b)] = -np.inf
        p = np.argpartition(-sc, K, axis=1)[:, :K]
        o = np.argsort(-np.take_along_axis(sc, p, 1), axis=1)
        best[lo:lo + b] = np.take_along_axis(p, o, 1)
    t2 = time.perf_counter()
    r = json.dumps({'load_s': t1 - t0, 'compute_s': t2 - t1, 'check': int(ids[best].sum()),
                    'threads': os.environ.get('OPENBLAS_NUM_THREADS', '')})
$$;

-- 9. Python with numpy loaded (a worker's start and memory with numpy).
CREATE FUNCTION py_np(a INT) RETURNS INT LANGUAGE PYTHON AS $$
    import numpy as np
    return int(np.int64(a) + 1)
$$;
CREATE FUNCTION cpy_np(a INT) RETURNS INT LANGUAGE CPYTHON AS $$
    import numpy as np
    return int(np.int64(a) + 1)
$$;

-- 10. Versions of the runtimes (the report's environment).
CREATE FUNCTION py_version() RETURNS TEXT LANGUAGE PYTHON AS $$
    import sys, numpy
    return 'GraalPy %s (Python %s), numpy %s' % (
        '.'.join(map(str, sys.implementation.version[:3])), sys.version.split()[0], numpy.__version__)
$$;
CREATE FUNCTION cpy_version() RETURNS TEXT LANGUAGE CPYTHON AS $$
    import sys, numpy
    return 'CPython %s, numpy %s' % (sys.version.split()[0], numpy.__version__)
$$;
CREATE FUNCTION java_version() RETURNS TEXT LANGUAGE JAVA AS $$
    return System.getProperty("java.vm.name") + " " + System.getProperty("java.vm.version");
$$;
