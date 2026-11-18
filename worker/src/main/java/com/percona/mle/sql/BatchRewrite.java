/* Copyright (c) 2026 Percona LLC and/or its affiliates. All rights
   reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

package com.percona.mle.sql;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * executeMany as multi-row statements (exp note 52): recognises an INSERT or REPLACE of exactly one
 * row of values, each a parameter marker, NULL, DEFAULT, TRUE, FALSE or a literal, with no marker
 * outside the row, so that the row can be repeated:
 *
 * <pre>
 * INSERT [LOW_PRIORITY | HIGH_PRIORITY | DELAYED] [IGNORE] [INTO] table [PARTITION (...)] [(columns)]
 *   {VALUES | VALUE} (item, ...) [AS alias [(columns)]] [ON DUPLICATE KEY UPDATE ...]
 * REPLACE [LOW_PRIORITY | DELAYED] [INTO] table [PARTITION (...)] [(columns)] {VALUES | VALUE} (item, ...)
 * </pre>
 *
 * Anything else (expressions in the row, several rows, INSERT ... SELECT or SET, a backslash anywhere,
 * an executable comment, double-quoted names) is not recognised: executeMany then runs it row by row.
 */
final class BatchRewrite {
  private BatchRewrite() {}

  /** A recognised statement: the text before the row, the row, the text after it, and the table. */
  record Form(String head, String row, String tail, String schema, String table) {
    /** The statement with k rows. */
    String statement(int k) {
      StringBuilder b = new StringBuilder(head.length() + tail.length() + k * (row.length() + 2)).append(head);
      for (int i = 0; i < k; i++) b.append(i == 0 ? "" : ", ").append(row);
      return b.append(tail).toString();
    }
  }

  /** The statement's form, or null when it is not one that can be repeated (see the class comment). */
  static Form parse(String sql, int parameterCount) {
    if (sql == null || parameterCount <= 0) return null;
    List<Tok> t = tokens(sql);
    return t == null ? null : form(sql, t, parameterCount);
  }

  /** A token: kind w (word), i (`identifier`), s (string), n (number), ? (marker), p (punctuation). */
  private record Tok(char kind, int start, int end, String text) {
    boolean word(String... ws) {
      if (kind != 'w') return false;
      String u = text.toUpperCase(Locale.ROOT);
      for (String w : ws)
        if (u.equals(w)) return true;
      return false;
    }

    boolean punct(char c) {
      return kind == 'p' && text.charAt(0) == c;
    }
  }

  /** The tokens of sql without whitespace and comments; null for text not to be rewritten. */
  private static List<Tok> tokens(String s) {
    if (s.indexOf('\\') >= 0) return null;  // its meaning depends on NO_BACKSLASH_ESCAPES
    List<Tok> out = new ArrayList<>();
    int n = s.length();
    for (int i = 0; i < n; ) {
      char c = s.charAt(i);
      if (Character.isWhitespace(c)) {
        i++;
      } else if (c == '#' || (c == '-' && i + 1 < n && s.charAt(i + 1) == '-'
          && (i + 2 == n || Character.isWhitespace(s.charAt(i + 2)) || Character.isISOControl(s.charAt(i + 2))))) {
        int e = s.indexOf('\n', i);
        i = e < 0 ? n : e + 1;
      } else if (c == '/' && i + 1 < n && s.charAt(i + 1) == '*') {
        if (i + 2 < n && s.charAt(i + 2) == '!') return null;  // an executable comment: SQL inside
        int e = s.indexOf("*/", i + 2);
        if (e < 0) return null;
        i = e + 2;
      } else if (c == '\'' || c == '"' || c == '`') {
        int j = i + 1;
        while (true) {
          if (j >= n) return null;
          if (s.charAt(j) == c) {
            if (j + 1 < n && s.charAt(j + 1) == c) {
              j += 2;
              continue;
            }
            break;
          }
          j++;
        }
        out.add(new Tok(c == '`' ? 'i' : 's', i, j + 1, s.substring(i, j + 1)));
        i = j + 1;
      } else if (Character.isDigit(c) || (c == '.' && i + 1 < n && Character.isDigit(s.charAt(i + 1)))) {
        int j = i + 1;
        while (j < n && (Character.isLetterOrDigit(s.charAt(j)) || s.charAt(j) == '.'
            || ((s.charAt(j) == '+' || s.charAt(j) == '-') && (s.charAt(j - 1) == 'e' || s.charAt(j - 1) == 'E'))))
          j++;
        out.add(new Tok('n', i, j, s.substring(i, j)));
        i = j;
      } else if (Character.isLetter(c) || c == '_' || c == '$' || c > 127) {
        int j = i + 1;
        while (j < n && (Character.isLetterOrDigit(s.charAt(j)) || s.charAt(j) == '_' || s.charAt(j) == '$'
            || s.charAt(j) > 127))
          j++;
        out.add(new Tok('w', i, j, s.substring(i, j)));
        i = j;
      } else {
        out.add(new Tok(c == '?' ? '?' : 'p', i, i + 1, String.valueOf(c)));
        i++;
      }
    }
    return out;
  }

  private static Tok at(List<Tok> t, int p) {
    return p < t.size() ? t.get(p) : new Tok('e', 0, 0, " ");
  }

  /** A table or schema name: a word or a `quoted` name (unquoted); null otherwise. */
  private static String ident(Tok x) {
    if (x.kind == 'w') return x.text;
    if (x.kind == 'i') return x.text.substring(1, x.text.length() - 1).replace("``", "`");
    return null;
  }

  /** The position after the parenthesised group starting at p, or -1. */
  private static int skipParens(List<Tok> t, int p) {
    if (!at(t, p).punct('(')) return -1;
    int depth = 0;
    for (; p < t.size(); p++) {
      Tok x = t.get(p);
      if (x.punct('(')) depth++;
      else if (x.punct(')') && --depth == 0) return p + 1;
      else if (x.kind == '?') return -1;  // a marker outside the row
    }
    return -1;
  }

  private static Form form(String s, List<Tok> t, int parameterCount) {
    int p = 0;
    boolean insert = at(t, p).word("INSERT"), replace = at(t, p).word("REPLACE");
    if (!insert && !replace) return null;
    p++;
    while (at(t, p).word("LOW_PRIORITY", "HIGH_PRIORITY", "DELAYED", "IGNORE")) {
      if (replace && at(t, p).word("HIGH_PRIORITY", "IGNORE")) return null;
      p++;
    }
    if (at(t, p).word("INTO")) p++;
    String schema = null, table = ident(at(t, p));
    if (table == null) return null;
    p++;
    if (at(t, p).punct('.')) {
      schema = table;
      table = ident(at(t, p + 1));
      if (table == null) return null;
      p += 2;
    }
    if (at(t, p).word("PARTITION") && (p = skipParens(t, p + 1)) < 0) return null;
    if (at(t, p).punct('(') && (p = skipParens(t, p)) < 0) return null;  // the columns
    if (!at(t, p).word("VALUES", "VALUE")) return null;
    p++;
    if (!at(t, p).punct('(')) return null;
    int rowStart = at(t, p).start;
    p++;
    int markers = 0;
    while (true) {
      Tok x = at(t, p);
      if (x.kind == '?') {
        markers++;
      } else if (x.punct('-') && at(t, p + 1).kind == 'n') {
        p++;  // a negative number
      } else if (!(x.kind == 'n' || x.kind == 's' || x.word("NULL", "DEFAULT", "TRUE", "FALSE"))) {
        return null;  // an expression, a function call, a column, ...
      }
      p++;
      if (at(t, p).punct(',')) {
        p++;
        continue;
      }
      if (at(t, p).punct(')')) break;
      return null;
    }
    int rowEnd = at(t, p).end;
    p++;
    if (at(t, p).punct(',')) return null;  // several rows already
    if (p < t.size()) {
      if (replace || !at(t, p).word("AS", "ON")) return null;
      for (int j = p; j < t.size(); j++)
        if (t.get(j).kind == '?' || t.get(j).punct(';')) return null;  // a marker outside the row
    }
    if (markers != parameterCount) return null;
    return new Form(s.substring(0, rowStart), s.substring(rowStart, rowEnd), s.substring(rowEnd), schema, table);
  }
}
