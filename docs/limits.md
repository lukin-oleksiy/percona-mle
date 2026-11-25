# Known limits

Behaviour that guest code, application developers and administrators can
meet, which is kept on purpose for now (a real fix is a larger change, or
costs speed). The wire protocol's own rules are in `protocol.md`; how
values map to each language is in its Appendix B.

## Routines and languages

* **JavaScript, async routines.** A body is an async function only if
  `await` stands at its top level (Appendix B.7). Such a routine cannot be
  called from another routine of the same worker on the JVM worker
  (`LANGUAGE JAVASCRIPT`): its promise cannot settle while the outer
  routine runs, and the call fails. On the V8 worker (`LANGUAGE JS`) it
  settles, because a nested call runs pending promise jobs, the outer
  routine's included (B.7).
* **JavaScript, fresh `globalThis`.** Each call of a body-form routine
  gets a fresh `globalThis`, but it is made from the worker's real global
  object: state put on its prototype or on built-in objects
  (`Array.prototype`, …) is shared by routines that run in the same
  worker, which are routines of the same account.
* **Java, the routine form.** Whether a Java source is a method body or a
  class, and the class's name, are found by regular expressions over the
  source; a comment or string that looks like a class declaration can
  mislead them. The compile then fails with an error.
* **Java libraries.** `CREATE LIBRARY … LANGUAGE JAVA` compiles the
  library on its own: a Java library cannot use another library.
* **Python parameter names** are ASCII identifiers (Python would accept
  more, but normalises them, so a name could change). JavaScript
  parameter names may be any JavaScript identifier.
* **Python, zero dates.** A zero or partial date and the zero TIMESTAMP
  are `None` in Python (B.4), which guest code cannot tell from NULL.

## SQL from routines

* **Row members and column labels.** A row's own members (JavaScript
  `getField`, `getLength`, `toArray`; Python's list methods) win over
  column labels of the same name: read such a column by index or with
  `getField()`.
* **Errors in stored functions.** A routine that ends with an SQL error
  raised before the current one, or its own SQL error, inside a stored
  function reaches the client as the component's routine error, not with
  its own number (Appendix A: the server lets the host raise no condition
  of its choice there; the same refusal reads differently in a trigger).

## Vectors and statistics

* **Frames** keep BIGINT and DECIMAL columns as doubles: keys above 2^53
  merge in `groupBy()` and `rows()`.
* **Float sums.** `inverseNorm` sums |v|² in float (tiny vectors count as
  zero, huge ones give NaN), and Euclidean distance and ranking sum
  squared differences in float, which overflow for huge values.

## Administration

* **The system time zone.** `time_zone = SYSTEM` reaches the workers as
  the IANA name of mysqld's zone; if none can be found, as the current
  offset, which is wrong after the next change of daylight saving time
  for as long as a session keeps its options (Appendix A).
* **`time_zone` changes.** A session's zone is fixed at its first routine;
  after `SET time_zone`, call `mle_session_reset()` (Oracle's rule, §7.2).
* **`CREATE_<LANGUAGE>_ROUTINE`** is checked by the component: routines
  created while the component was not installed were not checked.
* **`mle_package`** installs into the live package directory: a worker
  that imports a package while it is being installed can see it half
  written.
