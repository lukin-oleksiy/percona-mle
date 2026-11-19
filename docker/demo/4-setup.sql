-- Demo 4, setup (as root: docker/run.sh root --force < 4-setup.sql; not shown): a clean start.
USE app;
DROP FUNCTION IF EXISTS py_bad; DROP FUNCTION IF EXISTS java_bad; DROP FUNCTION IF EXISTS js_bad;
DROP FUNCTION IF EXISTS py_typo; DROP FUNCTION IF EXISTS py_share; DROP FUNCTION IF EXISTS js_share;
DROP FUNCTION IF EXISTS java_share;
