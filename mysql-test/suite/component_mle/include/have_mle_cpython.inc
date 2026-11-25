--disable_query_log
#
# The CPython worker (exp design note 62) is built with -DMLE_CPYTHON=ON (the
# default when Python 3.12+ with its headers is found) and installed in
# <plugin_dir>/mle/cpython.
#
--let $mle_plugin_dir = `SELECT @@plugin_dir`
# Without a file of the test's own: a skip would leave it behind, and MTR's
# check of the test case would fail.
--error 0,1
--file_exists $mle_plugin_dir/mle/cpython/mle_cpython_worker
if ($__error) {
  # ci.sh requires every worker (MVP review TEST-7): no skip then.
  if ($MLE_TEST_REQUIRE_WORKERS) {
    --die The CPython worker is not installed, and MLE_TEST_REQUIRE_WORKERS is set
  }
  --skip The CPython worker is not installed (build percona-mle with -DMLE_CPYTHON=ON)
}
--enable_query_log
