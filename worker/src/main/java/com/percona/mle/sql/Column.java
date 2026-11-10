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

import com.percona.mle.api.MleSql;
import java.util.Set;

/** A result column's metadata (Oracle's Column object). */
public final class Column {
  private static final int ZEROFILL_FLAG = 64;
  private static final int ENUM_FLAG = 256;
  private static final int SET_FLAG = 2048;
  private static final Set<String> NUMERIC = Set.of("TINYINT", "SMALLINT", "MEDIUMINT", "INT", "BIGINT", "YEAR",
      "BIT", "FLOAT", "DOUBLE", "DECIMAL");

  private final MleSql.Column c;
  private final String type;

  public Column(MleSql.Column c) {
    this.c = c;
    this.type = typeOf(c);
  }

  /**
   * The X DevAPI type names that Oracle's getType() shows (STRING, INT, ...): TINYINT, SMALLINT,
   * MEDIUMINT, INT (also YEAR), BIGINT, BIT, FLOAT, DOUBLE, DECIMAL, JSON, STRING, BYTES, TIME,
   * DATE, DATETIME, TIMESTAMP, SET, ENUM, GEOMETRY, and VECTOR.
   */
  private static String typeOf(MleSql.Column c) {
    if ((c.flags() & ENUM_FLAG) != 0) return "ENUM";
    if ((c.flags() & SET_FLAG) != 0) return "SET";
    return switch (c.type()) {
      case "TINYINT", "SMALLINT", "MEDIUMINT", "INT", "BIGINT", "BIT", "FLOAT", "DOUBLE", "DECIMAL", "JSON", "TIME",
           "DATE", "DATETIME", "TIMESTAMP", "SET", "ENUM", "GEOMETRY", "VECTOR" -> c.type();
      case "YEAR" -> "INT";
      default -> "binary".equals(c.charset()) ? "BYTES" : "STRING";
    };
  }

  /** The column's name in its table (or the expression's name). */
  public String getColumnName() {
    return c.originalName().isEmpty() ? c.name() : c.originalName();
  }

  /** The alias, or the name if none. */
  public String getColumnLabel() {
    return c.name();
  }

  public String getSchemaName() {
    return c.schema();
  }

  public String getTableName() {
    return c.originalTable();
  }

  /** The table's alias, or its name if none. */
  public String getTableLabel() {
    return c.table();
  }

  public String getType() {
    return type;
  }

  /** The MySQL type name (VARCHAR, INT, BLOB ...). */
  public String getSqlType() {
    return c.type();
  }

  public long getLength() {
    return c.length();
  }

  public int getFractionalDigits() {
    return c.decimals();
  }

  public String getCharacterSetName() {
    return c.charset();
  }

  public String getCollationName() {
    return c.collation();
  }

  public boolean isNumberSigned() {
    return NUMERIC.contains(c.type()) && !c.unsigned();
  }

  /** ZEROFILL numbers and fixed-length CHAR/BINARY. */
  public boolean isPadded() {
    return (c.flags() & ZEROFILL_FLAG) != 0 || c.type().equals("CHAR");
  }

  public boolean isUnsigned() {
    return c.unsigned();
  }

  public int getFlags() {
    return c.flags();
  }

  @Override
  public String toString() {
    return getColumnLabel();
  }
}
