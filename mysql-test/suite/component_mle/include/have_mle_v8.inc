--disable_query_log
#
# The V8 worker (exp design note 63) is built with -DMLE_V8=ON (the default when libnode's V8
# headers are found: libnode-dev, nodejs-devel) and installed in <plugin_dir>/mle/v8.
#
--let $mle_plugin_dir = `SELECT @@plugin_dir`
--let MLE_V8_EXE = $mle_plugin_dir/mle/v8/mle_v8_worker
--perl
open(my $f, '>', "$ENV{MYSQLTEST_VARDIR}/tmp/have_mle_v8.inc") or die "have_mle_v8: $!";
print $f (-e $ENV{MLE_V8_EXE} ? "" : "--skip The V8 worker is not installed (build percona-mle with -DMLE_V8=ON)\n");
close $f;
EOF
--source $MYSQLTEST_VARDIR/tmp/have_mle_v8.inc
--remove_file $MYSQLTEST_VARDIR/tmp/have_mle_v8.inc
--enable_query_log
