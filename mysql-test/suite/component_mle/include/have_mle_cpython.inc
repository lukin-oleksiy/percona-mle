--disable_query_log
#
# The CPython worker (exp design note 62) is built with -DMLE_CPYTHON=ON (the
# default when Python 3.12+ with its headers is found) and installed in
# <plugin_dir>/mle/cpython.
#
--let $mle_plugin_dir = `SELECT @@plugin_dir`
--let MLE_CPYTHON_EXE = $mle_plugin_dir/mle/cpython/mle_cpython_worker
--perl
open(my $f, '>', "$ENV{MYSQLTEST_VARDIR}/tmp/have_mle_cpython.inc") or die "have_mle_cpython: $!";
print $f (-e $ENV{MLE_CPYTHON_EXE} ? ""
  : "--skip The CPython worker is not installed (build percona-mle with -DMLE_CPYTHON=ON)\n");
close $f;
EOF
--source $MYSQLTEST_VARDIR/tmp/have_mle_cpython.inc
--remove_file $MYSQLTEST_VARDIR/tmp/have_mle_cpython.inc
--enable_query_log
