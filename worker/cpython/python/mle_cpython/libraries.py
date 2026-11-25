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

"""Database libraries (CREATE LIBRARY; docs/protocol.md §9.10) for Python routines: bodies come
from the host (LIBRARY), Python libraries become modules.

* An *answer* of the host (BODY or UNCHANGED) belongs to one routine id and one library and
  carries the library epoch of the request in which it was given. An import is resolved under an
  answer that holds for the request being served (the same routine id and epoch); if none does,
  the worker asks. A routine never uses an answer given to another.
* The worker keeps the most recent body of each library, by schema and name, with the version
  token the host gave, and the module it evaluated from it.
* A module is *linked* with the versions of the libraries it imported while it was evaluated.
  Before it is used for a request, those are resolved for that request too, to any depth; when
  one of them has another version, or is refused, the module is evaluated anew. Code that is
  running keeps the modules it has.
* Libraries are not in sys.modules and the import statement does not find them: guest code
  reaches a library only through USING and import_library(), which resolve it each time.
"""

import types

import _mlewire

from . import wire

LIBRARY_REFUSED, LIBRARY_UNCHANGED, LIBRARY_BODY = 0, 1, 2


class LibraryError(ImportError):
    """A library that is not there or not importable from the running routine."""


class Body:
    """The most recent body of a library the worker has."""
    __slots__ = ('language', 'text', 'version')

    def __init__(self, language, text, version):
        self.language, self.text, self.version = language, text, version


class Module:
    """A library's module, evaluated from the body of one version."""
    __slots__ = ('version', 'module', 'imports', 'checked', 'scope')

    def __init__(self, version, module, scope):
        self.version = version
        self.module = module
        self.imports = {}     # (schema, name) -> version: the libraries it imported while evaluated
        self.checked = None   # what its link was last found current for (Libraries._request)
        self.scope = scope    # the request that evaluates it


class Libraries:
    def __init__(self):
        self.bodies = {}      # (schema, name) -> Body
        self.answers = {}     # routine id -> {(schema, name): (epoch, version)}
        self.modules = {}     # (schema, name) -> Module
        self.evaluating = []  # the Modules being evaluated, innermost last
        self.globals = {}     # what a library module starts with (the SQL API ...)
        self.discards = 0     # counts forget() and reset()
        self.before_send = None  # called before LIBRARY is posted (Api.flush_raw: the arena is written over)

    def forget(self, routine_id):
        """A PREPARE of the id begins, or its DROP: its answers are discarded."""
        if self.answers.pop(routine_id, None) is not None:
            self.discards += 1

    def reset(self):
        """RESET: no answer is held. Bodies and modules stay (Appendix B.6)."""
        self.answers.clear()
        self.evaluating.clear()  # none is being evaluated between requests (MVP review CPY-6)
        self.discards += 1

    def resolve(self, key):
        """The Body of library key under an answer that holds for the request being served; asks
        the host if none does or the worker no longer has that body. LibraryError if the import
        is refused."""
        q = _mlewire.request()
        if q is None:
            raise LibraryError('libraries can be imported only while a routine runs')
        rid, epoch = q[2], q[3]
        answers = self.answers.get(rid)
        if answers is None:
            answers = self.answers[rid] = {}
        held = answers.get(key)
        known = self.bodies.get(key)
        if held is not None and held[0] == epoch and known is not None and known.version == held[1]:
            return known
        schema, name = key
        if self.before_send is not None:
            self.before_send()  # rows still in the arena are read before LIBRARY writes over it
        try:
            reply = _mlewire.library(schema, name, 0 if known is None else known.version)
        except ValueError as e:
            raise LibraryError(str(e)) from None
        if reply is None:
            raise LibraryError('the library %s.%s is larger than this MLE worker can hold' % key)
        status, language, text, version, message = reply
        if status == LIBRARY_REFUSED:
            # No answer is held for it, and one that is held is not changed.
            raise LibraryError(message or 'the library %s.%s is not available' % key)
        if status == LIBRARY_BODY:
            known = self.bodies[key] = Body(language, text, version)
        elif known is None or known.version != version:
            _mlewire.protocol_failure('LIBRARY_RESULT UNCHANGED for a body the worker does not hold')
        # The request may have ended the routine's answers meanwhile; this one is the newest.
        self.answers.setdefault(rid, {})[key] = (epoch, version)
        return known

    def _current(self, key, entry, request, seen):
        """Whether entry's link holds for the request: every library it imported resolves to the
        version it was linked with, and so on for those."""
        if entry.checked == request or key in seen:
            return True
        seen.add(key)
        for dep, version in entry.imports.items():
            try:
                if self.resolve(dep).version != version:
                    return False
            except LibraryError:
                return False  # evaluated anew, its import fails where the library's code has it
            below = self.modules.get(dep)
            if below is None or below.version != version or not self._current(dep, below, request, seen):
                return False
        entry.checked = request
        return True

    def module(self, schema, name):
        """A Python library's module for the request being served, for USING and import_library."""
        key = (schema, name)
        body = self.resolve(key)
        q = _mlewire.request()
        if self.evaluating and self.evaluating[-1].scope == q[1]:
            self.evaluating[-1].imports[key] = body.version
        if body.language != wire.LANGUAGE:
            raise LibraryError('the library %s.%s is a %s library: this worker runs Python only'
                               % (schema, name, _language_name(body.language)))
        entry = self.modules.get(key)
        # A link found current stays so while the routine's answers hold: for the same routine id
        # and epoch, until answers are discarded.
        if entry is not None and entry.version == body.version and (
                entry in self.evaluating or self._current(key, entry, (q[2], q[3], self.discards), set())):
            return entry.module
        dotted = schema + '.' + name
        m = types.ModuleType(dotted)
        m.__dict__.update(self.globals)
        entry = Module(body.version, m, q[1])
        code = compile(body.text, dotted, 'exec')
        # The worker's bookkeeping runs where no KeyboardInterrupt lands; only the library's own code
        # can be interrupted (guest_exec; MVP review CPY-6).
        was = _mlewire.guest_pause()
        try:
            # Kept before it runs: libraries that import each other get the partly initialised
            # module instead of recursing.
            former = self.modules.get(key)
            self.modules[key] = entry
            self.evaluating.append(entry)
            try:
                _mlewire.guest_exec(code, m.__dict__, was)
            except BaseException:
                if self.modules.get(key) is entry:
                    if former is None:
                        del self.modules[key]
                    else:
                        self.modules[key] = former
                raise
            finally:
                self.evaluating.remove(entry)
        finally:
            _mlewire.guest_unpause(was)
        return m

    def import_library(self, path):
        """import_library('/db/lib') or ('db.lib'): the library's module."""
        p = str(path).strip('/').replace('/', '.')
        schema, _, name = p.partition('.')
        if not name:
            raise ValueError("a library is '/schema/name' or 'schema.name', not %r" % (path,))
        return self.module(schema, name)


def _language_name(language):
    return {'js': 'JavaScript', 'java': 'Java'}.get(language, language)
