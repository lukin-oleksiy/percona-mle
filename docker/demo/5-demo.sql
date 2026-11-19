-- Demo 5: the CPython worker (as the application account, on a container started with MLE_WORKER=CPYTHON,
-- after docker/run.sh root --force < 5-setup.sql).

-- The same server and component; this one runs Python routines on the distribution's CPython.
SHOW GLOBAL STATUS WHERE Variable_name IN ('mle_status', 'mle_languages_supported');

-- The Python of the first demo, unchanged.
CREATE FUNCTION py_hello(name VARCHAR(20)) RETURNS VARCHAR(60) LANGUAGE PYTHON AS $$
    return f'Hello {name}, from Python'
$$;

SELECT py_hello('Percona') AS python;

CREATE FUNCTION py_lev(a VARCHAR(64), b VARCHAR(64)) RETURNS INT DETERMINISTIC LANGUAGE PYTHON AS $$
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]
$$;

SELECT COUNT(*) AS pairs, SUM(py_lev(a, b)) AS python FROM words;

-- numpy: a VECTOR argument arrives as a numpy array.
CREATE FUNCTION cosine(a VECTOR(3), b VECTOR(3)) RETURNS DOUBLE DETERMINISTIC LANGUAGE PYTHON AS $$
    import numpy as np
    return float(a @ b / (np.linalg.norm(a) * np.linalg.norm(b)))
$$;

SELECT cosine(STRING_TO_VECTOR('[1, 2, 3]'), STRING_TO_VECTOR('[3, 2, 1]')) AS cosine;

-- SQL from Python, the same API as on the JVM build: read rows, compute, write the results back.
CREATE PROCEDURE save_distances() MODIFIES SQL DATA LANGUAGE PYTHON AS $$
    def lev(a, b):
        prev = list(range(len(b) + 1))
        for i, ca in enumerate(a, 1):
            cur = [i]
            for j, cb in enumerate(b, 1):
                cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
            prev = cur
        return prev[-1]
    rows = session.run_sql("SELECT n, a, b FROM words").fetch_all()
    session.run_sql("DELETE FROM distances")
    session.execute_many("INSERT INTO distances (n, d) VALUES (?, ?)", [(n, lev(a, b)) for n, a, b in rows])
$$;

CALL save_distances();

SELECT d AS distance, COUNT(*) AS pairs FROM distances GROUP BY d ORDER BY d;

-- What a worker costs: the memory of the process that ran all of this, numpy loaded.
CREATE FUNCTION worker_memory() RETURNS TEXT LANGUAGE PYTHON AS $$
    import numpy
    pss = next(int(l.split()[1]) for l in open('/proc/self/smaps_rollup') if l.startswith('Pss:'))
    rss = next(int(l.split()[1]) for l in open('/proc/self/status') if l.startswith('VmRSS:'))
    return f'RSS {rss // 1024} MB, PSS {pss // 1024} MB'
$$;

SELECT worker_memory();

-- The same sandbox and limits as the JVM build.
CREATE FUNCTION try_escape() RETURNS TEXT LANGUAGE PYTHON AS $$
    out = []
    for path in ('/etc/passwd', '/var/lib/mysql/auto.cnf'):
        try:
            open(path).read()
            out.append(path + ': read')
        except OSError as e:
            out.append(path + ': ' + type(e).__name__)
    return ', '.join(out)
$$;

SELECT try_escape();

CREATE FUNCTION spin_py() RETURNS INT LANGUAGE PYTHON AS $$
    while True:
        pass
$$;

SELECT /*+ MAX_EXECUTION_TIME(1500) */ spin_py();

-- Python only: Java and JavaScript need the JVM build.
CREATE FUNCTION java_hello() RETURNS INT LANGUAGE JAVA AS $$ return 1; $$;
