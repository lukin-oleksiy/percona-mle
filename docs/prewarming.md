# Workers: pools, pre-warming and reuse

For administrators: what decides how long a session's first call of a
routine takes, and what memory buys a shorter one. Three groups of server
variables control it: the pools, `mle.pool_prewarm`, and the worker cache.
A fourth, `mle.idle_trim_ms`, decides what a worker gives back while its
session is idle.

## A worker's life

* A session gets one worker per kind (JVM, CPython, V8) and account when it
  first calls a routine of that kind. A JVM worker runs Java, JavaScript
  (`LANGUAGE JAVASCRIPT`) and Python (`LANGUAGE PYTHON`) in one process.
* The worker comes from the **pool** if one is waiting there: a worker the
  launcher started in advance. The launcher refills the pool at once.
  Otherwise the session waits while a worker starts.
* A new worker prepares what its kind always prepares, plus what
  `mle.pool_prewarm` lists, before it answers READY. The rest is set up on
  the first call that needs it.
* When the session ends, its workers are reset (no routines, no session
  state). The **worker cache** keeps them, warm, for the same account's next
  session, until it times out. Otherwise they end.

So a session's first call is fast when its worker comes warm from the cache,
or from a pool that prepared the routine's language. It is slow when the
worker starts while the session waits, or when its language still has to be
set up.

## What a worker prepares

| Worker | Always, before READY | With `mle.pool_prewarm` |
|---|---|---|
| JVM | Java (the compiler ready), the polyglot engine and context that its languages share | `js`: JavaScript; `python`: GraalPy; `numpy`: GraalPy and numpy; `vectors`: the vector library's kernels compiled |
| CPython | Python and MLE's own code | `numpy`: numpy imported |
| V8 | V8 and MLE's JavaScript API | nothing to choose |

`js`, `python` and `numpy` apply to every JVM worker the server starts:
pool workers, and workers started for a session that found the pool empty.
That session waits for them too. `vectors` runs after READY and only while
the worker waits for a request, so a pool worker warms up in the pool, and a
worker started for a waiting session skips it.

## `mle.pool_prewarm`

`none`, or a comma list of `js`, `python`, `numpy` and `vectors`, in any
letter case. Default: `vectors`. The value replaces the default, so keep
`vectors` in a list unless you do not want it:

    SET GLOBAL mle.pool_prewarm = 'js,vectors';

A change restarts the engine. It is refused while a session holds a worker.
Measured on main `3b5a8e2`: a pool worker's first call of each kind, ms at
the client including the connection (median of 7 new `mysql` processes),
and what an idle pool worker takes (PSS, two pool workers):

| `mle.pool_prewarm` | Idle pool worker | First JavaScript call | First Python call | Python with `import numpy` | First Java call |
|---|---:|---:|---:|---:|---:|
| `vectors` (default) | 183 MB | 174 | 1,996 | 5,032 | 56 |
| `js,vectors` | 206 MB | **18** | 1,987 | 5,307 | 56 |
| `python,vectors` | 539 MB | 139 | **96** | 3,927 | 59 |
| `numpy,vectors` | 755 MB | 148 | **42** | **44** | 58 |

* `js` costs about 20 MB per JVM worker and about 150 ms of its start. It
  saves about 155 ms on the first JavaScript call.
* `python` costs about 355 MB per JVM worker and 1–5 s of its start. It
  saves about 1.9 s on the first Python call.
* `numpy` costs about 570 MB per JVM worker. Python's first `import numpy`
  in a worker otherwise takes about 5 s.
* `vectors` costs about 2 CPU-seconds per pool worker, spent while it
  waits, and no memory to speak of. The vector library's first call is then
  as fast as later ones: related items of 10,000 × `VECTOR(768)` take
  0.23 s instead of 0.55 s (exp note 35).
* `numpy` also makes a CPython worker import numpy before READY. Its first
  `import numpy` otherwise adds about 190 ms (a pooled CPython worker's
  first call: 17 ms, with `import numpy` 206 ms), and a CPython worker with
  numpy takes about 11 MB more.

These costs hold for every JVM worker, not only for the pool: with `python`,
a Java-only workload pays 355 MB per worker for Python it never runs.

## Choosing

* **The default** (`vectors`) suits most servers. Java workers stay small (a
  Java worker takes about 145 MB with eight at once). Each language is set
  up once per worker, on its first call. With the worker cache, later
  sessions of the same account get that worker warm.
* **JavaScript first calls matter** (an application that opens a new
  connection per request and calls JavaScript routines at once): `js,vectors`.
* **Python with numpy is the main workload, and memory allows it:**
  `numpy,vectors`. Without it a fresh worker's first numpy call takes
  seconds.
* **Memory is tight:** `none`, small pools, and a smaller worker cache. Every
  first call then pays its language's setup.

## Pool and cache sizes

| Variable | Default | |
|---|---|---|
| `mle.pool_size`, `mle.cpython_pool_size`, `mle.v8_pool_size` | 2 each | workers of each kind kept ready for new sessions; 0: none |
| `mle.worker_cache_size` | 4 | idle, reset workers kept for the same account's next session; 0: a session's workers end with it |
| `mle.worker_cache_timeout` | 60 s | how long a kept worker waits for a session |

What the pools and the cache hold costs memory while nothing runs: for the
JVM pool, `mle.pool_size` × the idle pool worker above (366 MB with the
defaults). A CPython or V8 pool worker takes 10–20 MB, so their pools are
cheap.

## Memory of an idle session: `mle.idle_trim_ms`

A worker keeps the memory of its last calls while its session stays
connected and idle, as an application's connection pool keeps sessions. A
JVM worker with Python set up gives memory back after `mle.idle_trim_ms`
(default 5000) of idleness: a full GC and `malloc_trim`, once per idle
period. `0` turns it off. A call that comes while the trim runs waits for
it. Measured on one worker idle in its session:

| Worker | Given back | The trim's pause |
|---|---:|---:|
| GraalPy | about 80 MB (−15 %) | about 80 ms |
| GraalPy with numpy | about 60 MB (−9 %) | about 145 ms |
| Java, GraalJS | nothing: they are not trimmed | |

A worker counts as having Python set up once any Python ran or was checked
in it: also a Java session's worker that an earlier session of the account
used for Python, through the worker cache.

## Watching it

| Status variable | What it counts |
|---|---|
| `mle_pool_idle` | workers waiting in the pools |
| `mle_workers` | workers sessions hold now |
| `mle_workers_spawned` | workers started since the engine started |
| `mle_workers_cached` | reset workers kept for reuse |
| `mle_worker_reuses` | sessions that got a kept worker |
| `mle_worker_memory_gcs` | GCs the workers forced: their memory watchers' during calls, and idle trims, counted when a session releases its workers |

Many spawned workers against few reuses means sessions find no warm worker:
a larger worker cache or pool helps, if memory allows.

## How the numbers were measured

26 Nov 2026, release builds of main `3b5a8e2`, GraalVM CE 25.6.5.1, the
laptop of the README's Performance section, CPU governor `performance`.
`mle.worker_cache_size` was 0, so every call came from a fresh pool worker.
Each value was set with `SET GLOBAL mle.pool_prewarm`; the pools refilled,
and each call waited 10 s after the refill. The idle pool worker is the
mean PSS of the two pooled JVM workers before any call. The figures for
`vectors` are from exp note 35, and CPython's from the README's tables (the
same run). They vary by about ±5–10 % between runs on this machine.
