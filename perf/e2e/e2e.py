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
"""End-to-end benchmark of Percona MLE: the same logic in an application and in a stored procedure.

    perf/e2e/e2e.py --server <server build> --mle <MLE build> [--work DIR] [--port 13994] [--quick]
                    [--cases chatty,mmr500,...] [--only REGEX] [--net delay|loopback] [--delay-ms 1]
                    [--json FILE]

One server (the server build with the MLE build's component), 100,000 x VECTOR(768) with an HNSW
index; the data directory is made once (setup.sql) and reused. Clients in Java (Connector/J) and in
Python (mysql-connector-python, numpy) run each case either as an application (the logic in the
client, several round trips where the logic needs them) or as one CALL of a procedure with the same
logic: LANGUAGE JAVA (JVM worker) from the Java client; LANGUAGE PYTHON (GraalPy, JVM worker) and
LANGUAGE CPYTHON (CPython worker) from the Python client. Every variant must pick the same ids.

The clients sit in a network namespace whose packets from the server are delayed 1 ms (netem; a
round trip takes ~1.1 ms), as an application server on a LAN. That needs sudo -n; without it, or
with --net loopback, the clients use loopback (the results say which).

Cases (the deck's): chatty, 3 dependent queries; mmr500, MMR re-rank of 500 HNSW candidates;
rescore1000, rescore 1,000 candidates; bulk, all ~5,000 vectors of a category; wb200, wb1000,
write-back of 200 / 1,000 enriched rows in one transaction; wbvec200, of 200 rows with a
VECTOR(768) each. One client (latency) and, for chatty, mmr500 and wb200, several at once
(throughput). Each measurement warms its connections up until two batches in a row agree within
3 %. --quick: fewer requests, for checking the tool, not for numbers.

Results: e2e.<case>.<client>.<variant>.<metric> (p50_ms, p95_ms, qps; c<N>_... with N clients).
"""
import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import mle_perf  # noqa: E402

CONNECTOR_J = '9.5.0'
PY_PACKAGES = ['mysql-connector-python==26.10.0', 'numpy==2.5.5']

# name: (pipe, C, K, clients for the throughput point (0: none), what)
CASES = {
    'chatty':      (3, 200, 10, 8, '3 dependent queries: profile and history, 200 HNSW candidates, promotions'),
    'mmr500':      (2, 500, 10, 8, 'MMR re-rank of 500 HNSW candidates (with their vectors)'),
    'rescore1000': (1, 1000, 10, 0, 'rescore 1,000 HNSW candidates'),
    'bulk':        (4, 0, 10, 0, 'all ~5,000 vectors of a category: centroid, 10 typical, 10 outliers'),
    'wb200':       (5, 200, 10, 4, 'write-back: read 200 rows, score, upsert, commit'),
    'wb1000':      (5, 1000, 10, 0, 'write-back: read 1,000 rows, score, upsert, commit'),
    'wbvec200':    (6, 200, 10, 0, 'write-back: 200 rows with a VECTOR(768) each'),
}
# (client, variant, what)
VARIANTS = [
    ('java', 'app', 'Java application (Connector/J)'),
    ('java', 'jvm', 'MLE, LANGUAGE JAVA (JVM worker)'),
    ('python', 'app', 'Python application (numpy)'),
    ('python', 'jvm', 'MLE, LANGUAGE PYTHON (GraalPy)'),
    ('python', 'cpython', 'MLE, LANGUAGE CPYTHON'),
]


def mem_available_gib():
    for line in open('/proc/meminfo'):
        if line.startswith('MemAvailable:'):
            return int(line.split()[1]) / 1048576
    return 0.0


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.exit('failed: %s\n%s%s' % (' '.join(cmd), r.stdout[-2000:], r.stderr[-2000:]))
    return r.stdout


def sha1_of(*paths, extra=''):
    h = hashlib.sha1(extra.encode())
    for p in paths:
        h.update(open(p, 'rb').read())
    return h.hexdigest()


class E2E:
    def __init__(self, a):
        self.a = a
        self.server = mle_perf.Server(a.server, a.work, a.port)
        self.results = {}
        self.runs = []
        self.ids = {}
        self.netns = 'e2e%d' % a.port
        self.prefix = a.subnet
        self.net = None  # {'mode': ..., ...}
        self.host = '127.0.0.1'
        # warm-up per connection: (at least, at most, batch); the JVM worker's JIT needs hundreds of
        # calls: two batches that agree earlier do not end it before the minimum
        if a.quick:
            self.queries, self.queries_conc, self.warm = 100, 50, (150, 400, 25)
        else:
            self.queries, self.queries_conc, self.warm = 200, 200, (400, 2000, 50)

    # --- the clients ---------------------------------------------------------------------------

    def java_home(self):
        home = self.a.java_home or mle_perf.cmake_cache(self.a.mle, 'MLE_JAVA_HOME')
        if not home or not os.path.exists(os.path.join(home, 'bin', 'javac')):
            sys.exit('no JDK: MLE_JAVA_HOME of %s is "%s"; give --java-home' % (self.a.mle, home))
        return home

    def prepare_java(self):
        home = self.java_home()
        lib = os.path.join(self.a.work, 'lib')
        jar = os.path.join(lib, 'mysql-connector-j-%s.jar' % CONNECTOR_J)
        if not os.path.exists(jar):
            mvn = mle_perf.cmake_cache(self.a.mle, 'MLE_MAVEN') or shutil.which('mvn')
            if not mvn:
                sys.exit('no Maven to fetch Connector/J: put %s there by hand' % jar)
            print('# fetching Connector/J %s (once)' % CONNECTOR_J, flush=True)
            os.makedirs(lib, exist_ok=True)
            run([mvn, '-q', '-B', 'dependency:copy', '-Dartifact=com.mysql:mysql-connector-j:' + CONNECTOR_J,
                 '-DoutputDirectory=' + lib], env=dict(os.environ, JAVA_HOME=home))
        sources = [os.path.join(HERE, 'client', n) for n in ('E2E.java', 'Rerank.java', 'SimdDot.java')]
        classes = os.path.join(self.a.work, 'classes')
        stamp = os.path.join(classes, '.sha1')
        digest = sha1_of(*sources, extra=home)
        if not (os.path.exists(stamp) and open(stamp).read() == digest):
            shutil.rmtree(classes, ignore_errors=True)
            run([os.path.join(home, 'bin', 'javac'), '-nowarn', '-d', classes, '--add-modules', 'jdk.incubator.vector',
                 '-cp', jar] + sources)
            open(stamp, 'w').write(digest)
        self.java = [os.path.join(home, 'bin', 'java'), '--add-modules', 'jdk.incubator.vector',
                     '-cp', os.path.join(lib, '*') + ':' + classes, 'e2e.E2E']
        r = subprocess.run([os.path.join(home, 'bin', 'java'), '-version'], capture_output=True, text=True)
        self.java_version = r.stderr.splitlines()[1].strip() if r.returncode == 0 else ''
        self.connector_j = os.path.basename(jar)

    def prepare_python(self):
        if self.a.python:
            py = self.a.python
        else:
            venv = os.path.join(self.a.work, 'venv')
            py = os.path.join(venv, 'bin', 'python')
            if not os.path.exists(os.path.join(venv, '.ready')):
                print('# making the Python client venv (once): %s' % ' '.join(PY_PACKAGES), flush=True)
                shutil.rmtree(venv, ignore_errors=True)
                run([sys.executable, '-m', 'venv', venv])
                run([py, '-m', 'pip', 'install', '-q', '--disable-pip-version-check'] + PY_PACKAGES)
                open(os.path.join(venv, '.ready'), 'w').write('\n'.join(PY_PACKAGES) + '\n')
        r = subprocess.run([py, '-c', 'import mysql.connector, numpy'], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit('%s cannot import mysql.connector and numpy:\n%s' % (py, r.stderr))
        self.python = [py, os.path.join(HERE, 'client', 'e2e_client.py')]

    # --- the server ----------------------------------------------------------------------------

    def setup_file(self):
        """setup.sql with Rerank.java included and a LANGUAGE CPYTHON copy (*_cpy) of each *_py procedure."""
        s = open(os.path.join(HERE, 'setup.sql')).read()
        marker = '-- @include client/Rerank.java\n'
        if marker not in s:
            sys.exit('setup.sql: no "%s"' % marker.strip())
        s = s.replace(marker, open(os.path.join(HERE, 'client', 'Rerank.java')).read())
        copies = []
        for m in re.finditer(r'^CREATE PROCEDURE (e2e_\w+)_py\(.*?^\$\$;\n', s, re.M | re.S):
            body = m.group(0)
            copies.append(body.replace(m.group(1) + '_py(', m.group(1) + '_cpy(', 1)
                          .replace('LANGUAGE PYTHON', 'LANGUAGE CPYTHON', 1))
        if len(copies) != 4:
            sys.exit('setup.sql: expected 4 LANGUAGE PYTHON procedures, found %d' % len(copies))
        path = os.path.join(self.a.work, 'setup-expanded.sql')
        with open(path, 'w') as f:
            f.write(s + '\n-- The LANGUAGE CPYTHON copies (made by e2e.py).\n' + '\n'.join(copies))
        return path

    def options(self, bind):
        # as the 17-18 Nov runs: a 2 GB heap and 4 GB per JVM worker (the defaults: 512 MB, 2 GB)
        return ['--loose-mle.sql_result_max=1073741824', '--loose-mle.heap_max=2147483648',
                '--loose-mle.memory_max=4294967296',
                '--innodb-buffer-pool-size=4G', '--innodb-flush-log-at-trx-commit=1', '--max-connections=200',
                '--bind-address=' + bind]

    def net_up(self):
        """The clients' network: a namespace with netem, or loopback."""
        if self.a.net == 'delay':
            if subprocess.run(['sudo', '-n', 'true'], capture_output=True).returncode != 0:
                print('# WARNING: no sudo -n: the clients use loopback, without the network delay', flush=True)
            else:
                self.net_sh('down')
                self.net_sh('up', self.prefix)
                self.net_sh('delay', str(self.a.delay_ms))
                self.net = {'mode': 'netns+netem', 'delay_ms': self.a.delay_ms, 'mtu': 1500}
                self.host = self.prefix + '.1'
                return
        self.net = {'mode': 'loopback', 'delay_ms': 0}

    def net_down(self):
        if self.net and self.net['mode'] != 'loopback':
            self.net_sh('down')

    def net_sh(self, *args):
        r = subprocess.run([os.path.join(HERE, 'net.sh'), args[0], self.netns] + list(args[1:]),
                           capture_output=True, text=True)
        if r.returncode != 0 and args[0] != 'down':
            sys.exit('net.sh %s failed:\n%s' % (' '.join(args), r.stderr))

    def in_net(self, cmd, env):
        """cmd with the environment env, in the clients' network."""
        envs = ['%s=%s' % kv for kv in env.items()]
        if self.net['mode'] == 'loopback':
            return ['env'] + envs + cmd
        return [os.path.join(HERE, 'net.sh'), 'run', self.netns, 'env'] + envs + cmd

    def rtt_ms(self):
        r = subprocess.run(self.in_net(['ping', '-c', '10', '-i', '0.2', '-q', self.host], {}),
                           capture_output=True, text=True)
        m = re.search(r'= ([\d.]+)/([\d.]+)/([\d.]+)', r.stdout)
        return float(m.group(2)) if m else None

    def warm_server(self):
        """The buffer pool and the HNSW index (after a restart the first ~100 HNSW searches are slow)."""
        t0 = time.time()
        qs = self.server.sql('SELECT q FROM e2e.queries ORDER BY j').split()
        script = ['USE e2e;', 'SELECT SUM(LENGTH(embedding)) FROM items;', 'SELECT COUNT(*) FROM views;']
        for q in qs * 2:
            script.append("SET @q = STRING_TO_VECTOR('%s'); SELECT id FROM items "
                          "ORDER BY DISTANCE(embedding, @q, 'COSINE') LIMIT 500;" % q)
        self.server.sql('\n'.join(script))
        return round(time.time() - t0, 1)

    # --- measurements --------------------------------------------------------------------------

    def measure(self, case, client, variant, clients):
        pipe, c, k, _, _ = CASES[case]
        n = self.queries if clients == 1 else self.queries_conc
        args = ['--host', self.host, '--port', str(self.a.port), '--variant', variant, '--pipe', str(pipe),
                '--c', str(c), '--k', str(k), '--clients', str(clients), '--queries', str(n),
                '--warm-min', str(self.warm[0]), '--warm-max', str(self.warm[1]), '--batch', str(self.warm[2])]
        env = {'OPENBLAS_NUM_THREADS': '1', 'OMP_NUM_THREADS': '1', 'HOME': os.environ.get('HOME', '/tmp'),
               'PATH': os.environ.get('PATH', '/usr/bin:/bin')}
        cmd = self.in_net((self.java if client == 'java' else self.python) + args, env)
        waited = 0
        while mem_available_gib() < self.a.min_available_gib and waited < 60:
            time.sleep(5)
            waited += 5
        if mem_available_gib() < self.a.min_available_gib:
            print('# SKIPPED %s %s %s x%d: %.1f GiB available' % (case, client, variant, clients,
                                                                    mem_available_gib()), flush=True)
            return None
        t0 = time.time()
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        low = []
        stop = threading.Event()

        def guard():  # the clients go when the host runs short of memory
            while not stop.wait(0.5):
                if mem_available_gib() < self.a.min_available_gib:
                    low.append(mem_available_gib())
                    self.kill_tree(p.pid)
                    return
        g = threading.Thread(target=guard, daemon=True)
        g.start()
        try:
            out, err = p.communicate(timeout=self.a.timeout)
        except subprocess.TimeoutExpired:
            self.kill_tree(p.pid)
            out, err = p.communicate()
            err += '\ntimed out after %d s' % self.a.timeout
        stop.set()
        seconds = round(time.time() - t0, 1)
        if low:
            print('# STOPPED %s %s %s x%d: %.1f GiB available' % (case, client, variant, clients, low[0]), flush=True)
            return None
        line = out.strip().splitlines()[-1] if out.strip() else ''
        if p.returncode != 0 or not line.startswith('{'):
            print('# FAILED %s %s %s x%d (exit %s):\n%s' % (case, client, variant, clients, p.returncode,
                                                         '\n'.join(l for l in err.splitlines()
                                                                   if 'WARNING' not in l)[-3000:]), flush=True)
            return None
        r = json.loads(line)
        lat = sorted(x / 1000 for x in r['lat_us'])
        res = {'case': case, 'client': client, 'variant': variant, 'clients': clients, 'requests': len(lat),
               'p50_ms': round(statistics.median(lat), 3), 'p95_ms': round(lat[int(0.95 * (len(lat) - 1))], 3),
               'mean_ms': round(statistics.mean(lat), 3), 'qps': round(len(lat) / r['wall_s'], 1),
               'warm': r['warm'], 'converged': all(r['converged']), 'seconds': seconds, 'load': mle_perf.load()}
        if 'versions' in r:
            self.py_versions = r['versions']
        if 'dot' in r:
            self.java_dot = r['dot']
        self.runs.append(res)
        prefix = 'e2e.%s.%s.%s.' % (case, client, variant) + ('' if clients == 1 else 'c%d_' % clients)
        self.results[prefix + 'p50_ms'] = {'value': res['p50_ms'], 'unit': 'ms'}
        self.results[prefix + 'p95_ms'] = {'value': res['p95_ms'], 'unit': 'ms'}
        self.results[prefix + 'qps'] = {'value': res['qps'], 'unit': 'requests/s'}
        if clients == 1:
            self.ids.setdefault(case, {})['%s.%s' % (client, variant)] = r['ids']
        print('  %-12s %-6s %-8s %2d  p50 %8.2f  p95 %8.2f ms  %8.1f req/s  warm %s%s  (%s s)' % (
            case, client, variant, clients, res['p50_ms'], res['p95_ms'], res['qps'],
            min(r['warm']) if r['warm'] else 0, '' if res['converged'] else ' (not converged)', seconds), flush=True)
        return res

    @staticmethod
    def kill_tree(pid):
        """pid and its descendants that are ours (the clients run as this user, below sudo in a namespace)."""
        out = subprocess.run(['ps', '-e', '-o', 'pid=,ppid=,uid='], capture_output=True, text=True).stdout
        kids = {}
        for line in out.splitlines():
            p, pp, uid = (int(x) for x in line.split())
            kids.setdefault(pp, []).append((p, uid))
        todo, mine = [pid], []
        while todo:
            for k, uid in kids.get(todo.pop(), []):
                todo.append(k)
                if uid == os.getuid():
                    mine.append(k)
        for k in [pid] + mine:
            try:
                os.kill(k, 9)
            except OSError:
                pass

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
        settings = s.sql("SELECT VARIABLE_NAME, VARIABLE_VALUE FROM performance_schema.global_variables "
                         "WHERE VARIABLE_NAME LIKE 'mle.%' OR VARIABLE_NAME IN ('innodb_buffer_pool_size', "
                         "'innodb_flush_log_at_trx_commit', 'bind_address')")
        return {
            'cpu': first('/proc/cpuinfo', 'model name'), 'cpus': os.cpu_count(),
            'memory_gib': round(int(first('/proc/meminfo', 'MemTotal').split()[0]) / 1048576, 1),
            'kernel': platform.release(), 'os': osname, 'governor': mle_perf.governor(),
            'server_build': self.a.server, 'server_commit': mle_perf.source_commit(self.a.server),
            'server_build_type': mle_perf.cmake_cache(self.a.server, 'CMAKE_BUILD_TYPE'),
            'mle_build': self.a.mle, 'mle_commit': mle_perf.source_commit(self.a.mle),
            'mle_component_built': mle_perf.built_at(self.a.mle),
            'mle_build_type': mle_perf.cmake_cache(self.a.mle, 'CMAKE_BUILD_TYPE'),
            'server_version': s.value('SELECT VERSION()'),
            'languages': s.value("SELECT VARIABLE_VALUE FROM performance_schema.global_status "
                                 "WHERE VARIABLE_NAME = 'mle_languages_supported'"),
            'settings': dict(l.split('\t', 1) for l in settings.splitlines() if '\t' in l),
            'java_client': {'jdk': self.java_version, 'connector': self.connector_j},
            'tool_commit': subprocess.run(['git', '-C', HERE, 'rev-parse', '--short=10', 'HEAD'],
                                          capture_output=True, text=True).stdout.strip(),
        }

    def report(self):
        print('\n== median ms per request (p95), 1 client; requests/s with N clients; %s' %
              ('%s %.0f ms, round trip %s ms' % (self.net['mode'], self.net['delay_ms'], self.net.get('rtt_ms'))
               if self.net['mode'] != 'loopback' else 'loopback'))
        for case in [c for c in CASES if c in self.cases]:
            print('%s: %s' % (case, CASES[case][4]))
            for client in ('java', 'python'):
                base = self.results.get('e2e.%s.%s.app.p50_ms' % (case, client))
                for _, variant, what in [v for v in VARIANTS if v[0] == client]:
                    key = 'e2e.%s.%s.%s.' % (case, client, variant)
                    if key + 'p50_ms' not in self.results:
                        continue
                    p50 = self.results[key + 'p50_ms']['value']
                    conc = ['%d clients: %7.0f req/s, p50 %6.2f ms' % (
                        n, self.results['%sc%d_qps' % (key, n)]['value'], self.results['%sc%d_p50_ms' % (key, n)]['value'])
                        for n in sorted({r['clients'] for r in self.runs if r['clients'] > 1})
                        if '%sc%d_qps' % (key, n) in self.results]
                    rel = '' if variant == 'app' or not base else '%+5.0f %%' % (100 * (p50 / base['value'] - 1))
                    print('  %-32s %8.2f (%6.2f) %7s  %s' % (what, p50, self.results[key + 'p95_ms']['value'],
                                                            rel, '; '.join(conc)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--server', required=True, help='the server build directory (release)')
    ap.add_argument('--mle', required=True, help='the MLE build directory (release)')
    ap.add_argument('--work', default=None, help='data directory, clients, logs (default <MLE build>/perf-e2e)')
    ap.add_argument('--port', type=int, default=13994)
    ap.add_argument('--quick', action='store_true', help='fewer requests: checks the tool, not for numbers')
    ap.add_argument('--cases', default=','.join(CASES), help='cases, comma-separated (default: all)')
    ap.add_argument('--only', default='', help='measurements whose "<case>.<client>.<variant>" matches this')
    ap.add_argument('--no-concurrency', action='store_true', help='one client only')
    ap.add_argument('--net', choices=('delay', 'loopback'), default='delay')
    ap.add_argument('--delay-ms', type=float, default=1.0)
    ap.add_argument('--subnet', default='10.213.94', help='the namespace network (first three octets)')
    ap.add_argument('--java-home', default=None, help='the JDK for the Java client (default MLE_JAVA_HOME)')
    ap.add_argument('--python', default=None, help='a Python with mysql.connector and numpy (default: a venv)')
    ap.add_argument('--min-available-gib', type=float, default=10.0, help='stop clients below this')
    ap.add_argument('--timeout', type=int, default=900, help='seconds per measurement')
    ap.add_argument('--wait-quiet', action='store_true', help='wait for a load average below 1.5 first')
    ap.add_argument('--json', default=None, help='write the results here')
    args = ap.parse_args()
    args.server, args.mle = os.path.abspath(args.server), os.path.abspath(args.mle)
    args.work = os.path.abspath(args.work or os.path.join(args.mle, 'perf-e2e'))
    os.makedirs(args.work, exist_ok=True)
    cases = [x.strip() for x in args.cases.split(',') if x.strip()]
    for x in cases:
        if x not in CASES:
            sys.exit('no case %s (cases: %s)' % (x, ', '.join(CASES)))

    e = E2E(args)
    e.cases = cases
    e.prepare_java()
    e.prepare_python()
    started = time.time()
    e.server.prepare_data(args.mle, e.setup_file(), [('component_mle', True)], e.options('127.0.0.1'))
    e.net_up()
    try:
        e.server.start(args.mle, e.options('127.0.0.1' if e.net['mode'] == 'loopback' else '127.0.0.1,' + e.host))
        try:
            env = e.environment()
            env['network'] = e.net
            e.net['rtt_ms'] = e.rtt_ms()
            env['server_warm_s'] = e.warm_server()
            env['load_start'] = mle_perf.wait_quiet(enabled=args.wait_quiet)
            print('# %s, %d CPUs, %.0f GiB, %s, governor %s, load %.2f' %
                  (env['cpu'], env['cpus'], env['memory_gib'], env['os'], env['governor'], env['load_start']))
            print('# MLE %s (%s), server %s (%s); client JDK %s' % (
                env['mle_commit'], env['mle_build_type'], env['server_commit'], env['server_build_type'],
                env['java_client']['jdk']))
            print('# network: %s, round trip %s ms' % (
                'loopback' if e.net['mode'] == 'loopback' else 'namespace, netem %g ms' % args.delay_ms,
                e.net['rtt_ms']), flush=True)
            if env['governor'] not in ('performance', 'unknown'):
                print('# WARNING: the CPU governor is "%s": times vary more; use "performance"' % env['governor'])
            plan = [(c, cl, v, 1) for c in cases for cl, v, _ in VARIANTS]
            if not args.no_concurrency:
                plan += [(c, cl, v, CASES[c][3]) for c in cases if CASES[c][3] for cl, v, _ in VARIANTS]
            plan = [p for p in plan if not args.only or re.search(args.only, '%s.%s.%s' % p[:3])]
            for p in plan:
                e.measure(*p)
        finally:
            e.server.stop()
    finally:
        e.net_down()
    checks = {}
    for case, ids in e.ids.items():
        same = len({json.dumps(v) for v in ids.values()}) == 1
        checks[case] = 'same ids' if same else ids
        if not same:
            print('# WARNING: %s: the variants picked different ids: %s' % (case, ids))
    env['load_end'] = mle_perf.load()
    env['seconds'] = round(time.time() - started)
    env['quick'] = args.quick
    env['python_client'] = getattr(e, 'py_versions', {})
    env['java_client']['dot'] = getattr(e, 'java_dot', '')
    e.report()
    print('# done in %d s' % env['seconds'])
    if args.json:
        with open(args.json, 'w') as f:
            json.dump({'schema': 1, 'tool': 'perf/e2e/e2e.py', 'time': time.strftime('%Y-%m-%dT%H:%M:%S%z'),
                       'cases': {c: CASES[c][4] for c in cases}, 'variants': {'%s.%s' % v[:2]: v[2] for v in VARIANTS},
                       'environment': env, 'checks': checks, 'results': e.results, 'runs': e.runs}, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
