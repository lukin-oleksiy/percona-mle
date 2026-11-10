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

import java.util.List;
import java.util.Locale;

/**
 * Statement options (Oracle's names): passResultToClient, charsetName, integerType and
 * decimalType. A null field is not set: a statement's options are taken over the session's
 * ({@link Session#setOptions}), and those over the defaults (no passing to the client, utf8mb4,
 * and the session state's integer_type and decimal_type).
 *
 * <p>integerType and decimalType concern the guest languages' values (Java values are exact).
 */
public final class SqlOptions {
  /** The option names. */
  public static final List<String> NAMES = List.of("passResultToClient", "charsetName", "integerType", "decimalType");

  public static final List<String> INTEGER_TYPES = List.of("UNSAFE_STRING", "UNSAFE_BIGINT", "STRING", "BIGINT");
  public static final List<String> DECIMAL_TYPES = List.of("STRING", "NUMBER");

  private Boolean passResultToClient;
  private String charsetName;
  private String integerType;
  private String decimalType;

  public SqlOptions() {}

  private SqlOptions(SqlOptions o) {
    passResultToClient = o.passResultToClient;
    charsetName = o.charsetName;
    integerType = o.integerType;
    decimalType = o.decimalType;
  }

  public SqlOptions passResultToClient(boolean v) {
    passResultToClient = v;
    return this;
  }

  /** Only utf8mb4 (and its aliases): statements and values travel as utf8mb4. */
  public SqlOptions charsetName(String v) {
    String c = v.toLowerCase(Locale.ROOT);
    if (!c.equals("utf8mb4") && !c.equals("utf8") && !c.equals("utf8mb3"))
      throw new IllegalArgumentException("charsetName: only utf8mb4 is supported, not '" + v + "'");
    charsetName = "utf8mb4";
    return this;
  }

  /** UNSAFE_STRING, UNSAFE_BIGINT, STRING or BIGINT (any case). */
  public SqlOptions integerType(String v) {
    integerType = pick(v, INTEGER_TYPES, "integerType");
    return this;
  }

  /** STRING or NUMBER (any case). */
  public SqlOptions decimalType(String v) {
    decimalType = pick(v, DECIMAL_TYPES, "decimalType");
    return this;
  }

  /** Sets an option by name from a Boolean or String. */
  public SqlOptions set(String name, Object value) {
    switch (name) {
      case "passResultToClient" -> {
        if (!(value instanceof Boolean b)) throw new IllegalArgumentException("passResultToClient must be a boolean");
        passResultToClient(b);
      }
      case "charsetName" -> charsetName(String.valueOf(value));
      case "integerType" -> integerType(String.valueOf(value));
      case "decimalType" -> decimalType(String.valueOf(value));
      default -> throw new IllegalArgumentException("unknown option '" + name + "'");
    }
    return this;
  }

  private static String pick(String v, List<String> allowed, String what) {
    String u = v.toUpperCase(Locale.ROOT);
    if (!allowed.contains(u)) throw new IllegalArgumentException(what + " must be one of " + allowed + ", not '" + v + "'");
    return u;
  }

  public boolean passResultToClient() {
    return passResultToClient != null && passResultToClient;
  }

  public String charsetName() {
    return charsetName == null ? "utf8mb4" : charsetName;
  }

  /** The integer type, or null (the session state's). */
  public String integerType() {
    return integerType;
  }

  /** The decimal type, or null (the session state's). */
  public String decimalType() {
    return decimalType;
  }

  /** The value of an option by name: Boolean or String (null: the session state's type). */
  public Object get(String name) {
    return switch (name) {
      case "passResultToClient" -> passResultToClient();
      case "charsetName" -> charsetName();
      case "integerType" -> integerType;
      case "decimalType" -> decimalType;
      default -> throw new IllegalArgumentException("unknown option '" + name + "'");
    };
  }

  /** These options over base (the fields set here win). */
  public SqlOptions over(SqlOptions base) {
    SqlOptions r = new SqlOptions(base == null ? new SqlOptions() : base);
    if (passResultToClient != null) r.passResultToClient = passResultToClient;
    if (charsetName != null) r.charsetName = charsetName;
    if (integerType != null) r.integerType = integerType;
    if (decimalType != null) r.decimalType = decimalType;
    return r;
  }
}
