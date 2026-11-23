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
"""What the MLE performance tools share (perf/regression.py, perf/bench.py).

A server of a server build with one MLE build's component_mle.so and mle/ in a
plugin directory of its own; its data directory made once from a setup SQL
file and reused while the file (and the components installed) stay the same;
sessions kept open; the machine's state (load, CPU governor); the workers below
mysqld and their memory.
"""
import hashlib
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time


def load():
    """The 1-minute load average."""
    return float(open('/proc/loadavg').read().split()[0])


def governor():
    try:
        return open('/sys/devices/system/cpu/cpufreq/policy0/scaling_governor').read().strip()
    except OSError:
        return 'unknown'


def wait_quiet(limit=1.5, enabled=True):
    """Waits (when enabled) for a 1-minute load average below the limit; returns the load."""
    while enabled and load() >= limit:
        time.sleep(15)
    return load()


def cmake_cache(build, key):
    try:
        for line in open(os.path.join(build, 'CMakeCache.txt')):
            if line.startswith(key + ':'):
                return line.split('=', 1)[1].strip()
    except OSError:
        pass
    return ''


def source_commit(build):
    """The commit of a build's source tree, '+' if the tree has changes; '' when unknown."""
    src = cmake_cache(build, 'CMAKE_HOME_DIRECTORY')
    if not src:
        return ''
    r = subprocess.run(['git', '-C', src, 'rev-parse', '--short=10', 'HEAD'], capture_output=True, text=True)
    if r.returncode != 0:
        return ''
    dirty = subprocess.run(['git', '-C', src, 'status', '--porcelain', '--untracked-files=no'],
                           capture_output=True, text=True).stdout.strip()
    return r.stdout.strip() + ('+' if dirty else '')


def built_at(mle_build):
    """When the MLE build's component was linked (the source tree's commit may be newer than the build)."""
    try:
        return time.strftime('%Y-%m-%dT%H:%M:%S', time.localtime(os.path.getmtime(
            os.path.join(mle_build, 'plugin_output_directory', 'component_mle.so'))))
    except OSError:
        return ''


def read_pss(pid):
    """PSS and RSS (MiB) of a process: {'pss': ..., 'rss': ...}, or None. Workers are not
    dumpable: their smaps_rollup needs root (sudo -n); mysqld's is the user's own."""
    path = '/proc/%d/smaps_rollup' % pid
    try:
        text = open(path).read()
    except OSError:
        r = subprocess.run(['sudo', '-n', 'cat', path], capture_output=True, text=True)
        if r.returncode != 0:
            return None
        text = r.stdout
    kb = {}
    for line in text.splitlines()[1:]:
        k, _, v = line.partition(':')
        if v.strip():
            kb[k.strip()] = int(v.split()[0])
    return {'pss': kb.get('Pss', 0) / 1024, 'rss': kb.get('Rss', 0) / 1024}


class Server:
    """mysqld of a server build, with one MLE build at a time."""

    def __init__(self, server_build, work, port):
        self.build = server_build
        self.work = work
        self.port = port
        self.bin = os.path.join(server_build, 'runtime_output_directory')
        self.data = os.path.join(work, 'data')
        self.sock = os.path.join(work, 'mysqld.sock')
        if len(self.sock) > 100:  # a Unix socket's path has at most 107 bytes
            self.sock = os.path.join(tempfile.gettempdir(),
                                     'mle-perf-%s.sock' % hashlib.sha1(work.encode()).hexdigest()[:10])
        self.proc = None
        self.mle_build = None

    def plugin_dir(self, mle_build):
        """The server's plugins with this MLE build's component_mle and mle/."""
        d = os.path.join(self.work, 'plugins-' + hashlib.sha1(mle_build.encode()).hexdigest()[:8])
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        src = os.path.join(self.build, 'plugin_output_directory')
        for name in os.listdir(src):
            if name not in ('component_mle.so', 'mle'):
                os.symlink(os.path.join(src, name), os.path.join(d, name))
        for name in ('component_mle.so', 'mle'):
            target = os.path.join(mle_build, 'plugin_output_directory', name)
            if not os.path.exists(target):
                sys.exit('no %s: not an MLE build directory?' % target)
            os.symlink(os.path.realpath(target), os.path.join(d, name))
        return d

    def has_component(self, name):
        return os.path.exists(os.path.join(self.build, 'plugin_output_directory', name + '.so'))

    def mysqld(self, *more):
        return [os.path.join(self.bin, 'mysqld'), '--no-defaults', '--datadir=' + self.data,
                '--basedir=' + self.build, '--lc-messages-dir=' + os.path.join(self.build, 'share'), *more]

    def start(self, mle_build, options=(), env=None):
        """Starts mysqld and waits until it answers. env: more environment variables."""
        log = open(os.path.join(self.work, 'mysqld.log'), 'a')
        self.mle_build = mle_build
        self.proc = subprocess.Popen(
            self.mysqld('--plugin-dir=' + self.plugin_dir(mle_build), '--socket=' + self.sock,
                        '--port=%d' % self.port, '--mysqlx=OFF', '--skip-log-bin',
                        '--innodb-buffer-pool-size=2G', '--innodb-flush-log-at-trx-commit=2',
                        '--log-error=' + os.path.join(self.work, 'error.log'),
                        '--pid-file=' + os.path.join(self.work, 'mysqld.pid'), *options),
            stdout=log, stderr=log, env=dict(os.environ, **(env or {})))
        for _ in range(240):
            if subprocess.run([os.path.join(self.bin, 'mysqladmin'), '-uroot', '-S', self.sock, 'ping'],
                              capture_output=True).returncode == 0:
                return
            if self.proc.poll() is not None:
                break
            time.sleep(0.5)
        sys.exit('mysqld did not start: see %s/error.log' % self.work)

    def stop(self):
        if self.proc is None:
            return
        subprocess.run([os.path.join(self.bin, 'mysqladmin'), '-uroot', '-S', self.sock, 'shutdown'],
                       capture_output=True)
        self.proc.wait()
        self.proc = None

    def client(self, *more, user='root'):
        return [os.path.join(self.bin, 'mysql'), '-u' + user, '-S', self.sock, '-N', '-B',
                '--default-character-set=utf8mb4', *more]

    def sql(self, text, user='root', check=True):
        r = subprocess.run(self.client(user=user), input=text, capture_output=True, text=True)
        if check and r.returncode != 0:
            raise RuntimeError('%s\n%s' % (text[:200], r.stderr.strip()))
        return r.stdout

    def value(self, query):
        return self.sql(query).strip()

    def prepare_data(self, mle_build, setup_file, components, options=()):
        """The data directory, made once (with the MLE build given) and reused while setup_file and
        the components stay the same. components: [(name, required)], installed in that order."""
        setup = open(setup_file).read()
        present = [c for c, required in components if required or self.has_component(c)]
        stamp = os.path.join(self.work, 'setup.sha1')
        digest = hashlib.sha1((setup + '\n-- ' + ','.join(present)).encode()).hexdigest()
        if os.path.isdir(self.data) and os.path.exists(stamp):
            if open(stamp).read().strip() == digest:
                return
            sys.exit('%s (or the components) changed since %s was made: delete it and run again' %
                     (os.path.basename(setup_file), self.data))
        print('# preparing the data in %s (once)' % self.data, flush=True)
        shutil.rmtree(self.data, ignore_errors=True)
        r = subprocess.run(self.mysqld('--initialize-insecure'), capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit('mysqld --initialize-insecure failed:\n' + r.stderr[-2000:])
        self.start(mle_build, options)
        try:
            for c in present:
                self.sql("INSTALL COMPONENT 'file://%s'" % c)
            r = subprocess.run(self.client('--force'), input=setup, capture_output=True, text=True)
            errors = [l for l in r.stderr.splitlines() if l.startswith('ERROR')]
            if errors:
                sys.exit('%s failed:\n%s' % (os.path.basename(setup_file), '\n'.join(errors[:20])))
        finally:
            self.stop()
        open(stamp, 'w').write(digest)

    def workers(self):
        """[(pid, process name)] of the worker processes below mysqld."""
        out = subprocess.run(['ps', '-e', '-o', 'pid=,ppid=,comm='], capture_output=True, text=True).stdout
        kids, comm = {}, {}
        for line in out.splitlines():
            pid, ppid, c = line.split(None, 2)
            kids.setdefault(int(ppid), []).append(int(pid))
            comm[int(pid)] = c.strip()
        res, todo = [], [self.proc.pid]
        while todo:
            p = todo.pop()
            for k in kids.get(p, []):
                todo.append(k)
                if 'worker' in comm[k] or comm[k].startswith('mle_cpython'):
                    res.append((k, comm[k]))
        return res


class Session:
    """One mysql client session kept open: statements in, lines out."""

    def __init__(self, server, db, user='root'):
        self.p = subprocess.Popen(server.client('-n', '--force', db, user=user), stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)

    def q(self, text):
        self.p.stdin.write(text.strip().rstrip(';') + ";\nSELECT '__END__';\n")
        self.p.stdin.flush()
        lines = []
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError('mysql ended: ' + '\n'.join(lines))
            line = line.rstrip('\n')
            if line == '__END__':
                break
            lines.append(line)
        errors = [l for l in lines if l.startswith('ERROR')]
        if errors:
            raise RuntimeError('%s: %s' % (text[:120], errors[0]))
        return lines

    def value(self, text):
        lines = self.q(text)
        return lines[-1] if lines else ''

    def time(self, stmt):
        """One statement's time in microseconds, measured inside the server."""
        return int(self.value('SET @perf_t0 = SYSDATE(6); %s; '
                              'SELECT TIMESTAMPDIFF(MICROSECOND, @perf_t0, SYSDATE(6))' % stmt))

    def warm_and_time(self, stmt, max_warm, runs):
        """Runs stmt until two runs in a row are within 3 % (at most max_warm), then times it runs
        times: {'best', 'median', 'runs', 'warm', 'converged'} (microseconds)."""
        prev, n, converged = None, 0, False
        while n < max_warm:
            t = self.time(stmt)
            n += 1
            if prev is not None and abs(t - prev) <= 0.03 * min(t, prev):
                converged = True
                break
            prev = t
        ts = [self.time(stmt) for _ in range(runs)]
        return {'best': min(ts), 'median': statistics.median(ts), 'runs': ts, 'warm': n, 'converged': converged}

    def close(self):
        try:
            self.p.stdin.close()
        except OSError:
            pass
        self.p.wait()
