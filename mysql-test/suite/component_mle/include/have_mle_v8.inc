--disable_query_log
#
# The V8 worker (exp design note 63) is built with -DMLE_V8=ON (the default when libnode's V8
# headers are found: libnode-dev, nodejs-devel) and installed in <plugin_dir>/mle/v8.
#
--let $mle_plugin_dir = `SELECT @@plugin_dir`
# Without a file of the test's own: a skip would leave it behind, and MTR's
# check of the test case would fail.
--error 0,1
--file_exists $mle_plugin_dir/mle/v8/mle_v8_worker
if ($__error) {
  # ci.sh requires every worker (MVP review TEST-7): no skip then.
  if ($MLE_TEST_REQUIRE_WORKERS) {
    --die The V8 worker is not installed, and MLE_TEST_REQUIRE_WORKERS is set
  }
  --skip The V8 worker is not installed (build percona-mle with -DMLE_V8=ON)
}
--enable_query_log
