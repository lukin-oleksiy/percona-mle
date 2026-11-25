# KILL QUERY of the statement connection $kill_con_id runs, once its routine runs (MVP review
# TEST-8): the routine takes the user lock $kill_lock when its guest code starts, so that the KILL
# reaches guest code, not the statement before the worker got the call. A fixed sleep proved
# nothing. The caller --send's the statement before and --reap's it after, then releases the lock
# (DO RELEASE_ALL_LOCKS()) in its connection. Runs in connection default.
--connection default
--let $wait_condition = SELECT IS_USED_LOCK('$kill_lock') = $kill_con_id
--source include/wait_condition.inc
--replace_result $kill_con_id CON
--eval KILL QUERY $kill_con_id
