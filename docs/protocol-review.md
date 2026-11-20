# 64 — The worker protocol: specification and review (20 Nov)

User, 20 Nov: "Read ../Improve_protocol.md. It is AI written, so do not take it as a final source of
truth. What we really need is to formalize the protocol we use, then analyze the specification and
make it perfect. After that we can improve our protocol implementation."

Done so far (nothing committed):

* **The specification:** `percona-mle/docs/protocol.md`. It is normative and describes protocol
  version 3 as built on branch `v8-worker`: roles and trust, process setup, the region byte by
  byte, turn-taking with its memory-ordering and futex rules, chunks, the control socket, every
  encoding with its domain, a dialogue grammar, every op, the SQL actions, interruption and
  failure, limits, a conformance checklist for a new worker, and sequence diagrams. Places where
  an implementation departs from the text are marked **[Fn]** and listed in §3 below.
* **This review:** an assessment of the AI-written brief, the findings (§3), the decisions they
  need (§4), the conformance test suite (§5) and the order of work (§6).

## 1. The brief (`../Improve_protocol.md`)

Useful as a checklist of spec sections; the spec covers all of them. Where it is wrong or stale:

* The file is cut off in the middle of a sentence (§3.5, "Do not introduce concurrency not"):
  whatever followed is missing.
* "Native V8 worker: currently under development": built last night (note 63 §9). GraalPy native
  is retired.
* It asks for documentation, fixes, tests and commits in one go. We keep our order: spec and
  review first, decisions, then code (memory: design before code).
* "Version 3, though some comments still say version 1": still true (F4).
* It does not mention what matters most in our design: that the worker is **untrusted**, so the
  host validates everything, and that guest code can run *while* a worker writes a message (the
  reentrancy bug of note 63 §10). Both are now explicit in the spec (§1, §13 item 2).
* Its "do not replace the transport" is right. Nothing found here calls for a different
  transport.

## 2. Verdict

The protocol is sound. The transport is a strict one-token alternation (spec §4.1: the worker's
turn when H = W, the host's when H = W + 1), which makes the arena single-owner at every moment;
the futex handshake cannot lose a wake-up; the host copies before it validates and kills on any
violation; sizes and counts are bounded before allocation. No finding below is a memory-safety
problem in mysqld.

**No change to the wire format is needed.** All findings are fixed by documentation, by small host
or worker changes that stay compatible with version 3, and by tests. I propose to keep version 3.

## 3. Findings

Kind: **doc** (comments or docs wrong), **host**, **worker** (code), **design** (a rule to
decide). Severity: how much harm today.

| # | Kind | Severity | Finding | Proposal |
|---|---|---|---|---|
| F1 | doc, worker | low | `protocol.h` says statements live "until the top-level CALL ends". They live until **the CALL at their depth** ends: each routine invocation has its own `SqlRunner`, nested ones too (`component/session.cc:641`). The JVM worker's `closeDeferred()` comment repeats the error and sends a CLOSE the host does anyway. | Fix the comments; keep the CLOSE (harmless) or drop it. |
| F2 | host | low | The host recognizes an unknown time zone by searching the error *text* for "unknown time zone" (`session.cc:526`), although the worker marks it with the language `MLE-TimeZone`. A worker with other wording would turn the user's error into "internal error". | Test `Error.language == "MLE-TimeZone"`. Needs `Worker::options` to return the Error, not only its message. |
| F3 | design | medium | SQL while serving PREPARE is a protocol violation: the host kills the worker (`host/worker.cc:474` accepts SQL only during CALL). Nothing sends it today (library top-level code runs at the first CALL, checked live), but `session.cc:560` says "loading runs routine code, which may run SQL", and an engine change (eager module evaluation) would turn guest code into a worker kill. | **D1** below. |
| F4 | doc | low | `protocol.h` and `codec.h` call the protocol "v1"; it is v3. Several op comments are stale. | Header comments point to `docs/protocol.md`; constants stay in `protocol.h`. |
| F5 | design | low | CALL's `events` (statement end, commit, rollback) are never set by the host and ignored by every worker. | **D4**. |
| F6 | doc | low | `total_rows` = 0 means "unknown" and also "empty". Workers already end a set by `more` only. | Documented (spec §10.3). No change. |
| F7 | doc | low | Warnings come in a set's first chunk (buffered results) and again in its last; receivers keep the last non-empty list. Undocumented; a new worker could count them twice. | Documented (spec §10.3). |
| F8 | doc | none | Op 7 (FETCH) and control SHUTDOWN are never sent. | Reserved in the spec (Appendix A). |
| F9 | tests | unknown | Edge values are not tested across all four engines: NaN/±Inf/−0.0 for DOUBLE and FLOAT results (the JVM worker refuses non-finite parameters, `Values.java:225`; what V8 and CPython do and what mysqld stores is unchecked), I64/U64 extremes, 65-digit DECIMALs, TIME ±838:59:59, zero dates, VECTOR of 0 and 16383 dimensions, U+0000 and lone surrogates in strings, and the claim that TIMESTAMP values are session wall-clock times. | Conformance tests (§5); fix what they find. |
| F10 | host, design | low | `msg.reserved` is not checked by the host. The JVM worker never writes it (it stays 0 because the host's own posts zero it). Without a check the field cannot be used for anything later. | **D2**: every worker writes 0; the host rejects non-zero. Compatible with v3. |
| F11 | worker | low | The native workers do not check that the host's bodies are consumed exactly (the V8 SQL_RESULT error reply, for one). The host is trusted, so this only hides host bugs. | Check in `wire::Reader` with a `finish()`, as the JVM worker does. |
| F12 | worker | medium | A malformed or unexpected host message does not end every worker cleanly: **CPython** turns it into a `RuntimeError` that guest code can catch and go on with (`codec.hpp:154`); **V8** throws a C++ exception through V8's frames from the `sql` callback (`worker.cc:106`, `throw Malformed` in `sql()` and the value readers), which ends in `std::terminate`; the **JVM** worker throws `IllegalStateException` from `sqlRequest` into guest code. The host is trusted, so this needs a host bug to matter, but then the worker should stop at once, not run on in an unknown state. | One `fatal()` path per worker (message to stderr, `_exit(1)`), used for every protocol failure; guest code never sees it. |
| F13 | design | none | Workers answer an unknown top-level op with RESULT INTERNAL but exit on an unexpected op elsewhere. | Keep: spec §8.1 allows it (a newer host can probe). |
| F14 | design | low | CREATE's check findings travel as stderr lines marked `"\x1fmle-check:"`. Guest code can print the marker and forge warnings, but only of the CREATE that its own author runs. | Keep and document (done). A structured list in RESULT is the clean form if we ever touch RESULT. |
| F15 | design | low | A RESULT may leave out OUT parameters; the caller's variable then keeps its value. Every worker sends all of them today. | **D3**. |
| F16 | design | none | TIME "−00:00:00" (negative zero) passes the codec. MySQL prints `-00:00:00` for it, so it is a real value. | Keep. |
| F17 | worker | low | CPython `py_sql` ignores the result of the nested-request handler (`module.cc:563`): if it ever raised, the exception would stay set and surface later as a `SystemError`. | Check it and treat a failure as fatal (F12). |
| F18 | worker | to verify | The JVM worker writes RESULT in place in the arena and calls `String.valueOf(v)` / `v.toString()` on what a **Java** routine returns (`Values.java:327`, `:332`). If that `toString` runs SQL, the SQL message overwrites the half-written RESULT: the same class of bug as V8's (note 63 §10). The host then sees garbage and kills the worker (no harm to mysqld). JavaScript and Python values are converted before writing and are not affected. | A test first; if it fails, the guard V8 got (no SQL while a value is being converted). |
| F19 | design | low | **Memory held in mysqld across nesting.** A worker may send a 64 MiB SQL message at each nesting level, and the host keeps each decoded until its statement ends: up to 31 × 64 MiB, plus the bound parameters. It needs a routine author who wants it; plain stored procedures can hold comparable memory. | **D5**. |
| F20 | doc | low | The spec now states rules that were implicit: one thread per side uses the mailbox; `call_seq` grows over the worker's whole life, not per session (the pending-INTERRUPT logic depends on it); no guest code that can post may run while a worker writes a message. | Done in the spec. |

## 4. Decisions needed

* **D1 (F3). SQL during PREPARE.** (a) Keep it a violation; each worker refuses SQL locally while
  it serves PREPARE, so guest code gets an error and the worker survives. (b) The host accepts SQL
  during PREPARE and answers with an SQL error ("SQL is not available while a routine is
  loaded"), using the hooks PREPARE already passes. **Proposal: (b).** It needs one host change,
  turns a possible worker kill into a plain error, and leaves a later step open: actually running
  the SQL, if eager module loading ever needs it.
* **D2 (F10). Reserved fields.** Workers write 0 in `msg.reserved`; the host rejects non-zero.
  Version 3 is unchanged (every worker writes 0 today). **Proposal: yes.**
* **D3 (F15). OUT parameters.** Make "every OUT and INOUT parameter, exactly once" a MUST that the
  host checks, or keep "missing keeps its value". **Proposal: MUST.** It catches worker bugs, and
  every worker already complies.
* **D4 (F5). CALL `events`.** Keep the field as reserved (0), or define it now. **Proposal: keep it
  reserved.** It would be wanted only for per-statement or per-transaction guest state, which
  nobody asked for. Removing it would change the wire format for nothing.
* **D5 (F19). A budget for request bytes held across nesting.** Count the bodies of the worker's
  requests in progress per session against a limit (e.g. 256 MiB), refusing the statement beyond
  it. **Proposal: not now.** Record it as a known limit, like plain stored programs have it.
* **D6. Where the protocol is defined.** `docs/protocol.md` becomes the single description;
  `protocol.h` keeps the constants and layouts and points to it; `Wire.java` and the workers'
  comments point to it too. **Proposal: yes.**

## 5. A conformance test suite

What exists: host-side checks against a scripted bad worker (`unittest/worker_protocol-t.cc`,
`mle_test_worker.cc proto ...`), a JVM-only worker test (`java_worker-t.cc`), codec tests and a
codec fuzzer, V8 value tests, and the MySQL tests, which run the JavaScript tests on GraalJS and V8
and the Python tests on GraalPy and CPython. Nothing tests the native workers at the protocol level,
or all four engines against the same protocol cases.

Proposal, two parts, both in `unittest/`:

1. **Host conformance** (extend `worker_protocol-t.cc` and the `proto` modes of
   `mle_test_worker`): one case per host rule of the spec — every chunk rule of §5.2, every RESULT
   check of §9.4, SQL/LIBRARY at the wrong time or depth (§8.1), non-zero reserved (D2), status
   5..7 from a worker, non-UTF-8 str, bad tags and domains, a READY of the wrong version or
   sequence. Each must kill the worker with PROTOCOL_ERROR and leave the host usable.
2. **Worker conformance** (new `worker_conformance-t.cc`, a `TEST_P` over the engines JVM/Java,
   JVM/GraalJS, JVM/GraalPy, V8 and CPython, started through the real launcher): the host's
   `Worker` class drives each one with small routines written per language from one table of
   cases:
   * READY fields; OPTIONS with a good and a bad zone (the `MLE-TimeZone` marker).
   * Value round trips through arguments, return values, OUT parameters and SQL parameters and
     results, with the edge values of F9.
   * Chunking both ways with a 64 KiB arena: arguments, results, SQL texts and rows larger than
     the arena; a VECTOR straddling a chunk boundary.
   * Nesting: nested PREPARE/CALL/DROP during SQL, up to the depth limit; statement ids are per
     CALL (a nested call cannot fetch from its caller's statement).
   * INTERRUPT before the request, during guest code, during a nested call (the outer must not go
     on), after the request ended (the next request is unaffected).
   * Limits: an SQL message over 64 MiB or 65535 values and a RESULT over 64 MiB are refused by the
     worker itself.
   * Reentrancy: a value whose conversion runs SQL, as a parameter and as a result (F18).
   * Exhaustion sets `tainted`; RESET keeps a usable worker.
   * A malformed host message makes the worker exit non-zero, and guest code never sees it (F12):
     needs a test hook in the host's `Worker` to post raw bytes.

Cost: about two days, most of it the per-language routine table and the edge values.

## 6. Order of work, after the decisions

1. Doc fixes: header comments (F1, F4), pointers to `docs/protocol.md` (D6).
2. The conformance suite (§5), run against the current code to see what fails.
3. Fixes: F12 and F17 (fatal path per worker), F2, F11, F18 if its test fails, D1, D2, D3, and
   whatever §5 finds.
4. Update the spec: remove the **[Fn]** marks that are fixed.
5. MTR suite and unit tests on Ubuntu; OL10 with SELinux.

No benchmark is needed: nothing here touches the hot paths, except D2 and D3 (one compare each).

## 7. Corrections from the code check (20 Nov)

An independent agent checked every statement of the spec against the code: 23 discrepancies and 9
omissions. All are corrected in `docs/protocol.md`, which now describes the code as it is. Where
the code's behaviour looks accidental the spec keeps a mark; these join the findings of §3:

| # | Kind | Finding |
|---|---|---|
| F21 | worker | V8 never posts a failed READY (it aborts); the JVM worker posts one and then keeps serving. |
| F22 | worker | Ill-formed guest text: V8 writes U+FFFD, the JVM and CPython workers write `?`. |
| F23 | host | `ParamType.precision` is never set (always 0), also for DECIMAL(p, s). |
| F24 | host | A killed request is reported as interrupted despite an OK RESULT only for CALL, not for PREPARE, PARSE_LIBRARY or `mle_eval`. |
| F25 | worker | No worker registers PARSE_LIBRARY for INTERRUPT; in the native workers its INTERRUPT stays pending. |
| F26 | host | The Value path ends a chunk at a fixed 768 KiB (¾ of the *default* arena), whatever the arena is. |
| F27 | host, design | LIBRARY: any library in the routine's schema or in a schema of a USING library is importable; no privilege check at run time. |
| F28 | host, design | The time zone in OPTIONS is bound at the session's first MLE use and never re-sent; after `SET time_zone` the host's TIMESTAMP values and the worker's zone disagree. |
| F29 | host | A FLOAT column is declared F32 but its row values are sent as F64. |
| F30 | host | `status` of SQL, LIBRARY and CONTINUE from a worker and `row_width` of non-batch actions are not checked. |

The user's order of work (20 Nov): the code is the source of truth for *writing* the spec; next the
spec is analysed formally, without the code, and improved; then the code is brought to the spec.

## 8. Two independent design reviews (Fable 5.2 and Mythos 5.2, 20 Nov)

Each got only the spec and the code, not this note. Both confirmed the two proofs (no lost
wake-up; one owner of the arena) and found no race between INTERRUPT and the mailbox. Findings
both made independently are marked **both**.

| # | Source | Severity | Finding | Proposed rule for the spec |
|---|---|---|---|---|
| R1 | both | high | DROP, RESET, OPTIONS (and READY) are bounded *per wait*, not per request, and ignore KILL: a taken-over worker answering DROP with a 64 MiB RESULT, one chunk per 9.9 s, holds a session thread for 10 minutes (1 MiB arena) to 2.8 hours (64 KiB), inside DDL or session release. | One absolute deadline per request; RESULTs of DROP/RESET/OPTIONS and READY at most one chunk; KILL during them ends the worker. |
| R2 | Mythos | high | The interrupt grace also counts mysqld's own time: after KILL in a nested call, a statement that takes > 1 s to unwind in mysqld (rollback of a large UPDATE) gets a conformant worker SIGKILLed. | The grace counts only time while the worker holds the turn. |
| R3 | both | medium | The statement id has no defined state machine; after ERROR the id is open or closed depending on the path (one path leaves a failed EXECUTE registered). `row_width` 0 is a violation that guest input can reach. | A transition table (state × action → reply, next state). ERROR to EXECUTE or NEXT_RESULT closes a non-prepared id; a prepared id survives every ERROR. Rule: whatever guest input can reach is an error, never a violation. |
| R4 | Mythos | medium | NEXT_RESULT's "no more results" (OK, 0 affected) is the same bytes as a real final OK result. | A reply kind END (wire change), or define NEXT_RESULT only after `more_results` = 1. |
| R5 | Mythos | medium | "Propagates unchanged" breaks easily: any SQL message, CLOSE included, clears the pending error (`finally: cur.close()`), and identity is by string equality. | CLOSE, FETCH and NEXT_RESULT do not clear it; or an error token carried in ERROR and echoed in RESULT (wire change). |
| R6 | both | medium | A failed re-PREPARE of an existing id: workers drop the old routine first, the host still records it as prepared. PREPARE or DROP of a routine running at an outer depth is undefined (V8 holds a reference that dangles). | A running activation keeps its code; a PREPARE that is not OK leaves the id unprepared on both sides. |
| R7 | both | medium | TIMESTAMP and the session zone (= F28); also a wall-clock TIMESTAMP is ambiguous in the DST fall-back hour. | The host re-sends OPTIONS when the zone changed, at a top-level request; next version: TIMESTAMP as UTC microseconds (wire change). |
| R8 | both | medium | Non-finite floats from a worker reach the server unchecked. | Worker→host F64/F32 MUST be finite; else ERROR (SQL parameter) or `MLE-Type` (RESULT). −0.0 is preserved. |
| R9 | both | medium | Host→worker `str` is not validated by the host; CPython decodes strictly in one path, the others replace. | The host sends what the server gives; workers MUST replace ill-formed sequences with U+FFFD and MUST NOT exit. |
| R10 | both | low | A RESULT INTERRUPTED without any INTERRUPT gives the client ER_QUERY_INTERRUPTED; non-OK answers to DROP/RESET/OPTIONS have no defined outcome. | A table per request: allowed statuses and the host's reaction. INTERRUPTED without INTERRUPT is a violation. |
| R11 | Mythos | medium | §4.2 allowed "release" for the poster's increment, which loses wake-ups on AArch64 (the code uses seq_cst). | The increment MUST be seq_cst. |
| R12 | both | low | Version mismatch is silent (the worker exits, the host sees DIED); packages make skew reachable. No rule for reserved fields or extension. | A version-invariant prefix so a mismatching worker posts a failed READY naming both versions; one rule for reserved fields. |
| R13 | Fable | low | FNV-1a 64 as a library's identity: collisions can be built, so a worker may keep an old body under a new catalog entry. | The u64 is an opaque version token from the host. No layout change. |
| R14 | both | low | Value tags are defined only in an informative appendix (= F29: FLOAT rows are F64 under an F32 column). | The type-to-tag table becomes normative; values are self-describing. |
| R15 | both | low | Limits a worker enforces (4096 columns, 2^27 values) that the host does not respect when sending; `sql_statements_max` multiplies with nesting. | In §12 as host obligations, or removed from the workers. |
| R16 | Mythos | low | Control socket: byte order, `MSG_NOSIGNAL`, longer datagrams, a failed send; the real requirement is that "this INTERRUPT is for the running request" and the end of a request exclude each other. | Stated. |
| R17 | Fable | low | SIGNAL rules: class `01` is not raised; a CALL that ends OK clears the pending error. | Stated. |
| R18 | both | — | Document structure: initial state (zero-filled region, both `seen` 0); explicit state machines for the mailbox, the interrupt and the statement id; one table per request; a table "field, who validates, outcome"; wire rules apart from host policy and implementation values; test vectors. | The revised spec. |

Also confirmed by both: F19 (memory across nesting, about 4 GiB per session with ordinary guest
code), F27 (library scope), F30 (unchecked header fields), F25.

## 9. Specification 1.0 (20 Nov)

User: version 4 of the wire is fine, but this is the first *specification*, so it is **1.0**; the
code is adjusted to it in the next round; now only the specification is worked on.

`docs/protocol.md` is rewritten as specification 1.0 (region `version` = `major << 16 | minor` =
0x00010000). It no longer describes the code. What 1.0 changes against the code (wire v3) — the
work list for the code round:

* **Layout:** `h_sleep` on its own line (256), `msg` at 320, `stats` at 384; `version` encoding.
* **READY:** `version, info` first (frozen part); a failed READY and exit, also on a version
  mismatch; one chunk ≤ 64 KiB; the host checks the language list.
* **CALL:** `events` removed. **RESULT:** `findings` list (replaces the stderr marker); all OUT
  parameters, ascending; console empty for control requests; INTERRUPTED only after INTERRUPT.
* **Error:** `sql_error_id`; **SQL_RESULT ERROR:** `error_id`; hand-back by id (the host keeps the
  last 16 ERRORs) instead of string equality and the diagnostics-area dance.
* **SQL_RESULT:** kind END; `total_rows` 2^64 − 1 = unknown; `more_results` 0 while `more` = 1; a
  multi-row chunk fits one arena.
* **Statement ids:** the state tables of §10.1 (ERROR sends a direct statement to CLOSED, a
  prepared one to IDLE; wrong-state actions are errors); flags only at EXECUTE/PREPARE;
  `row_width` 0 is an error; EXECUTE_MANY returns the last row's insert id.
* **TIMESTAMP:** i64 UTC microseconds (−2^63: the zero timestamp); OPTIONS re-sent before a
  top-level request whenever integer type, decimal type or zone changed.
* **Values:** host sends exactly the declared tag (FLOAT rows as F32); non-finite floats from a
  worker are errors; workers repair guest text with U+FFFD and accept ill-formed text in values.
* **PREPARE:** SQL while serving it gets ERROR (not a kill); a failed PREPARE leaves the id
  unprepared on both sides; a running activation keeps its code; `precision` set.
* **LIBRARY:** `version` is the host's opaque token, not a hash of the body; policy = USING
  libraries and their imports.
* **Interrupts:** the grace clock runs only while the worker has the turn; PARSE_LIBRARY is
  interruptible; the host reports every interrupted request as interrupted.
* **Deadlines:** absolute per request; RESULT of DROP/RESET/OPTIONS one chunk.
* **Host:** all header fields validated (status, reserved, CONTINUE in full, depth and call_seq on
  the first chunk); bounds per session incl. request bytes across nesting (§10.7); never reads
  back the region.
* **Workers:** one fatal path for protocol failures; no worker-only limits on host messages.

Decisions taken in writing it (were D1–D6 of §4): D1 (b), D2 yes, D3 MUST, D4 the field is
removed, D5 a bound is required (value is host policy), D6 yes.

Round 2 of analysis (spec only, no code): two agents, Mythos 5.2 and Fable 5.2, started 20 Nov.

User, 20 Nov: the specification is independent of the guest language; anything language-specific
goes to appendices. Done: the body names no language; **OPTIONS** is now `time_zone` plus a
generic list of (name, value) settings (`js.integer_type`, `js.decimal_type` are defined in
Appendix B.2; new marker `MLE-Option`); identifiers, source forms, type mappings and what RESET
keeps are in Appendix B.

## 10. Round 2: analysis of specification 1.0 from its text alone (20 Nov)

Two agents (Mythos 5.2: 23 findings; Fable 5.2: 28), told to read nothing but the spec. Both
recomputed the test vectors: all correct. User, same day: the protocol is for MySQL and Percona
Server (MySQL names in the body are fine); the spec defines the border between MySQL and a worker
(§1.1 added). Corrections made in the spec:

* **One principal per worker** (Mythos): stated as a rule; it was only implied. A worker taken
  over by one definer's code must never get another principal's routine or SQL rights.
* **Phantom INTERRUPT** (both): INTERRUPT(s) only after the first chunk of s is posted; a
  `call_seq` is used up at that post; "interrupt requested" is recorded before the send.
* **Grace clock** (both): after an interrupt the host does no new work (SQL → ERROR, CLOSE → OK,
  LIBRARY → REFUSED), so a worker cannot buy time; turns inside a transfer that began earlier are
  bounded one by one; the worker's "within the grace period" is gone (it does not know the value).
* **Statement states** (both): rule 1 CLOSE is always OK; rule 2 a not-allowed action is ERROR,
  no change; rule 3 any ERROR to an allowed action goes home; table of legal replies per action;
  NEXT_RESULT gives END in IDLE and CLOSED.
* **Results for the client** (both): the host sends all results before one OK; at depth > 0 "the
  client" is the enclosing statement.
* **Batches** replace "chunks" of rows; columns only in a set's first batch; warnings per result.
* **OPTIONS** (both): "options in force" (last OPTIONS answered OK); none in force → no PREPARE or
  CALL; RESET ends them.
* **Libraries** (both): freshness rule (ask once per top-level request before first use); status
  table; version per (language, body); name limits; REFUSED must not reveal existence.
* **Posting** (both): a post is a store of the private count, never a read-modify-write of a
  word the worker can write.
* **Text** (both): type `text` for STRING/JSON; repair defined (Unicode maximal subpart); the host
  repairs messages it quotes; the host converts all character data to and from utf8mb4.
* **Floats** (both): the host is the checker (stated as the one deliberate exception).
* **Sizes** (both): a worker that cannot hold a host message drains it and fails it locally; the
  host decides the 1 GiB limit before the first chunk; RESULT trimming order; console carries all
  unsent output at any depth.
* **READY** (both): `features` field; failed READY may end after `info`; "incompatible worker" is
  not a violation; the frozen part is listed completely; a post just before death is not lost.
* **Control socket:** SHUTDOWN removed (never sent); empty datagrams forbidden; the launcher
  keeps no copy of the ends; all three seals.
* **SQL from the wrong place** (both): only the serving activation on the serving thread.
* Smaller: `out_params` index from 0; `Type.tag` 0 for untyped columns; TIMESTAMP taken from the
  stored instant; nested DROP after an interrupt answers OK; §12 and §13 rewritten with "kept by"
  and outcomes; more test vectors (READY, DATE, TIME, DECIMAL, zero TIMESTAMP); editorial fixes.

Not taken: dropping VECTOR alignment (Mythos; kept for zero-copy views), `grace_ms` in OPTIONS.
The copy of the spec before this round: scratchpad `protocol-r1.md` (session-local).

## 11. Round 3: analysis of the corrected specification (20 Nov)

Mythos 5.2 (16 findings, formal) and Fable 5.2 (13, implementation exercise), text only. Vectors,
cross-references and contents: no errors. Corrections made:

* **Principal** (Mythos): it is a pair (owner of the code, account whose rights apply), so
  invoker-rights code of A called by B never shares B's own worker; the leftover "RESET and
  retirement" row is gone; RESET before another session is a MUST.
* **Chains through several workers** (Mythos): "the session" covers all its workers: an interrupt
  goes to every top-level request in progress; bounds of §10.7 count over all workers; results
  "for the client" go to the consumer of the statement that invoked the routine.
* **Console** (both): only RESULTs of PREPARE and CALL carry it; other output waits; RESET
  discards it; guest code outside a request (finalizers, timers, threads) can do nothing the
  protocol sees.
* **RESULT size** (Mythos): fixed caps for message, frames and findings; cut console is dropped.
* **Sizes** (both): OPTIONS, DROP, RESET are one chunk, so EXHAUSTED is only for PREPARE, CALL and
  PARSE_LIBRARY, and without `tainted`; after a discarded SQL_RESULT the worker sends CLOSE.
* **Grace clock** (both): counted from when the session thread notices; turns inside any chunked
  transfer are bounded per turn and per transfer (grace × (1 + size / 16 MiB)), not added; after
  the record every SQL_RESULT but CLOSE's is ERROR, so no large reply starts; record, first-chunk
  post and last-RESULT-chunk receipt are serialized.
* **Libraries** (both): an answer is for one routine and holds while `library_epoch` — new u64 in
  PREPARE and CALL — is unchanged: per-routine permission without a round trip per call.
* **Statements** (both): sticky bindings removed (EXECUTE_PREPARED gives all values); refusal
  (PREPARE, interrupt) precedes END; argument errors go home by rule 3; a final batch may be
  empty; batch size counts columns and warnings; results-for-client reply fully defined.
* **Proof** (Mythos): the second case of the no-lost-wakeup argument was wrong as written (the
  conclusion held); corrected, and the poster's flag load is now required to be seq_cst.
* **Text** (Mythos): code and names (SQL text, library names) are never repaired — an error; the
  primitive is renamed `lstr`.
* **Death** (Mythos): only a READY or RESULT posted before death is processed.
* `Error.language` domain and markers per status; domains and limits for OPTIONS, READY,
  PREPARE fields; §12, §13, checklists; vectors for SQL and SQL_RESULT, CALL with the epoch.

The copy before this round: scratchpad `protocol-r2.md` (session-local).

## 12. Round 4: convergence check (20 Nov)

Mythos 5.2: 7 defects, "not yet ready" (3 blockers); Fable 5.2: 10 defects, "ready once two are
fixed". Vectors, sizes, references: no errors. Both found the same blocker:

* **Grace clock (both):** turns inside chunked transfers were exempt and a worker could start any
  number of them after the interrupt (1 s of grace bought days). Now the clock is real time from
  the moment of notice; it stops only while the server ends a running statement and during the
  transfer in progress at notice or a RESULT's transfer (≤ 32), which have size-scaled bounds.
* **Library answers (Mythos):** lifetime defined: an answer belongs to (routine id, library) with
  the epoch of its request; discarded at PREPARE of the id, DROP, RESET; the host samples the
  epoch at the first chunk and changes it only once the change is visible to its answers.
* **Discarded SQL message (both):** the host keeps `action` and `statement_id` and applies §10.1.
* **Principal (Mythos):** an account is one incarnation (workers retired on DROP/RENAME USER);
  the limit is stated: the rule protects from the code's owner, not a principal from its callers;
  §1.1 said "the session's own rights" — corrected to the principal's.
* **OPTIONS (both):** needed only before PREPARE, CALL, PARSE_LIBRARY; settings fit the 64 KiB.
* READY check order; `failed_row` when a batch stops for an interrupt or limit; Error cut to
  64 KiB for control requests; findings without `check`; nested DROP/PREPARE not sent after an
  interrupt (the id is then unprepared); empty library body legal.

Trend: round 1 structural (18), round 2 contradictions and gaps (23 + 28), round 3 (16 + 13, two
design issues), round 4 (7 + 10, one design issue: the clock). Copy before: `protocol-r3.md`.

## 13. Round 5: the three rewritten rules (20 Nov)

Fable 5.2 (7 defects) and Mythos 5.2 (10), text only, limited to the grace clock, library answers
and discarding. Both: "not ready", same two blockers, each a small fix. Verified without defect:
the no-lost-wakeup argument, discarding on the worker's side, §12, the epoch within a request.

* **Clock, exception (a)** (both): "while the host waits for the server" was undefined for nested
  calls and chains (one reading never ends a silent nested worker, the other ends an innocent
  worker while another one unwinds). Now: excepted is the time in which the session's thread
  neither waits for this worker nor handles its message; waiting for its nested RESULT counts.
* **Clock, bounds** (both): per-turn bound by the `len` the worker reads or posts; per-transfer
  bound is a sum of the worker's turns after notice, not a deadline from notice; the host's last
  chunk extends the clock by its size; "in progress at notice" defined by what the host posted or
  observed; step 2 starts at the moment of notice and completes messages already begun.
* **Worst case** (both derived the same): per worker G + 65 G + 32 × 5 G = 226 G with an arena
  below 64 MiB (226 s at the default), 66 G above; finite. In Appendix A.
* **Libraries** (both): imports of libraries by libraries, to any depth, are resolutions; a
  runtime's module cache must obey the rule; the epoch changes before the DDL reports success;
  one answer per (id, library); the host answers from the current catalog; a running activation
  keeps its link.
* **`failed_row`** (Mythos): in every ERROR to EXECUTE_MANY it is the number of rows executed.
* **Discarding** (both): `action` is checked; CLOSE is always 26 bytes; §13 row added.

Copy before: `protocol-r4.md`.

## 14. Round 6: grace clock and library answers (20 Nov)

Fable 5.2 (8) and Mythos 5.2 (8), text only. Verdict of both: sound in substance; the clock needed
precise terms, the library rule two decisions. No safety hole in §9.10 (Mythos tried six attacks;
each is prevented by a named sentence). All ten hand-run interruption schedules end at a defined
time. Fixed:

* Clock: its limit is stated (G plus one extension); a turn straddling the moment of notice is
  counted from the notice; the turn that reads the last chunk of a host message belongs to the
  transfer (no double allowance, and the worker gets the message's whole allowance to take it in);
  the extension is by `total_len` of the last host message; the record is looked at before the
  host acts on an observed post; "handling" = decoding and answering, not the server's run time;
  "requested" → "noticed" in §9.10, §10.1, §10.4.
* Worst case with these fixes is exactly 226 G (arena < 64 MiB) and 66 G (≥ 64 MiB), as both
  reviewers computed; Appendix A corrected, with a note that G should not be below ~100 ms.
* Libraries: what REFUSED does in the worker; an import resolves to the holding answer's version;
  a worker must hold the answer before guest code can observe the library and may ask earlier
  (lazy or eager both conform); a running activation keeps its link and its answers, a run-time
  import takes the version of the answer that holds then.

Copy before: `protocol-r5.md`.

## 15. Final full-document pass (20 Nov)

One agent (Mythos 5.2), six audits (references, terms, tables, grammar, diagrams and vectors,
§1–§7 against later edits). Clean: every cross-reference, the contents list, all test vectors and
sizes, all diagrams, Appendix A's arithmetic, the wake-up argument, region and structure sizes.
Verdict: "ready to implement from, after one-sentence fixes" — 10 defects, all applied:
nested DROP after an interrupt answers OK (step 2 said INTERRUPTED); answers of a running
activation that a nested request replaced are asked for again; the unchecked discard applies only
to the request-bytes bound; a discarded LIBRARY_RESULT is as REFUSED; retirement on a dropped
account listed in §11.5; §13 rows for `sql_error_id` and `tainted`; `features` excepted from
"non-zero reserved is malformed"; checklist item 1; "activation" → "guest code of the request";
REFUSED's meaning widened. Copy before: `protocol-r6.md`.

**State: specification 1.0 is complete for the code round.** `docs/protocol.md`, 1900 lines,
uncommitted on branch v8-worker. Work list for the code: §9 of this note plus the changes of
rounds 2–6 (§10–§14): principal pairs, library epoch, statement states and END, error ids,
TIMESTAMP as an instant, OPTIONS as settings, grace clock, absolute deadlines, discards.
