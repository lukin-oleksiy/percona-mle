# Runs the JavaScript test $v8_test (t/$v8_test.test, written for LANGUAGE JAVASCRIPT on the JVM
# worker's GraalJS) on the V8 worker: LANGUAGE JAVASCRIPT becomes LANGUAGE JS, its privilege
# CREATE_JS_ROUTINE (exp design note 63). The expected result r/v8_$v8_test.result is
# r/$v8_test.result with the same changes (include/js_on_v8.pl), so that a difference is a
# difference between the engines.
--source ../include/have_mle_v8.inc
--let MLE_V8_TEST = $v8_test
--perl
use strict;
require "$ENV{MLE_SUITE_DIR}/include/js_on_v8.pl";
my $dir = "$ENV{MLE_SUITE_DIR}/t";
open(my $in, '<', "$dir/$ENV{MLE_V8_TEST}.test") or die "js_on_v8: $ENV{MLE_V8_TEST}: $!";
local $/;
my $t = js_on_v8(<$in>, $ENV{MLE_V8_TEST});
close $in;
# Relative includes of the original resolve from the suite's t/ directory.
$t =~ s{--source \.\./include/}{--source $ENV{MLE_SUITE_DIR}/include/}g;
open(my $out, '>', "$ENV{MYSQLTEST_VARDIR}/tmp/v8_test.inc") or die "js_on_v8: $!";
print $out $t;
close $out;
EOF
--source $MYSQLTEST_VARDIR/tmp/v8_test.inc
--remove_file $MYSQLTEST_VARDIR/tmp/v8_test.inc
