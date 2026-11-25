--disable_query_log

#
# Check if the variable MLE_COMPONENT is set (by <percona-mle build>/mtr, the
# server's mysql-test-run.pl on this suite)
#
if (!$MLE_COMPONENT) {
  --skip component_mle tests run with <percona-mle build>/mtr, which sets \$MLE_COMPONENT
}

#
# Check if --plugin-dir was setup for component_mle
#
if (`SELECT CONCAT('--plugin-dir=', REPLACE(@@plugin_dir, '\\\\', '/')) != '$MLE_COMPONENT_OPT/'`) {
  --skip component_mle requires that --plugin-dir is set to the component_mle dir (the .opt file does not contain \$MLE_COMPONENT_OPT)
}

# MTR runs mysqld without a delegated cgroup.
call mtr.add_suppression("Component mle reported: 'MLE workers run without cgroups");
# MTR's mysqld runs as the user who owns the build tree, packages included (MVP review OPS-7).
call mtr.add_suppression("Component mle reported: 'mle.package_dir .* is writable by mysqld's OS user");

--enable_query_log
