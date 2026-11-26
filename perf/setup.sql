-- Data, routines and cases of the performance regression test (perf/regression.py).
-- Loaded once into the test's data directory by the mysql client (DELIMITER is the client's).
-- The cases are in perf.cases at the end: name, statement, warm-up runs, timed runs.

CREATE DATABASE perf;
USE perf;
GRANT CREATE_JAVASCRIPT_ROUTINE, CREATE_PYTHON_ROUTINE, CREATE_JAVA_ROUTINE ON *.* TO root@localhost;
SET SESSION cte_max_recursion_depth = 1000000;

CREATE TABLE t AS WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 500000) SELECT n FROM c;
CREATE TABLE words AS SELECT n, LEFT(SHA2(n, 256), 8) AS a, LEFT(SHA2(n + 1, 256), 8) AS b FROM t WHERE n <= 20000;
CREATE TABLE bench (id INT PRIMARY KEY, v VARCHAR(20), x DOUBLE);
INSERT INTO bench SELECT n, CONCAT('row', n), n * 0.5 FROM t WHERE n <= 100000;
-- Every kind of value the codec converts.
CREATE TABLE mix (i BIGINT PRIMARY KEY, d DECIMAL(20,6), s VARCHAR(100), dt DATETIME(6), tm TIME(6), b DOUBLE, j JSON);
INSERT INTO mix SELECT n, n * 1.000123 - 25000, CONCAT('héllo ', REPEAT('x', n % 40), n),
    TIMESTAMP('2026-01-01 00:00:00') + INTERVAL n * 1000003 MICROSECOND,
    SEC_TO_TIME(n * 7 - 100000), n / 7, JSON_OBJECT('n', n, 'tags', JSON_ARRAY('a', 'b', n))
  FROM t WHERE n <= 50000;
CREATE TABLE vecs (id BIGINT UNSIGNED PRIMARY KEY, embedding VECTOR(768) NOT NULL);
CREATE TABLE w (id INT PRIMARY KEY, a VARCHAR(20), x DOUBLE);
CREATE TABLE wv (id INT PRIMARY KEY, v VECTOR(768));
CREATE TABLE results (id BIGINT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(64), us BIGINT);
CREATE TABLE cases (ord INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(64) UNIQUE, stmt TEXT, warm INT, reps INT);

-- Another account's routines (INVOKER: they run in a worker of their own in the caller's session).
CREATE USER perf_author@localhost;
GRANT ALL ON perf.* TO perf_author@localhost;
GRANT CREATE_JAVASCRIPT_ROUTINE, CREATE_PYTHON_ROUTINE ON *.* TO perf_author@localhost;
-- Accounts that have no kept worker: their first call takes a worker from the pool.
CREATE USER perf_u1@localhost, perf_u2@localhost, perf_u3@localhost, perf_u4@localhost, perf_u5@localhost, perf_u6@localhost;
GRANT EXECUTE ON perf.* TO perf_u1@localhost, perf_u2@localhost, perf_u3@localhost, perf_u4@localhost, perf_u5@localhost, perf_u6@localhost;

-- 1. The call path.
CREATE FUNCTION js_add(a INT, b INT) RETURNS INT LANGUAGE JAVASCRIPT AS $$ return a + b $$;
CREATE FUNCTION py_add(a INT, b INT) RETURNS INT LANGUAGE PYTHON AS $$ return a + b $$;
CREATE FUNCTION java_add(a INT, b INT) RETURNS INT LANGUAGE JAVA AS $$ return a + b; $$;
CREATE DEFINER = perf_author@localhost FUNCTION inv_js_add(a INT, b INT) RETURNS INT SQL SECURITY INVOKER
  LANGUAGE JAVASCRIPT AS $$ return a + b $$;

-- 2. Strings in, compute.
CREATE FUNCTION js_lev(a VARCHAR(64), b VARCHAR(64)) RETURNS INT LANGUAGE JAVASCRIPT AS $$
  let prev = Array.from({ length: b.length + 1 }, (_, j) => j)
  for (let i = 1; i <= a.length; i++) {
    const cur = [i]
    for (let j = 1; j <= b.length; j++)
      cur[j] = Math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1))
    prev = cur
  }
  return prev[b.length]
$$;
CREATE FUNCTION py_lev(a VARCHAR(64), b VARCHAR(64)) RETURNS INT LANGUAGE PYTHON AS $$
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]
$$;
CREATE FUNCTION java_lev(a VARCHAR(64), b VARCHAR(64)) RETURNS INT LANGUAGE JAVA AS $$
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
$$;

-- 3. Arguments and results of every kind.
CREATE FUNCTION js_types(i BIGINT, d DECIMAL(20,6), s VARCHAR(100), dt DATETIME(6), tm TIME(6), b DOUBLE, j JSON)
  RETURNS INT LANGUAGE JAVASCRIPT AS $$ return [i, d, s, dt, tm, b, j].join('|').length $$;
CREATE FUNCTION py_types(i BIGINT, d DECIMAL(20,6), s VARCHAR(100), dt DATETIME(6), tm TIME(6), b DOUBLE, j JSON)
  RETURNS INT LANGUAGE PYTHON AS $$ return len('|'.join(str(v) for v in (i, d, s, dt, tm, b, j))) $$;
CREATE FUNCTION java_types(i BIGINT, d DECIMAL(20,6), s VARCHAR(100), dt DATETIME(6), tm TIME(6), b DOUBLE, j JSON)
  RETURNS INT LANGUAGE JAVA AS $$ return ("" + i + "|" + d + "|" + s + "|" + dt + "|" + tm + "|" + b + "|" + j).length(); $$;
CREATE FUNCTION js_dec(d DECIMAL(20,6)) RETURNS DECIMAL(20,6) LANGUAGE JAVASCRIPT AS $$ return d $$;
CREATE FUNCTION py_dec(d DECIMAL(20,6)) RETURNS DECIMAL(20,6) LANGUAGE PYTHON AS $$ return d $$;
CREATE FUNCTION java_dec(d DECIMAL(20,6)) RETURNS DECIMAL(20,6) LANGUAGE JAVA AS $$ return d; $$;
CREATE FUNCTION js_time(t TIME(6)) RETURNS TIME(6) LANGUAGE JAVASCRIPT AS $$ return t $$;
CREATE FUNCTION py_time(t TIME(6)) RETURNS TIME(6) LANGUAGE PYTHON AS $$ return t $$;
CREATE FUNCTION java_time(t TIME(6)) RETURNS TIME(6) LANGUAGE JAVA AS $$ return t; $$;
CREATE FUNCTION js_str(s VARCHAR(100)) RETURNS VARCHAR(100) LANGUAGE JAVASCRIPT AS $$ return s $$;
CREATE FUNCTION py_str(s VARCHAR(100)) RETURNS VARCHAR(100) LANGUAGE PYTHON AS $$ return s $$;
CREATE FUNCTION java_str(s VARCHAR(100)) RETURNS VARCHAR(100) LANGUAGE JAVA AS $$ return s; $$;

-- 4. Libraries.
CREATE LIBRARY jslib LANGUAGE JAVASCRIPT AS $$ export function twice(x) { return 2 * x } $$;
CREATE LIBRARY pylib LANGUAGE PYTHON AS $$
def twice(x):
    return 2 * x
$$;
CREATE FUNCTION js_lib(a INT) RETURNS INT LANGUAGE JAVASCRIPT USING (perf.jslib AS l) AS $$ return l.twice(a) $$;
CREATE FUNCTION py_lib(a INT) RETURNS INT LANGUAGE PYTHON USING (perf.pylib AS l) AS $$ return l.twice(a) $$;

-- 5. Errors: raised, caught by an SQL handler, 2,000 times.
CREATE FUNCTION py_fail(a INT) RETURNS INT LANGUAGE PYTHON AS $$ raise ValueError('no %d' % a) $$;
CREATE FUNCTION js_fail(a INT) RETURNS INT LANGUAGE JAVASCRIPT AS $$ throw new Error('no ' + a) $$;

DELIMITER //
CREATE PROCEDURE errors(IN which VARCHAR(8), IN n INT)
BEGIN
  DECLARE i INT DEFAULT 0;
  DECLARE x INT;
  DECLARE CONTINUE HANDLER FOR SQLEXCEPTION SET x = -1;
  WHILE i < n DO
    IF which = 'py' THEN SET x = py_fail(i); ELSE SET x = js_fail(i); END IF;
    SET i = i + 1;
  END WHILE;
END//

-- 6. SQL from routines: reading.
CREATE PROCEDURE js_rows(OUT s DOUBLE) LANGUAGE JAVASCRIPT AS $$
  const r = session.runSql('SELECT id, v, x FROM bench'); let t = 0
  for (let row = r.fetchOne(); row; row = r.fetchOne()) t += row.x
  s = t
$$//
CREATE PROCEDURE py_rows(OUT s DOUBLE) LANGUAGE PYTHON AS $$
    t = 0.0
    for row in session.run_sql('SELECT id, v, x FROM bench'):
        t += row.x
    s = t
$$//
CREATE PROCEDURE java_rows(OUT s DOUBLE) LANGUAGE JAVA AS $$
    double t = 0;
    for (Row row : Mle.session().runSql("SELECT id, v, x FROM bench")) t += ((Number) row.get(2)).doubleValue();
    s[0] = t;
$$//
CREATE PROCEDURE js_mix(OUT n INT) LANGUAGE JAVASCRIPT AS $$
  const r = session.runSql('SELECT i, d, s, dt, tm, b, j FROM mix'); let t = 0
  for (let row = r.fetchOne(); row; row = r.fetchOne())
    t += String(row.d).length + row.s.length + (row.dt ? 1 : 0) + (row.tm ? 1 : 0) + (row.j ? 1 : 0)
  n = t
$$//
CREATE PROCEDURE py_mix(OUT n INT) LANGUAGE PYTHON AS $$
    t = 0
    for row in session.run_sql('SELECT i, d, s, dt, tm, b, j FROM mix'):
        t += len(str(row.d)) + len(row.s) + (row.dt is not None) + (row.tm is not None) + (row.j is not None)
    n = t
$$//
CREATE PROCEDURE java_mix(OUT n INT) LANGUAGE JAVA AS $$
    int t = 0;
    for (Row row : Mle.session().runSql("SELECT i, d, s, dt, tm, b, j FROM mix"))
      t += String.valueOf(row.get(1)).length() + String.valueOf(row.get(2)).length()
          + (row.get(3) != null ? 1 : 0) + (row.get(4) != null ? 1 : 0) + (row.get(6) != null ? 1 : 0);
    n[0] = t;
$$//

-- 7. SQL from routines: many small statements.
CREATE PROCEDURE js_stmts(IN k INT, OUT n INT) LANGUAGE JAVASCRIPT AS $$
  let t = 0
  for (let i = 0; i < k; i++) t += session.runSql('SELECT ? + 1 AS v', [i]).fetchOne().v
  n = t
$$//
CREATE PROCEDURE py_stmts(IN k INT, OUT n INT) LANGUAGE PYTHON AS $$
    t = 0
    for i in range(k):
        t += session.run_sql('SELECT ? + 1 AS v', [i]).fetch_one().v
    n = t
$$//
CREATE PROCEDURE java_stmts(IN k INT, OUT n INT) LANGUAGE JAVA AS $$
    long t = 0;
    var s = Mle.session();
    for (int i = 0; i < k; i++) t += ((Number) s.runSql("SELECT ? + 1 AS v", i).fetchOne().get(0)).longValue();
    n[0] = (int) t;
$$//
CREATE PROCEDURE js_prep(IN k INT, OUT n INT) LANGUAGE JAVASCRIPT AS $$
  let t = 0
  const p = session.prepare('SELECT v FROM bench WHERE id = ?')
  for (let i = 1; i <= k; i++) t += p.bind(i).execute().fetchOne().v.length
  p.deallocate()
  n = t
$$//
CREATE PROCEDURE py_prep(IN k INT, OUT n INT) LANGUAGE PYTHON AS $$
    t = 0
    p = session.prepare('SELECT v FROM bench WHERE id = ?')
    for i in range(1, k + 1):
        t += len(p.bind(i).execute().fetch_one().v)
    p.deallocate()
    n = t
$$//
CREATE PROCEDURE java_prep(IN k INT, OUT n INT) LANGUAGE JAVA AS $$
    int t = 0;
    var p = Mle.session().prepare("SELECT v FROM bench WHERE id = ?");
    for (int i = 1; i <= k; i++) t += String.valueOf(p.bind(i).execute().fetchOne().get(0)).length();
    p.deallocate();
    n[0] = t;
$$//

-- 8. SQL from routines: writing (executeMany: 200 rows go row by row, 1,000 as multi-row statements).
CREATE PROCEDURE java_w(n INT) LANGUAGE JAVA AS $$
  var s = Mle.session();
  s.runSql("TRUNCATE w");
  java.util.List<Object[]> rows = new java.util.ArrayList<>();
  for (int i = 0; i < n; i++) rows.add(new Object[] {i, "row" + i, i * 0.5});
  s.startTransaction();
  s.prepare("INSERT INTO w VALUES (?, ?, ?)").executeMany(rows);
  s.commit();
$$//
CREATE PROCEDURE py_w(n INT) LANGUAGE PYTHON AS $$
    session.run_sql("TRUNCATE w")
    rows = [(i, 'row%d' % i, i * 0.5) for i in range(n)]
    session.start_transaction()
    session.execute_many("INSERT INTO w VALUES (?, ?, ?)", rows)
    session.commit()
$$//
CREATE PROCEDURE js_w(n INT) LANGUAGE JAVASCRIPT AS $$
  session.runSql("TRUNCATE w")
  const rows = []
  for (let i = 0; i < n; i++) rows.push([i, 'row' + i, i * 0.5])
  session.startTransaction()
  session.executeMany("INSERT INTO w VALUES (?, ?, ?)", rows)
  session.commit()
$$//
CREATE PROCEDURE py_wv(n INT) LANGUAGE PYTHON AS $$
    import numpy as np
    session.run_sql("TRUNCATE wv")
    rows = [(i, np.full(768, i, dtype=np.float32)) for i in range(n)]
    session.start_transaction()
    session.execute_many("INSERT INTO wv VALUES (?, ?)", rows)
    session.commit()
$$//
CREATE PROCEDURE repeat_w(what VARCHAR(8), n INT, calls INT) BEGIN
  DECLARE i INT DEFAULT 0;
  WHILE i < calls DO
    CASE what WHEN 'java' THEN CALL java_w(n); WHEN 'py' THEN CALL py_w(n); WHEN 'js' THEN CALL js_w(n);
      ELSE CALL py_wv(n); END CASE;
    SET i = i + 1;
  END WHILE;
END//

-- 9. A routine whose SQL calls routines (nested calls, two languages).
CREATE PROCEDURE js_nested(OUT s BIGINT) LANGUAGE JAVASCRIPT AS $$
  s = session.runSql('SELECT SUM(py_add(n, 1)) AS v FROM t WHERE n <= 100000').fetchOne().v
$$//

-- 10. Vectors.
CREATE PROCEDURE fill(IN n INT) LANGUAGE PYTHON AS $$
    import numpy as np
    rng = np.random.default_rng(42)
    session.start_transaction()
    ins = session.prepare('INSERT INTO vecs VALUES (?, ?)')
    for i in range(n):
        ins.bind(i, rng.random(768, dtype=np.float32)).execute()
    ins.deallocate()
    session.commit()
$$//
CREATE PROCEDURE py_topk(IN q VECTOR(768), OUT r TEXT) LANGUAGE PYTHON AS $$
    import numpy as np
    c = session.run_sql('SELECT id, embedding FROM vecs').fetch_columns()
    m = c['embedding']
    d = np.einsum('ij,ij->i', m, m) - 2 * (m @ q)
    k = np.argpartition(d, 10)[:10]
    r = ','.join(str(int(c['id'][i])) for i in k[np.argsort(d[k])])
$$//
CREATE PROCEDURE java_topk(IN q VECTOR(768), OUT ids TEXT) LANGUAGE JAVA AS $$
    long[] best = new long[10];
    double[] bestD = new double[10];
    java.util.Arrays.fill(bestD, Double.MAX_VALUE);
    for (Row r : Mle.session().runSql("SELECT id, embedding FROM vecs")) {
      float[] v = (float[]) r.get(1);
      double d = 0;
      for (int i = 0; i < v.length; i++) d += (v[i] - q[i]) * (v[i] - q[i]);
      if (d < bestD[9]) {
        int k = 9;
        while (k > 0 && bestD[k - 1] > d) { bestD[k] = bestD[k - 1]; best[k] = best[k - 1]; k--; }
        bestD[k] = d; best[k] = ((Number) r.get(0)).longValue();
      }
    }
    StringBuilder sb = new StringBuilder();
    for (long b : best) sb.append(sb.length() > 0 ? "," : "").append(b);
    ids[0] = sb.toString();
$$//
CREATE FUNCTION py_norm(v VECTOR(768)) RETURNS DOUBLE LANGUAGE PYTHON AS $$
    import numpy as np
    return float(np.linalg.norm(v))
$$//

-- The driver: one case = its statement prepared, run `warm` times, then timed `reps` times.
CREATE PROCEDURE run_case(IN p_name VARCHAR(64))
BEGIN
  DECLARE i INT DEFAULT 0;
  DECLARE p_warm, p_reps INT;
  DECLARE t0 DATETIME(6);
  SELECT stmt, warm, reps INTO @perf_stmt, p_warm, p_reps FROM cases WHERE name = p_name;
  PREPARE perf_s FROM @perf_stmt;
  WHILE i < p_warm DO EXECUTE perf_s; SET i = i + 1; END WHILE;
  SET i = 0;
  WHILE i < p_reps DO
    SET t0 = SYSDATE(6);
    EXECUTE perf_s;
    INSERT INTO results (name, us) VALUES (p_name, TIMESTAMPDIFF(MICROSECOND, t0, SYSDATE(6)));
    SET i = i + 1;
  END WHILE;
  DEALLOCATE PREPARE perf_s;
END//
DELIMITER ;

CALL fill(20000);

INSERT INTO cases (name, stmt, warm, reps) VALUES
  ('scan',        'SELECT SUM(n + 1) INTO @x FROM t', 2, 7),
  ('call_js',     'SELECT SUM(js_add(n, 1)) INTO @x FROM t', 3, 7),
  ('call_py',     'SELECT SUM(py_add(n, 1)) INTO @x FROM t', 3, 7),
  ('call_java',   'SELECT SUM(java_add(n, 1)) INTO @x FROM t', 3, 7),
  ('call_invoker','SELECT SUM(inv_js_add(n, 1)) INTO @x FROM t', 3, 7),
  ('lev_js',      'SELECT SUM(js_lev(a, b)) INTO @x FROM words', 10, 9),
  ('lev_py',      'SELECT SUM(py_lev(a, b)) INTO @x FROM words', 10, 9),
  ('lev_java',    'SELECT SUM(java_lev(a, b)) INTO @x FROM words', 10, 9),
  ('types_js',    'SELECT SUM(js_types(i, d, s, dt, tm, b, j)) INTO @x FROM mix', 5, 9),
  ('types_py',    'SELECT SUM(py_types(i, d, s, dt, tm, b, j)) INTO @x FROM mix', 5, 9),
  ('types_java',  'SELECT SUM(java_types(i, d, s, dt, tm, b, j)) INTO @x FROM mix', 5, 9),
  ('ret_js',      'SELECT SUM(js_dec(d)), MAX(js_time(tm)), MAX(js_str(s)) INTO @x, @y, @z FROM mix', 5, 9),
  ('ret_py',      'SELECT SUM(py_dec(d)), MAX(py_time(tm)), MAX(py_str(s)) INTO @x, @y, @z FROM mix', 5, 9),
  ('rdec_py',     'SELECT SUM(py_dec(d)) INTO @x FROM mix', 5, 9),
  ('rdec_js',     'SELECT SUM(js_dec(d)) INTO @x FROM mix', 5, 9),
  ('ret_java',    'SELECT SUM(java_dec(d)), MAX(java_time(tm)), MAX(java_str(s)) INTO @x, @y, @z FROM mix', 5, 9),
  ('lib_js',      'SELECT SUM(js_lib(n)) INTO @x FROM t WHERE n <= 200000', 3, 9),
  ('lib_py',      'SELECT SUM(py_lib(n)) INTO @x FROM t WHERE n <= 200000', 3, 9),
  ('errors_py',   'CALL errors(''py'', 2000)', 3, 7),
  ('errors_js',   'CALL errors(''js'', 2000)', 3, 7),
  ('rows_js',     'CALL js_rows(@x)', 10, 9),
  ('rows_py',     'CALL py_rows(@x)', 10, 9),
  ('rows_java',   'CALL java_rows(@x)', 10, 9),
  ('mix_js',      'CALL js_mix(@x)', 10, 9),
  ('mix_py',      'CALL py_mix(@x)', 10, 9),
  ('mix_java',    'CALL java_mix(@x)', 10, 9),
  ('stmts_js',    'CALL js_stmts(5000, @x)', 5, 9),
  ('stmts_py',    'CALL py_stmts(5000, @x)', 5, 9),
  ('stmts_java',  'CALL java_stmts(5000, @x)', 5, 9),
  ('prep_js',     'CALL js_prep(5000, @x)', 5, 9),
  ('prep_py',     'CALL py_prep(5000, @x)', 5, 9),
  ('prep_java',   'CALL java_prep(5000, @x)', 5, 9),
  ('write200_java',  'CALL repeat_w(''java'', 200, 20)', 3, 7),
  ('write1000_java', 'CALL repeat_w(''java'', 1000, 20)', 3, 7),
  ('write200_py',    'CALL repeat_w(''py'', 200, 20)', 3, 7),
  ('write1000_py',   'CALL repeat_w(''py'', 1000, 20)', 3, 7),
  ('write1000_js',   'CALL repeat_w(''js'', 1000, 20)', 3, 7),
  ('writevec_py',    'CALL repeat_w(''pyv'', 200, 10)', 3, 7),
  ('nested_js_py','CALL js_nested(@x)', 3, 7),
  ('topk_numpy',  'CALL py_topk((SELECT embedding FROM vecs WHERE id = 7), @x)', 5, 9),
  ('topk_java',   'CALL java_topk((SELECT embedding FROM vecs WHERE id = 7), @x)', 5, 9),
  ('vector_arg',  'SELECT SUM(py_norm(embedding)) INTO @x FROM vecs', 3, 7);
