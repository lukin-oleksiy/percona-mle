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

"""execute_many as multi-row statements (BatchRewrite.java, exp note 52): recognises an INSERT or
REPLACE of exactly one row of values, each a parameter marker, NULL, DEFAULT, TRUE, FALSE or a
literal, with no marker outside the row, so that the row can be repeated:

  INSERT [LOW_PRIORITY | HIGH_PRIORITY | DELAYED] [IGNORE] [INTO] table [PARTITION (...)] [(columns)]
    {VALUES | VALUE} (item, ...) [AS alias [(columns)]] [ON DUPLICATE KEY UPDATE ...]
  REPLACE [LOW_PRIORITY | DELAYED] [INTO] table [PARTITION (...)] [(columns)] {VALUES | VALUE} (item, ...)

Anything else (expressions in the row, several rows, INSERT ... SELECT or SET, a backslash anywhere,
an executable comment, double-quoted names) is not recognised: execute_many then runs it row by
row. The same decisions as the JVM kind's, so that a batch runs the same statements on both."""


class Form:
    """A recognised statement: the text before the row, the row, the text after it, the table."""
    __slots__ = ('head', 'row', 'tail', 'schema', 'table')

    def __init__(self, head, row, tail, schema, table):
        self.head, self.row, self.tail, self.schema, self.table = head, row, tail, schema, table

    def statement(self, k):
        """The statement with k rows."""
        return self.head + ', '.join([self.row] * k) + self.tail


def parse(sql, parameter_count):
    """The statement's form, or None when it is not one that can be repeated."""
    if sql is None or parameter_count <= 0:
        return None
    t = _tokens(sql)
    return None if t is None else _form(sql, t, parameter_count)


class _Tok:
    """kind w (word), i (`identifier`), s (string), n (number), ? (marker), p (punctuation), e (end)."""
    __slots__ = ('kind', 'start', 'end', 'text')

    def __init__(self, kind, start, end, text):
        self.kind, self.start, self.end, self.text = kind, start, end, text

    def word(self, *ws):
        return self.kind == 'w' and self.text.upper() in ws

    def punct(self, c):
        return self.kind == 'p' and self.text[0] == c


_END = _Tok('e', 0, 0, ' ')


def _control(c):
    # Java's Character.isISOControl
    o = ord(c)
    return o <= 0x1F or 0x7F <= o <= 0x9F


def _tokens(s):
    """The tokens of s without whitespace and comments; None for text not to be rewritten."""
    if '\\' in s:
        return None  # its meaning depends on NO_BACKSLASH_ESCAPES
    out = []
    n = len(s)
    i = 0
    while i < n:
        c = s[i]
        if c.isspace():
            i += 1
        elif c == '#' or (c == '-' and i + 1 < n and s[i + 1] == '-'
                          and (i + 2 == n or s[i + 2].isspace() or _control(s[i + 2]))):
            e = s.find('\n', i)
            i = n if e < 0 else e + 1
        elif c == '/' and i + 1 < n and s[i + 1] == '*':
            if i + 2 < n and s[i + 2] == '!':
                return None  # an executable comment: SQL inside
            e = s.find('*/', i + 2)
            if e < 0:
                return None
            i = e + 2
        elif c in '\'"`':
            j = i + 1
            while True:
                if j >= n:
                    return None
                if s[j] == c:
                    if j + 1 < n and s[j + 1] == c:
                        j += 2
                        continue
                    break
                j += 1
            out.append(_Tok('i' if c == '`' else 's', i, j + 1, s[i:j + 1]))
            i = j + 1
        elif c.isdecimal() or (c == '.' and i + 1 < n and s[i + 1].isdecimal()):
            j = i + 1
            while j < n and (s[j].isalnum() or s[j] == '.'
                             or (s[j] in '+-' and s[j - 1] in 'eE')):
                j += 1
            out.append(_Tok('n', i, j, s[i:j]))
            i = j
        elif c.isalpha() or c in '_$' or ord(c) > 127:
            j = i + 1
            while j < n and (s[j].isalnum() or s[j] in '_$' or ord(s[j]) > 127):
                j += 1
            out.append(_Tok('w', i, j, s[i:j]))
            i = j
        else:
            out.append(_Tok('?' if c == '?' else 'p', i, i + 1, c))
            i += 1
    return out


def _at(t, p):
    return t[p] if p < len(t) else _END


def _ident(x):
    """A table or schema name: a word or a `quoted` name (unquoted); None otherwise."""
    if x.kind == 'w':
        return x.text
    if x.kind == 'i':
        return x.text[1:-1].replace('``', '`')
    return None


def _skip_parens(t, p):
    """The position after the parenthesised group starting at p, or -1."""
    if not _at(t, p).punct('('):
        return -1
    depth = 0
    while p < len(t):
        x = t[p]
        if x.punct('('):
            depth += 1
        elif x.punct(')'):
            depth -= 1
            if depth == 0:
                return p + 1
        elif x.kind == '?':
            return -1  # a marker outside the row
        p += 1
    return -1


def _form(s, t, parameter_count):
    p = 0
    insert, replace = _at(t, p).word('INSERT'), _at(t, p).word('REPLACE')
    if not insert and not replace:
        return None
    p += 1
    while _at(t, p).word('LOW_PRIORITY', 'HIGH_PRIORITY', 'DELAYED', 'IGNORE'):
        if replace and _at(t, p).word('HIGH_PRIORITY', 'IGNORE'):
            return None
        p += 1
    if _at(t, p).word('INTO'):
        p += 1
    schema, table = None, _ident(_at(t, p))
    if table is None:
        return None
    p += 1
    if _at(t, p).punct('.'):
        schema = table
        table = _ident(_at(t, p + 1))
        if table is None:
            return None
        p += 2
    if _at(t, p).word('PARTITION'):
        p = _skip_parens(t, p + 1)
        if p < 0:
            return None
    if _at(t, p).punct('('):
        p = _skip_parens(t, p)  # the columns
        if p < 0:
            return None
    if not _at(t, p).word('VALUES', 'VALUE'):
        return None
    p += 1
    if not _at(t, p).punct('('):
        return None
    row_start = _at(t, p).start
    p += 1
    markers = 0
    while True:
        x = _at(t, p)
        if x.kind == '?':
            markers += 1
        elif x.punct('-') and _at(t, p + 1).kind == 'n':
            p += 1  # a negative number
        elif not (x.kind in 'ns' or x.word('NULL', 'DEFAULT', 'TRUE', 'FALSE')):
            return None  # an expression, a function call, a column, ...
        p += 1
        if _at(t, p).punct(','):
            p += 1
            continue
        if _at(t, p).punct(')'):
            break
        return None
    row_end = _at(t, p).end
    p += 1
    if _at(t, p).punct(','):
        return None  # several rows already
    if p < len(t):
        if replace or not _at(t, p).word('AS', 'ON'):
            return None
        for j in range(p, len(t)):
            if t[j].kind == '?' or t[j].punct(';'):
                return None  # a marker outside the row
    if markers != parameter_count:
        return None
    return Form(s[:row_start], s[row_start:row_end], s[row_end:], schema, table)
