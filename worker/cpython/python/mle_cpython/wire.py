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

"""Protocol v3 (include/mle/protocol.h): constants, a reader and a writer of message bodies,
and SQL values <-> Python values with the JVM kind's Python rules (Polyglot/PySql)."""

import datetime as _dt
import decimal as _decimal
import json as _json
import math as _math
import struct as _struct
import array as _array

VERSION = 3

READY, PREPARE, CALL, RESULT, SQL, SQL_RESULT, FETCH, DROP, RESET, CONTINUE, OPTIONS, \
    PARSE_LIBRARY, LIBRARY, LIBRARY_RESULT = range(1, 15)

OK, GUEST_ERROR, INTERRUPTED, EXHAUSTED, INTERNAL = range(5)

STATE_IDLE, STATE_RUNNING, STATE_COMPILING = 1, 2, 3

(T_NULL, T_I64, T_U64, T_F64, T_F32, T_DECIMAL, T_STRING, T_BYTES, T_DATE, T_TIME, T_DATETIME,
 T_TIMESTAMP, T_JSON, T_VECTOR) = range(14)
TAG_NAMES = ('NULL', 'BIGINT', 'BIGINT UNSIGNED', 'DOUBLE', 'FLOAT', 'DECIMAL', 'VARCHAR', 'VARBINARY',
             'DATE', 'TIME', 'DATETIME', 'TIMESTAMP', 'JSON', 'VECTOR')

SQL_ERROR, SQL_OK, SQL_ROWS = 0, 1, 2
NO_ROW = 0xFFFFFFFF
MAX_WORKER_MESSAGE = 64 << 20
MAX_VECTOR_DIMENSIONS = 16383
MAX_TIME_HOUR = 838
LANGUAGE_TYPE = 'MLE-Type'
LANGUAGE_TIME_ZONE = 'MLE-TimeZone'

_U8 = _struct.Struct('<B').unpack_from
_U16 = _struct.Struct('<H').unpack_from
_U32 = _struct.Struct('<I').unpack_from
_U64 = _struct.Struct('<Q').unpack_from
_I64 = _struct.Struct('<q').unpack_from
_F64 = _struct.Struct('<d').unpack_from
_F32 = _struct.Struct('<f').unpack_from
_TEMPORAL = _struct.Struct('<HBBIBBIB')
_TEMPORAL_UNPACK = _TEMPORAL.unpack_from

P_U8 = _struct.Struct('<B').pack
P_U32 = _struct.Struct('<I').pack
P_U64 = _struct.Struct('<Q').pack
P_I64 = _struct.Struct('<q').pack
P_F64 = _struct.Struct('<d').pack
P_F32 = _struct.Struct('<f').pack
_P_TAG_I64 = _struct.Struct('<Bq').pack
_P_TAG_U64 = _struct.Struct('<BQ').pack
_P_TAG_F64 = _struct.Struct('<Bd').pack
_P_TAG_F32 = _struct.Struct('<Bf').pack

class _LazyNumpy:
    """numpy on first use (as the JVM kind: a worker that never needs it does not pay for it);
    false if numpy is not installed."""
    __slots__ = ('m',)

    def __init__(self):
        self.m = False

    def get(self):
        if self.m is False:
            try:
                import numpy
                self.m = numpy
            except ImportError:
                self.m = None
        return self.m


_numpy = _LazyNumpy()


# The session's time zone (OPTIONS; sql.Api.zone sets it): aware datetimes bound as SQL parameters
# are converted to it.
PARAM_ZONE = _dt.timezone.utc


class ConversionError(Exception):
    """A value that cannot be converted (language MLE-Type)."""


class ParamType:
    __slots__ = ('tag', 'unsigned', 'length', 'precision', 'scale', 'name')

    def __init__(self, tag, unsigned, length, precision, scale, name):
        self.tag, self.unsigned, self.length, self.precision, self.scale, self.name = \
            tag, unsigned, length, precision, scale, name

    def sql_name(self):
        return self.name or (TAG_NAMES[self.tag] if self.tag < len(TAG_NAMES) else '?')


class Reader:
    __slots__ = ('b', 'p')

    def __init__(self, body, pos=0):
        self.b = body
        self.p = pos

    def u8(self):
        p = self.p
        self.p = p + 1
        return self.b[p]

    def u32(self):
        v, = _U32(self.b, self.p)
        self.p += 4
        return v

    def u64(self):
        v, = _U64(self.b, self.p)
        self.p += 8
        return v

    def str(self):
        n, = _U32(self.b, self.p)
        p = self.p + 4
        self.p = p + n
        return str(self.b[p:p + n], 'utf-8')

    def raw(self):
        n, = _U32(self.b, self.p)
        p = self.p + 4
        self.p = p + n
        return bytes(self.b[p:p + n])

    def param_type(self):
        b, p = self.b, self.p
        tag, uns = b[p], b[p + 1]
        length, precision, scale = _struct.unpack_from('<III', b, p + 2)
        self.p = p + 14
        return ParamType(tag, uns != 0, length, precision, scale, self.str())

    def finish(self):
        if self.p != len(self.b):
            raise RuntimeError('trailing bytes in a message')

    # values -----------------------------------------------------------------------------------

    def value(self, vector=None):
        """The next value. vector(dims, offset) makes VECTORs (default: a new numpy array)."""
        b = self.b
        p = self.p
        tag = b[p]
        p += 1
        if tag == T_I64:
            self.p = p + 8
            return _I64(b, p)[0]
        if tag == T_STRING:
            n, = _U32(b, p)
            self.p = p + 4 + n
            return str(b[p + 4:p + 4 + n], 'utf-8')
        if tag == T_NULL:
            self.p = p
            return None
        if tag == T_F64:
            self.p = p + 8
            return _F64(b, p)[0]
        if tag == T_U64:
            self.p = p + 8
            return _U64(b, p)[0]
        if tag == T_F32:
            self.p = p + 4
            return _F32(b, p)[0]
        if tag == T_VECTOR:
            dims, = _U32(b, p)
            p += 4
            p += (-p) % 16  # VECTOR data is 16-aligned relative to the message start
            self.p = p + dims * 4
            if vector is not None:
                return vector(dims, p)
            return new_vector(b, p, dims)
        if tag == T_DECIMAL:
            self.p = p
            return _decimal.Decimal(self.str())
        if tag == T_BYTES:
            self.p = p
            return self.raw()
        if tag == T_JSON:
            self.p = p
            return _json.loads(self.str())
        if T_DATE <= tag <= T_TIMESTAMP:
            year, month, day, hour, minute, second, micro, neg = _TEMPORAL_UNPACK(b, p)
            self.p = p + _TEMPORAL.size
            if tag == T_TIME:
                d = _dt.timedelta(hours=hour, minutes=minute, seconds=second, microseconds=micro)
                return -d if neg else d
            try:
                if tag == T_DATE:
                    return _dt.date(year, month, day)
                return _dt.datetime(year, month, day, hour, minute, second, micro)
            except ValueError:
                raise ConversionError('the %s value %04d-%02d-%02d has no Python equivalent (zero date?)'
                                      % (TAG_NAMES[tag], year, month, day)) from None
        raise RuntimeError('unknown value tag %d' % tag)


def new_vector(b, p, dims):
    np = _numpy.get()
    if np is not None:
        return np.frombuffer(b, np.float32, dims, p).copy()
    return _array.array('f', bytes(b[p:p + dims * 4]))


class Writer:
    """A message body in a bytearray (send() copies it into the arena)."""
    __slots__ = ('b',)

    def __init__(self):
        self.b = bytearray()

    def u8(self, v):
        self.b += P_U8(v)

    def u32(self, v):
        self.b += P_U32(v)

    def u64(self, v):
        self.b += P_U64(v)

    def str(self, s):
        e = s.encode('utf-8', 'replace')
        self.b += P_U32(len(e))
        self.b += e

    def raw(self, e):
        self.b += P_U32(len(e))
        self.b += e

    # parameters of SQL statements: the SQL type follows the Python type (PySql.kind) ----------

    def param(self, v):
        b = self.b
        if v is None:
            b.append(T_NULL)
        elif v is True or v is False:
            b += _P_TAG_I64(T_I64, 1 if v else 0)
        elif type(v) is int:
            if -2 ** 63 <= v < 2 ** 63:
                b += _P_TAG_I64(T_I64, v)
            elif 0 <= v < 2 ** 64:
                b += _P_TAG_U64(T_U64, v)
            else:
                b.append(T_DECIMAL)
                self.str(param_decimal_text(_decimal.Decimal(v)))
        elif type(v) is float:
            if not _math.isfinite(v):
                raise TypeError('cannot bind ' + repr(v))
            b += _P_TAG_F64(T_F64, v)
        elif isinstance(v, str):
            b.append(T_STRING)
            self.str(v)
        elif isinstance(v, _decimal.Decimal):
            if not v.is_finite():
                raise TypeError('cannot bind ' + str(v))
            text = param_decimal_text(v)
            b.append(T_DECIMAL)
            self.str(text)
        elif isinstance(v, _dt.datetime):
            if v.tzinfo is not None and v.utcoffset() is not None:
                # An aware datetime keeps its instant: in the session's time zone (PySql.param).
                v = v.astimezone(PARAM_ZONE).replace(tzinfo=None)
            self.temporal(T_DATETIME, v.year, v.month, v.day, v.hour, v.minute, v.second, v.microsecond, 0)
        elif isinstance(v, _dt.date):
            self.temporal(T_DATE, v.year, v.month, v.day, 0, 0, 0, 0, 0)
        elif isinstance(v, _dt.timedelta):
            self.time(v)
        elif isinstance(v, _dt.time):
            self.temporal(T_TIME, 0, 0, 0, v.hour, v.minute, v.second, v.microsecond, 0)
        elif isinstance(v, (bytes, bytearray, memoryview)):
            b.append(T_BYTES)
            self.raw(bytes(v))
        elif isinstance(v, _array.array):
            self.vector(v)
        elif type(v).__module__ == 'numpy':
            nd = getattr(v, 'ndim', None)
            if nd == 0:
                self.param(v.item())
            elif nd == 1:
                self.vector(v)
            else:
                raise TypeError('cannot bind a numpy array of %d dimensions' % nd)
        elif isinstance(v, int):  # bool subclasses, IntEnum
            self.param(int(v))
        elif isinstance(v, float):
            self.param(float(v))
        elif isinstance(v, (set, frozenset)):
            b.append(T_STRING)
            self.str(','.join(map(str, v)))
        elif isinstance(v, (list, tuple, dict)):
            b.append(T_JSON)
            self.str(_json.dumps(v))
        else:
            raise TypeError('cannot bind a value of type ' + type(v).__name__)

    def temporal(self, tag, year, month, day, hour, minute, second, micro, neg):
        if not 0 <= year <= 9999:
            raise ConversionError('the year %d is out of range (0 to 9999)' % year)
        self.b.append(tag)
        self.b += _TEMPORAL.pack(year, month, day, hour, minute, second, micro, neg)

    def time(self, d):
        neg = d < _dt.timedelta(0)
        a = -d if neg else d
        us = (a.days * 86400 + a.seconds) * 1000000 + a.microseconds
        hours, rest = divmod(us, 3600 * 1000000)
        minutes, rest = divmod(rest, 60 * 1000000)
        seconds, micro = divmod(rest, 1000000)
        if hours > MAX_TIME_HOUR or (hours == MAX_TIME_HOUR and minutes == 59 and seconds == 59 and micro > 0):
            raise ConversionError('TIME value out of range: %s' % d)
        self.temporal(T_TIME, 0, 0, 0, hours, minutes, seconds, micro, 1 if neg else 0)

    def vector(self, v):
        np = None if isinstance(v, _array.array) else _numpy.get()
        if np is not None:
            data = np.ascontiguousarray(v, dtype=np.float32)
            if data.ndim != 1:
                raise ConversionError('a VECTOR must have one dimension, not %d' % data.ndim)
            dims = data.shape[0]
            data = data.data
        else:
            a = v if isinstance(v, _array.array) and v.typecode == 'f' else _array.array('f', v)
            dims = len(a)
            data = memoryview(a).cast('B')
        if dims > MAX_VECTOR_DIMENSIONS:
            raise ConversionError('VECTOR too large: %d' % dims)
        b = self.b
        b.append(T_VECTOR)
        b += P_U32(dims)
        pad = (-len(b)) % 16
        if pad:
            b += bytes(pad)
        b += data

    # routine results as the declared SQL type (Values.writeAs) ------------------------------------

    def write_as(self, v, t):
        if v is None:
            self.b.append(T_NULL)
            return
        tag = t.tag
        b = self.b
        if tag == T_I64 or tag == T_U64:
            unsigned = t.unsigned or tag == T_U64
            if type(v) is not int:
                v = _integral(v, t)
            if unsigned:
                if not 0 <= v < 2 ** 64:
                    raise _range(v, t)
                b += _P_TAG_U64(T_U64, v)
            else:
                if not -2 ** 63 <= v < 2 ** 63:
                    raise _range(v, t)
                b += _P_TAG_I64(T_I64, v)
        elif tag == T_F64:
            b += _P_TAG_F64(T_F64, _number(v, t))
        elif tag == T_F32:
            f = _number(v, t)
            try:
                b += _P_TAG_F32(T_F32, f)
            except OverflowError:
                raise _range(v, t) from None
        elif tag == T_STRING:
            if isinstance(v, (bytes, bytearray, memoryview, _array.array)) or type(v).__module__ == 'numpy' \
                    and getattr(v, 'ndim', 0) > 0:
                raise _cannot(v, t)
            b.append(T_STRING)
            self.str(v if isinstance(v, str) else str(v))
        elif tag == T_DECIMAL:
            b.append(T_DECIMAL)
            self.str(_decimal_text(v, t))
        elif tag == T_JSON:
            b.append(T_JSON)
            self.str(v if isinstance(v, str) else _json.dumps(v, separators=(',', ':')))
        elif tag == T_BYTES:
            if isinstance(v, str):
                v = v.encode('utf-8')
            elif not isinstance(v, (bytes, bytearray, memoryview)):
                raise _cannot(v, t)
            b.append(T_BYTES)
            self.raw(bytes(v))
        elif tag == T_DATE:
            if isinstance(v, _dt.datetime) or not isinstance(v, _dt.date):
                raise _cannot(v, t)
            self.temporal(T_DATE, v.year, v.month, v.day, 0, 0, 0, 0, 0)
        elif tag == T_DATETIME or tag == T_TIMESTAMP:
            if isinstance(v, _dt.datetime):
                self.temporal(tag, v.year, v.month, v.day, v.hour, v.minute, v.second, v.microsecond, 0)
            elif isinstance(v, _dt.date):
                self.temporal(tag, v.year, v.month, v.day, 0, 0, 0, 0, 0)
            else:
                raise _cannot(v, t)
        elif tag == T_TIME:
            if isinstance(v, _dt.timedelta):
                self.time(v)
            elif isinstance(v, _dt.time):
                self.temporal(T_TIME, 0, 0, 0, v.hour, v.minute, v.second, v.microsecond, 0)
            else:
                raise _cannot(v, t)
        elif tag == T_VECTOR:
            if isinstance(v, (str, bytes, int, float)):
                raise _cannot(v, t)
            self.vector(v)
        else:
            raise _cannot(v, t)


_DECIMAL_MAX_DIGITS, _DECIMAL_MAX_SCALE = 65, 30
_DECIMAL_CONTEXT = _decimal.Context(prec=_DECIMAL_MAX_DIGITS + _DECIMAL_MAX_SCALE + 2)


def param_decimal_text(d, target='SQL'):
    """A finite Decimal as DECIMAL text mysqld accepts (Values.decimalText): digits and a point,
    at most 100 characters (the host ends a worker that sends more, independent review C13).
    Digits after the 30th decimal place are rounded as MySQL rounds them; more than 65 digits
    before the point is an error. Nothing is expanded before it is checked (1e2000000000)."""
    _, digits, exponent = d.as_tuple()
    precision, scale = len(digits), -exponent
    if scale < 0 or scale > _DECIMAL_MAX_SCALE or precision > _DECIMAL_MAX_DIGITS:  # unusual
        integer_digits = precision - scale
        if not d or integer_digits < -_DECIMAL_MAX_SCALE:
            return '0'
        if integer_digits > _DECIMAL_MAX_DIGITS:
            raise ConversionError('value %s is out of range for %s'
                                  % (str(d) if precision <= 40 else 'of %d digits' % integer_digits,
                                     'a DECIMAL parameter' if target == 'SQL' else target))
        if scale > _DECIMAL_MAX_SCALE:
            d = d.quantize(_decimal.Decimal(1).scaleb(-_DECIMAL_MAX_SCALE), rounding=_decimal.ROUND_HALF_UP,
                           context=_DECIMAL_CONTEXT)
        _, digits, exponent = d.as_tuple()
        if len(digits) + exponent > _DECIMAL_MAX_DIGITS:  # rounded up to one digit more
            raise ConversionError('value is out of range for %s'
                                  % ('a DECIMAL parameter' if target == 'SQL' else target))
    return format(d, 'f')


def _cannot(v, t):
    return ConversionError('cannot convert %s to %s' % (type(v).__name__, t.sql_name()))


def _range(v, t):
    return ConversionError('value %s is out of range for %s' % (v, t.sql_name()))


def _integral(v, t):
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, int):
        return int(v)
    if isinstance(v, float):
        if _math.isfinite(v) and v == int(v):
            return int(v)
        raise _cannot(v, t) if not _math.isfinite(v) else _range(v, t)
    if isinstance(v, _decimal.Decimal):
        if v.is_finite() and v == v.to_integral_value():
            return int(v)
        raise _range(v, t)
    if type(v).__module__ == 'numpy' and getattr(v, 'ndim', 1) == 0:
        return _integral(v.item(), t)
    raise _cannot(v, t)


def _number(v, t):
    if type(v) is float:
        return v
    if isinstance(v, (int, float, _decimal.Decimal)):
        return float(v)
    if type(v).__module__ == 'numpy' and getattr(v, 'ndim', 1) == 0:
        return float(v)
    raise _cannot(v, t)


def _decimal_text(v, t):
    if isinstance(v, _decimal.Decimal):
        d = v
    elif isinstance(v, (int, str)) and not isinstance(v, bool):
        try:
            d = _decimal.Decimal(v)
        except _decimal.InvalidOperation:
            raise _cannot(v, t) from None
    elif isinstance(v, float) and _math.isfinite(v):
        d = _decimal.Decimal(repr(v))
    else:
        raise _cannot(v, t)
    if not d.is_finite():
        raise _cannot(v, t)
    # Values.decimalText as for parameters: nothing is expanded before it is checked (a result of
    # Decimal('1e2000000000') must not make text the host refuses).
    return param_decimal_text(d, t.sql_name())


# ------------------------------------------------------------------ helpers of the C++ codec (S-21b)

def temporal(tag, year, month, day, hour, minute, second, micro, neg):
    """A temporal value Python's types cannot hold: raise the conversion error (zero dates)."""
    raise ConversionError('the %s value %04d-%02d-%02d %s%d:%02d:%02d.%06d has no Python equivalent (zero date?)'
                          % (TAG_NAMES[tag], year, month, day, '-' if neg else '', hour, minute, second, micro))


MODE_SKIP, MODE_VECTOR, MODE_INT64, MODE_FLOAT64, MODE_LIST = range(5)
_ITEM = {MODE_INT64: ('q', 8), MODE_FLOAT64: ('d', 8)}


class Collector:
    """One column of fetch_columns while the codec fills it: typed (a bytearray of float32 VECTOR
    rows, int64 or float64 values) until a value does not fit, then a list (demote)."""
    __slots__ = ('mode', 'data', 'dims', 'count', 'expect')

    def __init__(self, mode, expect):
        self.mode = mode
        self.data = [] if mode == MODE_LIST else bytearray()
        self.dims = 0
        self.count = 0
        self.expect = expect

    def add(self, v):
        """A value decoded by Python (rows read before fetch_columns)."""
        np = _numpy.get()
        if self.mode == MODE_VECTOR:
            if v is not None and hasattr(v, '__len__') and len(v) and (self.dims in (0, len(v))):
                self.dims = len(v)
                self.data += np.asarray(v, dtype=np.float32).tobytes()
                self.count += 1
                return
        elif self.mode == MODE_INT64:
            if type(v) is int and -2 ** 63 <= v < 2 ** 63:
                self.data += P_I64(v)
                self.count += 1
                return
        elif self.mode == MODE_FLOAT64:
            if type(v) is float:
                self.data += P_F64(v)
                self.count += 1
                return
        if self.mode != MODE_LIST:
            demote(self)
        self.data.append(v)

    def result(self):
        np = _numpy.get()
        if self.mode == MODE_LIST:
            return self.data
        if self.count == 0:
            return []
        if self.mode == MODE_VECTOR:
            n = self.count * self.dims * 4
            del self.data[n:]
            return np.frombuffer(self.data, np.float32).reshape(self.count, self.dims)
        del self.data[self.count * 8:]
        return np.frombuffer(self.data, np.int64 if self.mode == MODE_INT64 else np.float64)


def demote(c):
    """A typed collector becomes a list of its values so far."""
    np = _numpy.get()
    if c.mode == MODE_VECTOR:
        values = [] if c.count == 0 else list(
            np.frombuffer(c.data, np.float32, c.count * c.dims).reshape(c.count, c.dims).copy())
    elif c.mode in _ITEM:
        code, size = _ITEM[c.mode]
        values = list(_struct.unpack_from('<%d%s' % (c.count, code), c.data)) if c.count else []
    else:
        values = []
    c.mode = MODE_LIST
    c.data = values
    c.count = 0
