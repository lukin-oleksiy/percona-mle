-- Demo 2, setup (as root: docker/run.sh root --force < 2-setup.sql; not shown): a small shop with embeddings.
USE app;
DROP FUNCTION IF EXISTS py_cosine; DROP FUNCTION IF EXISTS js_norm; DROP FUNCTION IF EXISTS java_offer;
DROP FUNCTION IF EXISTS js_stars; DROP PROCEDURE IF EXISTS py_recommend; DROP PROCEDURE IF EXISTS java_order;
DROP PROCEDURE IF EXISTS js_shop; DROP PROCEDURE IF EXISTS py_price_stats;
DROP TABLE IF EXISTS recommendations, order_lines, products;
CREATE TABLE products (id INT PRIMARY KEY, name VARCHAR(40), category VARCHAR(20), price DECIMAL(8,2),
  embedding VECTOR(4) NOT NULL);
INSERT INTO products VALUES
  (1, 'red apple',      'fruit', 0.50, STRING_TO_VECTOR('[0.9, 0.1, 0.0, 0.2]')),
  (2, 'green apple',    'fruit', 0.45, STRING_TO_VECTOR('[0.8, 0.3, 0.0, 0.1]')),
  (3, 'banana',         'fruit', 0.25, STRING_TO_VECTOR('[0.1, 0.9, 0.1, 0.0]')),
  (4, 'orange',         'fruit', 0.60, STRING_TO_VECTOR('[0.3, 0.6, 0.2, 0.1]')),
  (5, 'apple juice',    'drink', 2.10, STRING_TO_VECTOR('[0.7, 0.0, 0.6, 0.3]')),
  (6, 'orange juice',   'drink', 2.40, STRING_TO_VECTOR('[0.2, 0.5, 0.7, 0.2]')),
  (7, 'sparkling water','drink', 0.90, STRING_TO_VECTOR('[0.0, 0.1, 0.9, 0.1]')),
  (8, 'apple pie',      'bakery', 4.20, STRING_TO_VECTOR('[0.8, 0.0, 0.1, 0.8]')),
  (9, 'bread',          'bakery', 2.80, STRING_TO_VECTOR('[0.1, 0.1, 0.0, 0.9]'));
CREATE TABLE recommendations (rank_no INT, product_id INT, similarity DOUBLE);
CREATE TABLE order_lines (id INT AUTO_INCREMENT PRIMARY KEY, product_id INT, qty INT, amount DECIMAL(10,2));
GRANT ALL ON app.* TO app;
