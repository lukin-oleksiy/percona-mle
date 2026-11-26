---
name: decision-records-with-triggers
description: Take open design or engineering decisions to the user one at a time, with options, a recommendation and the evidence, then record each as DECIDED with a measurable trigger for reopening it. Use when a plan, review or performance investigation leaves several decisions open, when the user says "let's discuss the decisions", or when a recorded decision's trigger may have fired.
---

# Decisions one at a time, with reopening triggers

A decision without a reopening condition gets argued again without new
evidence, or never revisited when the evidence changes. Record both.

## 1. Present one decision at a time

For each decision, in order of how much later work depends on it:

- **What it is:** the mechanism, in two or three sentences, with file
  references.
- **What was decided before**, if anything, and what has changed since.
- **Evidence:** measurements with how they were taken, and what was not
  measured. If the evidence the decision should rest on is missing, offer
  "measure first" as an option.
- **Options:** two to four, each with its cost and risk.
- **Recommendation:** one option and why.
- End with the question ("A, B or C?") and wait. Do not start the next
  decision, or implement this one, before the answer.

## 2. Record the decision

In the project's plan or decision log (ADRs where the project has them):

    - **D7 — <subject>. DECIDED <date>: <choice>.**
      - Why: the evidence, with numbers.
      - Rejected: the other options, and why.
      - Reopen when: <a measurable condition, and the tool that measures it>.

- The trigger must be checkable by a tool the project already has, with a
  number ("a viewer call longer than a quarter of the ring's buffering time
  in the load benchmark"), not "if performance becomes a problem".
- Name the workload the trigger applies to. A stress case that exaggerates
  the cost should not reopen a decision about realistic workloads.
- When a later measurement hits a trigger, say so plainly: the decision is
  open again. That does not mean the other option wins; it goes back through
  step 1.

## 3. Afterwards

- Implement the decided options together after the round, unless the user
  asks for each one at once.
- Keep the decision log and the pull request description in step: a
  "decisions settled in review" section, with each choice and its trigger.

## In Percona MLE

- Performance triggers can name `perf/regression.py` thresholds (its default
  is 5%, per-case overrides in `THRESHOLDS`) or `perf/bench.py` cases, with
  the machine and governor they were measured on.
- Record decisions where the repository already keeps them (the review and
  protocol documents under `docs/`, the README's performance section); ask
  before starting a new decision log.
