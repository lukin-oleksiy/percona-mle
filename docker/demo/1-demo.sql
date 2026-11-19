-- Demo 1: one function in three languages (as the application account: docker/run.sh sql < 1-demo.sql,
-- after docker/run.sh root --force < 1-setup.sql).

-- A first function, in Python, Java and JavaScript.
CREATE FUNCTION py_hello(name VARCHAR(20)) RETURNS VARCHAR(60) LANGUAGE PYTHON AS $$
    return f'Hello {name}, from Python'
$$;

CREATE FUNCTION java_hello(name VARCHAR(20)) RETURNS VARCHAR(60) LANGUAGE JAVA AS $$
    return "Hello " + name + ", from Java";
$$;

CREATE FUNCTION js_hello(name VARCHAR(20)) RETURNS VARCHAR(60) LANGUAGE JAVASCRIPT AS $$
    return `Hello ${name}, from JavaScript`
$$;

SELECT py_hello('Percona') AS python, java_hello('Percona') AS java, js_hello('Percona') AS javascript;

-- Real logic: the edit distance of two strings (Levenshtein). In Python:
CREATE FUNCTION py_lev(a VARCHAR(64), b VARCHAR(64)) RETURNS INT DETERMINISTIC LANGUAGE PYTHON AS $$
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]
$$;

-- The same algorithm exists as a SQL stored function, in Java, in JavaScript, and in V8 (js_lang).
SELECT sql_lev('kitten', 'sitting') AS `sql`, py_lev('kitten', 'sitting') AS python,
       java_lev('kitten', 'sitting') AS java, js_lev('kitten', 'sitting') AS javascript,
       v8_lev('kitten', 'sitting') AS v8;

--@ hidden (the workers' code warms up, as in a running application)
SELECT SUM(py_lev(a, b)) + SUM(java_lev(a, b)) + SUM(js_lev(a, b)) + SUM(v8_lev(a, b)) FROM words;

-- Now for 20,000 pairs of strings. The SQL stored function first:
SELECT COUNT(*) AS pairs, SUM(sql_lev(a, b)) AS `sql` FROM words;

-- Python, Java and JavaScript in MLE's worker, and V8 inside mysqld:
SELECT SUM(py_lev(a, b)) AS python FROM words;

SELECT SUM(java_lev(a, b)) AS java FROM words;

SELECT SUM(js_lev(a, b)) AS javascript FROM words;

SELECT SUM(v8_lev(a, b)) AS v8 FROM words;

-- One worker process served this session's three languages.
SHOW GLOBAL STATUS WHERE Variable_name IN ('mle_status', 'mle_languages_supported', 'mle_sessions', 'mle_workers');
