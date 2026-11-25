#!/usr/bin/perl
# A Python test of the JVM worker (LANGUAGE PYTHON, GraalPy) as a test of the CPython worker
# (LANGUAGE CPYTHON; exp design note 62). Used by include/py_on_cpython.inc (the test, at run
# time) and run by hand for the expected results:
#   cd mysql-test/suite/component_mle && perl include/py_on_cpython.pl sql_py ...
# writes r/cpython_<test>.result from r/<test>.result. Both go through the same changes, so that
# a difference between the results is one between the two Python workers. The known ones
# (Appendix B.4 of docs/protocol.md lets them differ) are listed below, per test, and change test
# and result alike.
use strict;

# Known differences: [test, pattern, replacement], applied to the test and its result.
our @KNOWN = (
  # GraalPy gives an SQL NULL as its foreign null, whose type is named ForeignNone (it is None).
  ['sql_py', qr/nul: BYTES: ForeignNone None/, 'nul: BYTES: NoneType None'],
  # GraalPy's java module (host access, closed for routines) does not exist in CPython.
  ['python_semantics', qr/\| java: KeyError/, '| java: ModuleNotFoundError'],
);

sub py_on_cpython {
  my ($t, $test) = @_;
  $t =~ s/LANGUAGE PYTHON\b/LANGUAGE CPYTHON/g;
  $t =~ s/CREATE_PYTHON_ROUTINE/CREATE_CPYTHON_ROUTINE/g;
  $t =~ s/mle_eval\('python'/mle_eval('cpython'/gi;
  for my $k (@KNOWN) {
    my ($name, $re, $to) = @$k;
    $t =~ s/$re/$to/g if $name eq $test;
  }
  return $t;
}

unless (caller) {
  for my $test (@ARGV) {
    open(my $in, '<', "r/$test.result") or die "$test: $!";
    local $/;
    my $r = <$in>;
    close $in;
    open(my $out, '>', "r/cpython_$test.result") or die "$test: $!";
    print $out py_on_cpython($r, $test);
    close $out;
  }
}
1;
