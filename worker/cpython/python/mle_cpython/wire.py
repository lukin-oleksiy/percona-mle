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

"""The MLE worker protocol (docs/protocol.md) as the Python side of the CPython worker needs it:
constants, a reader of host messages and a writer of message bodies, and SQL values <-> Python
values (the reading of values is C++: codec.cc)."""

import datetime as _dt
import decimal as _decimal
import json as _json
import math as _math
import struct as _struct
import array as _array
import codecs as _codecs

import _mlewire

# The protocol's constants, from include/mle/protocol.h through _mlewire (MVP review CPY-13):
# operations the dispatcher handles (§9), RESULT statuses (§9.4), value tags (§7.2), limits (§12),
# TIMESTAMP's range and zero (§7.2: microseconds since 1970-01-01 00:00:00 UTC).
from _mlewire import (PREPARE, CALL, DROP, RESET, OPTIONS, PARSE_LIBRARY,
                      OK, GUEST_ERROR, INTERRUPTED, EXHAUSTED, INTERNAL,
                      T_NULL, T_I64, T_U64, T_F64, T_F32, T_DECIMAL, T_STRING, T_BYTES, T_DATE, T_TIME,
                      T_DATETIME, T_TIMESTAMP, T_JSON, T_VECTOR,
                      MAX_WORKER_MESSAGE, MAX_VALUES, MAX_VECTOR_DIMENSIONS, MAX_TIME_HOUR,
                      MAX_ERROR_MESSAGE, MAX_FRAME_TEXT, MAX_FRAMES, MAX_FINDING, MAX_FINDINGS,
                      MAX_SQL_ERRNO, MAX_TIMESTAMP, ZERO_TIMESTAMP,
                      LANGUAGE_TYPE, LANGUAGE_TIME_ZONE, LANGUAGE_OPTION)

TAG_NAMES = ('NULL', 'BIGINT', 'BIGINT UNSIGNED', 'DOUBLE', 'FLOAT', 'DECIMAL', 'VARCHAR', 'VARBINARY',
             'DATE', 'TIME', 'DATETIME', 'TIMESTAMP', 'JSON', 'VECTOR')

# Error.language (§7.4): the worker's language (the markers are above).
LANGUAGE = 'python'

_U32 = _struct.Struct('<I').unpack_from
_U64 = _struct.Struct('<Q').unpack_from
_CIVIL = _struct.Struct('<HBBIBBIB')

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


def _replace_surrogates(e):
    """The encoder's error handler for utf8(): each surrogate of the run it found becomes U+FFFD
    (as bytes: the UTF-8 encoder takes only ASCII text from a handler)."""
    if not isinstance(e, UnicodeEncodeError):
        raise e
    return b'\xef\xbf\xbd' * (e.end - e.start), e.end


_codecs.register_error('mle.surrogate', _replace_surrogates)


def utf8(s):
    """Guest text as well-formed UTF-8 (§7.1): each unpaired surrogate becomes U+FFFD. One pass:
    the encoder calls the handler only where it finds a surrogate. str's own method, not the
    instance's: a str subclass of guest code may define its own encode()."""
    return str.encode(s, 'utf-8', 'mle.surrogate')


def cut(b, limit):
    """UTF-8 bytes cut to at most limit bytes at a character boundary."""
    if len(b) <= limit:
        return b
    while limit > 0 and (b[limit] & 0xC0) == 0x80:
        limit -= 1
    return b[:limit]


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


def have_numpy():
    """Whether numpy can be imported (typed columns need it)."""
    return _numpy.get() is not None


# The time zone of OPTIONS (sql.Api.zone sets it; +00:00 without OPTIONS, §1): a TIMESTAMP, which
# is an instant on the wire, is an aware datetime in this zone (Appendix B.4: zoneinfo.ZoneInfo for
# an IANA name, a fixed datetime.timezone for an offset). It shows the local fields and keeps the
# instant, so it goes back as a TIMESTAMP; a naive datetime (a DATETIME) goes back as a DATETIME.
ZONE = _dt.timezone(_dt.timedelta(0))
_EPOCH = _dt.datetime(1970, 1, 1, tzinfo=_dt.timezone.utc)
_MICROSECOND = _dt.timedelta(microseconds=1)


def timestamp(us, rows):
    """A TIMESTAMP of the host (helper of the C++ codec): an aware datetime in the zone of OPTIONS
    (ZoneInfo keeps the offset intervals of its zone). The zero timestamp, and what Python's
    datetime cannot hold, is None, as a routine argument and in a row (Appendix B.4). (rows: unused,
    the codec's signature.)"""
    try:
        if us != ZERO_TIMESTAMP:
            return (_EPOCH + _dt.timedelta(microseconds=us)).astimezone(ZONE)
    except OverflowError:
        pass
    return None


def aware(v):
    """Whether a datetime is an instant (it has a zone with an offset)."""
    return v.tzinfo is not None and v.utcoffset() is not None


def instant(v):
    """An aware datetime as a TIMESTAMP of the wire, or None if it is outside the wire's range."""
    us = (v - _EPOCH) // _MICROSECOND
    return us if 0 <= us <= MAX_TIMESTAMP else None


def local(v):
    """An aware datetime as local time in the zone of OPTIONS, naive."""
    return v.astimezone(ZONE).replace(tzinfo=None)


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
    """Reads a message of the host with the checks of §1: lengths against the bytes it has, and the
    tags and modes it reads. A message that breaks them is a protocol failure: the process ends."""
    __slots__ = ('b', 'p', 'n')

    def __init__(self, body, pos=0):
        self.b = body
        self.p = pos
        self.n = len(body)

    def _take(self, n):
        p = self.p
        if n > self.n - p:
            _mlewire.protocol_failure('message too short')
        self.p = p + n
        return p

    def u8(self):
        return self.b[self._take(1)]

    def u32(self):
        return _U32(self.b, self._take(4))[0]

    def u64(self):
        return _U64(self.b, self._take(8))[0]

    def count(self, min_size):
        """The count of a list whose elements have at least min_size bytes each."""
        n = self.u32()
        if n > (self.n - self.p) // min_size:
            _mlewire.protocol_failure('a list longer than the message')
        return n

    def str(self):
        n = self.u32()
        p = self._take(n)
        return str(self.b[p:p + n], 'utf-8', 'replace')

    def type(self):
        p = self._take(14)
        b = self.b
        tag, uns = b[p], b[p + 1]
        if tag > T_VECTOR:
            _mlewire.protocol_failure('an unknown tag in a Type')
        length, precision, scale = _struct.unpack_from('<III', b, p + 2)
        return ParamType(tag, uns != 0, length, precision, scale, self.str())


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
        """Text of guest code: repaired if it is not well-formed (§7.1)."""
        e = utf8(s)
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
            self.datetime(v)
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
        self.b += _CIVIL.pack(year, month, day, hour, minute, second, micro, neg)

    def datetime(self, v):
        """A datetime by its own kind (§7.2): an aware one is an instant, a TIMESTAMP (outside the
        wire's range of instants: its local time in the zone of OPTIONS); a naive one a DATETIME."""
        if aware(v):
            us = instant(v)
            if us is not None:
                self.b += _P_TAG_I64(T_TIMESTAMP, us)
                return
            v = local(v)
        self.temporal(T_DATETIME, v.year, v.month, v.day, v.hour, v.minute, v.second, v.microsecond, 0)

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
        if not 1 <= dims <= MAX_VECTOR_DIMENSIONS:
            raise ConversionError('a VECTOR has 1 to %d elements, not %d' % (MAX_VECTOR_DIMENSIONS, dims))
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
            if _math.isinf(_struct.unpack_from('<f', b, len(b) - 4)[0]):
                del b[-5:]
                raise _range(v, t)
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
                v = str.encode(v, 'utf-8')
            elif not isinstance(v, (bytes, bytearray, memoryview)):
                raise _cannot(v, t)
            b.append(T_BYTES)
            self.raw(bytes(v))
        elif tag == T_DATE:
            if isinstance(v, _dt.datetime) or not isinstance(v, _dt.date):
                raise _cannot(v, t)
            self.temporal(T_DATE, v.year, v.month, v.day, 0, 0, 0, 0, 0)
        elif tag == T_DATETIME or tag == T_TIMESTAMP:
            # By the value's own kind, as a parameter (the host converts to the declared type).
            if isinstance(v, _dt.datetime):
                if tag == T_TIMESTAMP and aware(v) and instant(v) is None:
                    raise _range(v, t)
                self.datetime(v)
            elif isinstance(v, _dt.date):
                self.temporal(T_DATETIME, v.year, v.month, v.day, 0, 0, 0, 0, 0)
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
    """A finite Decimal as DECIMAL text mysqld accepts: digits and a point, within the 100 bytes
    of the wire (§7.2).
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
    # The value, as the JVM worker shows it (shortened); its type for what has no short form.
    shown = repr(v) if isinstance(v, (str, int, float, bytes)) else type(v).__name__
    if len(shown) > 60:
        shown = shown[:57] + '...'
    return ConversionError('cannot convert %s to %s' % (shown, t.sql_name()))


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
    """v as a float of the declared type; NaN and the infinities are not SQL values (§7.2)."""
    if type(v) is float:
        f = v
    elif isinstance(v, (int, float, _decimal.Decimal)):
        try:
            f = float(v)
        except OverflowError:
            raise _range(v, t) from None
    elif type(v).__module__ == 'numpy' and getattr(v, 'ndim', 1) == 0:
        f = float(v)
    else:
        raise _cannot(v, t)
    if not _math.isfinite(f):
        raise _cannot(v, t)
    return f


def _decimal_text(v, t):
    if type(v).__module__ == 'numpy' and getattr(v, 'ndim', 1) == 0:
        v = v.item()  # a numpy number: as the Python number (Values.toBigDecimal takes any Number)
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


# ------------------------------------------------------------------ helpers of the C++ codec

def temporal(tag, year, month, day, hour, minute, second, micro, neg):
    """A temporal value of a routine argument that Python's types cannot hold: a DATE or DATETIME
    (a zero date, a zero month or day, an impossible date, year 0) is None, as in rows (Appendix
    B.4); a TIME out of range is a conversion error."""
    if tag != T_TIME:
        return None
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
