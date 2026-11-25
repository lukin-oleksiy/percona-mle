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
   * s as well-formed UTF-8, repaired (§7.1). A string the JDK keeps as Latin-1 has no surrogate:
   * its encoder's output (a copy for ASCII) is the answer. Another one is encoded here, repairing
   * as it goes, in place of the JDK's encoder, which writes '?' for an unpaired surrogate. (Any
   * scan of a large string or of its output, on top of the encoding, cost up to 0.36 ns per byte
   * where it was compiled into its caller: echo of 64 KiB +4..+6 %.) A short string is checked
   * for unpaired surrogates first.
   */
  static byte[] utf8(String s) {
    int n = s.length();
    if (n <= SHORT ? firstUnpaired(s) < 0 : latin1(s)) return s.getBytes(StandardCharsets.UTF_8);
    return utf8Repairing(s);
  }

  private static final int SHORT = 32;

  /**
   * The class of String.chars()'s spliterator for a string kept as Latin-1 (the JDK makes one of
   * its own for each of its two forms), or null where it cannot be told (compact strings off).
   */
  private static final Class<?> LATIN1_CHARS = latin1Chars();

  private static Class<?> latin1Chars() {
    Class<?> c = "\u00e9".chars().spliterator().getClass();
    Class<?> other = "\u0100".chars().spliterator().getClass();
    return c != other && c.getName().equals("java.lang.StringLatin1$CharsSpliterator") ? c : null;
  }

  /** Whether the JDK keeps s as Latin-1 (it then has no surrogate); false when not known. */
  static boolean latin1(String s) {
    return LATIN1_CHARS != null && s.chars().spliterator().getClass() == LATIN1_CHARS;
  }

  /** s as UTF-8, each unpaired surrogate as U+FFFD (EF BF BD); longer than a message may be: TooLarge. */
  private static byte[] utf8Repairing(String s) {
    int n = s.length();
    if (n > ONE_PASS) return utf8Exact(s);
    // One pass into room for the longest form (3 bytes per char; a pair takes 4 of its 6), then
    // the bytes written, as the JDK's encoder does.
    byte[] out = new byte[n * 3];
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
    if (p > Wire.MAX_WORKER_MESSAGE) throw new MessageWriter.TooLarge();
    return p == out.length ? out : java.util.Arrays.copyOf(out, p);
  }

  /** Strings longer than this many chars are measured first (utf8Exact), not given 3 bytes per char. */
  private static final int ONE_PASS = 1 << 20;

  /** utf8Repairing for a long string: its length first, then the bytes. */
  private static byte[] utf8Exact(String s) {
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
   * an ill-formed sequence becomes U+FFFD (§7.1). Java's decoder does that except for an encoded
   * surrogate (ED A0..BF): it gives one U+FFFD for ED A0 80, not three (JDK 25). Text where each
   * byte became one char has no such sequence (it would have shrunk); other text is scanned for
   * one and only then decoded again by hand.
   */
  static String decodeRepairing(byte[] b) {
    String s = new String(b, StandardCharsets.UTF_8);
    if (s.length() == b.length) return s;
    for (int i = 0, n = b.length - 1; i < n; i++)
      if (b[i] == (byte) 0xED && (b[i + 1] & 0xE0) == 0xA0) return decodeMaximalSubparts(b);
    return s;
  }

  /** UTF-8 decoding with U+FFFD for each maximal subpart (the WHATWG "UTF-8 decode"). */
  static String decodeMaximalSubparts(byte[] b) {
    StringBuilder o = new StringBuilder(b.length);
    int i = 0;
    int n = b.length;
    while (i < n) {
      int c = b[i] & 0xFF;
      if (c < 0x80) {
        o.append((char) c);
        i++;
        continue;
      }
      int need;
      int cp;
      int lo = 0x80;
      int hi = 0xBF;
      if (c >= 0xC2 && c <= 0xDF) {
        need = 1;
        cp = c & 0x1F;
      } else if (c >= 0xE0 && c <= 0xEF) {
        need = 2;
        cp = c & 0x0F;
        if (c == 0xE0) lo = 0xA0;
        else if (c == 0xED) hi = 0x9F;
      } else if (c >= 0xF0 && c <= 0xF4) {
        need = 3;
        cp = c & 0x07;
        if (c == 0xF0) lo = 0x90;
        else if (c == 0xF4) hi = 0x8F;
      } else {
        o.append(REPLACEMENT);
        i++;
        continue;
      }
      int j = i + 1;
      boolean bad = false;
      for (int k = 0; k < need; k++, j++) {
        int d = j < n ? b[j] & 0xFF : -1;
        if (d < lo || d > hi) {
          bad = true;
          break;
        }
        lo = 0x80;
        hi = 0xBF;
        cp = (cp << 6) | (d & 0x3F);
      }
      // A maximal subpart (the lead and the continuations that fitted) is one U+FFFD; the byte
      // that did not fit starts the next sequence.
      if (bad) o.append(REPLACEMENT);
      else o.appendCodePoint(cp);
      i = j;
    }
    return o.toString();
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
