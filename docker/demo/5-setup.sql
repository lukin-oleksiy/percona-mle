-- Demo 5, setup (as root: docker/run.sh root --force < 5-setup.sql; not shown): a clean start.
USE app;
DROP FUNCTION IF EXISTS try_escape; DROP FUNCTION IF EXISTS cpy_try_escape;
DROP FUNCTION IF EXISTS spin_py; DROP FUNCTION IF EXISTS spin_cpy; DROP FUNCTION IF EXISTS spin_java;
DROP FUNCTION IF EXISTS spin_v8; DROP FUNCTION IF EXISTS crash_cpy; DROP FUNCTION IF EXISTS hog_v8;
DROP FUNCTION IF EXISTS hog_py; DROP FUNCTION IF EXISTS worker_memory;
DROP FUNCTION IF EXISTS alive_py; DROP FUNCTION IF EXISTS alive_cpy; DROP FUNCTION IF EXISTS alive_java;
DROP FUNCTION IF EXISTS alive_gjs; DROP FUNCTION IF EXISTS alive_v8;
