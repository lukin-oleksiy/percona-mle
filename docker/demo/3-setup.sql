-- Demo 3, setup (as root: docker/run.sh root --force < 3-setup.sql; not shown): nothing but a clean start.
USE app;
DROP FUNCTION IF EXISTS spin_py; DROP FUNCTION IF EXISTS spin_java; DROP FUNCTION IF EXISTS spin_js;
DROP FUNCTION IF EXISTS try_escape; DROP FUNCTION IF EXISTS crash; DROP FUNCTION IF EXISTS hog;
DROP FUNCTION IF EXISTS alive_py; DROP FUNCTION IF EXISTS alive_java; DROP FUNCTION IF EXISTS alive_js;
