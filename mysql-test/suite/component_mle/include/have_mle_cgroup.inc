--disable_query_log
#
# The memory limits of workers (mle.memory_max, mle.v8_memory_max, mle.cpython_memory_max) are
# cgroup limits: mysqld needs a delegated cgroup v2 (mle_cgroup ON). MTR gives it one only when it
# runs in such a cgroup itself:
#   systemd-run --user --scope -p Delegate=yes BUILD-mle/mtr ...
# Without one these tests are skipped, never faked (MVP review TEST-5).
#
# The status exists while the component is installed: installed for the look, then uninstalled,
# so that a skip leaves nothing behind. Source it before the test's own INSTALL COMPONENT.
INSTALL COMPONENT "file://component_mle";
--let $mle_cgroup = `SELECT VARIABLE_VALUE = 'ON' FROM performance_schema.global_status WHERE VARIABLE_NAME = 'mle_cgroup'`
UNINSTALL COMPONENT "file://component_mle";
if (!$mle_cgroup) {
  if ($MLE_TEST_REQUIRE_CGROUP) {
    --die No delegated cgroup v2 for mysqld, and MLE_TEST_REQUIRE_CGROUP is set
  }
  --skip Needs a delegated cgroup v2 (run MTR in systemd-run --user --scope -p Delegate=yes)
}
--enable_query_log
