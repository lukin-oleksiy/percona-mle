# Waits until the JVM worker's pool holds $mle_pool workers (default 2, mle.pool_size) and the
# other installed kinds' pools are full (mle.cpython_pool_size, mle.v8_pool_size): mle_pool_idle
# counts every kind's (exp design note 63).
if (!$mle_pool) {
  --let $mle_pool = 2
}
--let $wait_timeout = 120
--let $wait_condition = SELECT g.VARIABLE_VALUE = $mle_pool + IF(FIND_IN_SET('CPython', l.VARIABLE_VALUE), @@mle.cpython_pool_size, 0) + IF(FIND_IN_SET('JS', l.VARIABLE_VALUE), @@mle.v8_pool_size, 0) FROM performance_schema.global_status g, performance_schema.global_status l WHERE g.VARIABLE_NAME = 'mle_pool_idle' AND l.VARIABLE_NAME = 'mle_languages_supported'
--source include/wait_condition.inc
--let $mle_pool =
