-- Demo 3, setup (as root: docker/run.sh root --force < 3-setup.sql; not shown): 50,000 embeddings of 768
-- dimensions with and without an HNSW index, and 10,000 more for "related items". About a minute.
-- The 50,000 x 768 matrix is larger than the default limit of a routine's SQL result (64 MB).
SET PERSIST mle.sql_result_max = 1073741824;
USE app;
DROP PROCEDURE IF EXISTS py_topk; DROP PROCEDURE IF EXISTS cpy_topk; DROP PROCEDURE IF EXISTS js_diverse;
DROP PROCEDURE IF EXISTS py_related; DROP PROCEDURE IF EXISTS cpy_related; DROP PROCEDURE IF EXISTS fill;
DROP TABLE IF EXISTS vecs, vecs_hnsw, items;
CREATE TABLE vecs (id BIGINT UNSIGNED PRIMARY KEY, embedding VECTOR(768) NOT NULL);
CREATE TABLE vecs_hnsw (id BIGINT UNSIGNED PRIMARY KEY, embedding VECTOR(768) NOT NULL,
  VECTOR KEY vk (embedding) TYPE hnsw (M = 16));
CREATE TABLE items (id BIGINT UNSIGNED PRIMARY KEY, embedding VECTOR(768) NOT NULL);
GRANT CREATE_CPYTHON_ROUTINE ON *.* TO root@localhost;
DELIMITER //
CREATE PROCEDURE fill(IN t VARCHAR(20), IN n INT, IN normal BOOL) LANGUAGE CPYTHON AS $$
    import numpy as np
    rng = np.random.default_rng(42 if not normal else 7)
    for lo in range(0, n, 1000):
        k = min(1000, n - lo)
        m = rng.standard_normal((k, 768), dtype=np.float32) if normal else rng.random((k, 768), dtype=np.float32)
        session.start_transaction()
        session.execute_many('INSERT INTO %s VALUES (?, ?)' % t, [(lo + i, m[i]) for i in range(k)])
        session.commit()
$$ //
DELIMITER ;
CALL fill('vecs', 50000, FALSE);
CALL fill('items', 10000, TRUE);
INSERT INTO vecs_hnsw SELECT * FROM vecs;
DROP PROCEDURE fill;
GRANT ALL ON app.* TO app;
