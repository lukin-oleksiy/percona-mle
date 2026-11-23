-- Demo 3: vectors and the built-in library (as the application account: docker/run.sh sql < 3-demo.sql,
-- after docker/run.sh root --force < 3-setup.sql).

-- 50,000 embeddings of 768 dimensions. The ten nearest to one of them, in SQL, exactly and with HNSW:
SET @q = (SELECT embedding FROM vecs WHERE id = 7);

SELECT GROUP_CONCAT(id) AS sql_exact FROM
  (SELECT id FROM vecs ORDER BY DISTANCE(embedding, @q, 'EUCLIDEAN') LIMIT 10) x;

SELECT GROUP_CONCAT(id) AS hnsw_index FROM
  (SELECT id FROM vecs_hnsw ORDER BY DISTANCE(embedding, @q, 'EUCLIDEAN') LIMIT 10) x;

-- With numpy: all rows arrive as one matrix. The same Python on the JVM worker and on the CPython worker.
CREATE PROCEDURE py_topk(IN q VECTOR(768), OUT r TEXT) LANGUAGE PYTHON AS $$
    import numpy as np
    c = session.run_sql('SELECT id, embedding FROM vecs').fetch_columns()
    m = c['embedding']
    d = np.einsum('ij,ij->i', m, m) - 2 * (m @ q)
    k = np.argpartition(d, 10)[:10]
    r = ','.join(str(int(c['id'][i])) for i in k[np.argsort(d[k])])
$$;

CREATE PROCEDURE cpy_topk(IN q VECTOR(768), OUT r TEXT) LANGUAGE CPYTHON AS $$
    import numpy as np
    c = session.run_sql('SELECT id, embedding FROM vecs').fetch_columns()
    m = c['embedding']
    d = np.einsum('ij,ij->i', m, m) - 2 * (m @ q)
    k = np.argpartition(d, 10)[:10]
    r = ','.join(str(int(c['id'][i])) for i in k[np.argsort(d[k])])
$$;

CALL py_topk(@q, @graalpy);

CALL cpy_topk(@q, @cpython);

SELECT @graalpy AS graalpy, @cpython AS cpython;

-- MLE's vector library is written once in Java (SIMD, threads). From JavaScript:
-- "ten near the query but not near each other" (MMR), which SQL has no syntax for.
CREATE PROCEDURE js_diverse(IN q VECTOR(768), OUT nearest TEXT, OUT diverse TEXT) LANGUAGE JAVASCRIPT AS $$
    const Vectors = Java.type('com.percona.mle.api.Vectors')
    const set = Vectors.load('SELECT id, embedding FROM vecs')
    const ids = (m) => Array.from({ length: m.size() }, (_, i) => m.id(i)).join(',')
    nearest = ids(set.nearest(q, 10, 'euclidean'))
    diverse = ids(set.mmr(q, 10, 0.5))
$$;

CALL js_diverse(@q, @nearest, @diverse);

SELECT @nearest AS nearest, @diverse AS diverse;

-- Related items: the 10 most similar for each of 10,000 vectors. The library, from Python:
CREATE PROCEDURE py_related(OUT r TEXT) LANGUAGE PYTHON AS $$
    import java, time
    V = java.type('com.percona.mle.api.Vectors')
    s = V.load('SELECT id, embedding FROM items ORDER BY id')
    t = time.perf_counter()
    rel = s.related(10, 'cosine')
    r = '%d rows: %.2f s, %d threads' % (len(rel), time.perf_counter() - t, V.threads())
$$;

CALL py_related(@r);

SELECT @r AS vector_library;

-- The same with numpy on the CPython worker (one BLAS thread):
CREATE PROCEDURE cpy_related(OUT r TEXT) LANGUAGE CPYTHON AS $$
    import time
    import numpy as np
    c = session.run_sql('SELECT id, embedding FROM items ORDER BY id').fetch_columns()
    m = c['embedding'] / np.linalg.norm(c['embedding'], axis=1, keepdims=True)
    t = time.perf_counter()
    best = []
    for lo in range(0, len(m), 1024):
        sc = m[lo:lo + 1024] @ m.T
        sc[np.arange(len(sc)), np.arange(lo, lo + len(sc))] = -np.inf
        best.append(np.argpartition(-sc, 10, axis=1)[:, :10])
    r = '%d rows: %.2f s' % (sum(len(b) for b in best), time.perf_counter() - t)
$$;

CALL cpy_related(@r);

SELECT @r AS numpy_on_cpython;
