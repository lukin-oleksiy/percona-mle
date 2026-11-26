---
name: cost-attribution-ab
description: Measure what one component, option or code path costs by running two variants that differ only in it (built with it and without it, or a JVM/runtime option on and off), alternating runs on a shared machine. Use when a decision depends on "how much does X cost?", when deciding whether an optimization is worth it, or when a profile share needs confirming before acting on it. Not for whole-build regression checks when the project already has a runner for them.
---

# Cost attribution by A/B

A profiler's share is a hint; the cost of X is the difference between two
otherwise identical runs, one with X and one without. Measure that difference
before deciding anything that depends on it.

## 1. Use the project's runner first

Look for an existing benchmark or regression runner before writing anything.
If it already alternates builds, waits for a quiet machine and reports
medians, feed it the two variants. Write a new tool only when nothing fits,
and then keep it in the repository with `--help`, not as a one-off command.

## 2. Make the variants differ in exactly one thing

- **Code path:** build the same commit twice, the second with X compiled out
  behind a macro defined only for that build (`-DSKIP_X`). Put the macro in a
  temporary worktree, never in the committed source, and remove the worktree
  afterwards.
- **Option:** the same build, run with the option on and off.
- Check that each binary loads its own libraries (`ldd`, rpath), not the
  other build's.
- Skipping X must not change results on the measured workload. Compare the
  outputs or checksums of both variants; if they differ, the measurement is
  invalid, and that difference is itself a finding.

## 3. Run fairly on a shared machine

- Alternate the order (A-B, B-A, ...) so background load hits both, and take
  the median of at least 5 rounds (11 for small differences).
- Make each run long enough to cover timer resolution and scheduler noise:
  if per-step medians come out quantized (multiples of 10 ns), use averages
  over longer runs.
- Wait until the 1-minute load average is low and record the load each run
  saw. Use the CPU governor `performance` where the project requires it.
- Other agents may be benchmarking: ask before a long run if the host is
  shared, and pause (do not abandon) when one asks you to.
- Pin to cores (`taskset`) for single-thread code paths.

## 4. Know what a "slow machine" emulation does and does not do

A CPU quota (`systemd-run --user --scope -p CPUQuota=60%`) throttles in
quota periods; it does not make the CPU slower. Shares and ratios come out
the same as unthrottled, short runs pick up throttling noise, and maximum
latencies show the quota period (tens of ms), not lock contention. Use it to
study multi-thread contention for a limited CPU, not to predict cost ratios
on slower hardware.

## 5. Report

- A table per case: A, B, difference, share of total. Name the measure
  (average or median), the run length, rounds and load.
- Separate cases that stress X artificially from realistic ones, and say
  which one a decision should rest on.
- State what was not measured.
- If the result settles a decision, record it with a trigger for reopening
  (see the `decision-records-with-triggers` skill).

## In Percona MLE

- `perf/regression.py --base <build> --new <build> --wait-quiet` already
  alternates two MLE builds per round, compares medians of the best and the
  median runs, and checks the load and governor. For "build with X" against
  "build without X", configure two builds and pass them as base and new.
- `perf/bench.py ... --wait-quiet --json` measures every runtime of one build;
  run it once per variant for option A/Bs (JVM options), alternating the
  variant between invocations.
- Release (RelWithDebInfo) builds only. Delete the `--work` data directory
  after changing `perf/setup.sql`.
- The worker processes are not dumpable; memory measurements need `sudo -n`.
