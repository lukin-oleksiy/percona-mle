-- Demo 4, setup (as root: docker/run.sh root --force < 4-setup.sql; not shown): a clean start.
USE app;
DROP FUNCTION IF EXISTS py_bad; DROP FUNCTION IF EXISTS cpy_bad; DROP FUNCTION IF EXISTS java_bad;
DROP FUNCTION IF EXISTS gjs_bad; DROP FUNCTION IF EXISTS v8_bad; DROP FUNCTION IF EXISTS py_typo;
DROP FUNCTION IF EXISTS py_share; DROP FUNCTION IF EXISTS cpy_share; DROP FUNCTION IF EXISTS v8_share;
DROP FUNCTION IF EXISTS java_share;
