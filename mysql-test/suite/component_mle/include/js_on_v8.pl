#!/usr/bin/perl
# A JavaScript test of the JVM worker (LANGUAGE JAVASCRIPT, GraalJS) as a test of the V8 worker
# (LANGUAGE JS; exp design note 63). Used by include/js_on_v8.inc (the test, at run time) and run
# by hand for the expected results:
#   cd mysql-test/suite/component_mle && perl include/js_on_v8.pl sql_js ...
# writes r/v8_<test>.result from r/<test>.result. Both go through the same changes, so that a
# difference between the results is one between the engines. The few known ones (V8 is not
# GraalJS) are listed below, per test, and change test and result alike.
use strict;

# Known differences: [test, pattern, replacement], applied to the test and its result.
our @KNOWN = (
  # V8's Date.prototype.toString() ends with the zone's long name ("GMT+0200 (Eastern European
  # Standard Time)"), GraalJS's with its abbreviation ("(EET)"): longer than Oracle's VARCHAR(50).
  ['oracle_examples', qr/CREATE PROCEDURE d1\(OUT res VARCHAR\(50\)\)/, 'CREATE PROCEDURE d1(OUT res VARCHAR(80))'],
  # Oracle's limit of 1000 routine frames (the JVM worker's FrameLimit instrument) is not in V8,
  # whose limit is its stack size (RangeError: Maximum call stack size exceeded).
  ['oracle_examples', qr/--error ER_LANGUAGE_COMPONENT_RUNTIME\nSELECT recursive_sum\(1000\);/,
   "SELECT recursive_sum(1000);"],
  ['oracle_examples', qr/SELECT recursive_sum\(1000\);\nERROR HY000: JavaScript> Maximum frame limit of 1000 exceeded\. Frames on stack: 1001\. \(recursive_sum, line 2, in sum\)/,
   "SELECT recursive_sum(1000);\nrecursive_sum(1000)\n500500"],
  # The engines' own words: a zone's name in Date.toString(); the expression in a TypeError
  # (V8 names it, GraalJS says "(intermediate value)"); a frame's position (V8: the column of the
  # failing call, GraalJS: the statement's column range).
  ['oracle_examples', qr/<date> GMT\+0300 \(GMT\+3\)/, '<date> GMT+0300 (GMT+03:00)'],
  ['oracle_examples', qr/TypeError: \(intermediate value\)\.lob is not a function/, 'TypeError: console.lob is not a function'],
  ['oracle_examples', qr/<js> pc2:3:1-47/, '<js> pc2:3:9'],
  # Libraries created with LANGUAGE JS show it in information_schema.LIBRARIES.
  ['libraries', qr/(?<=jslib\tlib\d\t)JAVASCRIPT$/m, 'JS'],
  ['libraries', qr/SyntaxError: lib3:3:10 Expected an operand but found \*/, "SyntaxError: lib3:3:11 Unexpected token '*'"],
  # GraalJS prefixes a failed import: "Error reading module '/other/secret' imported from peek: ".
  ['libraries', qr/Error: Error reading module '\/other\/secret' imported from peek: library/, 'Error: library'],
  # The session holds a V8 worker (JavaScript) and a JVM worker (its Python).
  ['packages', qr/cannot refresh: 1 session workers are in use/, 'cannot refresh: 2 session workers are in use'],
);

sub js_on_v8 {
  my ($t, $test) = @_;
  $t =~ s/LANGUAGE JAVASCRIPT/LANGUAGE JS/g;
  $t =~ s/CREATE_JAVASCRIPT_ROUTINE/CREATE_JS_ROUTINE/g;
  $t =~ s/mle_eval\('javascript'/mle_eval('js'/gi;
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
    open(my $out, '>', "r/v8_$test.result") or die "$test: $!";
    print $out js_on_v8($r, $test);
    close $out;
  }
}
1;
