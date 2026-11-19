# Writes the file IN as a C++ raw string literal NAME into the header OUT (cmake -P).
FILE(READ ${IN} text)
STRING(LENGTH "${text}" length)
# Raw string literals are limited by compilers to ~64 KiB per piece on some; split in 16 KiB parts.
SET(out "// Generated from ${IN} by embed.cmake: do not edit.\n#pragma once\ninline constexpr const char ${NAME}[] =\n")
SET(pos 0)
WHILE(pos LESS length)
  STRING(SUBSTRING "${text}" ${pos} 16384 part)
  STRING(APPEND out "R\"mle_js(${part})mle_js\"\n")
  MATH(EXPR pos "${pos} + 16384")
ENDWHILE()
STRING(APPEND out ";\n")
FILE(WRITE ${OUT} "${out}")
