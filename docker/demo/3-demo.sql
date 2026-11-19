-- Demo 3: routines that misbehave (as the application account, after docker/run.sh root --force < 3-setup.sql).
-- The step marked "session 2" runs in a second connection while the statement before it is running.

-- How long has the server been up?
SHOW GLOBAL STATUS LIKE 'Uptime';

-- A routine cannot read the server's files (or open the network, or start processes).
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

-- Endless loops in Python, Java and JavaScript.
CREATE FUNCTION spin_py() RETURNS INT LANGUAGE PYTHON AS $$
    while True:
        pass
$$;

CREATE FUNCTION spin_java() RETURNS INT LANGUAGE JAVA AS $$ while (true) {} $$;

CREATE FUNCTION spin_js() RETURNS INT LANGUAGE JAVASCRIPT AS $$ for (;;) {} $$;

-- The statement time limit stops them.
SELECT /*+ MAX_EXECUTION_TIME(1500) */ spin_py();

SELECT /*+ MAX_EXECUTION_TIME(1500) */ spin_java();

SELECT /*+ MAX_EXECUTION_TIME(1500) */ spin_js();

-- So does KILL QUERY from another session.
--@ background
SELECT spin_py();

--@ session2
KILL QUERY @first_session;

-- Native code that crashes: numpy reading through a broken view of memory.
CREATE FUNCTION crash() RETURNS DOUBLE LANGUAGE PYTHON AS $$
    import numpy as np
    bad = np.lib.stride_tricks.as_strided(np.zeros(1), shape=(2,), strides=(1 << 46,))
    return float(bad.sum())
$$;

SELECT crash();

-- A routine that takes all the memory it can get.
CREATE FUNCTION hog() RETURNS INT LANGUAGE JAVASCRIPT AS $$
    const keep = []
    for (;;) keep.push(new Array(1000000).fill(1.5))
$$;

SELECT hog();

-- Only the session's worker was lost each time. All three languages answer:
CREATE FUNCTION alive_py() RETURNS TEXT LANGUAGE PYTHON AS $$ return 'Python' $$;

CREATE FUNCTION alive_java() RETURNS TEXT LANGUAGE JAVA AS $$ return "Java"; $$;

CREATE FUNCTION alive_js() RETURNS TEXT LANGUAGE JAVASCRIPT AS $$ return 'JavaScript' $$;

SELECT alive_py(), alive_java(), alive_js();

-- mysqld never stopped: its uptime went on, and it counted what happened.
SHOW GLOBAL STATUS WHERE Variable_name IN ('Uptime', 'mle_status', 'mle_worker_failures', 'mle_oom_errors');

-- The server's log names each cause.
--@ shell docker logs $NAME 2>&1 | grep "Component mle reported" | tail -3 | sed "s/.*reported: //" | fold -w 120
