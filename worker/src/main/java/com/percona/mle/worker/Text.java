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

package com.percona.mle.worker;

import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;

/**
 * Text on the wire (§7.1): every str and lstr the worker sends is well-formed UTF-8. Java strings
 * are UTF-16 and may hold unpaired surrogates; in data and messages each becomes U+FFFD (repair),
 * in code and names (SQL text, library names) it is an error of the guest code.
 */
final class Text {
  private Text() {}

  private static final char REPLACEMENT = '�';

  /** Whether s has no unpaired surrogate. */
  static boolean wellFormed(String s) {
    return firstUnpaired(s) < 0;
  }

  /** The index of the first unpaired surrogate of s, or -1. */
  private static int firstUnpaired(String s) {
    int n = s.length();
    for (int i = 0; i < n; i++) {
      char c = s.charAt(i);
      if (!Character.isSurrogate(c)) continue;
      if (Character.isHighSurrogate(c) && i + 1 < n && Character.isLowSurrogate(s.charAt(i + 1))) i++;
      else return i;
    }
    return -1;
  }

  /** s with each unpaired surrogate replaced by U+FFFD. */
  static String repair(String s) {
    int bad = firstUnpaired(s);
    if (bad < 0) return s;
    int n = s.length();
    StringBuilder b = new StringBuilder(n).append(s, 0, bad);
    for (int i = bad; i < n; i++) {
      char c = s.charAt(i);
      if (!Character.isSurrogate(c)) {
        b.append(c);
      } else if (Character.isHighSurrogate(c) && i + 1 < n && Character.isLowSurrogate(s.charAt(i + 1))) {
        b.append(c).append(s.charAt(++i));
      } else {
        b.append(REPLACEMENT);
      }
    }
    return b.toString();
  }

  /**
   * s as well-formed UTF-8, repaired (§7.1), in one pass where nothing needs repair: the JDK's
   * encoder (vectorized for Latin-1 strings) writes '?' for an unpaired surrogate, so only output
   * with more '?' than s has is encoded again, repairing. (A scan for unpaired surrogates before
   * getBytes cost ~0.7 ns per character after a large fetch, protocol-review.md §17.) The output
   * is searched for a '?' 8 bytes at a time; only where there is one are the '?' counted.
   */
  static byte[] utf8(String s) {
    byte[] b = s.getBytes(StandardCharsets.UTF_8);
    int first = indexOfMark(b);
    if (first < 0) return b;
    int marks = 0;
    for (int i = first; i < b.length; i++) marks += b[i] == '?' ? 1 : 0;
    int own = 0;  // String.indexOf is vectorized, for either coder of a String
    for (int i = s.indexOf('?'); i >= 0 && own <= marks; i = s.indexOf('?', i + 1)) own++;
    return own == marks ? b : utf8Repairing(s);
  }

  private static final java.lang.invoke.VarHandle LONGS =
      java.lang.invoke.MethodHandles.byteArrayViewVarHandle(long[].class, java.nio.ByteOrder.LITTLE_ENDIAN);
  private static final long MARKS = 0x3F3F3F3F3F3F3F3FL, ONES = 0x0101010101010101L, HIGHS = 0x8080808080808080L;

  /** The index of the first '?' (0x3F) in b, or -1; 8 bytes at a time (a zero byte of b ^ '?'). */
  static int indexOfMark(byte[] b) {
    int n = b.length, i = 0;
    for (; i <= n - 8; i += 8) {
      long v = (long) LONGS.get(b, i) ^ MARKS;
      if (((v - ONES) & ~v & HIGHS) != 0) break;  // exact for "some byte is zero"
    }
    for (; i < n; i++)
      if (b[i] == '?') return i;
    return -1;
  }

  /** s as UTF-8, each unpaired surrogate as U+FFFD (EF BF BD); longer than a message may be: TooLarge. */
  private static byte[] utf8Repairing(String s) {
    int n = s.length();
    long length = 0;
    for (int i = 0; i < n; i++) {
      char c = s.charAt(i);
      // A pair is 4 bytes, 2 for each of its chars; a lone surrogate is 3 (U+FFFD).
      length += c < 0x80 ? 1 : c < 0x800 ? 2 : Character.isSurrogate(c)
          && (Character.isHighSurrogate(c) ? i + 1 < n && Character.isLowSurrogate(s.charAt(i + 1))
              : i > 0 && Character.isHighSurrogate(s.charAt(i - 1))) ? 2 : 3;
    }
    if (length > Wire.MAX_WORKER_MESSAGE) throw new MessageWriter.TooLarge();
    byte[] out = new byte[(int) length];
    int p = 0;
    for (int i = 0; i < n; i++) {
      char c = s.charAt(i);
      if (c < 0x80) {
        out[p++] = (byte) c;
      } else if (c < 0x800) {
        out[p++] = (byte) (0xC0 | c >> 6);
        out[p++] = (byte) (0x80 | (c & 0x3F));
      } else if (!Character.isSurrogate(c)) {
        out[p++] = (byte) (0xE0 | c >> 12);
        out[p++] = (byte) (0x80 | (c >> 6 & 0x3F));
        out[p++] = (byte) (0x80 | (c & 0x3F));
      } else if (Character.isHighSurrogate(c) && i + 1 < n && Character.isLowSurrogate(s.charAt(i + 1))) {
        int cp = Character.toCodePoint(c, s.charAt(++i));
        out[p++] = (byte) (0xF0 | cp >> 18);
        out[p++] = (byte) (0x80 | (cp >> 12 & 0x3F));
        out[p++] = (byte) (0x80 | (cp >> 6 & 0x3F));
        out[p++] = (byte) (0x80 | (cp & 0x3F));
      } else {
        out[p++] = (byte) 0xEF;
        out[p++] = (byte) 0xBF;
        out[p++] = (byte) 0xBD;
      }
    }
    return out;
  }

  /** s repaired and cut at a character boundary so that its UTF-8 form has at most maxBytes bytes. */
  static String cut(String s, int maxBytes) {
    s = repair(s);
    if (s.length() <= maxBytes / 3) return s;  // a char is at most 3 bytes, a pair 4
    int bytes = 0;
    for (int i = 0; i < s.length(); ) {
      int cp = s.codePointAt(i);
      bytes += cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
      if (bytes > maxBytes) return s.substring(0, i);
      i += Character.charCount(cp);
    }
    return s;
  }

  /** The text of well-formed UTF-8 bytes; null if they are not well-formed. */
  static String decode(byte[] b) {
    String s = new String(b, StandardCharsets.UTF_8);
    // The lenient decoder puts U+FFFD where the bytes are ill-formed, so text without it was
    // well-formed; with it, the strict decoder tells a real U+FFFD from a replacement.
    if (s.indexOf(REPLACEMENT) < 0) return s;
    try {
      StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
          .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(b));
      return s;
    } catch (CharacterCodingException e) {
      return null;
    }
  }

  /**
   * The text of bytes that may be ill-formed UTF-8 (an lstr of the host): each maximal subpart of
   * an ill-formed sequence becomes U+FFFD, which is what Java's decoder does.
   */
  static String decodeRepairing(byte[] b) {
    return new String(b, StandardCharsets.UTF_8);
  }

  /** A language identifier or worker class (§9.1): 1 to 32 characters of [a-z0-9_]. */
  static boolean identifier(String s) {
    if (s.isEmpty() || s.length() > Wire.MAX_IDENTIFIER) return false;
    for (int i = 0; i < s.length(); i++) {
      char c = s.charAt(i);
      if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_') return false;
    }
    return true;
  }

  /** An SQLSTATE (§7.4): 5 characters of [0-9A-Z]. */
  static boolean sqlState(String s) {
    if (s.length() != 5) return false;
    for (int i = 0; i < 5; i++) {
      char c = s.charAt(i);
      if (!(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9')) return false;
    }
    return true;
  }
}
