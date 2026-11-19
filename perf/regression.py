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
"""Performance regression test of Percona MLE: two builds, side by side.

    perf/regression.py --server <server build> --base <MLE build> --new <MLE build>
                       [--rounds 5] [--only REGEX] [--threshold 5]
                       [--work DIR] [--port 13997] [--wait-quiet] [--json FILE]

Runs the cases of perf/setup.sql (the call path, values of every kind, SQL
from routines read and written, libraries, errors, vectors, a worker for a
new connection) on the base build and on the new build, in turn, on the same
data and server. A round is one server run per build; the order alternates
(base-new, new-base, ...), so what the machine does meanwhile hits both.

A case is slower when, over the rounds, the new build's times are above the
base build's by more than the threshold (percent) both for the best run and
for the median run of a round, and in all rounds but one at most. Exit status
1 if any case is slower. "check" marks a case over the threshold in one of
the two measures only: noise more often than not; run it again with --only.

Release builds only (RelWithDebInfo), a quiet machine, the CPU governor
"performance". The data directory is made once in --work (default
<new build>/perf) and reused; delete it after changing perf/setup.sql (the
test says so). The builds differ in <build>/plugin_output_directory
(component_mle.so and mle/); the server and its other components are shared.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
# Cases measured from outside: a new connection's first call (ms are too coarse inside).
CONNECT = {
    'connect_kept_js': ('root', 'SELECT perf.js_add(1, 1)'),    # the account's kept worker
    'connect_kept_py': ('root', 'SELECT perf.py_add(1, 1)'),
    'connect_pool_py': ('perf_u%d', 'SELECT perf.py_add(1, 1)'),  # a worker from the pool
}
# Percent over which a case counts, where the default is too tight for what the case measures.
THRESHOLDS = {'connect_kept_js': 15, 'connect_kept_py': 15, 'connect_pool_py': 25, 'scan': 100,
              'errors_js': 10, 'errors_py': 10}
# call_* are judged without the scan they ride on.
NET_OF_SCAN = ('call_js', 'call_py', 'call_java', 'call_invoker')


class Server:
    def __init__(self, args):
        self.a = args
        self.bin = os.path.join(args.server, 'runtime_output_directory')
        self.data = os.path.join(args.work, 'data')
        self.sock = os.path.join(args.work, 'mysqld.sock')
        self.proc = None

    def plugin_dir(self, build):
        """The server's plugins with this build's component_mle and mle/."""
        d = os.path.join(self.a.work, 'plugins-' + hashlib.sha1(build.encode()).hexdigest()[:8])
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        src = os.path.join(self.a.server, 'plugin_output_directory')
        for name in os.listdir(src):
            if name not in ('component_mle.so', 'mle'):
                os.symlink(os.path.join(src, name), os.path.join(d, name))
        for name in ('component_mle.so', 'mle'):
            target = os.path.join(build, 'plugin_output_directory', name)
            if not os.path.exists(target):
                sys.exit('no %s: not an MLE build directory?' % target)
            os.symlink(os.path.realpath(target), os.path.join(d, name))
        return d

    def mysqld(self, *more):
        return [os.path.join(self.bin, 'mysqld'), '--no-defaults', '--datadir=' + self.data,
                '--basedir=' + self.a.server, '--lc-messages-dir=' + os.path.join(self.a.server, 'share'),
                *more]

    def start(self, build):
        log = open(os.path.join(self.a.work, 'mysqld.log'), 'a')
        self.proc = subprocess.Popen(
            self.mysqld('--plugin-dir=' + self.plugin_dir(build), '--socket=' + self.sock,
                        '--port=%d' % self.a.port, '--mysqlx=OFF', '--skip-log-bin',
                        '--innodb-buffer-pool-size=2G', '--innodb-flush-log-at-trx-commit=2',
                        '--log-error=' + os.path.join(self.a.work, 'error.log'),
                        '--pid-file=' + os.path.join(self.a.work, 'mysqld.pid'),
                        '--loose-mle.pool_prewarm=python',
                        '--loose-mle.sql_result_max=1073741824'),
            stdout=log, stderr=log)
        for _ in range(120):
            if subprocess.run([os.path.join(self.bin, 'mysqladmin'), '-uroot', '-S', self.sock, 'ping'],
                              capture_output=True).returncode == 0:
                return
            if self.proc.poll() is not None:
                break
            time.sleep(0.5)
        sys.exit('mysqld did not start: see %s/error.log' % self.a.work)

    def stop(self):
        if self.proc is None:
            return
        subprocess.run([os.path.join(self.bin, 'mysqladmin'), '-uroot', '-S', self.sock, 'shutdown'],
                       capture_output=True)
        self.proc.wait()
        self.proc = None

    def sql(self, text, user='root', check=True):
        r = subprocess.run([os.path.join(self.bin, 'mysql'), '-u' + user, '-S', self.sock, '-N', '-B',
                            '--default-character-set=utf8mb4'],
                           input=text, capture_output=True, text=True)
        if check and r.returncode != 0:
            raise RuntimeError('%s\n%s' % (text[:200], r.stderr.strip()))
        return r.stdout

    def prepare_data(self, build):
        """The data directory, made once (with the build given) and reused."""
        setup = open(os.path.join(HERE, 'setup.sql')).read()
        stamp = os.path.join(self.a.work, 'setup.sha1')
        digest = hashlib.sha1(setup.encode()).hexdigest()
        if os.path.isdir(self.data) and os.path.exists(stamp):
            if open(stamp).read().strip() == digest:
                return
            sys.exit('perf/setup.sql changed since %s was made: delete it and run again' % self.data)
        print('# preparing the data in %s (once; a few minutes)' % self.data, flush=True)
        shutil.rmtree(self.data, ignore_errors=True)
        r = subprocess.run(self.mysqld('--initialize-insecure'), capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit('mysqld --initialize-insecure failed:\n' + r.stderr[-2000:])
        self.start(build)
        try:
            self.sql("INSTALL COMPONENT 'file://component_lang_router'", check=False)
            self.sql("INSTALL COMPONENT 'file://component_mle'")
            langs = self.sql("SELECT VARIABLE_VALUE FROM performance_schema.global_status "
                             "WHERE VARIABLE_NAME = 'mle_languages_supported'").strip()
            r = subprocess.run([os.path.join(self.bin, 'mysql'), '-uroot', '-S', self.sock, '--force',
                                '--default-character-set=utf8mb4'],
                               input=setup, capture_output=True, text=True)
            errors = [l for l in r.stderr.splitlines() if l.startswith('ERROR')]
            if errors and 'JavaScript' in langs:  # the Python-only build refuses the other languages' routines
                sys.exit('perf/setup.sql failed:\n' + '\n'.join(errors[:20]))
        finally:
            self.stop()
        open(stamp, 'w').write(digest)


def quiet(args):
    load = float(open('/proc/loadavg').read().split()[0])
    try:
        governor = open('/sys/devices/system/cpu/cpufreq/policy0/scaling_governor').read().strip()
    except OSError:
        governor = 'unknown'
    while args.wait_quiet and load >= 1.5:
        time.sleep(15)
        load = float(open('/proc/loadavg').read().split()[0])
    return load, governor


def measure(server, args, cases):
    """One server run: {case: [microseconds...]}."""
    time.sleep(args.settle)  # the pool fills and pre-warms
    out = {}
    names = [c for c in cases if c not in CONNECT]
    server.sql('TRUNCATE perf.results')
    # One session for all cases, as an application's connection would be.
    server.sql('USE perf;\n' + '\n'.join("CALL run_case('%s');" % n for n in names))
    for line in server.sql('SELECT name, us FROM perf.results ORDER BY id').splitlines():
        name, us = line.split('\t')
        out.setdefault(name, []).append(int(us))
    mysql = [os.path.join(server.bin, 'mysql'), '-S', server.sock, '-N', '-B']
    for name in (c for c in cases if c in CONNECT):
        user, stmt = CONNECT[name]
        n = 6 if '%d' in user else 25
        if '%d' not in user:  # the account has its worker
            subprocess.run(mysql + ['-u' + user, '-e', stmt], capture_output=True)
        times = []
        for i in range(n):
            if '%d' in user:
                time.sleep(4)  # the pool refills
            t0 = time.perf_counter()
            r = subprocess.run(mysql + ['-u' + (user % (i + 1) if '%d' in user else user), '-e', stmt],
                               capture_output=True, text=True)
            if r.returncode != 0:
                raise RuntimeError('%s: %s' % (name, r.stderr.strip()))
            times.append(int((time.perf_counter() - t0) * 1e6))
        out[name] = times
    return out


def verdicts(results, args):
    """results: {variant: [round: {case: [us]}]} -> rows of the report."""
    rows = []
    cases = [c for c in results['new'][0] if c in results['base'][0]]
    for case in cases:
        def stat(variant, rnd, fn):
            v = fn(results[variant][rnd][case])
            if case in NET_OF_SCAN and 'scan' in results[variant][rnd]:
                v -= fn(results[variant][rnd]['scan'])
            return max(v, 1)
        rounds = range(len(results['new']))
        ratio_min = [stat('new', r, min) / stat('base', r, min) for r in rounds]
        ratio_med = [stat('new', r, statistics.median) / stat('base', r, statistics.median) for r in rounds]
        thr = 1 + THRESHOLDS.get(case, args.threshold) / 100
        m_min, m_med = statistics.median(ratio_min), statistics.median(ratio_med)
        slower_rounds = sum(1 for a, b in zip(ratio_min, ratio_med) if a > 1 and b > 1)
        if m_min > thr and m_med > thr and slower_rounds >= max(len(ratio_min) - 1, 1):
            verdict = 'SLOWER'
        elif m_min > thr or m_med > thr:
            verdict = 'check'
        elif m_min < 2 - thr and m_med < 2 - thr:
            verdict = 'faster'
        else:
            verdict = 'ok'
        rows.append({
            'case': case, 'verdict': verdict,
            'base_ms': statistics.median(stat('base', r, min) for r in rounds) / 1000,
            'new_ms': statistics.median(stat('new', r, min) for r in rounds) / 1000,
            'ratio_min': m_min, 'ratio_med': m_med,
            'range': (min(ratio_min), max(ratio_min)),
        })
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--server', required=True, help='the server build directory (release)')
    ap.add_argument('--base', required=True, help='the MLE build to compare with')
    ap.add_argument('--new', required=True, help='the MLE build under test')
    ap.add_argument('--rounds', type=int, default=5)
    ap.add_argument('--only', default='', help='cases whose name matches this regular expression')
    ap.add_argument('--threshold', type=float, default=5, help='percent (default 5)')
    ap.add_argument('--work', default=None)
    ap.add_argument('--port', type=int, default=13997)
    ap.add_argument('--settle', type=int, default=20, help='seconds after a server start')
    ap.add_argument('--wait-quiet', action='store_true', help='wait for a load average below 1.5 before each run')
    ap.add_argument('--json', default=None, help='write the raw times and the report here')
    args = ap.parse_args()
    for k in ('server', 'base', 'new'):
        setattr(args, k, os.path.abspath(getattr(args, k)))
    args.work = os.path.abspath(args.work or os.path.join(args.new, 'perf'))
    os.makedirs(args.work, exist_ok=True)

    server = Server(args)
    server.prepare_data(args.new)
    load, governor = quiet(args)
    print('# base %s\n# new  %s\n# %d rounds, threshold %g %%, load %.2f, governor %s' %
          (args.base, args.new, args.rounds, args.threshold, load, governor), flush=True)
    if governor not in ('performance', 'unknown'):
        print('# WARNING: the CPU governor is "%s": times vary more; use "performance"' % governor)

    results = {'base': [], 'new': []}
    cases = None
    try:
        for rnd in range(args.rounds):
            order = ('base', 'new') if rnd % 2 == 0 else ('new', 'base')
            for variant in order:
                quiet(args)
                server.start(getattr(args, variant))
                try:
                    if cases is None:
                        cases = server.sql('SELECT name FROM perf.cases ORDER BY ord').split() + list(CONNECT)
                        langs = server.sql("SELECT VARIABLE_VALUE FROM performance_schema.global_status "
                                           "WHERE VARIABLE_NAME = 'mle_languages_supported'")
                        if 'JavaScript' not in langs:  # the Python-only build
                            cases = [c for c in cases if not re.search(r'js|java|invoker', c)]
                        if args.only:
                            cases = [c for c in cases if re.search(args.only, c) or
                                     (c == 'scan' and any(re.search(args.only, n) for n in NET_OF_SCAN))]
                    results[variant].append(measure(server, args, cases))
                finally:
                    server.stop()
            print('# round %d of %d done (%s), load %.2f' %
                  (rnd + 1, args.rounds, time.strftime('%H:%M:%S'), float(open('/proc/loadavg').read().split()[0])),
                  flush=True)
    finally:
        server.stop()

    rows = verdicts(results, args)
    print('\n%-18s %10s %10s %8s %8s  %-13s %s' %
          ('case', 'base ms', 'new ms', 'best', 'median', 'best, rounds', 'verdict'))
    for r in rows:
        print('%-18s %10.2f %10.2f %+7.1f%% %+7.1f%%  %+5.1f..%+5.1f%%  %s' %
              (r['case'], r['base_ms'], r['new_ms'], (r['ratio_min'] - 1) * 100, (r['ratio_med'] - 1) * 100,
               (r['range'][0] - 1) * 100, (r['range'][1] - 1) * 100, r['verdict'] if r['verdict'] != 'ok' else ''))
    slower = [r['case'] for r in rows if r['verdict'] == 'SLOWER']
    check = [r['case'] for r in rows if r['verdict'] == 'check']
    print('\n%d cases: %d slower%s, %d to check%s' %
          (len(rows), len(slower), ' (%s)' % ', '.join(slower) if slower else '',
           len(check), ' (%s)' % ', '.join(check) if check else ''))
    if args.json:
        json.dump({'args': vars(args), 'results': results, 'report': rows}, open(args.json, 'w'), indent=1)
    return 1 if slower else 0


if __name__ == '__main__':
    sys.exit(main())
