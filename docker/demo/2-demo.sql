-- Demo 2: one CALL, three languages, and vectors (as the application account: docker/run.sh sql < 2-demo.sql,
-- after docker/run.sh root --force < 2-setup.sql).

-- A small shop: products with embeddings.
SELECT id, name, price, VECTOR_DIM(embedding) AS embedding_dimensions FROM products;

-- Python reads the embeddings with SQL and ranks the products with numpy.
CREATE PROCEDURE py_rank(IN q VECTOR(4), IN k INT, OUT ids JSON) LANGUAGE PYTHON AS $$
    import numpy as np
    c = session.run_sql('SELECT id, embedding FROM products').fetch_columns()
    m = c['embedding']
    score = (m @ q) / (np.linalg.norm(m, axis=1) * np.linalg.norm(q))
    ids = [int(c['id'][i]) for i in np.argsort(-score)[:k]]
$$;

-- Java books the order in a transaction, with exact decimal arithmetic.
CREATE PROCEDURE java_book(IN ids JSON, OUT total DECIMAL(10,2)) LANGUAGE JAVA AS $$
    var s = Mle.session();
    var sum = java.math.BigDecimal.ZERO;
    s.startTransaction();
    for (Row r : s.runSql("SELECT id, price FROM products WHERE JSON_CONTAINS(?, CAST(id AS JSON))", ids)) {
      s.runSql("INSERT INTO orders (product_id, price) VALUES (?, ?)", r.get(0), r.get(1));
      sum = sum.add((java.math.BigDecimal) r.get(1));
    }
    s.commit();
    total[0] = sum.multiply(new java.math.BigDecimal("1.07")).setScale(2, java.math.RoundingMode.HALF_UP);
$$;

-- JavaScript calls both: one procedure, one round trip for the application.
CREATE PROCEDURE js_order(IN q VECTOR(4), OUT report TEXT) LANGUAGE JAVASCRIPT AS $$
    session.runSql('CALL py_rank(?, 3, @ids)', [q])
    session.runSql('CALL java_book(@ids, @total)')
    const row = session.runSql('SELECT @ids AS ids, @total AS total').fetchOne()
    report = `products ${row.ids} ordered, total with 7 % tax: ${row.total}`
$$;

CALL js_order(STRING_TO_VECTOR('[1, 0.1, 0, 0]'), @report);

SELECT @report;

SELECT o.id, p.name, o.price FROM orders o JOIN products p ON p.id = o.product_id;

-- Vectors at scale: 50,000 embeddings of 768 dimensions. The ten nearest to one of them, in SQL:
SET @q = (SELECT embedding FROM vecs WHERE id = 7);

SELECT GROUP_CONCAT(id) AS sql_exact FROM
  (SELECT id FROM vecs ORDER BY DISTANCE(embedding, @q, 'EUCLIDEAN') LIMIT 10) x;

SELECT GROUP_CONCAT(id) AS hnsw_index FROM
  (SELECT id FROM vecs_hnsw ORDER BY DISTANCE(embedding, @q, 'EUCLIDEAN') LIMIT 10) x;

-- The same with numpy: all rows arrive as one matrix.
CREATE PROCEDURE py_topk(IN q VECTOR(768), OUT r TEXT) LANGUAGE PYTHON AS $$
    import numpy as np
    c = session.run_sql('SELECT id, embedding FROM vecs').fetch_columns()
    m = c['embedding']
    d = np.einsum('ij,ij->i', m, m) - 2 * (m @ q)
    k = np.argpartition(d, 10)[:10]
    r = ','.join(str(int(c['id'][i])) for i in k[np.argsort(d[k])])
$$;

CALL py_topk(@q, @numpy);

SELECT @numpy;

-- MLE's vector library is written once, in Java (SIMD, threads); here JavaScript uses it.
-- "Ten near the query but not near each other" (MMR) is something SQL has no syntax for.
CREATE PROCEDURE js_diverse(IN q VECTOR(768), OUT nearest TEXT, OUT diverse TEXT) LANGUAGE JAVASCRIPT AS $$
    const Vectors = Java.type('com.percona.mle.api.Vectors')
    const set = Vectors.load('SELECT id, embedding FROM vecs')
    const ids = (m) => Array.from({ length: m.size() }, (_, i) => m.id(i)).join(',')
    nearest = ids(set.nearest(q, 10, 'euclidean'))
    diverse = ids(set.mmr(q, 10, 0.5))
$$;

CALL js_diverse(@q, @nearest, @diverse);

SELECT @nearest AS nearest, @diverse AS diverse;
