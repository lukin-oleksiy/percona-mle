-- Demo 2, setup (as root: docker/run.sh root --force < 2-setup.sql; not shown): a small shop, and 50,000 embeddings of 768 dimensions with and
-- without an HNSW index. Filling them takes a minute or two.
-- The 50,000 x 768 matrix is larger than the default limit of a routine's SQL result (64 MB).
SET PERSIST mle.sql_result_max = 1073741824;
USE app;
DROP PROCEDURE IF EXISTS py_rank; DROP PROCEDURE IF EXISTS java_book; DROP PROCEDURE IF EXISTS js_order;
DROP PROCEDURE IF EXISTS py_topk; DROP PROCEDURE IF EXISTS js_topk; DROP PROCEDURE IF EXISTS js_diverse;
DROP PROCEDURE IF EXISTS fill_vecs;
DROP TABLE IF EXISTS orders, products, vecs, vecs_hnsw;
CREATE TABLE products (id INT PRIMARY KEY, name VARCHAR(30), price DECIMAL(10,2), embedding VECTOR(4) NOT NULL);
INSERT INTO products VALUES
  (1, 'espresso beans', 12.40, STRING_TO_VECTOR('[0.9, 0.1, 0.0, 0.1]')),
  (2, 'filter coffee',   9.90, STRING_TO_VECTOR('[0.8, 0.2, 0.1, 0.0]')),
  (3, 'green tea',       7.25, STRING_TO_VECTOR('[0.3, 0.9, 0.0, 0.1]')),
  (4, 'black tea',       6.10, STRING_TO_VECTOR('[0.4, 0.8, 0.1, 0.0]')),
  (5, 'coffee grinder', 49.00, STRING_TO_VECTOR('[0.7, 0.0, 0.7, 0.1]')),
  (6, 'tea pot',        24.50, STRING_TO_VECTOR('[0.1, 0.6, 0.7, 0.0]')),
  (7, 'dark chocolate',  3.80, STRING_TO_VECTOR('[0.3, 0.2, 0.0, 0.9]'));
CREATE TABLE orders (id INT AUTO_INCREMENT PRIMARY KEY, product_id INT, price DECIMAL(10,2));
CREATE TABLE vecs (id BIGINT UNSIGNED PRIMARY KEY, embedding VECTOR(768) NOT NULL);
CREATE TABLE vecs_hnsw (id BIGINT UNSIGNED PRIMARY KEY, embedding VECTOR(768) NOT NULL,
  VECTOR KEY vk (embedding) TYPE hnsw (M = 16));
DELIMITER //
CREATE PROCEDURE fill_vecs(IN n INT) LANGUAGE PYTHON AS $$
    import numpy as np
    rng = np.random.default_rng(42)
    for start in range(0, n, 1000):
        session.start_transaction()
        session.execute_many('INSERT INTO vecs VALUES (?, ?)',
                             [(i, rng.random(768, dtype=np.float32)) for i in range(start, min(start + 1000, n))])
        session.commit()
$$ //
DELIMITER ;
CALL fill_vecs(50000);
INSERT INTO vecs_hnsw SELECT * FROM vecs;
DROP PROCEDURE fill_vecs;
GRANT ALL ON app.* TO app;
