# Performance tools

| Tool | What it answers |
|---|---|
| `regression.py` | Is a change slower? Two MLE builds side by side, alternating, with a verdict per case (`setup.sql`). |
| `bench.py` | How fast is one build, and how much memory does it take, per runtime? Absolute numbers for every worker kind and for SQL stored programs (`bench_setup.sql`). |
| `e2e/` | Against the same logic in an application: clients over a network with a delay (its own README). |

`mle_perf.py` is what they share: a server of a server build with one MLE build in a plugin directory of its own,
a data directory made once from a setup file and reused, sessions kept open, the machine's state.

## bench.py

    perf/bench.py --server <release server build> --mle <release MLE build> [--json results.json]
                  [--suite calls,library,cold,memory] [--quick] [--only REGEX] [--wait-quiet]

One server runs every worker kind at once (a routine's `LANGUAGE` chooses it): the JVM worker (GraalJS, GraalPy,
Java), the CPython worker and the V8 worker. When the server build has them, js_lang (V8 inside mysqld) and the
language router are installed too; the router maps `LANGUAGE JS` to the V8 worker, and to js_lang around the
js_lang measurements. The same work is written once per runtime in `bench_setup.sql`.

| Suite | Measures |
|---|---|
| `calls` | a call (`a + b`) and per-row logic (Levenshtein) per row, net of a plain scan; JavaScript compute (numbers, objects, JSON); SQL from routines: 100,000 rows read one by one, small statements, `executeMany` of 1,000 rows; vectors: the 10 nearest of 100,000 (SQL, HNSW with its recall, numpy, Java's Vector API), a `VECTOR(768)` argument per row. The runtimes' results are compared. |
| `library` | the built-in vector library (related items of 10,000 vectors) from Java, JavaScript and Python, against numpy in GraalPy and in CPython (one BLAS thread in both workers), and the library with one thread |
| `cold` | a new connection's first call, at the client, per runtime (with numpy too): without a pool (a new worker) and with one |
| `memory` | PSS per worker: one worker, and eight at once, per runtime (with numpy too); the JVM worker with its AOT cache relocated (the JDK's default) against mapped at its own address (MLE's); js_lang's cost per session in mysqld |

Statements are timed inside the server (`SYSDATE(6)`), in one session, after a warm-up that ends when two runs in a
row are within 3 % (at most 20); the best of 5 runs is reported (all runs are in the JSON). Cold starts are the
median of 7 new `mysql` processes. A full run takes about 10 minutes on a 16-CPU laptop; the data directory (1M rows, 100,000 vectors
with an HNSW index) is made once in `<MLE build>/perf-bench` in about 2 minutes. `--quick` (fewer runs) checks the
tool; its numbers are not for publishing.

Needs:
- release builds (RelWithDebInfo) of the server (with the router and js_lang for those numbers) and of MLE
  (`MLE_CPYTHON` and `MLE_V8` on);
- `sudo -n` for the `memory` suite: workers are not dumpable, so their `/proc/<pid>/smaps_rollup` needs root (the
  suite is skipped without it);
- a quiet machine and the CPU governor `performance` for stable numbers (`--wait-quiet` waits for a load average
  below 1.5); the report and the JSON state the machine, the builds' commits and the runtimes' versions.

The JSON: `environment` (CPU, memory, kernel, governor, load, commits, versions, the MLE settings) and `results`,
keyed `<suite>.<case>.<runtime>`, each with `value` and `unit` (`us_per_row`, `us_per_stmt`, `ms`,
`MiB_pss_per_worker`, ...) and the measurements behind it.
