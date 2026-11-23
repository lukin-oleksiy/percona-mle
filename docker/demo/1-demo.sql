-- Demo 1: one function on every worker (as the application account: docker/run.sh sql < 1-demo.sql,
-- after docker/run.sh root --force < 1-setup.sql).

-- One server, one component, three kinds of worker; the routine's LANGUAGE chooses.
SHOW GLOBAL STATUS WHERE Variable_name IN ('mle_status', 'mle_languages_supported');

-- The same function five times: Python on the JVM worker (GraalPy) and on the CPython worker,
CREATE FUNCTION py_hello(name VARCHAR(20)) RETURNS VARCHAR(60) LANGUAGE PYTHON AS $$
    import sys
    return f'Hello {name}, from {sys.implementation.name}'
$$;

CREATE FUNCTION cpy_hello(name VARCHAR(20)) RETURNS VARCHAR(60) LANGUAGE CPYTHON AS $$
    import sys
    return f'Hello {name}, from {sys.implementation.name}'
$$;

-- JavaScript on the JVM worker (GraalJS) and on the V8 worker, and Java.
CREATE FUNCTION gjs_hello(name VARCHAR(20)) RETURNS VARCHAR(60) LANGUAGE JAVASCRIPT AS $$
    return `Hello ${name}, from GraalJS`
$$;

CREATE FUNCTION v8_hello(name VARCHAR(20)) RETURNS VARCHAR(60) LANGUAGE JS AS $$
    return `Hello ${name}, from V8`
$$;

CREATE FUNCTION java_hello(name VARCHAR(20)) RETURNS VARCHAR(60) LANGUAGE JAVA AS $$
    return "Hello " + name + ", from Java " + Runtime.version().feature();
$$;

SELECT 'PYTHON' AS language, py_hello('Percona') AS answer UNION ALL SELECT 'CPYTHON', cpy_hello('Percona')
UNION ALL SELECT 'JAVASCRIPT', gjs_hello('Percona') UNION ALL SELECT 'JS', v8_hello('Percona')
UNION ALL SELECT 'JAVA', java_hello('Percona');

-- Real logic: the edit distance of two strings (Levenshtein). One Python body, two workers:
CREATE FUNCTION py_lev(a VARCHAR(64), b VARCHAR(64)) RETURNS INT DETERMINISTIC LANGUAGE PYTHON AS $$
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]
$$;

CREATE FUNCTION cpy_lev(a VARCHAR(64), b VARCHAR(64)) RETURNS INT DETERMINISTIC LANGUAGE CPYTHON AS $$
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]
$$;

-- The same algorithm also exists as an SQL stored function, in Java, and in JavaScript for both engines.
SELECT sql_lev('kitten', 'sitting') AS `sql`, py_lev('kitten', 'sitting') AS graalpy,
       cpy_lev('kitten', 'sitting') AS cpython, java_lev('kitten', 'sitting') AS java,
       gjs_lev('kitten', 'sitting') AS graaljs, v8_lev('kitten', 'sitting') AS v8;

--@ hidden (the workers' code warms up, as in a running application)
SELECT SUM(py_lev(a, b)) + SUM(cpy_lev(a, b)) + SUM(java_lev(a, b)) + SUM(gjs_lev(a, b)) + SUM(v8_lev(a, b)) FROM words;

-- Now for 20,000 pairs of strings. The SQL stored function first:
SELECT COUNT(*) AS pairs, SUM(sql_lev(a, b)) AS `sql` FROM words;

-- Then each worker:
SELECT SUM(java_lev(a, b)) AS java FROM words;

SELECT SUM(v8_lev(a, b)) AS v8 FROM words;

SELECT SUM(gjs_lev(a, b)) AS graaljs FROM words;

SELECT SUM(py_lev(a, b)) AS graalpy FROM words;

SELECT SUM(cpy_lev(a, b)) AS cpython FROM words;

-- This session holds one worker of each kind it used: the JVM worker, the CPython worker and the V8 worker.
SHOW GLOBAL STATUS WHERE Variable_name IN ('mle_sessions', 'mle_workers');
