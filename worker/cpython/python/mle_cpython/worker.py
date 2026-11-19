# Copyright (c) 2026 Percona LLC and/or its affiliates. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

"""The dispatcher of the CPython worker (Dispatcher.java's role): READY, then requests from the
host on the main thread; SQL round trips serve nested requests meanwhile."""

import ast
import gc
import io
import os
import platform
import struct
import sys
import textwrap
import time
import traceback
import types

import _mlewire

from . import wire
from .wire import Reader, Writer, ConversionError


class Routine:
    __slots__ = ('fn', 'params', 'ret', 'outs', 'nparams', 'name', 'lines', 'vector_params', 'imports', 'base')

    def __init__(self, fn, params, ret, outs, name, lines, imports, base):
        self.fn, self.params, self.ret, self.outs, self.name, self.lines = fn, params, ret, outs, name, lines
        self.nparams = len(params)
        self.vector_params = any(p[2].tag == wire.T_VECTOR for p in params)
        self.imports = imports  # USING: (schema, name, binding)
        self.base = base        # the globals each call starts from (USING binds into them)


class _Output(io.StringIO):
    """sys.stdout/stderr of routines: a write marks the call, so that the C++ call loop leaves its
    RESULT (with the output) to Python."""

    def write(self, s):
        _mlewire.mark()
        return io.StringIO.write(self, s)

    def writelines(self, lines):
        _mlewire.mark()
        return io.StringIO.writelines(self, lines)


class Console:
    """print() output of a call, returned in its RESULT."""

    def __init__(self):
        self.out = _Output()
        self.err = _Output()

    @staticmethod
    def take(s):
        if not s.tell():
            return b''
        v = s.getvalue()
        s.seek(0)
        s.truncate()
        return v.encode('utf-8', 'replace')


# ------------------------------------------------------------------------------------ loaders
# The JVM kind's Python helpers (Polyglot.pythonHelpers), minus what is polyglot-only.

def _no_threads(*args, **kwargs):
    raise RuntimeError('threads are not available in MLE routines')


def _ban_threads():
    import _thread
    import threading
    for n in ('start_new_thread', 'start_new', 'start_joinable_thread'):
        if hasattr(_thread, n):
            setattr(_thread, n, _no_threads)
    threading.Thread.start = lambda self: _no_threads()
    for n in ('_start_joinable_thread', '_start_new_thread', '_start_thread'):
        if hasattr(threading, n):
            setattr(threading, n, _no_threads)


def _parse_body(src, name):
    try:
        return ast.parse(textwrap.dedent(src), filename=name)
    except (IndentationError, SyntaxError):
        first, _, rest = src.partition('\n')
        if not first.strip() or not rest.strip():
            raise
        return ast.parse(first.strip() + '\n' + textwrap.dedent(rest), filename=name)


def _fn(fname, args, body):
    kw = dict(name=fname, args=ast.arguments(posonlyargs=[], args=[ast.arg(arg=a) for a in args],
                                            vararg=None, kwonlyargs=[], kw_defaults=[], kwarg=None, defaults=[]),
              body=body or [ast.Pass()], decorator_list=[], returns=None)
    if 'type_params' in ast.FunctionDef._fields:
        kw['type_params'] = []
    return ast.FunctionDef(**kw)


def load_body(src, name, params, has_outs, globals_):
    """The body as a function of the parameters; fresh globals per call (Phase 7)."""
    tree = _parse_body(src, name)
    body = tree.body
    if has_outs:
        inner = _fn('__mle_body', [], [ast.Nonlocal(names=list(params))] + body)
        call = ast.Assign(targets=[ast.Name(id='__mle_r', ctx=ast.Store())],
                          value=ast.Call(func=ast.Name(id='__mle_body', ctx=ast.Load()), args=[], keywords=[]))
        ret = ast.Return(value=ast.Tuple(elts=[ast.Name(id='__mle_r', ctx=ast.Load())]
                                         + [ast.Name(id=p, ctx=ast.Load()) for p in params], ctx=ast.Load()))
        body = [inner, call, ret]
    mod = ast.Module(body=[_fn('__mle_routine', list(params), body)], type_ignores=[])
    ast.fix_missing_locations(mod)
    code = compile(mod, name, 'exec')
    ns = {'__name__': name}
    ns.update(globals_)
    exec(code, ns)
    code, base, make = ns['__mle_routine'].__code__, ns, types.FunctionType

    def fresh(*args):
        return make(code, base.copy())(*args)
    fresh.__mle_code__ = code  # the C++ call loop makes the function itself
    return fresh, base


def load_entry(src, name, entry, globals_):
    ns = {'__name__': name}
    ns.update(globals_)
    exec(compile(src, name + '.py', 'exec'), ns)
    fn = ns[entry]
    return fn, getattr(fn, '__globals__', ns)


def load_eval(src, globals_):
    """mle_eval (exp note 39): the snippet's statements, then the value of its last expression as
    text (repr, as a REPL shows it; a str as it is), or None. Fresh globals per call."""
    tree = ast.parse(textwrap.dedent(src), filename='mle_eval')
    tail = None
    if tree.body and isinstance(tree.body[-1], ast.Expr):
        tail = compile(ast.Expression(tree.body.pop().value), 'mle_eval', 'eval')
    body = compile(tree, 'mle_eval', 'exec')
    base = {'__name__': 'mle_eval'}
    base.update(globals_)

    def run():
        ns = base.copy()
        exec(body, ns)
        if tail is None:
            return None
        v = eval(tail, ns)
        return None if v is None else v if isinstance(v, str) else repr(v)
    return run, base


def check_names(src, params, known, globals_):
    """CREATE (exp note 39): names the code reads but never binds (a typo such as lenght), not
    built-ins, parameters, MLE's globals or USING aliases. Scopes are not told apart, so it never
    reports a name bound anywhere; exec/eval/globals/vars and star imports turn it off. Line numbers
    are the body's."""
    import builtins
    try:
        tree = ast.parse(textwrap.dedent(src))
    except SyntaxError:
        first, _, rest = src.partition('\n')
        try:
            tree = ast.parse(first.strip() + '\n' + textwrap.dedent(rest))
        except SyntaxError:
            return []
    bound = set(params) | set(known) | set(dir(builtins)) | set(globals_)
    bound |= {'Mle', '__name__', '__file__', '__doc__'}
    for n in ast.walk(tree):
        if isinstance(n, ast.Name) and isinstance(n.ctx, (ast.Store, ast.Del)):
            bound.add(n.id)
        elif isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            bound.add(n.name)
        elif isinstance(n, ast.arg):
            bound.add(n.arg)
        elif isinstance(n, (ast.Import, ast.ImportFrom)):
            for a in n.names:
                if a.name == '*':
                    return []
                bound.add((a.asname or a.name).split('.')[0])
        elif isinstance(n, ast.ExceptHandler) and n.name:
            bound.add(n.name)
        elif isinstance(n, (ast.Global, ast.Nonlocal)):
            bound.update(n.names)
        elif type(n).__name__ in ('MatchAs', 'MatchStar') and getattr(n, 'name', None):
            bound.add(n.name)
        elif type(n).__name__ == 'MatchMapping' and getattr(n, 'rest', None):
            bound.add(n.rest)
        elif (isinstance(n, ast.Call) and isinstance(n.func, ast.Name)
              and n.func.id in ('exec', 'eval', 'globals', 'locals', 'vars')):
            return []
    found = {}
    for n in ast.walk(tree):
        if isinstance(n, ast.Name) and isinstance(n.ctx, ast.Load) and n.id not in bound:
            found.setdefault(n.id, n.lineno)
    return ["line %d: name '%s' is not defined" % (line, name)
            for name, line in sorted(found.items(), key=lambda x: x[1])]


# ------------------------------------------------------------------------------------ dispatcher

class Dispatcher:
    def __init__(self, arena_size, cls):
        self.arena_size = arena_size
        self.cls = cls
        self.routines = {}
        self.depth = 0
        self.seq = 0
        # The request being handled, unique in this worker (nested ones have their own): the SQL
        # API's per-call options (Session.callOptions).
        self.scope = 0
        self.scopes = 0
        self.schemas = {}  # each prepared routine's schema (execute_many's engine lookups)
        self._tainted = False
        self.console = Console()
        self.vector_buffers = {}
        self.routine = 0  # the routine whose PREPARE or call runs now (libraries are per routine)
        from . import libraries, sql
        self.sql = sql.Api(self)
        self.libraries = libraries.Libraries(self)
        self.globals = self.sql.globals()
        self.globals['import_library'] = self.libraries.import_library
        self.globals['import_js'] = _no_js
        self.libraries.globals = self.globals

    # -- messages

    def send(self, op, status, w):
        if len(w.b) > wire.MAX_WORKER_MESSAGE:
            raise MessageTooLarge()
        _mlewire.send(op, status, self.depth, self.seq, w.b)

    def ready(self, status, info):
        w = Writer()
        w.u32(wire.VERSION)
        w.str(self.cls)
        w.u32(1)
        w.str('python')
        w.str('CPython %s' % platform.python_version())
        w.str(info)
        _mlewire.stats(wire.STATE_IDLE, 0)
        _mlewire.send(wire.READY, status, 0, 0, w.b)

    @property
    def tainted(self):
        return self._tainted

    @tainted.setter
    def tainted(self, value):
        self._tainted = bool(value)
        _mlewire.taint(self._tainted)

    def loop(self):
        """The request loop is C++ (_mlewire.serve): top-level calls of simple routines run there,
        every other request comes to handle(), and the end of a fast call that needs Python (output,
        SQL, an error, other result types) to fast_finish()."""
        _mlewire.serve(self.handle, self.fast_finish, self.vector_arg, self.__dict__, self.libraries.open)

    def fast_finish(self, rid, ret, failure, interrupted):
        """The tail of call() for a call the C++ loop ran (its state is set: depth 0, seq, scope)."""
        routine = self.routines[rid]
        self.sql.call_ended(True)
        if failure is None and interrupted:
            self.result(wire.INTERRUPTED, error=self.guest('python', 'interrupted'))
        elif failure is None:
            self.result(wire.OK, ret, routine)
        else:
            self.failed(failure, interrupted, routine.name, routine.lines)

    def handle(self, op, depth, seq, body):
        saved = (self.depth, self.seq, self.scope)
        self.scopes += 1
        self.scope = self.scopes
        self.depth = depth
        if depth == 0:
            self.seq = seq
            self.libraries.request_start()
        try:
            if op == wire.CALL:
                self.call(body)
            elif op == wire.PREPARE:
                self.prepare(body)
            elif op == wire.DROP:
                r = Reader(body)
                rid = r.u64()
                self.routines.pop(rid, None)
                self.schemas.pop(rid, None)
                _mlewire.unregister(rid)
                self.result(wire.OK)
            elif op == wire.RESET:
                self.routines.clear()
                self.schemas.clear()
                _mlewire.unregister()
                self.vector_buffers.clear()
                gc.collect()
                self.result(wire.OK)
            elif op == wire.OPTIONS:
                r = Reader(body)
                integer_type, decimal_type, zone = r.u8(), r.u8(), r.str()
                from .sql import zone_of
                if zone_of(zone) is None:  # as Options.parseZone on the JVM
                    self.result(wire.GUEST_ERROR,
                                error=self.guest(wire.LANGUAGE_TIME_ZONE, "unknown time zone '%s'" % zone))
                else:
                    self.sql.integer_type, self.sql.decimal_type, self.sql.zone = integer_type, decimal_type, zone
                    self.result(wire.OK)
            elif op == wire.PARSE_LIBRARY:
                r = Reader(body)
                language, name, src = r.str(), r.str(), r.str()
                try:
                    compile(src, name, 'exec')
                    self.result(wire.OK)
                except SyntaxError as e:
                    self.result(wire.GUEST_ERROR, error=self.guest(language, 'SyntaxError: %s' % e))
            else:
                self.result(wire.INTERNAL, error=('', 'unexpected message %d' % op, [], 0, ''))
        finally:
            self.depth, self.seq, self.scope = saved

    # -- PREPARE

    def prepare(self, body):
        r = Reader(body)
        rid = r.u64()
        language, name, entry, source = r.str(), r.str(), r.str(), r.str()
        params = [(r.str(), r.u8(), r.param_type()) for _ in range(r.u32())]
        ret = r.param_type() if r.u8() else None
        imports = [(r.str(), r.str(), r.str()) for _ in range(r.u32())]
        self.schemas[rid] = r.str()  # the routine's schema
        check = r.u8() != 0
        if language != 'python':
            self.result(wire.GUEST_ERROR, error=self.guest(language, 'the language %s is not available in this '
                                                                     'worker (CPython: Python only)' % language))
            return
        _mlewire.stats(wire.STATE_COMPILING, int(self.tainted))
        # A routine is (re)loaded: library DDL may have happened.
        self.libraries.revalidate()
        top = self.depth == 0
        if top:
            _mlewire.begin(self.seq)
        self.routines.pop(rid, None)
        _mlewire.unregister(rid)
        outer, self.routine = self.routine, rid
        failure = None
        lines = source.split('\n')
        try:
            names = [p[0] for p in params]
            outs = [i for i, p in enumerate(params) if p[1] != 0]
            if entry == '<eval>':
                fn, base = load_eval(source, self.globals)
                name, lines = 'mle_eval', source.split('\n')
            elif entry == '':
                fn, base = load_body(source, name, names, bool(outs), self.globals)
            else:
                fn, base = load_entry(source, name, entry, self.globals)
            if check and entry != '<eval>':
                known = [alias or lib for _, lib, alias in imports]
                for finding in check_names(source, names if entry == '' else [], known, self.globals):
                    self.console.err.write('\x1fmle-check:' + finding + '\n')
            routine = Routine(fn, params, ret, outs, name, lines,
                              [(schema, lib, alias or lib) for schema, lib, alias in imports], base)
            self.routines[rid] = routine
            if not routine.outs and not routine.imports and len(params) <= 64:
                # The C++ call loop runs its top-level calls (exp note 62 §8).
                code = getattr(fn, '__mle_code__', None)
                _mlewire.register(rid, fn, code, base if code is not None else None, len(params),
                                  -1 if ret is None else ret.tag, False if ret is None else ret.unsigned,
                                  routine.vector_params)
        except BaseException as e:
            failure = e
        finally:
            self.routine = outer
        interrupted = top and _mlewire.end()
        if failure is not None:
            self.failed(failure, interrupted, name, lines)
        else:
            self.result(wire.OK)

    # -- CALL

    def vector_arg(self, i, dims, b, p):
        """A VECTOR argument as a reused read-only numpy array (Phase 7, S-14)."""
        np = wire._numpy.get()
        if np is None:
            return wire.new_vector(b, p, dims)
        key = (self.depth, i, dims)
        e = self.vector_buffers.get(key)
        if e is None:
            a = np.empty(dims, np.float32)
            v = a.view()
            v.setflags(write=False)
            e = self.vector_buffers[key] = (a, v)
        e[0][:] = np.frombuffer(b, np.float32, dims, p)
        return e[1]

    def call(self, body):
        rid, _, n = _CALL_HEAD(body)
        routine = self.routines.get(rid)
        if routine is None or routine.nparams != n:
            self.result(wire.INTERNAL, error=('', 'routine %d is not prepared with %d parameters' % (rid, n), [], 0, ''))
            return
        try:
            if routine.vector_params:
                # VECTOR arguments into reused read-only numpy arrays, by parameter position.
                at = [0]

                def vector(dims, p, b=body):
                    i = at[0]
                    at[0] += 1
                    return self.vector_arg(i, dims, b, p)
                args, _ = _decode(body, 16, n, vector)
            else:
                args, _ = _decode(body, 16, n)
        except ConversionError as e:
            self.result(wire.GUEST_ERROR, error=(wire.LANGUAGE_TYPE, str(e), [], 0, ''))
            return
        top = self.depth == 0
        if top:
            _begin(self.seq)
            _mlewire.stats(wire.STATE_RUNNING, int(self.tainted))
        failure = None
        ret = None
        outer, self.routine = self.routine, rid
        try:
            # USING: the libraries' current modules, bound before each call.
            for schema, lib, binding in routine.imports:
                routine.base[binding] = self.libraries.module(schema, lib)
            ret = routine.fn(*args)
        except BaseException as e:
            failure = e
        finally:
            self.routine = outer
        self.sql.call_ended(top)
        interrupted = top and _end()
        if failure is None and interrupted:
            self.result(wire.INTERRUPTED, error=self.guest('python', 'interrupted'))
        elif failure is None:
            self.sql.flush_raw()
            rt = routine.ret
            out, err = Console.take(self.console.out), Console.take(self.console.err)
            if routine.outs or not _result_fast(
                    wire.STATE_IDLE if top else wire.STATE_RUNNING, int(self.tainted), self.depth, self.seq, ret,
                    -1 if rt is None else rt.tag, False if rt is None else rt.unsigned, out, err):
                self.result(wire.OK, ret, routine, console=(out, err))
        else:
            self.failed(failure, interrupted, routine.name, routine.lines)

    # -- RESULT

    def result(self, status, ret=None, routine=None, error=None, console=None):
        self.sql.flush_raw()  # rows still in the arena are read before it is written over
        if console is None:
            console = (Console.take(self.console.out), Console.take(self.console.err))
        try:
            w = self.write_result(ret, routine, error, console)
            if len(w.b) > wire.MAX_WORKER_MESSAGE:
                raise MessageTooLarge()
        except (ConversionError, MessageTooLarge) as e:
            status = wire.GUEST_ERROR
            message = str(e) if isinstance(e, ConversionError) else \
                "the routine's result with its output is larger than MLE allows (64 MiB)"
            w = self.write_result(None, None, (wire.LANGUAGE_TYPE, message, [], 0, ''),
                                  console if isinstance(e, ConversionError) else (b'', b''))
        _mlewire.stats(wire.STATE_IDLE if self.depth == 0 else wire.STATE_RUNNING, int(self.tainted))
        _mlewire.send(wire.RESULT, status, self.depth, self.seq, w.b)

    def write_result(self, ret, routine, error, console):
        w = Writer()
        outs = ()
        if routine is not None and error is None:
            if routine.outs:
                tup = ret
                ret = tup[0]
                outs = [(i, tup[i + 1]) for i in routine.outs]
            if routine.ret is not None:
                w.u8(1)
                w.write_as(ret, routine.ret)
            else:
                w.u8(0)
            w.u32(len(outs))
            for i, v in outs:
                w.u32(i)
                w.write_as(v, routine.params[i][2])
        else:
            w.u8(0)
            w.u32(0)
        w.raw(console[0])
        w.raw(console[1])
        w.u8(0)
        if error is None:
            w.u8(0)
            return w
        language, message, frames, errno, state = error
        w.u8(1)
        w.str(language)
        w.str(message)
        w.u32(len(frames))
        for f in frames:
            w.str(f[0])
            w.str(f[1])
            w.u32(f[2])
            w.u32(0)
            w.u32(0)
            w.str(f[3])
        w.u32(errno)
        w.str(state)
        return w

    # -- errors

    @staticmethod
    def guest(language, message):
        return (language, message, [], 0, '')

    @staticmethod
    def frames(e, name, lines):
        out = []
        # Innermost first, as the JVM worker (and the host's message: its first frame).
        for fs in reversed(traceback.extract_tb(e.__traceback__)):
            if fs.filename != name:
                continue
            fn = fs.name
            if fn in ('__mle_routine', '__mle_body', '<module>'):
                fn = ''
            line = fs.lineno or 0
            text = lines[line - 1].strip()[:200] if 0 < line <= len(lines) else ''
            out.append((fn, name, line, text))
        return out[:1000]

    def failed(self, e, interrupted, name, lines):
        frames = self.frames(e, name, lines)
        if isinstance(e, KeyboardInterrupt) or (interrupted and not isinstance(e, MemoryError)):
            self.result(wire.INTERRUPTED, error=('python', 'interrupted', frames, 0, ''))
        elif isinstance(e, (MemoryError, RecursionError)):
            self.tainted = isinstance(e, MemoryError) or self.tainted
            self.result(wire.EXHAUSTED, error=('python', '%s: %s' % (type(e).__name__, e), frames, 0, ''))
        elif isinstance(e, ConversionError):
            self.result(wire.GUEST_ERROR, error=(wire.LANGUAGE_TYPE, str(e), frames, 0, ''))
        elif getattr(e, '$mle$sql', False):
            state = getattr(e, 'sql_state', '') or ''
            if state and not (len(state) == 5 and state.isalnum() and state.upper() == state):
                self.result(wire.GUEST_ERROR, error=('python', '%s (SqlError with the invalid SQLSTATE %r)'
                                                     % (e.message, state), frames, 0, ''))
            else:
                self.result(wire.GUEST_ERROR, error=('python', e.message, frames, int(e.code), state))
        elif isinstance(e, SyntaxError) and e.filename == name:
            self.result(wire.GUEST_ERROR, error=('python', 'SyntaxError: %s' % e.msg,
                                                 [('', name, e.lineno or 0, (e.text or '').strip()[:200])], 0, ''))
        else:
            msg = ''.join(traceback.format_exception_only(type(e), e)).strip()
            self.result(wire.GUEST_ERROR, error=('python', msg, frames, 0, ''))

    # -- requests to the host while a routine runs

    def library_request(self, schema, name, known_hash):
        """A database library's body (Phase 6): LIBRARY, answered by LIBRARY_RESULT."""
        _mlewire.mark()
        self.sql.flush_raw()
        w = Writer()
        w.str(schema)
        w.str(name)
        w.u64(known_hash)
        _mlewire.send(wire.LIBRARY, wire.OK, self.depth, self.seq, w.b)
        op, status, depth, seq, body = _mlewire.receive()
        if op != wire.LIBRARY_RESULT or depth != self.depth:
            raise RuntimeError('unexpected message %d while loading a library' % op)
        from .libraries import read_reply
        return read_reply(body)

    # -- SQL round trip (called by sql.Api)

    def sql_request(self, w):
        self.sql.flush_raw()
        _mlewire.send(wire.SQL, wire.OK, self.depth, self.seq, w.b)
        while True:
            op, status, depth, seq, body = _mlewire.receive()
            if op == wire.SQL_RESULT and depth == self.depth:
                return body
            if op in (wire.CALL, wire.PREPARE, wire.DROP) and depth == self.depth + 1:
                self.handle(op, depth, seq, body)
                continue
            raise RuntimeError('unexpected message %d at depth %d during SQL' % (op, depth))


class MessageTooLarge(Exception):
    pass


def _no_js(name):
    raise ImportError('import_js(%r): JavaScript needs the JVM build of MLE (this worker runs Python only)' % (name,))


_CALL_HEAD = struct.Struct('<QII').unpack_from  # routine id, events, argument count
_decode = _mlewire.decode
_begin = _mlewire.begin
_end = _mlewire.end
_result_fast = _mlewire.result_fast


def main():
    t0 = time.monotonic()
    cls = os.environ.get('MLE_CLASS', 'cpython')
    import decimal
    import json
    _mlewire.setup(decimal.Decimal, json.loads, wire.temporal, wire.new_vector, wire.demote)
    arena = _mlewire.open(int(os.environ.get("MLE_SPIN", "2000")))
    d = Dispatcher(arena, cls)
    sys.stdout = d.console.out
    sys.stderr = d.console.err
    status, info = wire.OK, ''
    try:
        _ban_threads()
        if os.environ.get('MLE_PREWARM_NUMPY') == '1':
            import numpy  # noqa: F401
        gc.collect()
        info = 'ready in %d ms' % ((time.monotonic() - t0) * 1000)
    except BaseException as e:
        status, info = wire.INTERNAL, 'start failed: %r' % e
    d.ready(status, info)
    d.loop()
