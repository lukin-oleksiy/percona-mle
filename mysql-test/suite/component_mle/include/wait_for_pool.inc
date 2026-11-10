# Waits until the MLE worker pool holds $mle_pool workers (default 2, mle.pool_size).
if (!$mle_pool) {
  --let $mle_pool = 2
}
--let $wait_timeout = 120
--let $wait_condition = SELECT VARIABLE_VALUE = $mle_pool FROM performance_schema.global_status WHERE VARIABLE_NAME = 'mle_pool_idle'
--source include/wait_condition.inc
--let $mle_pool =
