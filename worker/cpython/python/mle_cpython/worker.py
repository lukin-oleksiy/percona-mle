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

"""The dispatcher of the CPython worker (docs/protocol.md): what each request does and what its
RESULT says. The request loop, the requests being served and the SQL statements' states are C++
(_mlewire, module.cc); SQL round trips serve nested requests meanwhile, through handle() again."""

import ast
import gc
import io
import os
import platform
import re
import signal
import struct
import sys
import textwrap
import threading
import time
import traceback
import types

import _mlewire

from . import wire
from .wire import Reader, Writer, ConversionError


class Source:
    """What a request's code is called and its lines, for the frames of an Error."""
    __slots__ = ('file', 'lines')

    def __init__(self, file, lines):
        self.file, self.lines = file, lines


class Routine(Source):
    """A prepared routine. A running activation holds it, and the function it started with, also
    when a nested PREPARE or DROP takes the id away (§8.3)."""
    __slots__ = ('fn', 'params', 'ret', 'outs', 'nparams', 'vector_params', 'imports', 'base', 'schema',
                 'relink', 'linked', 'active')

    def __init__(self, fn, params, ret, outs, file, lines, imports, base, schema, relink):
        Source.__init__(self, file, lines)
        self.fn, self.params, self.ret, self.outs = fn, params, ret, outs
        self.nparams = len(params)
        self.vector_params = any(p[2].tag == wire.T_VECTOR for p in params)
        self.imports = imports  # USING: (schema, name, binding)
        self.base = base        # the globals each call starts from (USING binds into them)
        self.schema = schema
        self.relink = relink    # entry form: makes the function anew with other bindings
        self.linked = None      # the modules of the imports it is linked with
        self.active = 0         # its activations that are running


class _Output(io.StringIO):
    """sys.stdout/stderr of routines: a write tells the C++ call loop that output waits for a
    RESULT, which Python then writes. A RESULT carries the last Console.LIMIT bytes at most, so
    more than twice that in characters is cut to its end while it is written (as the V8 worker
    does): a request that prints without end does not hold all of it (MVP review CPY-8)."""

    def write(self, s):
        _mlewire.printed()
        n = io.StringIO.write(self, s)
        if self.tell() > 2 * Console.LIMIT:
            self._keep_end()
        return n

    def writelines(self, lines):
        for line in lines:
            self.write(line)

    def _keep_end(self):
        v = self.getvalue()[-Console.LIMIT:]
        self.seek(0)
        self.truncate()
        io.StringIO.write(self, v)


class Console:
    """What guest code printed and no RESULT has carried yet (§9.4)."""
    # Kept per stream, at most: the end of longer output.
    LIMIT = 1 << 20

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
        return keep_end(wire.utf8(v), Console.LIMIT)


def keep_end(b, limit):
    """The last limit bytes of UTF-8 text at most, from a character boundary."""
    if len(b) <= limit:
        return b
    at = len(b) - limit
    while at < len(b) and (b[at] & 0xC0) == 0x80:
        at += 1
    return b[at:]


# ------------------------------------------------------------------------------------ loaders

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
    """The body as a function of the parameters; fresh globals per call."""
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


def load_entry(src, name, file, entry, globals_):
    ns = {'__name__': name}
    ns.update(globals_)
    exec(compile(src, file, 'exec'), ns)
    fn = ns[entry]
    return fn, getattr(fn, '__globals__', ns)


def load_eval(src, globals_):
    """Entry <eval>: the snippet's statements, then the value of its last expression as text (repr,
    as a REPL shows it; a str as it is), or None. Fresh globals per call."""
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
    """The findings of a PREPARE with check: names the code reads but never binds (a typo such as lenght), not
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

class _TooLarge(Exception):
    """The worker could not hold the request (§5.3)."""


class _Refused(Exception):
    """OPTIONS the worker does not accept: language is MLE-TimeZone or MLE-Option (§9.8)."""

    def __init__(self, language, message):
        Exception.__init__(self, message)
        self.language = language


class _Failed:
    """What a request failed with: the RESULT's status and Error (language, message, frames,
    sql_errno, sqlstate, sql_error_id), and whether the worker must be retired after it."""
    __slots__ = ('status', 'error', 'taint')

    def __init__(self, status, language, message, frames=(), errno=0, state='', error_id=0, taint=False):
        self.status = status
        self.error = (language, message, frames, errno, state, error_id)
        self.taint = taint


_SQLSTATE = re.compile('[0-9A-Z]{5}', re.ASCII)
_NO_VALUES = bytes(5)  # has_return 0, no out_params
_CALL_HEAD = struct.Struct('<QQI')  # routine_id, library_epoch, the argument count
_decode = _mlewire.decode


class Dispatcher:
    def __init__(self, arena_size):
        self.arena_size = arena_size
        self.routines = {}
        self.console = Console()
        self.vector_buffers = {}
        self.info = ''
        from . import libraries, sql
        self.libraries = libraries.Libraries()
        self.sql = sql.Api(self)
        self.libraries.before_send = self.sql.flush_raw
        self.globals = self.sql.globals()
        self.globals['import_library'] = self.libraries.import_library
        self.globals['import_js'] = _no_js
        self.libraries.globals = self.globals

    def serve(self):
        """READY, then the request loop (C++): top-level calls of simple routines run there, every
        other request comes to handle(), and every request's end to finish()."""
        _mlewire.ready(self.info, 'CPython %s' % platform.python_version())
        _mlewire.serve(self.handle, self.finish, self.values, self.failed, self.vector_arg)

    # -- requests

    def handle(self, op, body, head, ctx):
        """Serves a request up to its outcome: the values of a CALL, the findings of a PREPARE, or
        what it failed with (a _Failed). body is None if the worker could not hold the message;
        head is then its first 8 bytes. ctx, a list, gets the request's Source for finish().

        Guest code of a PREPARE or CALL runs only inside this function, and an interrupt raises
        KeyboardInterrupt only here; what leaves it as an exception is that interrupt."""
        try:
            if body is None:
                if op == wire.PREPARE:
                    self.drop(head)
                raise _TooLarge()
            if op == wire.CALL:
                return self.call(body, ctx)
            if op == wire.PREPARE:
                return self.prepare(body, ctx)
            if op == wire.DROP:
                self.drop(Reader(body).u64())
            elif op == wire.RESET:
                self.reset()
            elif op == wire.OPTIONS:
                self.options(body)
            elif op == wire.PARSE_LIBRARY:
                r = Reader(body)
                _language(r.str())
                name, source = r.str(), r.str()
                ctx.append(Source(name, source.split('\n')))
                compile(source, name, 'exec')
            return None
        except BaseException as e:
            return self.failed(ctx[0] if ctx else None, e)

    def drop(self, rid):
        """The id is not prepared (§8.3), and the answers of its libraries are discarded (§9.10)."""
        if self.routines.pop(rid, None) is not None:
            _mlewire.unregister(rid)
        self.libraries.forget(rid)

    def reset(self):
        """RESET (§9.7): no routine, no options, no session state and no waiting console output;
        imported modules and loaded libraries stay (Appendix B.6)."""
        for rid in list(self.routines):
            _mlewire.unregister(rid)
        self.routines.clear()
        self.libraries.reset()
        self.vector_buffers.clear()
        self.sql.reset()
        Console.take(self.console.out)
        Console.take(self.console.err)
        _restore_process_state()
        gc.collect()
        _mlewire.trim()  # malloc'ed memory back to the system (§9.7; MVP review CPY-11)
        # Guest threads left running: the worker cannot be reused safely.
        if threading.active_count() > 1:
            _mlewire.taint()

    def options(self, body):
        """OPTIONS (§9.8, Appendix B.2): the zone and decimal_type; integer_type and names of other
        languages are not this worker's. On a refusal the options in force stay."""
        r = Reader(body)
        zone = r.str()
        decimal_type = 0
        for _ in range(r.count(8)):
            name, value = r.str(), r.str()
            if name == 'decimal_type':
                if value not in ('string', 'number'):
                    raise _Refused(wire.LANGUAGE_OPTION, "'%s' is not a value of decimal_type" % value)
                decimal_type = 1 if value == 'number' else 0
        from .sql import zone_of
        tz = zone_of(zone)
        if tz is None:
            raise _Refused(wire.LANGUAGE_TIME_ZONE, "unknown time zone '%s'" % zone)
        self.sql.decimal_type = decimal_type
        self.sql.zone = tz

    # -- PREPARE

    def prepare(self, body, ctx):
        # The whole message is taken out of the arena before guest code, which may post, runs.
        r = Reader(body)
        rid = r.u64()
        _language(r.str())
        name, entry, source = r.str(), r.str(), r.str()
        params = []
        for _ in range(r.count(23)):
            pname, mode = r.str(), r.u8()
            if mode > 2:
                _mlewire.protocol_failure('an unknown Param mode')
            params.append((pname, mode, r.type()))
        ret = r.type() if r.u8() else None
        imports = [(r.str(), r.str(), r.str()) for _ in range(r.count(12))]
        schema = r.str()
        check = r.u8() != 0
        _mlewire.epoch(r.u64())
        # The id is not prepared from here until this PREPARE answers OK (§8.3).
        self.drop(rid)
        lines = source.split('\n')
        names = [p[0] for p in params]
        outs = [i for i, p in enumerate(params) if p[1] != 0]
        relink = None
        if entry == '<eval>':
            file = 'mle_eval'
            ctx.append(Source(file, lines))
            fn, base = load_eval(source, self.globals)
        elif entry == '':
            file = name
            ctx.append(Source(file, lines))
            fn, base = load_body(source, name, names, bool(outs), self.globals)
        else:
            file = name + '.py'
            ctx.append(Source(file, lines))
            fn, base = load_entry(source, name, file, entry, self.globals)

            def relink(bindings):
                g = dict(self.globals)
                g.update(bindings)
                return load_entry(source, name, file, entry, g)
        findings = []
        if check and entry != '<eval>':
            known = [alias or lib for _, lib, alias in imports]
            findings = check_names(source, names if entry == '' else [], known, self.globals)
        routine = Routine(fn, params, ret, outs, file, lines,
                          [(s, lib, alias or lib) for s, lib, alias in imports], base, schema, relink)
        if not routine.outs and not routine.imports and len(params) <= 64:
            # The C++ call loop runs its top-level calls.
            code = getattr(fn, '__mle_code__', None)
            _mlewire.register(rid, routine, fn, code, base if code is not None else None, len(params),
                              -1 if ret is None else ret.tag, False if ret is None else ret.unsigned,
                              routine.vector_params)
        self.routines[rid] = routine
        return findings

    # -- CALL

    def vector_arg(self, i, dims, b, p):
        """A VECTOR argument as a reused read-only numpy array: a copy of the message's elements."""
        np = wire._numpy.get()
        if np is None:
            return wire.new_vector(b, p, dims)
        key = (_mlewire.request()[0], i, dims)
        e = self.vector_buffers.get(key)
        if e is None:
            a = np.empty(dims, np.float32)
            v = a.view()
            v.setflags(write=False)
            e = self.vector_buffers[key] = (a, v)
        e[0][:] = np.frombuffer(b, np.float32, dims, p)
        return e[1]

    def link(self, routine):
        """Resolves the routine's USING libraries for this request (§9.10) and binds their modules.
        When one is another module than the routine is linked with, the routine is linked anew;
        an activation that is running keeps the link it started with."""
        modules = [self.libraries.module(schema, lib) for schema, lib, _ in routine.imports]
        if modules == routine.linked:
            return
        bindings = {i[2]: m for i, m in zip(routine.imports, modules)}
        if routine.relink is not None and routine.active and routine.linked is not None:
            # Entry form: its functions share the globals the running activation uses.
            routine.fn, routine.base = routine.relink(bindings)
        else:
            # Body form: each call starts from a copy of base. Entry form: nothing of it runs.
            routine.base.update(bindings)
        routine.linked = modules

    def call(self, body, ctx):
        if len(body) < _CALL_HEAD.size:
            _mlewire.protocol_failure('message too short')
        rid, _, n = _CALL_HEAD.unpack_from(body)
        routine = self.routines.get(rid)
        if routine is None or routine.nparams != n:
            _mlewire.protocol_failure('CALL of a routine that is not prepared, or with another argument count')
        ctx.append(routine)
        if routine.vector_params:
            # VECTOR arguments into reused read-only numpy arrays, by parameter position.
            at = [0]

            def vector(dims, p, b=body):
                i = at[0]
                at[0] += 1
                return self.vector_arg(i, dims, b, p)
            args, _ = _decode(body, _CALL_HEAD.size, n, vector)
        else:
            args, _ = _decode(body, _CALL_HEAD.size, n)
        if routine.imports:
            self.link(routine)
        fn = routine.fn
        routine.active += 1
        try:
            ret = fn(*args)
        finally:
            routine.active -= 1
        return self.values(routine, ret)

    def values(self, routine, ret):
        """The start of an OK RESULT of a CALL: has_return, the return value, out_params (all OUT
        and INOUT parameters, ascending). Converting can run guest code (__str__, __index__ ...):
        it is done here, before the request is over for interrupts, and into memory of the
        worker, not the arena (§4.1)."""
        w = Writer()
        outs = ()
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
        return w.b

    # -- errors

    @staticmethod
    def frames(e, source):
        out = []
        if source is None:
            return out
        file, lines = source.file, source.lines
        # Innermost first: the host's message shows the first frame.
        for fs in reversed(traceback.extract_tb(e.__traceback__)):
            if fs.filename != file:
                continue
            fn = fs.name
            if fn in ('__mle_routine', '__mle_body', '<module>'):
                fn = ''
            line = fs.lineno or 0
            text = lines[line - 1].strip() if 0 < line <= len(lines) else ''
            out.append((fn, file, line, text))
            if len(out) == wire.MAX_FRAMES:
                break
        return out

    def failed(self, source, e):
        """What the exception e means for the request (a _Failed). An exception's text is guest
        code (__str__): it is made here, while the request can still be interrupted."""
        if isinstance(e, _TooLarge):
            return _Failed(wire.EXHAUSTED, '', 'the request is larger than this MLE worker can hold')
        if isinstance(e, _Refused):
            return _Failed(wire.GUEST_ERROR, e.language, str(e))
        try:
            return self.described(source, e)
        except Exception:
            # The exception's own text failed (its __str__ raised).
            return _Failed(wire.GUEST_ERROR, wire.LANGUAGE, type(e).__name__)

    def described(self, source, e):
        frames = self.frames(e, source)
        if isinstance(e, MemoryError):
            # The heap is exhausted: the runtime may be unsound; the worker asks to be retired (§9.4,
            # B.5). A RecursionError is a limit guest code can catch and that leaves the runtime
            # sound: an ordinary error of guest code.
            return _Failed(wire.EXHAUSTED, wire.LANGUAGE, '%s: %s' % (type(e).__name__, e), frames, taint=True)
        if isinstance(e, ConversionError):
            return _Failed(wire.GUEST_ERROR, wire.LANGUAGE_TYPE, str(e), frames)
        if getattr(e, '$mle$sql', False):
            return self.sql_failed(e, frames)
        if isinstance(e, SyntaxError) and source is not None and e.filename == source.file:
            return _Failed(wire.GUEST_ERROR, wire.LANGUAGE, '%s: %s' % (type(e).__name__, e.msg),
                           [('', source.file, e.lineno or 0, (e.text or '').strip())])
        return _Failed(wire.GUEST_ERROR, wire.LANGUAGE, _describe(e), frames)

    @staticmethod
    def sql_failed(e, frames):
        """A routine that ends with a SqlError (§10.8). One made from an ERROR of the host in this
        top-level request hands that error back by its id, with its number, SQLSTATE and message
        as the host sent them; any other raises its own SQL error, if its fields are legal."""
        handed = e.__dict__.get('$mle$error')
        q = _mlewire.request()
        if type(handed) is tuple and len(handed) == 5 and q is not None and handed[0] == q[4]:
            _, error_id, errno, state, message = handed
            if not (type(errno) is int and 0 <= errno <= wire.MAX_SQL_ERRNO and type(error_id) is int
                    and 0 < error_id < 2 ** 64 and type(state) is str and type(message) is str):
                errno, error_id, state = 0, 0, ''
            return _Failed(wire.GUEST_ERROR, wire.LANGUAGE, message, frames, errno,
                           state if _SQLSTATE.fullmatch(state) else '', error_id)
        message = str(getattr(e, 'message', ''))
        state = getattr(e, 'sql_state', '') or ''
        if not isinstance(state, str) or (state and not _SQLSTATE.fullmatch(state)):
            return _Failed(wire.GUEST_ERROR, wire.LANGUAGE,
                           '%s (SqlError with the invalid SQLSTATE %r)' % (message, state), frames)
        try:
            errno = int(e.code)
        except (TypeError, ValueError, AttributeError):
            errno = 0
        return _Failed(wire.GUEST_ERROR, wire.LANGUAGE, message, frames,
                       errno if 0 <= errno <= wire.MAX_SQL_ERRNO else 0, state)

    # -- RESULT

    def finish(self, op, ctx, outcome, raised, delivered):
        """The RESULT of a request: (status, body). outcome: what handle() returned (for a CALL of
        the C++ call loop: values() or failed()); raised: the exception that left it instead;
        delivered: an interrupt was delivered to the top-level request. No guest code of the
        request runs any more, and no interrupt raises."""
        guest = op == wire.CALL or op == wire.PREPARE
        failed = None
        if raised is not None:
            failed = self.failed(ctx[0] if ctx else None, raised)
        elif type(outcome) is _Failed:
            failed = outcome
        if failed is not None and delivered and not (failed.status == wire.EXHAUSTED and failed.taint):
            # INTERRUPTED only for a delivered interrupt (§11.1); a KeyboardInterrupt of the guest
            # code's own is its error.
            failed = _Failed(wire.INTERRUPTED, wire.LANGUAGE, 'interrupted', failed.error[2])
        if failed is not None and op in (wire.DROP, wire.RESET, wire.OPTIONS) and \
                failed.error[0] not in (wire.LANGUAGE_TIME_ZONE, wire.LANGUAGE_OPTION):
            # These requests run no guest code: the worker itself failed (§9.4).
            failed = _Failed(wire.INTERNAL, '', failed.error[1], taint=True)
        status, error, values, findings = wire.OK, None, _NO_VALUES, ()
        if failed is None:
            if op == wire.CALL:
                values = outcome
            elif op == wire.PREPARE:
                findings = outcome
        else:
            status, error = failed.status, failed.error
            if failed.taint:
                _mlewire.taint()
            if op == wire.PREPARE:
                self.drop(_mlewire.request()[2])  # a PREPARE that does not answer OK (§8.3)
        out = err = b''
        if guest:
            self.sql.flush_raw()  # rows still in the arena are read before it is written over
            out, err = Console.take(self.console.out), Console.take(self.console.err)
        body = self.result(values, out, err, findings, error)
        if len(body) > wire.MAX_WORKER_MESSAGE:
            # §9.4: console output is cut, the end of each stream kept; if the RESULT is too large
            # without any, it is a conversion error.
            room = wire.MAX_WORKER_MESSAGE - (len(body) - len(out) - len(err))
            if room < 0:
                status, values, findings = wire.GUEST_ERROR, _NO_VALUES, ()
                error = (wire.LANGUAGE_TYPE, "the routine's result is larger than MLE allows (64 MiB)", (), 0, '', 0)
                if op == wire.PREPARE:
                    self.drop(_mlewire.request()[2])
                room = wire.MAX_WORKER_MESSAGE - len(self.result(values, b'', b'', findings, error))
            err = keep_end(err, max(room // 2, room - len(out)))
            out = keep_end(out, room - len(err))
            body = self.result(values, out, err, findings, error)
        return status, body

    @staticmethod
    def result(values, out, err, findings, error):
        """A RESULT's body (§9.4) with the caps of §7.4 and §9.4."""
        w = Writer()
        b = w.b
        b += values
        w.raw(out)
        w.raw(err)
        w.u8(0)  # console_cleared: Python has no call that clears the console
        findings = findings[:wire.MAX_FINDINGS]
        w.u32(len(findings))
        for f in findings:
            w.raw(wire.cut(wire.utf8(f), wire.MAX_FINDING))
        if error is None:
            w.u8(0)
            return b
        language, message, frames, errno, state, error_id = error
        w.u8(1)
        w.str(language)
        w.raw(wire.cut(wire.utf8(message), wire.MAX_ERROR_MESSAGE))
        frames = frames[:wire.MAX_FRAMES]
        w.u32(len(frames))
        for function, file, line, text in frames:
            w.raw(wire.cut(wire.utf8(function), wire.MAX_FRAME_TEXT))
            w.raw(wire.cut(wire.utf8(file), wire.MAX_FRAME_TEXT))
            w.u32(line if 0 <= line < 2 ** 32 else 0)
            w.u32(0)
            w.u32(0)
            w.raw(wire.cut(wire.utf8(text), wire.MAX_FRAME_TEXT))
        w.u32(errno)
        w.str(state)
        w.u64(error_id)
        return b


def _describe(e):
    """An exception as Python's last traceback line, named by its class's own name: a class defined
    in a routine is MyError, not r.__mle_routine.<locals>.MyError (that wrapper is ours); module
    prefixes go the same way."""
    try:
        text = ''.join(traceback.format_exception_only(type(e), e)).strip()
    except Exception:
        return type(e).__name__
    prefix = text.split(':', 1)[0]
    name = type(e).__name__
    if prefix != name and prefix.endswith('.' + name):
        text = name + text[len(prefix):]
    return text


def _language(language):
    """The language of a PREPARE or PARSE_LIBRARY: the one READY announced (§1 item 4)."""
    if language != wire.LANGUAGE:
        _mlewire.protocol_failure('a request for a language the worker did not announce')


# --------------------------------------------------------------- signals (D11, CPY-10)

def _on_interrupt(signum, frame):
    """SIGUSR1, the worker's own: an interrupt (§11.1) wakes a blocking call of guest code
    (time.sleep, a lock, a wait) with it, and it raises KeyboardInterrupt only while guest code of
    the interrupted request runs; elsewhere it returns, and the call goes on (MVP review D11)."""
    if _mlewire.interrupt_due():
        raise KeyboardInterrupt


def _interrupt_handler_installed():
    return signal.getsignal(signal.SIGUSR1) is _on_interrupt


# The process's signal handlers and interval timers as the worker set them up (start()): RESET
# puts back any that guest code changed, and drops trace and profile hooks (MVP review CPY-10).
_handlers = {}
_ITIMERS = (signal.ITIMER_REAL, signal.ITIMER_VIRTUAL, signal.ITIMER_PROF)


def _install_signals():
    signal.signal(signal.SIGUSR1, _on_interrupt)
    _mlewire.interrupt_signal(_interrupt_handler_installed)
    _handlers.clear()
    for s in signal.valid_signals():
        try:
            _handlers[s] = signal.getsignal(s)
        except (OSError, ValueError):
            pass


def _restore_process_state():
    for t in _ITIMERS:
        signal.setitimer(t, 0)
    for s, h in _handlers.items():
        try:
            if signal.getsignal(s) is not h and h is not None:
                signal.signal(s, h)
        except (OSError, ValueError, TypeError):
            pass
    sys.settrace(None)
    sys.setprofile(None)
    threading.settrace(None)
    threading.setprofile(None)


def _no_js(name):
    raise ImportError('import_js(%r): this worker runs Python only' % (name,))


def start():
    """The worker's Python side, before READY: the Dispatcher. What fails here is the reason of a
    failed READY (main.cc)."""
    t0 = time.monotonic()
    import decimal
    import json
    from . import sql
    # The values of routine arguments, and of rows (zero dates as None; DECIMAL as float with NUMBER).
    _mlewire.setup(decimal.Decimal, json.loads, wire.temporal, wire.new_vector, wire.demote, sql._row_temporal,
                   float, wire.timestamp)
    d = Dispatcher(_mlewire.arena_size())
    _install_signals()
    sys.stdout = d.console.out
    sys.stderr = d.console.err
    _ban_threads()
    if os.environ.get('MLE_PREWARM_NUMPY') == '1':
        import numpy  # noqa: F401
    gc.collect()
    d.info = 'class %s, ready in %d ms' % (os.environ.get('MLE_CLASS', 'cpython'), (time.monotonic() - t0) * 1000)
    return d
