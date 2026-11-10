--disable_query_log

#
# Check if the variable MLE_COMPONENT is set (component_mle is built with
# -DWITH_MLE=ON)
#
if (!$MLE_COMPONENT) {
  --skip component requires the environment variable \$MLE_COMPONENT to be set (normally done by mtr), see the file plugin.defs
}

#
# Check if --plugin-dir was setup for component_mle
#
if (`SELECT CONCAT('--plugin-dir=', REPLACE(@@plugin_dir, '\\\\', '/')) != '$MLE_COMPONENT_OPT/'`) {
  --skip component_mle requires that --plugin-dir is set to the component_mle dir (the .opt file does not contain \$MLE_COMPONENT_OPT)
}

--enable_query_log
