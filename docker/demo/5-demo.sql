-- Demo 5: routines that misbehave (as the application account, after docker/run.sh root --force < 5-setup.sql).
-- The step marked "session 2" runs in a second connection while the statement before it is running.

-- How long has the server been up?
SHOW GLOBAL STATUS LIKE 'Uptime';

-- A routine cannot read the server's files (or open the network, or start processes): on the CPython worker,
CREATE FUNCTION cpy_try_escape() RETURNS TEXT LANGUAGE CPYTHON AS $$
    out = []
    for path in ('/etc/passwd', '/var/lib/mysql/auto.cnf'):
        try:
            open(path).read()
            out.append(path + ': read')
        except OSError as e:
            out.append(path + ': ' + type(e).__name__)
    return ', '.join(out)
$$;

SELECT cpy_try_escape();

-- Endless loops on every worker: GraalPy, CPython, Java, V8.
CREATE FUNCTION spin_py() RETURNS INT LANGUAGE PYTHON AS $$
    while True:
        pass
$$;

CREATE FUNCTION spin_cpy() RETURNS INT LANGUAGE CPYTHON AS $$
    while True:
        pass
$$;

CREATE FUNCTION spin_java() RETURNS INT LANGUAGE JAVA AS $$ while (true) {} $$;

CREATE FUNCTION spin_v8() RETURNS INT LANGUAGE JS AS $$ for (;;) {} $$;

-- The statement time limit stops them.
SELECT /*+ MAX_EXECUTION_TIME(1500) */ spin_py();

SELECT /*+ MAX_EXECUTION_TIME(1500) */ spin_cpy();

SELECT /*+ MAX_EXECUTION_TIME(1500) */ spin_java();

SELECT /*+ MAX_EXECUTION_TIME(1500) */ spin_v8();

-- So does KILL QUERY from another session.
--@ background
SELECT spin_cpy();

--@ session2
KILL QUERY @first_session;

-- Native code that crashes: numpy reading through a broken view of memory, on the CPython worker.
CREATE FUNCTION crash_cpy() RETURNS DOUBLE LANGUAGE CPYTHON AS $$
    import numpy as np
    bad = np.lib.stride_tricks.as_strided(np.zeros(1), shape=(2,), strides=(1 << 46,))
    return float(bad.sum())
$$;

SELECT crash_cpy();

-- Routines that take all the memory they can get: on V8, and on the JVM worker.
CREATE FUNCTION hog_v8() RETURNS INT LANGUAGE JS AS $$
    const keep = []
    for (;;) keep.push(new Array(1000000).fill(1.5))
$$;

SELECT hog_v8();

CREATE FUNCTION hog_py() RETURNS INT LANGUAGE PYTHON AS $$
    keep = []
    while True:
        keep.append([1.5] * 1000000)
$$;

SELECT hog_py();

-- Only the session's worker was lost each time. Every worker answers:
CREATE FUNCTION alive_py() RETURNS TEXT LANGUAGE PYTHON AS $$ return 'GraalPy' $$;

CREATE FUNCTION alive_cpy() RETURNS TEXT LANGUAGE CPYTHON AS $$ return 'CPython' $$;

CREATE FUNCTION alive_java() RETURNS TEXT LANGUAGE JAVA AS $$ return "Java"; $$;

CREATE FUNCTION alive_gjs() RETURNS TEXT LANGUAGE JAVASCRIPT AS $$ return 'GraalJS' $$;

CREATE FUNCTION alive_v8() RETURNS TEXT LANGUAGE JS AS $$ return 'V8' $$;

SELECT alive_py(), alive_cpy(), alive_java(), alive_gjs(), alive_v8();

-- mysqld never stopped: its uptime went on, and it counted what happened.
SHOW GLOBAL STATUS WHERE Variable_name IN ('Uptime', 'mle_status', 'mle_worker_failures', 'mle_oom_errors');

-- The server's log names each cause.
--@ shell docker logs $NAME 2>&1 | grep "Component mle reported" | tail -4 | sed "s/.*reported: //" | fold -w 120
