-- Demo 5, setup (as root, not shown), on a container started with MLE_WORKER=CPYTHON
-- (MLE_NAME=mle-cpython MLE_PORT=3307 MLE_WORKER=CPYTHON docker/run.sh start): 20,000 pairs of strings and a
-- table for the distances the demo writes back.
USE app;
DROP FUNCTION IF EXISTS py_hello; DROP FUNCTION IF EXISTS py_lev; DROP FUNCTION IF EXISTS cosine;
DROP FUNCTION IF EXISTS worker_memory; DROP FUNCTION IF EXISTS java_hello; DROP FUNCTION IF EXISTS spin_py;
DROP FUNCTION IF EXISTS try_escape; DROP PROCEDURE IF EXISTS save_distances;
DROP TABLE IF EXISTS words; DROP TABLE IF EXISTS distances;
SET SESSION cte_max_recursion_depth = 100000;
CREATE TABLE words AS WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM c WHERE n < 20000)
  SELECT n, LEFT(SHA2(n, 256), 8) AS a, LEFT(SHA2(n + 1, 256), 8) AS b FROM c;
CREATE TABLE distances (n INT PRIMARY KEY, d INT NOT NULL);
GRANT SELECT, INSERT, DELETE ON app.distances TO app;
