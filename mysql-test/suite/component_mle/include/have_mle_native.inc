--disable_query_log
#
# The Python-only native build (exp design note 23) is built with
# -DMLE_PYTHON_NATIVE=ON and installed in <plugin_dir>/mle/native.
#
--let $mle_plugin_dir = `SELECT @@plugin_dir`
--let MLE_NATIVE_LIB = $mle_plugin_dir/mle/native/libmleworker.so
--perl
open(my $f, '>', "$ENV{MYSQLTEST_VARDIR}/tmp/have_mle_native.inc") or die "have_mle_native: $!";
print $f (-e $ENV{MLE_NATIVE_LIB} ? ""
  : "--skip The Python-only native build is not installed (build percona-mle with -DMLE_PYTHON_NATIVE=ON)\n");
close $f;
EOF
--source $MYSQLTEST_VARDIR/tmp/have_mle_native.inc
--remove_file $MYSQLTEST_VARDIR/tmp/have_mle_native.inc
--enable_query_log
