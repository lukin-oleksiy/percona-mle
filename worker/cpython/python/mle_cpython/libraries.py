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

"""Database libraries (CREATE LIBRARY, 14-phase6-libraries.md) for Python routines: bodies come
from mysqld (LIBRARY requests), Python libraries become modules evaluated once per version.

The JVM worker's rules (LibraryFileSystem.java), ported as they are:

* Epochs: library DDL makes mysqld prepare the routines again, and each PREPARE starts an epoch;
  within one, a library confirmed for a routine is used without asking mysqld again ("routine:key"
  in confirmed: mysqld decides per importing routine).
* Loadsets and generations (independent review C17): an evaluated library keeps the modules it
  imported then. So every library has a loadset: the libraries (with their hashes) fetched in the
  top-level request that first evaluated it, and their loadsets. When a library is confirmed for a
  routine, its loadset is confirmed too; if one of those changed or is refused, a new generation
  starts and every library is evaluated again at its next use (a new stamp).
"""

import types

from . import wire

LIBRARY_REFUSED, LIBRARY_UNCHANGED, LIBRARY_BODY = 0, 1, 2
_STAMP_MIX = 0x9E3779B97F4A7C15
_MASK = (1 << 64) - 1


class LibraryError(ImportError):
    """A library that is not there or not importable from the running routine."""


class Library:
    __slots__ = ('language', 'body', 'hash', 'checked', 'generation', 'loadset')

    def __init__(self, language, body, hash_, checked, generation, loadset):
        self.language, self.body, self.hash = language, body, hash_
        self.checked, self.generation, self.loadset = checked, generation, loadset

    def confirmed(self, epoch):
        if self.checked == epoch:
            return self
        return Library(self.language, self.body, self.hash, epoch, self.generation, self.loadset)

    def stamp(self):
        """What the cache of evaluated modules goes by: the hash, and another value per generation."""
        return self.hash if self.generation == 0 else self.hash ^ ((self.generation * _STAMP_MIX) & _MASK)


class Libraries:
    def __init__(self, dispatcher):
        self.d = dispatcher
        self.libraries = {}      # "schema/name" -> Library (the newest body seen)
        self.generation = 0
        self.open = []           # loadsets still collecting (libraries first evaluated in this request)
        self.epoch = 1
        self.confirmed = set()   # "routine:schema/name" confirmed in this epoch
        self.modules = {}        # "schema/name" -> (stamp, module): evaluated Python libraries
        self.globals = {}        # what a library module starts with (the SQL API ...)

    # -- the JVM worker's LibraryFileSystem ---------------------------------------------------

    def request_start(self):
        """A top-level request starts: the libraries evaluated in the last one have their loadsets."""
        self.open.clear()

    def revalidate(self):
        """A routine is (re)loaded: libraries are confirmed with mysqld again at their next use."""
        self.epoch += 1
        self.confirmed.clear()

    def _touch(self, key, lib):
        for s in self.open:
            if s is lib.loadset:
                continue
            s[key] = lib.hash
            s.update(lib.loadset)

    def _renewed(self, key, lib):
        n = Library(lib.language, lib.body, lib.hash, lib.checked, self.generation, {})
        self.libraries[key] = n
        self._touch(key, n)
        self.open.append(n.loadset)
        return n

    def _confirm_loadset(self, lib):
        for key, h in list(lib.loadset.items()):
            schema, _, name = key.partition('/')
            try:
                if self.fetch(schema, name).hash == h:
                    continue
            except LibraryError:
                return  # dropped, or not for this routine: fetch started a generation
            self._new_generation()
            return

    def _new_generation(self):
        self.generation += 1
        self.confirmed.clear()

    def evaluating(self, schema, name):
        lib = self.libraries.get(schema + '/' + name)
        if lib is None:
            return
        if any(s is lib.loadset for s in self.open):
            return
        self.open.append(lib.loadset)

    def fetch(self, schema, name):
        key = schema + '/' + name
        known = self.libraries.get(key)
        confirmation = '%d:%s' % (self.d.routine, key)
        if known is not None and known.checked == self.epoch and confirmation in self.confirmed:
            if known.generation != self.generation:
                return self._renewed(key, known)
            if self.open:
                self._touch(key, known)
            return known
        status, language, body, h, message = self.d.library_request(schema, name, 0 if known is None else known.hash)
        if status == LIBRARY_UNCHANGED and known is not None:
            lib = known.confirmed(self.epoch)
            self.libraries[key] = lib
            self.confirmed.add(confirmation)
            if lib.generation == self.generation:
                self._touch(key, lib)
                self._confirm_loadset(lib)
            return self._renewed(key, lib) if lib.generation != self.generation else lib
        if status == LIBRARY_BODY:
            lib = Library(language, body, h, self.epoch, self.generation, {})
            self.libraries[key] = lib
            if known is not None:
                self.confirmed.clear()  # a changed body: routines confirmed for the old one confirm again
            self.confirmed.add(confirmation)
            self._touch(key, lib)
            self.open.append(lib.loadset)  # a new body: evaluated from here on
            return lib
        self.libraries.pop(key, None)
        if known is not None or self.open:
            self._new_generation()
        raise LibraryError(message or 'the library %s.%s is not available' % (schema, name))

    # -- modules ----------------------------------------------------------------------------------

    def module(self, schema, name):
        """A Python library's module (evaluated once per version), for USING and import_library."""
        lib = self.fetch(schema, name)
        if lib.language != 'python':
            raise LibraryError('the library %s.%s is a %s library: it needs the JVM build of MLE '
                               '(this worker runs Python only)' % (schema, name, _language_name(lib.language)))
        key = schema + '/' + name
        stamp = lib.stamp()
        hit = self.modules.get(key)
        if hit is not None and hit[0] == stamp:
            return hit[1]
        self.evaluating(schema, name)
        dotted = schema + '.' + name
        m = types.ModuleType(dotted)
        m.__dict__.update(self.globals)
        # Kept before it runs (as sys.modules): libraries that import each other get the partly
        # initialised module instead of recursing.
        self.modules[key] = (stamp, m)
        try:
            exec(compile(lib.body, dotted, 'exec'), m.__dict__)
        except BaseException:
            del self.modules[key]
            raise
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


def read_reply(body):
    """LIBRARY_RESULT: (status, language, body, hash, message)."""
    r = wire.Reader(body)
    status = r.u8()
    language, text = r.str(), r.str()
    h = r.u64()
    message = r.str()
    r.finish()
    return status, language, text, h, message
