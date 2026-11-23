#!/usr/bin/env python3
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
"""Benchmark of Percona MLE: every worker kind, and SQL stored programs, on one build.

    perf/bench.py --server <server build> --mle <MLE build> [--suite calls,library,cold,memory]
                  [--quick] [--only REGEX] [--work DIR] [--port 13995] [--wait-quiet] [--json FILE]

One server with every worker kind (the JVM worker: GraalJS, GraalPy, Java; the CPython worker;
the V8 worker) and, when the server build has them, js_lang (V8 inside mysqld) and the language
router (it maps LANGUAGE JS to the V8 worker or to js_lang). The same work is written once per
runtime (perf/bench_setup.sql); the data directory is made once and reused.

Suites:
  calls    the call (a + b) and per-row logic (Levenshtein) per row, net of a plain scan;
           JavaScript compute; SQL from routines (read 100k rows, small statements, executeMany);
           vectors (10 nearest of 100k: SQL, HNSW, numpy, Java's Vector API; a VECTOR argument)
  library  the built-in vector library (related items, 10k vectors) from Java, JavaScript and
           Python, against numpy in GraalPy and in CPython
  cold     a new connection's first call, per runtime: no pool (a new worker) and a pool worker
  memory   PSS per worker, per runtime: one worker, and eight at once; the JVM worker with its
           AOT cache relocated (the JDK's default) against mapped at its own address (ours);
           js_lang's cost in mysqld. Needs sudo -n (workers are not dumpable).

Times of statements are measured inside the server (SYSDATE(6)) in one session, after a warm-up
that ends when two runs in a row are within 3 %; cold starts at the client (a new mysql process).
Release builds (RelWithDebInfo), a quiet machine and the CPU governor "performance" give stable
numbers; the report states the machine. --quick: fewer runs, for checking the tool.
"""
import argparse
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import threading
import time

import mle_perf

HERE = os.path.dirname(os.path.abspath(__file__))
DB = 'bench'

# Runtime: (routine prefix, process name of its worker, its pool variable, what it is).
RUNTIMES = {
    'sql':     ('sql', None, None, 'SQL stored program'),
    'js_lang': ('v8', None, None, 'js_lang (V8 inside mysqld), LANGUAGE JS'),
    'v8':      ('v8', 'mle_v8_worker', 'v8_pool_size', 'V8 worker, LANGUAGE JS'),
    'graaljs': ('gjs', 'mle_worker', 'pool_size', 'JVM worker, GraalJS, LANGUAGE JAVASCRIPT'),
    'graalpy': ('py', 'mle_worker', 'pool_size', 'JVM worker, GraalPy, LANGUAGE PYTHON'),
    'java':    ('java', 'mle_worker', 'pool_size', 'JVM worker, LANGUAGE JAVA'),
    'cpython': ('cpy', 'mle_cpython', 'cpython_pool_size', 'CPython worker, LANGUAGE CPYTHON'),
}
WORKER_KINDS = ('v8', 'graaljs', 'graalpy', 'java', 'cpython')

SCAN_T = 'SELECT SUM(n + 1) INTO @x FROM t'
SCAN_WORDS = 'SELECT SUM(LENGTH(a) + LENGTH(b)) INTO @x FROM words'
SCAN_VECS = 'SELECT SUM(id + 1) INTO @x FROM vecs'

# calls: (case, runtimes, statement with {p} for the prefix, unit, (scan, rows) or divisor)
#   unit us_per_row: (time - scan) / rows; us_per_stmt: time / divisor; ms: time.
CALLS = [
    ('add', ('sql', 'js_lang', 'v8', 'graaljs', 'graalpy', 'cpython', 'java'),
     'SELECT SUM({p}_add(n, 1)) INTO @x FROM t', 'us_per_row', (SCAN_T, 1000000)),
    ('levenshtein', ('sql', 'js_lang', 'v8', 'graaljs', 'graalpy', 'cpython', 'java'),
     'SELECT SUM({p}_lev(a, b)) INTO @x FROM words', 'us_per_row', (SCAN_WORDS, 20000)),
    ('js_math_5m', ('js_lang', 'v8', 'graaljs'), 'SELECT {p}_math(5000000) INTO @x', 'ms', None),
    ('js_sort_200k_objects', ('js_lang', 'v8', 'graaljs'), 'SELECT {p}_objects(200000) INTO @x', 'ms', None),
    ('js_json_200k', ('js_lang', 'v8', 'graaljs'), 'SELECT {p}_strbuild(200000) INTO @x', 'ms', None),
    ('read_100k_rows', ('sql', 'v8', 'graaljs', 'graalpy', 'cpython', 'java'), 'CALL {p}_rows(@x)', 'ms', None),
    ('small_statements', ('v8', 'graaljs', 'graalpy', 'cpython', 'java'), 'CALL {p}_stmts(5000, @x)',
     'us_per_stmt', 5000),
    ('execute_many_1000', ('v8', 'graaljs', 'graalpy', 'cpython', 'java'), 'CALL {p}_write(1000)', 'ms', None),
    ('top10_of_100k', ('sql', 'hnsw', 'graalpy', 'cpython', 'java'), 'CALL {p}_topk(@q, @x)', 'ms', None),
    ('vector_argument', ('graalpy', 'cpython'), 'SELECT SUM({p}_first(embedding)) INTO @x FROM vecs',
     'us_per_row', (SCAN_VECS, 100000)),
]
TOPK_SQL = {
    'sql': "SELECT GROUP_CONCAT(id) INTO @x FROM (SELECT id FROM vecs "
           "ORDER BY DISTANCE(embedding, @q, 'EUCLIDEAN') LIMIT 10) s",
    'hnsw': "SELECT GROUP_CONCAT(id) INTO @x FROM (SELECT id FROM vecs_hnsw "
            "ORDER BY DISTANCE(embedding, @q, 'EUCLIDEAN') LIMIT 10) s",
}
LIBRARY = [  # (variant, procedure, what); *_1_thread with mle.vector_threads = 1 (last: it restarts workers)
    ('library_java', 'java_related', 'the library from Java'),
    ('library_graaljs', 'gjs_related', 'the library from JavaScript'),
    ('library_graalpy', 'py_related', 'the library from Python (GraalPy)'),
    ('numpy_graalpy', 'py_related_numpy', 'numpy in GraalPy (JVM worker)'),
    ('numpy_cpython', 'cpy_related_numpy', 'numpy in CPython (CPython worker)'),
    ('library_java_1_thread', 'java_related', 'the library from Java, one thread (mle.vector_threads = 1)'),
]
# A runtime's first call, and the statements that bring a worker to the state measured.
FIRST_CALL = {r: 'SELECT %s_add(1, 1)' % RUNTIMES[r][0] for r in RUNTIMES if r != 'sql'}
FIRST_CALL.update({'graalpy_numpy': 'SELECT py_np(1)', 'cpython_numpy': 'SELECT cpy_np(1)', 'none': 'SELECT 1'})
MEMORY_STATES = {
    'v8': ['SELECT SUM(v8_add(n, 1)) INTO @x FROM t WHERE n <= 10000', 'SELECT v8_lev("kitten", "sitting")'],
    'graaljs': ['SELECT SUM(gjs_add(n, 1)) INTO @x FROM t WHERE n <= 10000', 'SELECT gjs_lev("kitten", "sitting")'],
    'graalpy': ['SELECT SUM(py_add(n, 1)) INTO @x FROM t WHERE n <= 10000', 'SELECT py_lev("kitten", "sitting")'],
    'graalpy_numpy': ['SELECT SUM(py_add(n, 1)) INTO @x FROM t WHERE n <= 10000', 'SELECT py_np(1)'],
    'java': ['SELECT SUM(java_add(n, 1)) INTO @x FROM t WHERE n <= 10000', 'SELECT java_lev("kitten", "sitting")'],
    'cpython': ['SELECT SUM(cpy_add(n, 1)) INTO @x FROM t WHERE n <= 10000', 'SELECT cpy_lev("kitten", "sitting")'],
    'cpython_numpy': ['SELECT SUM(cpy_add(n, 1)) INTO @x FROM t WHERE n <= 10000', 'SELECT cpy_np(1)'],
}
AOT_RELOCATED = {'MLE_JVM_OPTIONS': '-XX:+UnlockDiagnosticVMOptions -XX:ArchiveRelocationMode=1'}


def kind_of(state):
    return state.split('_numpy')[0]


class Bench:
    def __init__(self, args):
        self.a = args
        self.server = mle_perf.Server(args.server, args.work, args.port)
        self.has_router = self.server.has_component('component_lang_router')
        self.has_js_lang = self.has_router and self.server.has_component('component_js_lang')
        self.results = {}
        self.max_warm, self.runs, self.samples = (4, 3, 3) if args.quick else (20, 5, 7)
        self.defaults = {}
        self.map_now = 'mle'
        self.globals_now = {}

    # --- the server ----------------------------------------------------------------------------

    def options(self):
        opts = ['--loose-mle.sql_result_max=1073741824']
        if self.has_router:
            opts.append('--loose-lang_router.language_map=JS=mle')
        if self.has_js_lang:
            opts.append('--loose-js_lang.max_mem_size=1073741824')
        return opts

    def start(self, env=None):
        self.server.start(self.a.mle, self.options(), env)
        self.defaults = dict(zip(('pool_size', 'cpython_pool_size', 'v8_pool_size', 'worker_cache_size',
                                  'vector_threads'),
                                 self.server.value('SELECT @@mle.pool_size, @@mle.cpython_pool_size, '
                                                   '@@mle.v8_pool_size, @@mle.worker_cache_size, '
                                                   '@@mle.vector_threads').split('\t')))
        self.globals_now = dict(self.defaults)
        self.map_now = 'mle'
        self.wait_pools()

    def set_global(self, **kv):
        """Sets MLE variables that differ from their current values (a pool setting restarts workers)."""
        kv = {k: str(v) for k, v in kv.items() if self.globals_now.get(k) != str(v)}
        if kv:
            self.server.sql('SET ' + ', '.join('GLOBAL mle.%s = %s' % (k, v) for k, v in kv.items()))
            self.globals_now.update(kv)

    def wait_pools(self, timeout=90):
        """Until every kind's pool is full (a changed pool setting restarts the workers)."""
        want = {'mle_worker': int(self.server.value('SELECT @@mle.pool_size')),
                'mle_cpython': int(self.server.value('SELECT @@mle.cpython_pool_size')),
                'mle_v8_worker': int(self.server.value('SELECT @@mle.v8_pool_size'))}
        end = time.time() + timeout
        while time.time() < end:
            have = self.count_workers()
            if all(have.get(k, 0) >= n for k, n in want.items()):
                time.sleep(1.0)  # pool workers pre-warm after READY
                return
            time.sleep(0.25)
        print('# WARNING: the pools did not fill in %d s: %s' % (timeout, self.count_workers()), flush=True)

    def count_workers(self):
        have = {}
        for _, comm in self.server.workers():
            for k in ('mle_worker', 'mle_cpython', 'mle_v8_worker'):
                if comm == k or (k == 'mle_cpython' and comm.startswith(k)):
                    have[k] = have.get(k, 0) + 1
        return have

    def workers_of(self, runtime):
        name = RUNTIMES[kind_of(runtime)][1]
        return [pid for pid, comm in self.server.workers() if comm == name or
                (name == 'mle_cpython' and comm.startswith(name))]

    def js_map(self, engine):
        """Where the router sends LANGUAGE JS: a global map, looked up when a routine runs. js_lang
        is measured with the map set around it, in sessions of its own (routines cached per session)."""
        if self.has_router and engine != self.map_now:
            self.server.sql("SET GLOBAL lang_router.language_map = 'JS=%s'" % engine)
            self.map_now = engine

    def session(self):
        return mle_perf.Session(self.server, DB)

    def wanted(self, key):
        return not self.a.only or re.search(self.a.only, key)

    def record(self, key, value, unit, **detail):
        self.results[key] = dict(value=value, unit=unit, load=mle_perf.load(), **detail)
        print('  %-44s %12.3f %s%s' % (key, value, unit,
                                       '' if detail.get('converged', True) else '  (warm-up did not converge)'),
              flush=True)

    def runtimes(self, names):
        return [r for r in names if r != 'js_lang' or self.has_js_lang]

    # --- suites --------------------------------------------------------------------------------

    def suite_calls(self):
        print('== calls', flush=True)
        sessions = {}
        scans = {}
        try:
            for case, runtimes, stmt, unit, base in CALLS:
                checks = {}
                for r in self.runtimes(runtimes):
                    key = 'calls.%s.%s' % (case, r)
                    if not self.wanted(key):
                        continue
                    self.js_map('js_lang' if r == 'js_lang' else 'mle')
                    s = sessions.get(r == 'js_lang')
                    if s is None:
                        s = sessions[r == 'js_lang'] = self.session()
                        s.q('SET @q = (SELECT embedding FROM vecs WHERE id = 7)')
                    if case == 'top10_of_100k' and r in TOPK_SQL:
                        q = TOPK_SQL[r]
                    else:
                        q = stmt.format(p=RUNTIMES[r][0])
                    m = s.warm_and_time(q, self.max_warm, self.runs)
                    checks[r] = s.value('SELECT @x')
                    if unit == 'us_per_row':
                        scan, rows = base
                        sk = (r == 'js_lang', scan)
                        if sk not in scans:
                            scans[sk] = s.warm_and_time(scan, self.max_warm, self.runs)
                        value = (m['best'] - scans[sk]['best']) / rows
                        m['scan_us'] = scans[sk]['best']
                        m['rows'] = rows
                    elif unit == 'us_per_stmt':
                        value = m['best'] / base
                    else:
                        value = m['best'] / 1000
                    self.record(key, value, unit, **m)
                self.check_same(case, checks)
        finally:
            self.js_map('mle')
            for s in sessions.values():
                s.close()

    def check_same(self, case, checks):
        """Every runtime computed the same (the sums; the ids of the 10 nearest as a set). HNSW is
        approximate: its recall against the exact 10 nearest is recorded instead."""
        if 'hnsw' in checks and 'sql' in checks:
            got = set(checks.pop('hnsw').split(','))
            key = 'calls.top10_of_100k.hnsw'
            if key in self.results:
                self.results[key]['recall'] = len(got & set(checks['sql'].split(','))) / 10
        def norm(v):
            if case == 'top10_of_100k':
                return frozenset(v.split(','))
            try:
                return round(float(v), 3)
            except ValueError:
                return v
        values = {norm(v) for v in checks.values()}
        if len(values) > 1:
            print('# WARNING: %s: the runtimes disagree: %s' % (case, checks), flush=True)
            self.results.setdefault('warnings', {'value': 0, 'unit': '', 'list': []})['list'].append(
                {'case': case, 'values': checks})

    def suite_library(self):
        print('== library (related items of 10,000 vectors, cosine)', flush=True)
        s = self.session()
        try:
            checks = {}
            for variant, proc, what in LIBRARY:
                key = 'library.related_10k.' + variant
                if not self.wanted(key):
                    continue
                threads = '1' if variant.endswith('_1_thread') else self.defaults['vector_threads']
                if self.globals_now.get('vector_threads') != threads:
                    s.close()
                    self.set_global(vector_threads=threads)  # restarts the workers
                    self.wait_pools()
                    s = self.session()
                runs = []
                for _ in range(self.runs if not self.a.quick else 2):
                    runs.append(json.loads(s.value('CALL %s(@r); SELECT @r' % proc)))
                best = min(runs, key=lambda x: x['compute_s'])
                checks[variant] = best['check']
                self.record(key, best['compute_s'] * 1000, 'ms', what=what, runs=runs,
                            load_ms=best['load_s'] * 1000, threads=best['threads'])
            if len(set(checks.values())) > 1:
                print('# NOTE: library check sums differ (ties in cosine order): %s' % checks, flush=True)
        finally:
            s.close()
            self.set_global(vector_threads=self.defaults['vector_threads'])

    def cold_once(self, stmt):
        t0 = time.perf_counter()
        r = subprocess.run(self.server.client(DB, '-e', stmt), capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError('%s: %s' % (stmt, r.stderr.strip()))
        return (time.perf_counter() - t0) * 1000

    def suite_cold(self):
        print('== cold start: a new connection and its first call (ms at the client)', flush=True)
        variants = ['none', 'js_lang'] if self.has_js_lang else ['none']
        variants += ['v8', 'graaljs', 'graalpy', 'graalpy_numpy', 'java', 'cpython', 'cpython_numpy']
        self.set_global(worker_cache_size=0)
        try:
            for mode in ('no_pool', 'pool'):
                for v in variants:
                    key = 'cold.%s.%s' % (mode, v)
                    if (mode == 'pool' and v in ('none', 'js_lang')) or not self.wanted(key):
                        continue
                    kind = kind_of(v)
                    pool_var = RUNTIMES[kind][2] if kind in RUNTIMES else None
                    if pool_var:
                        self.set_global(**{pool_var: 0 if mode == 'no_pool' else self.defaults[pool_var]})
                        self.wait_pools()
                    self.js_map('js_lang' if v == 'js_lang' else 'mle')
                    ts = []
                    for _ in range(self.samples):
                        ts.append(self.cold_once(FIRST_CALL[v]))
                        if mode == 'pool':
                            self.wait_pools()
                        else:
                            time.sleep(0.3)
                    self.js_map('mle')
                    self.record(key, statistics.median(ts), 'ms', runs=ts)
        finally:
            self.js_map('mle')
            self.set_global(**{k: v for k, v in self.defaults.items()})
            self.wait_pools()

    def pss_of(self, runtime):
        vals = [mle_perf.read_pss(p) for p in self.workers_of(runtime)]
        if any(v is None for v in vals):
            raise RuntimeError('cannot read the workers\' memory: needs sudo -n')
        return vals

    def memory_states(self, prefix, states):
        for state in states:
            for n in (1, 8):
                key = 'memory.%s.%s.%s' % (prefix, 'one' if n == 1 else 'eight', state)
                if not self.wanted(key):
                    continue
                sessions = [self.session() for _ in range(n)]
                try:
                    def warm(s):
                        for q in MEMORY_STATES[state]:
                            s.q(q)
                    ts = [threading.Thread(target=warm, args=(s,)) for s in sessions]
                    for t in ts:
                        t.start()
                    for t in ts:
                        t.join()
                    time.sleep(2)
                    vals = self.pss_of(state)
                    if len(vals) != n:
                        print('# WARNING: %s: %d workers, expected %d' % (key, len(vals), n), flush=True)
                    pss = sorted(v['pss'] for v in vals)
                    self.record(key, statistics.median(pss), 'MiB_pss_per_worker', pss=pss,
                                rss=sorted(v['rss'] for v in vals), total_pss=sum(pss))
                finally:
                    for s in sessions:
                        s.close()
                    time.sleep(1)

    def js_lang_memory(self):
        key = 'memory.mle.eight.js_lang'
        if not self.has_js_lang or not self.wanted(key):
            return
        before = mle_perf.read_pss(self.server.proc.pid)['pss']
        self.js_map('js_lang')
        sessions = [self.session() for _ in range(8)]
        try:
            for s in sessions:
                s.q(MEMORY_STATES['v8'][0])
                s.q(MEMORY_STATES['v8'][1])
            time.sleep(2)
            after = mle_perf.read_pss(self.server.proc.pid)['pss']
            self.record(key, (after - before) / 8, 'MiB_pss_per_session_in_mysqld', mysqld_before=before,
                        mysqld_after=after)
        finally:
            self.js_map('mle')
            for s in sessions:
                s.close()

    def suite_memory(self):
        print('== memory: PSS per worker (MiB); pools off, no kept workers', flush=True)
        if subprocess.run(['sudo', '-n', 'true'], capture_output=True).returncode != 0:
            print('# memory: skipped, sudo -n is not available (worker memory needs root to read)', flush=True)
            return
        states = ['v8', 'graaljs', 'graalpy', 'graalpy_numpy', 'java', 'cpython', 'cpython_numpy']
        off = dict(pool_size=0, cpython_pool_size=0, v8_pool_size=0, worker_cache_size=0)
        self.set_global(**off)
        time.sleep(2)
        try:
            self.memory_states('mle', states)
            self.js_lang_memory()
        finally:
            self.set_global(**self.defaults)
        if any(self.wanted('memory.aot_relocated.%s.%s' % (n, s)) for n in ('one', 'eight')
               for s in ('graaljs', 'graalpy_numpy')):
            print('== memory: the JVM worker with its AOT cache relocated (the JDK default)', flush=True)
            self.server.stop()
            self.start(env=AOT_RELOCATED)
            try:
                self.set_global(**off)
                time.sleep(2)
                self.memory_states('aot_relocated', ['graaljs', 'graalpy_numpy'])
            finally:
                self.server.stop()
                self.start()

    # --- the environment -----------------------------------------------------------------------

    def environment(self):
        def first(path, key):
            try:
                for line in open(path):
                    if line.startswith(key):
                        return line.split(':', 1)[1].strip()
            except OSError:
                pass
            return ''
        osname = ''
        try:
            osname = dict(l.rstrip('\n').split('=', 1) for l in open('/etc/os-release') if '=' in l
                          ).get('PRETTY_NAME', '').strip('"')
        except OSError:
            pass
        s = self.server
        versions = {}
        for f in ('py_version', 'cpy_version', 'java_version'):
            try:
                versions[f.split('_')[0]] = s.value('SELECT %s.%s()' % (DB, f))
            except RuntimeError as e:
                versions[f.split('_')[0]] = 'error: %s' % str(e).splitlines()[-1]
        libnode = mle_perf.cmake_cache(self.a.mle, 'MLE_LIBNODE')
        settings = s.sql("SELECT VARIABLE_NAME, VARIABLE_VALUE FROM performance_schema.global_variables "
                         "WHERE VARIABLE_NAME LIKE 'mle.%' OR VARIABLE_NAME LIKE 'lang_router.%'")
        return {
            'cpu': first('/proc/cpuinfo', 'model name'), 'cpus': os.cpu_count(),
            'memory_gib': round(int(first('/proc/meminfo', 'MemTotal').split()[0]) / 1048576, 1),
            'kernel': platform.release(), 'os': osname, 'governor': mle_perf.governor(),
            'server_build': self.a.server, 'server_commit': mle_perf.source_commit(self.a.server),
            'server_build_type': mle_perf.cmake_cache(self.a.server, 'CMAKE_BUILD_TYPE'),
            'mle_build': self.a.mle, 'mle_commit': mle_perf.source_commit(self.a.mle),
            'mle_build_type': mle_perf.cmake_cache(self.a.mle, 'CMAKE_BUILD_TYPE'),
            'server_version': s.value('SELECT VERSION()'),
            'languages': s.value("SELECT VARIABLE_VALUE FROM performance_schema.global_status "
                                 "WHERE VARIABLE_NAME = 'mle_languages_supported'"),
            'graalpy': versions.get('py'), 'cpython': versions.get('cpy'), 'jvm': versions.get('java'),
            'libnode': os.path.basename(os.path.realpath(libnode)) if libnode else '',
            'js_lang': self.has_js_lang, 'router': self.has_router,
            'settings': dict(l.split('\t', 1) for l in settings.splitlines() if '\t' in l),
        }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--server', required=True, help='the server build directory (release)')
    ap.add_argument('--mle', required=True, help='the MLE build directory (release)')
    ap.add_argument('--suite', default='calls,library,cold,memory', help='suites, comma-separated')
    ap.add_argument('--quick', action='store_true', help='fewer runs: checks the tool, not for numbers')
    ap.add_argument('--only', default='', help='results whose key matches this regular expression')
    ap.add_argument('--work', default=None, help='data directory and logs (default <MLE build>/perf-bench)')
    ap.add_argument('--port', type=int, default=13995)
    ap.add_argument('--wait-quiet', action='store_true', help='wait for a load average below 1.5 first')
    ap.add_argument('--json', default=None, help='write the results here')
    args = ap.parse_args()
    args.server, args.mle = os.path.abspath(args.server), os.path.abspath(args.mle)
    args.work = os.path.abspath(args.work or os.path.join(args.mle, 'perf-bench'))
    os.makedirs(args.work, exist_ok=True)
    suites = [x.strip() for x in args.suite.split(',') if x.strip()]
    for x in suites:
        if not hasattr(Bench, 'suite_' + x):
            sys.exit('no suite %s' % x)

    b = Bench(args)
    components = [('component_js_lang', False), ('component_mle', True), ('component_lang_router', False)]
    b.server.prepare_data(args.mle, os.path.join(HERE, 'bench_setup.sql'), components, b.options())
    load = mle_perf.wait_quiet(enabled=args.wait_quiet)
    started = time.time()
    b.start()
    try:
        env = b.environment()
        env['load_start'] = load
        print('# %s, %d CPUs, %.0f GiB, %s, governor %s, load %.2f' %
              (env['cpu'], env['cpus'], env['memory_gib'], env['os'], env['governor'], load))
        print('# MLE %s (%s), server %s (%s)' % (env['mle_commit'], env['mle_build_type'], env['server_commit'],
                                                 env['server_build_type']))
        print('# %s; %s; %s; %s; js_lang %s' % (env['graalpy'], env['cpython'], env['jvm'], env['libnode'],
                                                'yes' if env['js_lang'] else 'no'), flush=True)
        if env['governor'] not in ('performance', 'unknown'):
            print('# WARNING: the CPU governor is "%s": times vary more; use "performance"' % env['governor'])
        for x in suites:
            getattr(b, 'suite_' + x)()
    finally:
        b.server.stop()
    env['load_end'] = mle_perf.load()
    env['seconds'] = round(time.time() - started)
    env['quick'] = args.quick
    print('# done in %d s, load %.2f' % (env['seconds'], env['load_end']))
    if args.json:
        with open(args.json, 'w') as f:
            json.dump({'schema': 1, 'tool': 'perf/bench.py', 'time': time.strftime('%Y-%m-%dT%H:%M:%S%z'),
                       'suites': suites, 'environment': env, 'results': b.results}, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
