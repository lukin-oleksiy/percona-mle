-- Demo 1, setup (as root: docker/run.sh root --force < 1-setup.sql; not shown): V8 (js_lang) for comparison, 20,000 pairs of strings, and the
-- Levenshtein distance in SQL, Java, JavaScript and V8 (the demo adds Python).
INSTALL COMPONENT 'file://component_js_lang';
GRANT CREATE_JS_ROUTINE ON *.* TO app;
USE app;
DROP FUNCTION IF EXISTS py_hello; DROP FUNCTION IF EXISTS java_hello; DROP FUNCTION IF EXISTS js_hello;
DROP FUNCTION IF EXISTS sql_lev; DROP FUNCTION IF EXISTS py_lev; DROP FUNCTION IF EXISTS java_lev;
DROP FUNCTION IF EXISTS js_lev; DROP FUNCTION IF EXISTS v8_lev;
DROP TABLE IF EXISTS words;
SET SESSION cte_max_recursion_depth = 100000;
CREATE TABLE words AS WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 20000)
  SELECT n, LEFT(SHA2(n, 256), 8) AS a, LEFT(SHA2(n + 1, 256), 8) AS b FROM c;
DELIMITER //
CREATE DEFINER = app FUNCTION sql_lev(a VARCHAR(64), b VARCHAR(64)) RETURNS INT DETERMINISTIC
BEGIN
  DECLARE la, lb, i, j, c INT;
  DECLARE prev, cur VARBINARY(256);
  SET la = CHAR_LENGTH(a), lb = CHAR_LENGTH(b), prev = '', j = 0;
  WHILE j <= lb DO SET prev = CONCAT(prev, CHAR(j)), j = j + 1; END WHILE;
  SET i = 1;
  WHILE i <= la DO
    SET cur = CHAR(i), j = 1;
    WHILE j <= lb DO
      SET c = LEAST(ORD(SUBSTRING(prev, j + 1, 1)) + 1, ORD(SUBSTRING(cur, j, 1)) + 1,
                    ORD(SUBSTRING(prev, j, 1)) + IF(SUBSTRING(a, i, 1) = SUBSTRING(b, j, 1), 0, 1));
      SET cur = CONCAT(cur, CHAR(c)), j = j + 1;
    END WHILE;
    SET prev = cur, i = i + 1;
  END WHILE;
  RETURN ORD(SUBSTRING(prev, lb + 1, 1));
END //
CREATE DEFINER = app FUNCTION java_lev(a VARCHAR(64), b VARCHAR(64)) RETURNS INT DETERMINISTIC LANGUAGE JAVA AS $$
    int[] prev = new int[b.length() + 1], cur = new int[b.length() + 1];
    for (int j = 0; j <= b.length(); j++) prev[j] = j;
    for (int i = 1; i <= a.length(); i++) {
      cur[0] = i;
      for (int j = 1; j <= b.length(); j++)
        cur[j] = Math.min(Math.min(prev[j] + 1, cur[j - 1] + 1),
                          prev[j - 1] + (a.charAt(i - 1) == b.charAt(j - 1) ? 0 : 1));
      int[] t = prev; prev = cur; cur = t;
    }
    return prev[b.length()];
$$ //
CREATE DEFINER = app FUNCTION js_lev(a VARCHAR(64), b VARCHAR(64)) RETURNS INT DETERMINISTIC LANGUAGE JAVASCRIPT AS $$
  let prev = Array.from({ length: b.length + 1 }, (_, j) => j)
  for (let i = 1; i <= a.length; i++) {
    const cur = [i]
    for (let j = 1; j <= b.length; j++)
      cur[j] = Math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1))
    prev = cur
  }
  return prev[b.length]
$$ //
CREATE DEFINER = app FUNCTION v8_lev(a VARCHAR(64), b VARCHAR(64)) RETURNS INT DETERMINISTIC LANGUAGE JS AS $$
  let prev = Array.from({ length: b.length + 1 }, (_, j) => j)
  for (let i = 1; i <= a.length; i++) {
    const cur = [i]
    for (let j = 1; j <= b.length; j++)
      cur[j] = Math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1))
    prev = cur
  }
  return prev[b.length]
$$ //
DELIMITER ;
