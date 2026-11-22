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
  # The V8 worker strips only ASCII white space from a numeric string (the JVM worker: what
  # JavaScript's Number() strips), and keeps a Date's local date for DATE (the JVM worker rounds
  # the time part off to the closest second, as Oracle's Table 27.3 says).
  # Libraries created with LANGUAGE JS show it in information_schema.LIBRARIES.
  ['libraries', qr/(?<=jslib\tlib\d\t)JAVASCRIPT$/m, 'JS'],
  ['libraries', qr/SyntaxError: lib3:3:10 Expected an operand but found \*/, "SyntaxError: lib3:3:11 Unexpected token '*'"],
  # Oracle's 1000-frame limit (see oracle_examples).
  ['execute', qr/--error ER_LANGUAGE_COMPONENT_RUNTIME\nSELECT recursive_sum\(999\);/, 'SELECT recursive_sum(999);'],
  ['execute', qr/SELECT recursive_sum\(999\);\nERROR HY000: JavaScript> Maximum frame limit[^\n]*/,
   "SELECT recursive_sum(999);\nrecursive_sum(999)\n499500"],
  # Positions: V8 points at the property read, GraalJS at the expression's start.
  ['debugging', qr/at helper \(js_div:2:31\)/, 'at helper (js_div:2:41)'],
  ['session_state', qr/<js> pc2:4:1-6/, '<js> pc2:4:3'],
  ['review_isolation', qr/at helper \(js_secret:2:29\)/, 'at helper (js_secret:2:39)'],
  ['review_isolation', qr/at js_lib_secret:2:8/, 'at js_lib_secret:2:10'],
  ['review_isolation', qr/at author_fails:2:8/, 'at author_fails:2:18'],
  # A session that runs JavaScript and Python holds a V8 and a JVM worker: one more kept.
  ['review_isolation', qr/SELECT VARIABLE_VALUE = 3 FROM performance_schema\.global_status WHERE VARIABLE_NAME = 'mle_workers_cached'/,
   "SELECT VARIABLE_VALUE = 4 FROM performance_schema.global_status WHERE VARIABLE_NAME = 'mle_workers_cached'"],
  # GraalJS prefixes a failed import: "Error reading module '/other/secret' imported from peek: ".
  # The session holds a V8 worker (JavaScript) and a JVM worker (its Python).
  ['packages', qr/cannot refresh: 1 session workers are in use/, 'cannot refresh: 2 session workers are in use'],
);

sub js_on_v8 {
  my ($t, $test) = @_;
  $t =~ s/LANGUAGE JAVASCRIPT/LANGUAGE JS/g;
  $t =~ s/CREATE_JAVASCRIPT_ROUTINE/CREATE_JS_ROUTINE/g;
  $t =~ s/mle_eval\('javascript'/mle_eval('js'/gi;
  # V8's words in every test: a property of null or undefined, and a missing operand (V8: an
  # unexpected token, one column later, the caret with it).
  $t =~ s/Cannot read property '([^']*)' of (null|undefined)/Cannot read properties of $2 (reading '$1')/g;
  $t =~ s{SyntaxError: ([\w.]+):(\d+):(\d+) Expected an operand but found (\S+)}
         {"SyntaxError: $1:$2:" . ($3 + 1) . " Unexpected token '$4'"}ge;
  # GraalJS prefixes a failed import with the module that imported it.
  $t =~ s/Error: Error reading module '[^']*' imported from [^:]*: library/Error: library/g;
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
