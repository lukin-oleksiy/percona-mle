---
name: load-sensitive-test-failures
description: Diagnose a test that fails in the full parallel suite or in CI but passes when run alone. Use when a failure is intermittent, when a change makes the code faster or slower and tests start failing, or before calling any failure "flaky". Covers reproducing under load, separating new failures from pre-existing ones, and fixing the cause instead of raising timeouts.
---

# Load-sensitive test failures

"Passes alone" is not "flaky". Under load, threads interleave differently,
loops run more or fewer iterations, and timing assumptions in the code or in
the test break. Find which one before changing anything.

## 1. Make sure the binary is current

- Rebuild every target after a header change. A stale test binary against a
  new library gives crashes (`free(): invalid pointer`) and layout
  mismatches that look like real bugs.
- Test targets are often excluded from the default build target: build them
  explicitly, and check the binary's timestamp.

## 2. Is it new?

Run the same test on the base commit (a temporary worktree and build) under
the same load. Same failure, same message: pre-existing, report it, do not
claim it. Compare the failure messages, not just pass/fail.

## 3. Reproduce under load

- Start many copies at once and count the failures:

      for i in $(seq 1 24); do (./the_test >log.$i 2>&1; echo "$i $?" >>rc) & done; wait

- Or the test runner's own repetition: `ctest -R name --repeat until-fail:N`,
  `--gtest_repeat=N`, MTR `--repeat=N --parallel=N`.
- Keep the failure output: rerun with output on failure, because a later
  single run overwrites the runner's last log.
- A baseline rate (for example 13 of 24) lets the fix be verified
  (0 of 24), not just believed.

## 4. Common causes

- **The code under test:** a wait or race that only shows when a thread is
  descheduled (a hand-over that waits at the end of a run counted as
  throttling; a notification sent before the data is visible).
- **Absolute floating-point tolerances:** values grow with run length when a
  faster run gets further before the test stops it. `|t - n*0.001| < 1e-12`
  fails past about 8.2 million steps because one unit in the last place
  exceeds 1e-12 there. Use a tolerance relative to the expected value.
- **Fixed sizes or counts** that a faster or slower producer exceeds
  (buffer trims, preview limits, "at most N notifications").
- **Wall-clock deadlines** too tight for a loaded host.
- **Combined checks:** one `require(a && b, "a or b failed")` hides which
  one failed. Split them and report the failing step or value.

## 5. Fix and verify

- Fix the cause. Raise a timeout only when the deadline itself is the
  problem, and say so.
- Run the reproduction loop again and report both rates.
- Rerun the affected suite in parallel, then report: passed, failed with the
  same messages as the base commit, and new failures, if any.

## In Percona MLE

- `./ci.sh BUILD --parallel N` runs the unit-test binaries and MTR in a
  delegated cgroup and turns skips into failures; run the base commit's build
  the same way for step 2.
- MTR: the build's `mtr` wrapper with `--parallel=N --repeat=N` and a single
  test name; ci.sh uses `--retry=0`, which keeps intermittent failures
  visible. Unit tests are `runtime_output_directory/mle_*-t` (gtest:
  `--gtest_repeat=N`, `--gtest_filter=`).
- Worker start-up, pools and cgroup limits depend on load: suspect timing
  first for failures in cold-start, pool and memory-limit tests.
