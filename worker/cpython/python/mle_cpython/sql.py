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

"""The SQL API for routines: the JVM kind's Python API (PySql, GuestSql and com.percona.mle.sql's
Session, SqlResult, PreparedStatement ...) with the same names (snake_case and camelCase), values,
errors and messages. Rows stay in the reply until they are read (decode_rows / decode_columns in
C++); the api reads raw chunks before the arena is written over (Api.flush_raw)."""

import collections as _collections
import datetime as _dt
import decimal as _decimal
import json as _json
import math as _math
import random as _random
import re as _re
import secrets as _secrets
import threading as _threading
import time as _time
import types as _types
import weakref as _weakref

import _mlewire

from . import sql_batch, wire
from .wire import Reader, Writer

_decode_rows = _mlewire.decode_rows
_encode_values = _mlewire.encode_values
_sql = _mlewire.sql
_decode_columns = _mlewire.decode_columns

EXECUTE, PREPARE, EXECUTE_PREPARED, FETCH, NEXT_RESULT, CLOSE, EXECUTE_MANY = range(7)
TO_CLIENT = 1
MAX_VALUES = 65535


class SqlError(Exception):
    """Oracle's SqlError(sql_state, message, error_number). Raised, MySQL raises it like SIGNAL;
    errors of SQL statements are SqlErrors too (code, sql_state, message)."""

    def __init__(self, sql_state=None, message='', error_number=None):
        for n, v in (('sql_state', sql_state), ('error_number', error_number)):
            if isinstance(v, float) and not _math.isfinite(v):
                raise TypeError('SqlError: ' + n + ' must be a finite number')
        if sql_state is None:
            state = '45000'
        elif isinstance(sql_state, (int, float)) and not isinstance(sql_state, bool):
            state = str(int(sql_state)).rjust(5, '0')
        else:
            state = str(sql_state)
        self.message = '' if message is None else str(message)
        super().__init__(self.message)
        self.sql_state = state
        self.sqlState = state
        self.code = 1644 if error_number is None else int(error_number)
        self.row = None  # execute_many: the failing row (exp note 51)
        setattr(self, '$mle$sql', True)

    def __str__(self):
        return ('SqlError: Error code: %d Error state: %s Error message: %s'
                % (self.code, self.sql_state, self.message))


def _sql_error(code, state, message, row=None):
    e = SqlError(state, message, code)
    if row is not None and row >= 0:
        e.row = row
    return e


# ---------------------------------------------------------------------------- guest conventions
# GuestSql/PySql: the JVM kind's errors (IllegalArgumentException and conversion errors are
# TypeErrors, other failures RuntimeErrors), argument rules and snake_case names.

def _camel(name):
    """PySql.member: snake_case (run_sql) as the API's camelCase (runSql); names starting with _
    unchanged."""
    if '_' not in name or name.startswith('_'):
        return name
    parts = name.split('_')
    return parts[0] + ''.join(p[:1].upper() + p[1:] for p in parts[1:])


def _aliases(cls):
    """The camelCase names of a class's snake_case methods (both work on the JVM kind)."""
    for name, v in list(vars(cls).items()):
        if callable(v) and '_' in name and not name.startswith('_'):
            setattr(cls, _camel(name), v)
    return cls


_MISSING = object()


def _arg(a, i):
    """GuestSql.arg: the argument, or None if it is missing or None."""
    return a[i] if i < len(a) and a[i] is not None else None


def _str(a, i):
    """GuestSql.str: a required argument as text."""
    v = _arg(a, i)
    if v is None:
        raise TypeError('argument %d is missing' % (i + 1))
    return v if isinstance(v, str) else _jstr(v)


def _jstr(v):
    """String.valueOf of a guest value (booleans as Java writes them)."""
    if v is True:
        return 'true'
    if v is False:
        return 'false'
    return str(v)


def _is_array(v):
    """Whether v has array elements as GraalPy says (lists, tuples, numpy arrays ...; not text)."""
    if isinstance(v, (list, tuple)):
        return True
    if v is None or isinstance(v, (str, bytes, bytearray, dict, set, frozenset, memoryview)):
        return False
    t = type(v)
    if hasattr(t, 'keys'):
        return False  # a mapping
    return hasattr(t, '__len__') and hasattr(t, '__getitem__') and getattr(v, 'ndim', 1) != 0


def _elements(v):
    """GuestSql.elements: a guest array's elements (or one value as a single parameter)."""
    if v is None:
        return []
    if not _is_array(v):
        return [v]
    return v if isinstance(v, (list, tuple)) else list(v)


def _rows(v):
    """GuestSql.rows: the rows of execute_many (lists or tuples of values), checked before any
    of them runs."""
    if v is None or not _is_array(v):
        raise TypeError('executeMany takes a list of rows, each a list of values')
    rows = v if isinstance(v, list) else list(v)
    for i, r in enumerate(rows):
        if type(r) is not tuple and type(r) is not list and (r is None or not _is_array(r)):
            raise TypeError('row %d of executeMany is not a list of values' % i)
    return rows


def _truthy(v):
    if v is None:
        return False
    if isinstance(v, bool):
        return v
    if isinstance(v, (int, float, _decimal.Decimal)):
        return v != 0 and (not isinstance(v, float) or not _math.isnan(v))
    if isinstance(v, str):
        return v != ''
    return True


# ---------------------------------------------------------------------------- options

_INTEGER_TYPES = ('UNSAFE_STRING', 'UNSAFE_BIGINT', 'STRING', 'BIGINT')
_DECIMAL_TYPES = ('STRING', 'NUMBER')


def _java_list(names):
    return '[' + ', '.join(names) + ']'


class Options:
    """SqlOptions.java: passResultToClient, charsetName, integerType, decimalType, batchRewrite; None
    is not set. A statement's options are taken over the call's (set_options), those over the
    defaults (the session state's decimal type)."""
    __slots__ = ('pass_result', 'charset', 'integer_type', 'decimal_type', 'batch_rewrite')

    def __init__(self, pass_result=None, charset=None, integer_type=None, decimal_type=None, batch_rewrite=None):
        self.pass_result = pass_result
        self.charset = charset
        self.integer_type = integer_type
        self.decimal_type = decimal_type
        self.batch_rewrite = batch_rewrite

    def over(self, base):
        """These options over base (the ones set here win)."""
        b = base if base is not None else _NO_OPTIONS
        return Options(b.pass_result if self.pass_result is None else self.pass_result,
                       b.charset if self.charset is None else self.charset,
                       b.integer_type if self.integer_type is None else self.integer_type,
                       b.decimal_type if self.decimal_type is None else self.decimal_type,
                       b.batch_rewrite if self.batch_rewrite is None else self.batch_rewrite)

    @property
    def to_client(self):
        return self.pass_result is True

    @property
    def rewrite(self):
        return self.batch_rewrite is not False

    @property
    def number(self):
        return self.decimal_type == 'NUMBER'

    def set(self, name, value):
        if name == 'passResultToClient':
            if not isinstance(value, bool):
                raise TypeError('passResultToClient must be a boolean')
            self.pass_result = value
        elif name == 'charsetName':
            v = _jstr(value)
            if v.lower() not in ('utf8mb4', 'utf8', 'utf8mb3'):
                raise TypeError("charsetName: only utf8mb4 is supported, not '%s'" % v)
            self.charset = 'utf8mb4'
        elif name == 'integerType':
            self.integer_type = self._pick(_jstr(value), _INTEGER_TYPES, 'integerType')
        elif name == 'decimalType':
            self.decimal_type = self._pick(_jstr(value), _DECIMAL_TYPES, 'decimalType')
        elif name == 'batchRewrite':
            if not isinstance(value, bool):
                raise TypeError('batchRewrite must be a boolean')
            self.batch_rewrite = value
        else:
            raise TypeError("unknown option '%s'" % name)
        return self

    @staticmethod
    def _pick(v, allowed, what):
        u = v.upper()
        if u not in allowed:
            raise TypeError("%s must be one of %s, not '%s'" % (what, _java_list(allowed), v))
        return u

    def get(self, name):
        if name == 'passResultToClient':
            return self.pass_result is True
        if name == 'charsetName':
            return 'utf8mb4' if self.charset is None else self.charset
        if name == 'integerType':
            return self.integer_type
        if name == 'decimalType':
            return self.decimal_type
        if name == 'batchRewrite':
            return self.batch_rewrite is not False
        raise TypeError("unknown option '%s'" % name)


_NO_OPTIONS = Options()


def _options(v):
    """PySql.options: a dict of options, keys in camelCase or snake_case; None for None."""
    if v is None:
        return None
    if not isinstance(v, dict) and not hasattr(type(v), 'keys'):
        raise TypeError('options must be a dict')
    o = Options()
    for k in v.keys():
        x = v[k]
        if x is None:
            continue
        o.set(_camel(str(k)), x if isinstance(x, (bool, str)) else str(x))
    return o


# ---------------------------------------------------------------------------- values

def _row_temporal(tag, year, month, day, hour, minute, second, micro, neg):
    """Temporal values of rows that Python's types cannot hold: None (a zero date, year 0; as
    PySql.temporal), a TIME as its timedelta."""
    if tag == wire.T_TIME:
        d = _dt.timedelta(hours=hour, minutes=minute, seconds=second, microseconds=micro)
        return -d if neg else d
    return None


# The codec's rules for values of rows (PySql.value): DECIMAL as Decimal (as float with the
# decimalType NUMBER), zero dates as None (_row_temporal). Routine arguments keep the worker's
# (wire.temporal raises for zero dates, as the JVM kind's arguments). The worker gives the codec
# both sets of helpers once (worker.main); each decode call says which rules apply.
_RULES_ROWS, _RULES_ROWS_NUMBER = 1, 2


def _decode_rows_as(number, body, pos, nrows, ncols, row_type):
    return _decode_rows(body, pos, nrows, ncols, row_type, _RULES_ROWS_NUMBER if number else _RULES_ROWS)


def _decode_columns_as(number, body, pos, nrows, collectors):
    return _decode_columns(body, pos, nrows, collectors, _RULES_ROWS_NUMBER if number else _RULES_ROWS)


class Argument:
    """An OUT or INOUT argument of a stored procedure called through a Routine (Oracle's
    mysql.arg(); Argument.java): val is passed in (INOUT) and replaced by the procedure's."""
    __slots__ = ('val',)

    def __init__(self, val=None):
        self.val = val

    def __repr__(self):
        return 'mysql.arg(%r)' % (self.val,)


def _param_error(values, e):
    """The error of a value Writer.param refused, as the JVM kind's: PySql.param's message for a
    mysql.arg() value; a conversion error (Values.writeParam) as a TypeError (GuestSql.guard)."""
    for v in values:
        if isinstance(v, Argument):
            return TypeError('mysql.arg() values are for OUT and INOUT parameters of procedures')
    return TypeError(str(e)) if isinstance(e, wire.ConversionError) else None


class _Msg:
    """An SQL message being built (protocol.h kSql): the header (action and statement id, filled in
    when it is sent; the text; the value count), the values encoded in place (VECTOR data is
    16-aligned relative to the message start) before anything is sent, so that a value that cannot
    be bound fails first (as PySql.param), and the trailer when it is sent (Api._send)."""
    __slots__ = ('w', 'at', 'n')

    def __init__(self, text=''):
        w = Writer()
        w.b += bytes(9)  # action, statement id
        w.str(text)
        self.at = len(w.b)
        w.b += bytes(4)  # the value count
        self.w = w
        self.n = 0

    def add(self, values):
        # The common types in C++ (_mlewire.encode_values), the others by Writer.param (_encode_one).
        try:
            self.n += _encode_values(self.w.b, values, _encode_one)
        except (TypeError, wire.ConversionError) as e:
            x = _param_error(values, e)
            if x is not None:
                raise x from None
            raise
        return self

    def size(self):
        """The bytes of the values so far."""
        return len(self.w.b) - self.at - 4


def _add_row(param, row):
    """A row of execute_many's values into a message being built."""
    try:
        for v in row:
            param(v)
    except (TypeError, wire.ConversionError) as e:
        x = _param_error(row, e)
        if x is not None:
            raise x from None
        raise


def _encode_one(v, offset):
    """One parameter by Writer.param, as the bytes it takes at offset of a message (VECTOR data is
    16-aligned relative to the message start)."""
    w = Writer()
    pad = offset % 16
    w.b += bytes(pad)
    w.param(v)
    return memoryview(w.b)[pad:]


def _msg(text, params=None):
    m = _Msg(text or '')
    return m.add(params) if params else m


# ---------------------------------------------------------------------------- rows

try:
    from collections import _tuplegetter
except ImportError:  # pragma: no cover
    _tuplegetter = None

_row_classes = {}
_ROW_METHODS = frozenset(('get_field', 'getField', 'get_length', 'getLength', 'to_array', 'toArray'))


def _row_class(labels):
    """Rows (GuestSql.GRow, exp note 41): tuples with row.label, row['label'], row[i], len() and
    iteration; get_field / get_length / to_array. A label is an attribute of the row's class (a
    column named count or index is the column, as on the JVM); the methods win over labels."""
    cls = _row_classes.get(labels)
    if cls is not None:
        return cls
    index = {}
    for i, n in enumerate(labels):
        index.setdefault(n, i)

    def __getitem__(self, key):
        if isinstance(key, str):
            i = index.get(key)
            if i is None:
                raise KeyError(key)
            return tuple.__getitem__(self, i)
        return tuple.__getitem__(self, key)

    def get_field(self, *a):
        name = _str(a, 0)
        i = index.get(name)
        if i is None:
            raise TypeError("no column '%s' in the row" % name)
        return tuple.__getitem__(self, i)

    def get_length(self):
        return len(self)

    def to_array(self):
        return list(self)

    def __repr__(self):
        return repr(list(self))

    ns = {'__slots__': (), '__getitem__': __getitem__, '__repr__': __repr__,
          'get_field': get_field, 'getField': get_field, 'get_length': get_length, 'getLength': get_length,
          'to_array': to_array, 'toArray': to_array}
    for n, i in index.items():
        if n in _ROW_METHODS or n.startswith('__') or n in ns:
            continue
        ns[n] = _tuplegetter(i, None) if _tuplegetter is not None else property(lambda self, i=i: tuple.__getitem__(self, i))
    cls = type('Row', (tuple,), ns)
    if len(_row_classes) < 1024:
        _row_classes[labels] = cls
    return cls


# ---------------------------------------------------------------------------- columns, warnings

_ZEROFILL_FLAG, _ENUM_FLAG, _SET_FLAG = 64, 256, 2048
_NUMERIC = frozenset(('TINYINT', 'SMALLINT', 'MEDIUMINT', 'INT', 'BIGINT', 'YEAR', 'BIT', 'FLOAT', 'DOUBLE',
                      'DECIMAL'))
_OWN_TYPES = frozenset(('TINYINT', 'SMALLINT', 'MEDIUMINT', 'INT', 'BIGINT', 'BIT', 'FLOAT', 'DOUBLE', 'DECIMAL',
                        'JSON', 'TIME', 'DATE', 'DATETIME', 'TIMESTAMP', 'SET', 'ENUM', 'GEOMETRY', 'VECTOR'))


class _Columns:
    """A result set's columns: the labels and type tags at once (from the C++ reply parser), Column
    objects only when a routine asks for them (most statements never do)."""
    __slots__ = ('labels', 'tags', '_block', '_list')

    def __init__(self, block, labels, tags):
        self.labels, self.tags, self._block, self._list = labels, tags, block, None

    def _all(self):
        cols = self._list
        if cols is None:
            r = Reader(self._block)
            cols = self._list = [Column(r) for _ in self.labels]
        return cols

    def __len__(self):
        return len(self.labels)

    def __iter__(self):
        return iter(self._all())

    def __getitem__(self, i):
        return self._all()[i]


_NO_COLUMNS = _Columns(b'', (), ())


@_aliases
class Column:
    """A result column's metadata (Column.java; Oracle's Column object)."""
    __slots__ = ('_name', '_table', '_ptype', '_original_name', '_schema', '_original_table', '_charset',
                 '_collation', '_flags', '_type')

    def __init__(self, r):
        self._name, self._table = r.str(), r.str()
        self._ptype = r.param_type()
        self._original_name, self._schema, self._original_table = r.str(), r.str(), r.str()
        self._charset, self._collation = r.str(), r.str()
        self._flags = r.u32()
        self._type = None

    def get_column_name(self):
        return self._original_name or self._name

    def get_column_label(self):
        return self._name

    def get_schema_name(self):
        return self._schema

    def get_table_name(self):
        return self._original_table

    def get_table_label(self):
        return self._table

    def get_type(self):
        """The X DevAPI type names that Oracle's getType() shows (STRING, INT, ...)."""
        t = self._type
        if t is None:
            sql = self._ptype.sql_name()
            if self._flags & _ENUM_FLAG:
                t = 'ENUM'
            elif self._flags & _SET_FLAG:
                t = 'SET'
            elif sql in _OWN_TYPES:
                t = sql
            elif sql == 'YEAR':
                t = 'INT'
            else:
                t = 'BYTES' if self._charset == 'binary' else 'STRING'
            self._type = t
        return t

    def get_length(self):
        return self._ptype.length

    def get_fractional_digits(self):
        return self._ptype.scale

    def get_character_set_name(self):
        return self._charset

    def get_collation_name(self):
        return self._collation

    def is_number_signed(self):
        return self._ptype.sql_name() in _NUMERIC and not self._ptype.unsigned

    def is_padded(self):
        return bool(self._flags & _ZEROFILL_FLAG) or self._ptype.sql_name() == 'CHAR'

    def __repr__(self):
        return self._name


class Warning_:
    """A warning of the statement (Oracle's Warning: level 1 NOTE, 2 WARNING, 3 ERROR; the MySQL
    error code; the message)."""
    __slots__ = ('_level', '_code', '_message')

    def __init__(self, level, code, message):
        self._level, self._code, self._message = level, code, message

    level = property(lambda self: self._level)
    code = property(lambda self: self._code)
    message = property(lambda self: self._message)

    def __repr__(self):
        return '{level: %d, code: %d, message: %r}' % (self._level, self._code, self._message)


Warning_.__name__ = Warning_.__qualname__ = 'Warning'


# ---------------------------------------------------------------------------- replies

class Reply:
    __slots__ = ('columns', 'raw', 'nrows', 'total', 'more', 'more_results', 'affected', 'insert_id', 'warnings',
                 'param_count', 'exec_us')


def _warning_list(ws):
    # mysqld's levels count from 0 (note, warning, error); Oracle's from 1.
    return [Warning_(level + 1, code, message) for level, code, message in ws] if ws else []


def _reply(t):
    """A reply tuple of _mlewire.sql (module.cc, parse_sql_result) as a Reply."""
    kind = t[0]
    rep = Reply()
    if kind == 2:
        _, block, labels, tags, rep.total, body, at, nrows, rep.more, rep.more_results, ws, rep.exec_us = t
        rep.columns = _Columns(block, labels, tags)
        rep.raw = (body, at, nrows) if nrows else None
        rep.nrows = nrows
        rep.warnings = _warning_list(ws)
        rep.affected, rep.insert_id, rep.param_count = 0, 0, 0
        return rep
    if kind == 1:
        _, rep.affected, rep.insert_id, ws, rep.more_results, rep.param_count, rep.exec_us = t
        rep.warnings = _warning_list(ws)
        rep.raw, rep.nrows, rep.columns, rep.total, rep.more = None, 0, None, 0, False
        return rep
    _, errno, state, message, row = t
    raise _sql_error(errno, state, message, row)




_COLUMN_MODE = {wire.T_VECTOR: wire.MODE_VECTOR, wire.T_I64: wire.MODE_INT64, wire.T_U64: wire.MODE_INT64,
                wire.T_F64: wire.MODE_FLOAT64, wire.T_F32: wire.MODE_FLOAT64}

_REPLACED = 'the result was replaced by a new execution of its prepared statement'


@_aliases
class SqlResult:
    """The results of a statement (SqlResult.java): one or more result sets or OKs, navigated with
    next_result(). Rows arrive in chunks; a chunk's rows stay in the reply (raw: body, position,
    count) until they are needed, then decode_rows makes the rows (or decode_columns the columns).

    direct: a statement run from text (not a prepared one). When mysqld buffered its whole result
    (total > 0), the rest is fetched when the routine reads it, or drained into the worker before
    the routine runs another statement (Api._defer, exp note 44 step 3)."""
    __slots__ = ('_api', '_id', '_owned', '_direct', '_options', '_number', '_has_data', '_columns', '_row_type',
                 '_rows', '_at', '_raw', '_more', '_more_results', '_affected', '_insert_id', '_exec_us',
                 '_warnings', '_replaced', '_total', '__weakref__')

    def __init__(self, api, sid, owned, direct, options, reply):
        self._api = api
        self._id = sid
        self._owned = owned
        self._direct = direct
        self._options = options
        self._number = options.number
        self._replaced = False
        self._raw = None
        self._load(reply)

    def _load(self, reply):
        api = self._api
        api._drained(self)  # a former result set's rows are skipped
        self._has_data = reply.columns is not None
        self._columns = reply.columns if self._has_data else _NO_COLUMNS
        self._row_type = None
        self._rows = []
        self._at = 0
        self._set_raw(reply)
        self._more = reply.more
        self._more_results = reply.more_results
        self._affected = reply.affected
        self._insert_id = reply.insert_id
        self._exec_us = reply.exec_us
        self._warnings = reply.warnings
        self._total = reply.total
        if self._direct and self._more and self._total > 0:
            api._defer(self)

    def _set_raw(self, reply):
        raw = reply.raw
        if raw is not None:
            self._raw = raw
            self._api._raw_results.add(self)
        elif self._raw is not None:
            self._raw = None
            self._api._raw_results.discard(self)

    def _fetched(self, reply):
        """A FETCH reply arrived."""
        self._more = reply.more
        self._more_results = reply.more_results
        if reply.warnings:
            self._warnings = reply.warnings
        if not self._more:
            self._api._drained(self)

    def _type(self):
        t = self._row_type
        if t is None:
            t = self._row_type = _row_class(self._columns.labels)
        return t

    def _materialize(self):
        """The raw chunk as rows (after the rows not read yet)."""
        raw = self._raw
        if raw is None:
            return
        self._raw = None
        self._api._raw_results.discard(self)
        body, pos, nrows = raw
        rows, _ = _decode_rows_as(self._number, body, pos, nrows, len(self._columns), self._type())
        at = self._at
        self._rows = self._rows[at:] + rows if at < len(self._rows) else rows
        self._at = 0

    def _fetch(self):
        """The next chunk of the current result set (its rows stay raw)."""
        reply = self._api._request(FETCH, self._id, None, None)
        self._fetched(reply)
        return reply

    def _next_chunk(self):
        """Rows to read: the raw chunk or the next one; False at the end of the result set."""
        if self._raw is not None:
            self._materialize()
            return True
        if not self._more:
            return False
        reply = self._fetch()
        if self._at >= len(self._rows):
            self._rows, self._at = [], 0
        self._set_raw(reply)
        return True

    def _drain(self):
        """Another statement is about to run (Api._defer): the unread rows into the worker."""
        if not self._more or self._replaced:
            return
        self._materialize()
        rows = self._rows[self._at:]
        self._at = 0
        while self._more:
            reply = self._fetch()
            if reply.raw is not None:
                body, pos, nrows = reply.raw
                more, _ = _decode_rows_as(self._number, body, pos, nrows, len(self._columns), self._type())
                rows += more
        self._rows = rows

    def _replace(self):
        """A new execution of the prepared statement replaced these results."""
        if self._more:
            self._api._drained(self)
        self._replaced = True
        self._rows, self._at = [], 0
        self._set_raw_none()
        self._more = False
        self._more_results = False

    def _set_raw_none(self):
        if self._raw is not None:
            self._raw = None
            self._api._raw_results.discard(self)

    def _live(self):
        if self._replaced:
            raise RuntimeError(_REPLACED)

    # API ----------------------------------------------------------------------------------------

    def __iter__(self):
        """The remaining rows of the current result set, fetched as the iteration goes."""
        while True:
            self._live()
            rows, at = self._rows, self._at
            n = len(rows)
            while at < n:
                at += 1
                self._at = at
                yield rows[at - 1]
                if self._at != at or self._rows is not rows:
                    break  # the loop's body read or drained this result
            else:
                if not self._next_chunk():
                    return

    def fetch_one(self):
        """The next row of the current result set, or None at its end (or for an OK)."""
        self._live()
        while self._at >= len(self._rows):
            if not self._next_chunk():
                return None
        r = self._rows[self._at]
        self._at += 1
        return r

    def fetch_all(self):
        """The remaining rows of the current result set."""
        self._live()
        self._materialize()
        rows = self._rows[self._at:]
        self._rows, self._at = [], 0
        while self._more:
            reply = self._fetch()
            if reply.raw is not None:
                body, pos, nrows = reply.raw
                more, _ = _decode_rows_as(self._number, body, pos, nrows, len(self._columns), self._type())
                rows += more
        return rows

    def has_data(self):
        """Whether the current result is a result set (rows and columns), not an OK."""
        return self._has_data

    def next_result(self):
        """Moves to the next result (the rows left of the current one are skipped); True if it is a
        result set, False when there are no more results."""
        self._live()
        if not self._more_results and not self._more:
            self._has_data = False
            self._rows, self._at = [], 0  # the rows left are skipped here too
            self._set_raw_none()
            return False
        self._set_raw_none()
        self._load(self._api._request(NEXT_RESULT, self._id, None, None))
        return self._has_data

    def get_columns(self):
        return list(self._columns)

    def get_column_names(self):
        return list(self._columns.labels)

    def get_column_count(self):
        return len(self._columns)

    def get_affected_items_count(self):
        return self._affected

    def get_auto_increment_value(self):
        return self._insert_id

    def get_warnings(self):
        return list(self._warnings)

    def get_warnings_count(self):
        return len(self._warnings)

    def get_execution_time(self):
        """The statement's execution time in mysqld, e.g. "0.0012 sec"."""
        return '%.4f sec' % (self._exec_us / 1e6)

    def close(self):
        """Ends the results; a statement run from text is closed in mysqld."""
        if self._more:
            self._api._drained(self)
        if self._owned and (self._more or self._more_results) and not self._replaced:
            self._api._request(CLOSE, self._id, None, None)
        self._rows, self._at = [], 0
        self._set_raw_none()
        self._more = False
        self._more_results = False
        self._has_data = False

    def fetch_columns(self, *names):
        """PySql.columns (Phase 7, S-14): the remaining rows column-wise, a dict by column label:
        VECTOR columns as one n x d float32 numpy matrix, integer and floating-point columns as
        int64 / float64 arrays, others (or columns with NULLs) as lists. The rows not read yet go
        straight from the messages into the arrays."""
        if not names or (len(names) == 1 and names[0] is None):
            want = list(self._columns.labels)
        else:
            want = []
            for v in names:
                if _is_array(v):
                    want.extend(v)
                elif isinstance(v, str):
                    want.append(v)
                else:
                    raise TypeError('a column name must be a string, not %s' % type(v).__name__)
        labels = list(self._columns.labels)
        index = {}
        for label in want:
            if label not in index:
                try:
                    index[label] = labels.index(label)
                except ValueError:
                    raise TypeError("no column '%s' in the result" % label) from None
        self._live()
        collectors = [None] * len(labels)
        for i in index.values():
            if collectors[i] is None:
                collectors[i] = wire.Collector(_COLUMN_MODE.get(self._columns.tags[i], wire.MODE_LIST),
                                               self._total)
        for row in self._rows[self._at:]:  # rows decoded before (fetch_one ...)
            for i, c in enumerate(collectors):
                if c is not None:
                    c.add(row[i])
        self._rows, self._at = [], 0
        raw = self._raw
        if raw is not None:
            self._set_raw_none()
            _decode_columns_as(self._number, raw[0], raw[1], raw[2], collectors)
        while self._more:
            reply = self._fetch()
            if reply.raw is not None:
                _decode_columns_as(self._number, reply.raw[0], reply.raw[1], reply.raw[2], collectors)
        return {label: collectors[i].result() for label, i in index.items()}


# ---------------------------------------------------------------------------- statements

@_aliases
class SqlExecute:
    """A simple statement (Oracle's SqlExecute), from session.sql()."""
    __slots__ = ('_session', '_sql', '_options')

    def __init__(self, session, sql, options):
        self._session, self._sql, self._options = session, sql, options

    def execute(self):
        """Runs the statement; None with passResultToClient."""
        return self._session._run(self._sql, None, self._options)

    def get_option(self, *a):
        return self._options.get(_str(a, 0))


# PreparedStatement.java: execute_many's limits (exp notes 51, 52).
BATCH_MIN_ROWS = 256      # smaller batches run row by row (preparing the multi-row form costs more)
REWRITE_MIN_ROWS = 16     # chunks of a rewritten batch smaller than this (its last rows) run row by row
REWRITE_MAX_ROWS = 1000
# Errors a row's values cause: only after these a failed chunk is replayed row by row to find the
# row (independent review C24).
REPLAY = frozenset((1022, 1048, 1062, 1169, 1216, 1264, 1265, 1292, 1364, 1365, 1366, 1367, 1406, 1411, 1452,
                    1526, 1586, 1690, 3140, 3141, 3156, 3819, 6136, 6137, 6138))


def _bind_error(message, row=None):
    return _sql_error(1210, 'HY000', message, row)


@_aliases
class PreparedStatement:
    """A prepared statement (PreparedStatement.java), from session.prepare(). Values given to bind()
    bind the markers in order at the next execute(); the first execution needs all of them, later
    ones keep the values of markers not bound again."""
    __slots__ = ('_session', '_api', '_id', '_sql', '_options', '_count', '_pending', '_executed',
                 '_deallocated', '_last', '_form_checked', '_form', '_chunks')

    def __init__(self, session, sid, sql, options, count):
        self._session = session
        self._api = session._api
        self._id = sid
        self._sql = sql
        self._options = options
        self._count = count
        self._pending = []
        self._executed = False
        self._deallocated = False
        self._last = None
        self._form_checked = False
        self._form = None
        self._chunks = _collections.OrderedDict()  # multi-row forms by rows per statement (LRU, 4)

    def _open(self):
        if self._deallocated:
            raise RuntimeError('the prepared statement was deallocated')

    def _drop_last(self):
        if self._last is not None:
            self._last._replace()
            self._last = None

    def bind(self, *values):
        self._open()
        n = len(self._pending) + len(values)
        if n > self._count:
            raise _bind_error('Incorrect arguments to bind: %d values for %d parameter markers' % (n, self._count))
        self._pending.extend(values)
        return self

    def execute(self):
        """Executes with the values bound since the last execution; None with passResultToClient."""
        self._open()
        if not self._executed and len(self._pending) < self._count:
            raise _bind_error('Incorrect arguments to EXECUTE: %d of %d parameter markers bound'
                              % (len(self._pending), self._count))
        self._drop_last()
        msg = _msg(None, self._pending)
        self._pending.clear()
        reply = self._api._send(EXECUTE_PREPARED, self._id, msg)
        self._executed = True
        if self._options.to_client:
            return None
        self._last = SqlResult(self._api, self._id, False, False, self._options, reply)
        return self._last

    def execute_many(self, *a):
        """Executes the statement once per row, each row giving every parameter marker a value (exp
        notes 51, 52), in as few messages as fit the mailbox. For statements that return no rows.
        Stops at the first failing row (SqlError.row says which); the rows before it stay written.
        Returns the sum of the affected rows. An INSERT or REPLACE of one row of values into an
        InnoDB table, with at least 256 rows, runs as multi-row statements of up to 1,000 rows
        (option batch_rewrite, True by default)."""
        return self._execute_many(_rows(_arg(a, 0)))

    executemany = execute_many

    def _execute_many(self, rows):
        self._open()
        # The same answer for every size of batch, before anything is written.
        if self._options.to_client:
            raise _bind_error('Incorrect arguments to executeMany: it cannot send results to the client '
                              '(passResultToClient)')
        if self._count == 0 and rows:
            raise _bind_error('Incorrect arguments to executeMany: the statement has no parameter markers')
        if len(rows) >= BATCH_MIN_ROWS and self._options.rewrite:
            if not self._form_checked:
                self._form_checked = True
                self._form = sql_batch.parse(self._sql, self._count)
            f = self._form
            if f is not None and self._session._innodb(f.schema, f.table, False):
                return self._execute_rewritten(rows, f)
        return self._result(*self._execute_rows(rows, 0))

    def _result(self, affected, exec_us, warnings):
        r = Reply()
        r.columns, r.raw, r.nrows, r.total, r.more, r.more_results = None, None, 0, 0, False, False
        r.affected, r.insert_id, r.warnings, r.param_count, r.exec_us = affected, 0, warnings, 0, exec_us
        return SqlResult(self._api, self._id, False, False, self._options, r)

    def _execute_rewritten(self, rows, f):
        """The rows in chunks of the multi-row form (exp note 52): at most 1,000 rows, 65,535
        parameters and one mailbox arena of parameters per statement. A chunk that fails because of a
        row's values (InnoDB rolls the whole statement back) is replayed row by row, which stops at
        the failing row with its index. It is not replayed when the failed statement may have left
        something behind or the error is not a row's (Session._replayable, REPLAY)."""
        self._drop_last()
        self._pending.clear()
        pc = self._count
        max_rows = min(REWRITE_MAX_ROWS, MAX_VALUES // pc)
        # The chunks run on statements of their own: this one keeps the values bound before the
        # batch, which a later partial bind must not inherit (independent review C11).
        self._executed = False
        budget = self._api.batch_bytes()
        affected = exec_us = 0
        warnings = []
        n = len(rows)
        i = 0
        while i < n:
            start, k = i, 0
            msg = _Msg()  # the chunk's statement is chosen once its rows are known (the header's size is fixed)
            b = msg.w.b
            bad = None  # a malformed row: raised once the rows before it are written
            while i < n and k < max_rows:
                row = rows[i]
                mark = len(b)
                _add_row(msg.w.param, row)
                if len(row) != pc:
                    del b[mark:]
                    bad = _bind_error('Incorrect arguments to executeMany: row %d has %d values for %d parameter '
                                      'markers' % (i, len(row), pc), i)
                    break
                if k > 0 and msg.size() > budget:
                    del b[mark:]
                    break
                k += 1
                i += 1
            msg.n = k * pc
            if k < REWRITE_MIN_ROWS:  # a short last chunk: row by row
                if k > 0:
                    a, e, warnings = self._execute_rows(rows[start:start + k], start)
                    affected += a
                    exec_us += e
                if bad is not None:
                    raise bad
                continue
            try:
                reply = self._chunk(f, k)._execute_mapped(msg)
                affected += reply.affected
                exec_us += reply.exec_us
                warnings = reply.warnings
            except SqlError as e:
                # Replaying is only right if the failed statement left nothing behind (looked up
                # now: the table may have changed since the batch was found eligible).
                why = ("the error is not one of a row's values" if e.code not in REPLAY
                       else self._session._replayable(f.schema, f.table))
                if why is not None:
                    raise _sql_error(e.code, e.sql_state, '%s (executeMany: rows %d to %d ran as one statement and '
                                     'are not run again row by row, so the failing row is not known: %s)'
                                     % (e.message, start, start + k - 1, why), start) from None
                a, x, warnings = self._execute_rows(rows[start:start + k], start)  # raises at the failing row
                affected += a
                exec_us += x
            if bad is not None:
                raise bad
        return self._result(affected, exec_us, warnings)

    def _chunk(self, f, k):
        """The multi-row form with k rows, prepared once (the 4 used last are kept)."""
        p = self._chunks.get(k)
        if p is not None:
            self._chunks.move_to_end(k)
            return p
        p = self._session._prepare(f.statement(k), Options(pass_result=False).over(self._options))
        self._chunks[k] = p
        if len(self._chunks) > 4:
            _, old = self._chunks.popitem(last=False)
            old.deallocate()
        return p

    def _execute_mapped(self, msg):
        """Executes with values already encoded (all markers); the reply only."""
        self._open()
        self._drop_last()
        self._pending.clear()
        reply = self._api._send(EXECUTE_PREPARED, self._id, msg)
        self._executed = True
        return reply

    def _execute_rows(self, rows, offset):
        """execute_many row by row on mysqld's side (EXECUTE_MANY); row indexes in errors count from
        offset. (affected, execution us, the last message's warnings)."""
        self._open()
        self._drop_last()
        self._pending.clear()
        pc = self._count
        api = self._api
        affected = exec_us = 0
        done = offset
        warnings = []
        budget = api.batch_bytes()
        msg = _Msg()
        in_batch = 0
        bad = None
        n = len(rows)
        i = 0
        while i <= n and bad is None:
            row = None
            b = msg.w.b
            mark = len(b)
            if i < n:
                row = rows[i]
                _add_row(msg.w.param, row)
                if len(row) != pc:
                    # The rows before it stay written: those not sent yet go first.
                    del b[mark:]
                    bad = _bind_error('Incorrect arguments to executeMany: row %d has %d values for %d parameter '
                                      'markers' % (offset + i, len(row), pc), offset + i)
                    row = None
            # A message carries at most MAX_VALUES values (the host's limit, independent review C10).
            if in_batch > 0 and (row is None or msg.size() > budget or (in_batch + 1) * pc > MAX_VALUES):
                del b[mark:]  # the new row goes into the next message (its VECTORs aligned there)
                msg.n = in_batch * pc
                try:
                    reply = api._send(EXECUTE_MANY, self._id, msg, width=pc)
                except SqlError as e:
                    if e.row is None:
                        raise
                    raise _sql_error(e.code, e.sql_state, e.message, done + e.row) from None
                affected += reply.affected
                exec_us += reply.exec_us
                warnings = reply.warnings
                done += in_batch
                in_batch = 0
                msg = _Msg()
                if row is not None:
                    _add_row(msg.w.param, row)
            if row is not None:
                in_batch += 1
            i += 1
        self._executed = self._executed or done > offset  # rows ran on this statement: its last row stays bound
        if bad is not None:
            raise bad
        return affected, exec_us, warnings

    def deallocate(self):
        """Closes the statement in mysqld."""
        if self._deallocated:
            return
        self._deallocated = True
        for p in self._chunks.values():
            p.deallocate()
        self._chunks.clear()
        if self._last is not None:
            self._last._replace()
        self._api._request(CLOSE, self._id, None, None)

    def get_parameter_count(self):
        return self._count

    def get_option(self, *a):
        return self._options.get(_str(a, 0))


# ---------------------------------------------------------------------------- schemas, routines

def quote_name(name):
    """name quoted as an identifier: `name`, with backticks doubled."""
    return '`' + name.replace('`', '``') + '`'


@_aliases
class Schema:
    """A database (Schema.java; Oracle's Schema object)."""
    __slots__ = ('_session', '_name')

    def __init__(self, session, name):
        self._session, self._name = session, name

    def get_name(self):
        return self._name

    def get_session(self):
        return self._session

    def exists_in_database(self):
        return self._session._schema_exists(self._name)

    def get_table(self, *a):
        """The table or view name (whether it exists is not checked)."""
        return Table(self._session, self, _str(a, 0))

    def get_tables(self):
        """The schema's tables and views, by name."""
        return [Table(self._session, self, t) for t in self._session._column(
            'SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = ? ORDER BY TABLE_NAME',
            self._name)]

    def get_function(self, *a):
        """A stored function of this schema, to call."""
        return self._session._routine(self._name, _str(a, 0), False)

    def get_procedure(self, *a):
        """A stored procedure of this schema, to call; OUT and INOUT arguments are mysql.arg()s."""
        return self._session._routine(self._name, _str(a, 0), True)

    def __repr__(self):
        return self._name


@_aliases
class Table:
    """A table or view (Table.java; Oracle's Table object)."""
    __slots__ = ('_session', '_schema', '_name')

    def __init__(self, session, schema, name):
        self._session, self._schema, self._name = session, schema, name

    def get_name(self):
        return self._name

    def get_schema(self):
        return self._schema

    def get_session(self):
        return self._session

    def exists_in_database(self):
        return bool(self._session._column('SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = ? '
                                          'AND TABLE_NAME = ?', self._schema._name, self._name))

    def is_view(self):
        return bool(self._session._column("SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = ? "
                                          "AND TABLE_NAME = ? AND TABLE_TYPE = 'VIEW'", self._schema._name,
                                          self._name))

    def count(self):
        """The number of rows (an error if the table does not exist)."""
        return int(self._session._column('SELECT COUNT(*) FROM ' + quote_name(self._schema._name) + '.'
                                         + quote_name(self._name))[0])

    def __repr__(self):
        return self._schema._name + '.' + self._name


class Routine:
    """A stored function or procedure to call (Routine.java; Oracle's stored routine API). A function
    call is SELECT f(?, ...) and returns the value; a procedure call is CALL p(...), its result sets
    are dropped, and its OUT and INOUT arguments are mysql.arg()s (passed through the user variables
    @_mle_o1, ...)."""
    __slots__ = ('_session', '_schema', '_name', '_procedure', '_closed')

    def __init__(self, session, schema, name, procedure):
        self._session, self._schema, self._name, self._procedure = session, schema, name, procedure
        self._closed = False

    def _check(self):
        if self._closed:
            raise RuntimeError('%s %s was closed' % ('procedure' if self._procedure else 'function', self._name))

    def __call__(self, *args):
        self._check()
        s = self._session
        qualified = quote_name(self._schema) + '.' + quote_name(self._name)
        if not self._procedure:
            r = s._run('SELECT %s(%s)' % (qualified, ', '.join('?' * len(args))), list(args), s._internal())
            v = r.fetch_one()[0]
            r.close()
            return v
        marks, params, outs, sets, gets, initial = [], [], [], [], [], []
        for a in args:
            if isinstance(a, Argument):
                var = '@_mle_o%d' % (len(outs) + 1)
                marks.append(var)
                sets.append(var + ' = ?')
                gets.append(var)
                initial.append(a.val)
                outs.append(a)
            else:
                marks.append('?')
                params.append(a)
        # The procedure reads INOUT values when the CALL starts and writes OUT values when it ends,
        # so nested calls using the same variables meanwhile do not matter.
        if outs:
            s._exec('SET ' + ', '.join(sets), initial)
        r = s._run('CALL %s(%s)' % (qualified, ', '.join(marks)), params, s._effective(None))
        if r is not None:
            r.close()
        if outs:
            row = s._run('SELECT ' + ', '.join(gets), None, s._internal()).fetch_one()
            for i, a in enumerate(outs):
                a.val = row[i]
        return None

    def close(self):
        self._check()
        self._closed = True


# ---------------------------------------------------------------------------- mysql.*
# Functions.java: Oracle's mysql.rand/sleep/uuid/isUUID. They run in the worker, so they also work
# in stored functions (where SQL is not allowed).

def _rand():
    """Like RAND(): 0 <= v < 1."""
    return _random.random()


def _sleep(seconds):
    """Like SLEEP(): 0. A KILL meanwhile interrupts it as any Python code (KeyboardInterrupt; the call
    reports the KILL): the JVM kind returns 1 and stops the routine at its next safepoint, which a
    Python routine would not reach if it swallowed the interrupt here (while True: pass)."""
    seconds = float(seconds)
    if not seconds > 0:
        return 0
    end = _time.monotonic() + min(seconds, 1e12)
    while True:
        left = end - _time.monotonic()
        if left <= 0:
            return 0
        _time.sleep(min(left, 0.01))


_GREGORIAN_OFFSET = 0x01B21DD213814000  # 100 ns units, 1582 to 1970
_NODE = (_secrets.randbits(48) & 0xFFFFFFFFFFFF) | 0x010000000000  # multicast bit
_CLOCK_SEQ = _secrets.randbits(14)
_uuid_last = 0
_uuid_lock = _threading.Lock()


def _uuid():
    """Like UUID(): a version 1 UUID (time based; a random node id)."""
    global _uuid_last
    with _uuid_lock:
        t = int(_time.time() * 1000) * 10000 + _GREGORIAN_OFFSET
        if t <= _uuid_last:
            t = _uuid_last + 1
        _uuid_last = t
    hi = ((t & 0xFFFFFFFF) << 32) | (((t >> 32) & 0xFFFF) << 16) | 0x1000 | ((t >> 48) & 0x0FFF)
    lo = ((0x8000 | _CLOCK_SEQ) << 48) | _NODE
    h = '%016x%016x' % (hi, lo)
    return '%s-%s-%s-%s-%s' % (h[:8], h[8:12], h[12:16], h[16:20], h[20:])


_UUID = _re.compile(r'[0-9a-fA-F]{32}|[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}'
                    r'|\{[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\}')


def _is_uuid(v):
    """Like IS_UUID(): 1 or 0, None for None."""
    if v is None:
        return None
    return 1 if _UUID.fullmatch(str(v)) else 0


_TYPES = ('TINYINT', 'SMALLINT', 'MEDIUMINT', 'INT', 'BIGINT', 'BIT', 'FLOAT', 'DOUBLE', 'DECIMAL', 'JSON',
          'STRING', 'BYTES', 'TIME', 'DATE', 'DATETIME', 'TIMESTAMP', 'SET', 'ENUM', 'GEOMETRY', 'VECTOR')


def _mysql():
    ns = _types.SimpleNamespace
    return ns(rand=_rand, sleep=_sleep, uuid=_uuid, is_uuid=_is_uuid, isUUID=_is_uuid,
              arg=lambda *v: Argument(v[0] if v else None),
              IntegerType=ns(BIGINT='BIGINT', STRING='STRING', UNSAFE_BIGINT='UNSAFE_BIGINT',
                             UNSAFE_STRING='UNSAFE_STRING'),
              DecimalType=ns(STRING='STRING', NUMBER='NUMBER'),
              Type=ns(**{t: t for t in _TYPES}),
              SQLError=SqlError, SqlError=SqlError)


# ---------------------------------------------------------------------------- session

_SESSION_SLOTS = ('_api', '_savepoints', '_innodb_cache', '_call_options')


@_aliases
class Session:
    """SQL from a routine (Session.java and GuestSql.GSession; Oracle's session global): statements
    run in mysqld in the calling session and transaction, with the routine's privileges, and only
    in stored procedures (functions and triggers: error 6120). Other members are user variables:
    session.x reads @x, session.x = v sets it."""
    __slots__ = _SESSION_SLOTS

    def __init__(self, api):
        object.__setattr__(self, '_api', api)
        object.__setattr__(self, '_savepoints', 0)
        object.__setattr__(self, '_innodb_cache', {})
        # The options of the routine calls in progress, by nesting depth: (call scope, options). A
        # nested call's set_options must not replace its caller's, and a call must not see those of
        # an earlier one (independent review C15).
        object.__setattr__(self, '_call_options', {})

    # -- user variables as members

    def __getattr__(self, name):
        if name.startswith('__'):
            raise AttributeError(name)  # Python looks such names up itself
        return self._get_variable(name)

    def __setattr__(self, name, value):
        if name in _SESSION_METHODS or name.startswith('__'):
            raise TypeError('cannot set ' + name)
        self._set_variable(name, value)

    def get_variable(self, *a):
        """The user variable @name (None if not set). (CPython worker; the JVM kind has session.name)"""
        return self._get_variable(_str(a, 0))

    def set_variable(self, *a):
        """Sets the user variable @name. (CPython worker; the JVM kind has session.name = value)"""
        self._set_variable(_str(a, 0), _arg(a, 1))

    def _get_variable(self, name):
        r = self._run('SELECT @' + quote_name(name), None, self._internal())
        return r.fetch_one()[0]

    def _set_variable(self, name, value):
        api = self._api
        msg = _msg('SET @' + quote_name(name) + ' = ?', [value])
        api._send(EXECUTE, api.new_id(), msg)

    # -- options

    def _call(self):
        d = self._api.d
        e = self._call_options.get(d.depth)
        return e[1] if e is not None and e[0] == d.scope else None

    def _effective(self, o):
        """The options of a statement: o over this call's session options over the defaults."""
        base = self._call()
        if base is None:
            base = Options(decimal_type=_DECIMAL_TYPES[self._api.decimal_type]
                           if self._api.decimal_type < len(_DECIMAL_TYPES) else None)
        return base.over(None) if o is None else o.over(base)

    def _internal(self):
        """Options of a statement the API runs for itself (a user variable, a function call, OUT
        values): its rows are for the API, never for the client."""
        return self._effective(Options(pass_result=False))

    def set_options(self, *a):
        """Sets defaults for the statements of the rest of this routine call (unset ones are kept)."""
        o = _options(_arg(a, 0))
        if o is not None:
            d = self._api.d
            self._call_options[d.depth] = (d.scope, o.over(self._call()))

    def get_option(self, *a):
        return self._effective(None).get(_str(a, 0))

    # -- statements

    def sql(self, *a):
        """A simple statement (no parameter markers), run with execute()."""
        return SqlExecute(self, _str(a, 0), self._effective(_options(_arg(a, 1))))

    def run_sql(self, *a):
        """run_sql(query[, parameters][, options]): runs a statement, with parameters as a one-shot
        prepared statement. None with pass_result_to_client (the results went to the client)."""
        query = _str(a, 0)
        second = _arg(a, 1)
        params = second is not None and _is_array(second)
        # run_sql(query, None, options): the options are the third argument then too.
        o = _options(_arg(a, 2) if params or (second is None and len(a) > 2) else second)
        return self._run(query, _elements(second) if params else None, self._effective(o))

    def _run(self, sql, params, o):
        api = self._api
        msg = _msg(sql, params)
        sid = api.new_id()
        reply = api._send(EXECUTE, sid, msg, TO_CLIENT if o.to_client else 0)
        return None if o.to_client else SqlResult(api, sid, True, True, o, reply)

    def _exec(self, sql, params=None):
        """Runs a statement for its effect; its results are dropped."""
        api = self._api
        sid = api.new_id()
        reply = api._send(EXECUTE, sid, _msg(sql, params))
        if reply.more or reply.more_results:
            api._request(CLOSE, sid, None, None)

    def _column(self, sql, *params):
        """The first column of each row of an internal query (catalog lookups)."""
        api = self._api
        sid = api.new_id()
        reply = api._send(EXECUTE, sid, _msg(sql, params))
        values = []
        while True:
            if reply.raw is not None:
                body, pos, nrows = reply.raw
                rows, _ = _decode_rows(body, pos, nrows, len(reply.columns), None)
                values.extend(r[0] for r in rows)
            if not reply.more:
                break
            reply = api._request(FETCH, sid, None, None)
        if reply.more_results:
            api._request(CLOSE, sid, None, None)
        return values

    def prepare(self, *a):
        return self._prepare(_str(a, 0), _options(_arg(a, 1)))

    def _prepare(self, sql, options):
        o = self._effective(options)
        api = self._api
        sid = api.new_id()
        reply = api._send(PREPARE, sid, _msg(sql), TO_CLIENT if o.to_client else 0)
        return PreparedStatement(self, sid, sql, o, reply.param_count)

    def execute_many(self, *a):
        """Prepares sql, executes it once per row (PreparedStatement.execute_many) and deallocates it
        (exp note 51); options: batch_rewrite False runs the rows one statement each."""
        sql = _str(a, 0)
        rows = _rows(_arg(a, 1))
        options = _options(_arg(a, 2))
        p = self._prepare(sql, Options(pass_result=False).over(options))
        try:
            return p._execute_many(rows)
        finally:
            p.deallocate()

    executemany = execute_many

    # -- execute_many's engine lookups (exp note 52)

    def _innodb(self, schema, table, fresh):
        """Whether schema.table (schema None: the current database, the running routine's schema)
        is an InnoDB base table. Looked up once per worker and table; fresh: look up again."""
        s = schema if schema is not None else self._api.routine_schema()
        key = None if s is None else (s, table)
        b = None if key is None or fresh else self._innodb_cache.get(key)
        if b is None:
            e = self._column("SELECT ENGINE FROM information_schema.TABLES WHERE TABLE_SCHEMA = COALESCE(?, "
                             "DATABASE()) AND TABLE_NAME = ? AND TABLE_TYPE = 'BASE TABLE'", schema, table)
            b = len(e) == 1 and str(e[0]).lower() == 'innodb'
            if key is not None:
                self._innodb_cache[key] = b
        return b

    def _replayable(self, schema, table):
        """Why a failed multi-row INSERT into schema.table must not be run again row by row, or None
        when it may (Session.replayable: an InnoDB base table, not hidden by a temporary table, no
        trigger; independent review C5, C62)."""
        name = ('' if schema is None else quote_name(schema) + '.') + quote_name(table)
        api = self._api
        try:
            sid = api.new_id()
            reply = api._send(EXECUTE, sid, _msg('SHOW CREATE TABLE ' + name))
            rows = []
            if reply.raw is not None:
                body, pos, nrows = reply.raw
                rows, _ = _decode_rows(body, pos, nrows, len(reply.columns), None)
            if reply.more or reply.more_results:
                api._request(CLOSE, sid, None, None)
            if not rows or str(rows[0][1]).startswith('CREATE TEMPORARY'):
                return 'the table is a temporary one'
        except SqlError:
            return "the table's definition cannot be read"
        if not self._innodb(schema, table, True):
            return 'the table is not an InnoDB table'
        n = self._column('SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE EVENT_OBJECT_SCHEMA = '
                         'COALESCE(?, DATABASE()) AND EVENT_OBJECT_TABLE = ?', schema, table)
        if len(n) != 1 or int(n[0]) != 0:
            return 'the table has triggers'
        return None

    # -- schemas

    def quote_name(self, *a):
        return quote_name(_str(a, 0))

    def _schema_exists(self, name):
        return bool(self._column('SELECT 1 FROM information_schema.SCHEMATA WHERE SCHEMA_NAME = ?', name))

    def get_schema(self, *a):
        """The schema; error 1049 if it does not exist."""
        name = _str(a, 0)
        if not self._schema_exists(name):
            raise _sql_error(1049, '42000', "Unknown database '%s'" % name)
        return Schema(self, name)

    def get_schemas(self):
        return [Schema(self, n) for n in self._column('SELECT SCHEMA_NAME FROM information_schema.SCHEMATA '
                                                      'ORDER BY SCHEMA_NAME')]

    def get_default_schema(self):
        """The current database (in a routine: the routine's), or None."""
        n = self._column('SELECT DATABASE()')[0]
        return None if n is None else Schema(self, n)

    def create_schema(self, *a):
        name = _str(a, 0)
        self._exec('CREATE DATABASE ' + quote_name(name))
        return Schema(self, name)

    def drop_schema(self, *a):
        self._exec('DROP DATABASE ' + quote_name(_str(a, 0)))

    def _routine(self, schema, name, procedure):
        """A stored function or procedure of schema; error 1305 if it does not exist."""
        kind = 'PROCEDURE' if procedure else 'FUNCTION'
        if not self._column('SELECT 1 FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = ? AND ROUTINE_NAME = ? '
                            'AND ROUTINE_TYPE = ?', schema, name, kind):
            raise _sql_error(1305, '42000', '%s %s.%s does not exist' % (kind, schema, name))
        return Routine(self, schema, name, procedure)

    # -- transactions

    def start_transaction(self):
        self._exec('START TRANSACTION')

    def commit(self):
        self._exec('COMMIT')

    def rollback(self):
        self._exec('ROLLBACK')

    def set_savepoint(self, *a):
        """A savepoint, by default with a generated name (TxSp1, TxSp2, ...); returns the name."""
        if _arg(a, 0) is None:
            n = self._savepoints + 1
            object.__setattr__(self, '_savepoints', n)
            name = 'TxSp%d' % n
        else:
            name = _str(a, 0)
        self._exec('SAVEPOINT ' + quote_name(_savepoint_name(name)))
        return name

    def rollback_to(self, *a):
        self._exec('ROLLBACK TO SAVEPOINT ' + quote_name(_savepoint_name('' if _arg(a, 0) is None else _str(a, 0))))

    def release_savepoint(self, *a):
        self._exec('RELEASE SAVEPOINT ' + quote_name(_savepoint_name('' if _arg(a, 0) is None else _str(a, 0))))

    def autocommit(self, *a):
        """autocommit(): the autocommit system variable (1 or 0); autocommit(on) sets it."""
        if not a:
            return int(self._column('SELECT @@autocommit')[0])
        self._exec('SET autocommit = 1' if _truthy(a[0]) else 'SET autocommit = 0')
        return None


def _savepoint_name(name):
    if not name:
        raise TypeError('the savepoint name must not be empty')
    return name


_SESSION_METHODS = frozenset(n for n in vars(Session) if not n.startswith('_'))


# ---------------------------------------------------------------------------- the api

def zone_of(text):
    """Options.parseZone: an IANA name or an offset (+hh:mm); None if unknown."""
    t = (text or '').strip()
    if not t or t.upper() in ('UTC', 'Z'):
        return _dt.timezone.utc
    m = _re.fullmatch(r'([+-])(\d{1,2})(?::?(\d{2}))?(?::?(\d{2}))?', t)
    if m:
        d = _dt.timedelta(hours=int(m.group(2)), minutes=int(m.group(3) or 0), seconds=int(m.group(4) or 0))
        return _dt.timezone(-d if m.group(1) == '-' else d)
    try:
        import zoneinfo
        return zoneinfo.ZoneInfo(t)
    except Exception:
        return None


class Api:
    def __init__(self, dispatcher):
        self.d = dispatcher
        self.next_id = 0
        # A statement run from text whose rows are not all read yet (exp note 44 step 3): mysqld
        # buffers the whole result, so before any other statement runs its rows are drained into
        # the worker (mysqld holds at most one buffered result, 12 §11). One at a time.
        self._pending = None
        self._pending_seq = 0
        self._pending_depth = 0
        # Results whose last chunk is still raw in the arena (SqlResult._raw).
        self._raw_results = _weakref.WeakSet()
        self.integer_type = 0
        self.decimal_type = 0
        self._zone = ''
        self.session = Session(self)

    @property
    def zone(self):
        return self._zone

    @zone.setter
    def zone(self, text):
        """The session's time zone: aware datetimes bound as parameters are converted to it."""
        self._zone = text
        wire.PARAM_ZONE = zone_of(text) or _dt.timezone.utc

    def globals(self):
        return {'session': self.session, 'mysql': _mysql(), 'SqlError': SqlError}

    def new_id(self):
        self.next_id += 1
        return self.next_id

    def batch_bytes(self):
        """The bytes of parameters a batch message may carry (one mailbox arena, with room to spare)."""
        return self.d.arena_size // 4 * 3

    def routine_schema(self):
        return getattr(self.d, 'schemas', {}).get(self.d.routine)

    def flush_raw(self):
        """Before the worker posts a message (the arena is written over): decode raw chunks that
        are still referenced (a routine's dropped results are gone from the WeakSet already)."""
        if self._raw_results:
            for r in list(self._raw_results):
                r._materialize()

    # -- deferred drains (Dispatcher.deferDrain, drained, closeDeferred)

    def _defer(self, r):
        self._drain_other(r._id)
        self._pending = r
        self._pending_seq = self.d.seq
        self._pending_depth = self.d.depth

    def _drained(self, r):
        if self._pending is r:
            self._pending = None

    def _drain_other(self, sid):
        p = self._pending
        if p is None or p._id == sid:
            return
        self._pending = None
        if self._pending_seq == self.d.seq:
            p._drain()  # statements of an earlier call are closed by the host

    def call_ended(self, top):
        """At the end of a call: the host closes the statements of a top-level call; a nested call's
        unread result is closed now, so that a loop of nested calls does not pile results up."""
        p = self._pending
        if p is None or self._pending_depth < self.d.depth:
            return
        self._pending = None
        if top or self._pending_seq != self.d.seq:
            return
        try:
            self._request(CLOSE, p._id, None, None)
        except SqlError:
            pass  # already closed (an error in the statement)

    # -- requests

    def _request(self, action, sid, text, params, flags=0):
        return self._send(action, sid, _msg(text, params), flags)

    def _send(self, action, sid, msg, flags=0, width=0):
        """One SQL request (protocol.h kSql, Dispatcher.sqlRequest) of the message msg."""
        _mlewire.mark()  # the C++ call loop leaves the end of this call to Python
        n = msg.n
        # More values than the host accepts in one message would end the worker (independent review C10).
        if n > MAX_VALUES:
            raise _sql_error(1210, 'HY000', 'Incorrect arguments: %d values in one statement (at most %d)'
                             % (n, MAX_VALUES))
        self._drain_other(sid)
        self.flush_raw()  # rows still in the arena are read before it is written over
        d = self.d
        # Sent, nested requests served, the reply parsed: C++ (_mlewire.sql).
        t = _sql(msg.w.b, action, sid, msg.at, n, flags, width, d.depth, d.seq, d.handle)
        if t is None:
            # MySQL's error for a packet over max_allowed_packet, but not its SQLSTATE 08S01.
            raise _sql_error(1153, 'HY000', 'the SQL statement with its parameters is larger than MLE allows '
                             '(%d MiB)' % (wire.MAX_WORKER_MESSAGE >> 20))
        return _reply(t)
