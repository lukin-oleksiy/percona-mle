# Runs the Python test $cpython_test (t/$cpython_test.test, written for LANGUAGE PYTHON on the JVM
# worker's GraalPy) on the CPython worker: LANGUAGE PYTHON becomes LANGUAGE CPYTHON, its privilege
# CREATE_CPYTHON_ROUTINE (exp design note 62). The expected result r/cpython_$cpython_test.result
# is r/$cpython_test.result with the same changes (include/py_on_cpython.pl), so that a difference
# is a difference between the two Python workers.
--source ../include/have_mle_cpython.inc
--let MLE_CPYTHON_TEST = $cpython_test
--perl
use strict;
require "$ENV{MLE_SUITE_DIR}/include/py_on_cpython.pl";
my $dir = "$ENV{MLE_SUITE_DIR}/t";
open(my $in, '<', "$dir/$ENV{MLE_CPYTHON_TEST}.test") or die "py_on_cpython: $ENV{MLE_CPYTHON_TEST}: $!";
local $/;
my $t = py_on_cpython(<$in>, $ENV{MLE_CPYTHON_TEST});
close $in;
# Relative includes of the original resolve from the suite's t/ directory.
$t =~ s{--source \.\./include/}{--source $ENV{MLE_SUITE_DIR}/include/}g;
open(my $out, '>', "$ENV{MYSQLTEST_VARDIR}/tmp/cpython_test.inc") or die "py_on_cpython: $!";
print $out $t;
close $out;
EOF
--source $MYSQLTEST_VARDIR/tmp/cpython_test.inc
--remove_file $MYSQLTEST_VARDIR/tmp/cpython_test.inc
