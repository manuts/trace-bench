# trace-bench

A small, self-contained benchmark that measures the **per-event instrumentation
overhead** of [Perfetto](https://perfetto.dev) versus
[Tracy](https://github.com/wolfpld/tracy) in a multi-threaded C++ workload.

The same source is compiled three ways — `none`, `perfetto`, `tracy` — behind a
single neutral macro layer, and a driver script sweeps thread counts and
per-region work sizes, then reports each tool's marginal cost relative to the
uninstrumented baseline.

> **TL;DR on feasibility.** Yes, this is measurable — *if* you measure the
> marginal cost (backend minus baseline) over millions of events and control
> the confounders below. A single context-free "Perfetto is N× Tracy" number is
> meaningless; the ratio swings 2–4× with configuration. This harness makes the
> configuration explicit and reproducible.

---

## What it measures (and what it can't)

| Metric | Reported as | Meaning |
|---|---|---|
| **Disabled overhead** | build `none` vs the others with tracing off | should be ~0; sanity check |
| **Marginal producer cost** | `wall ns/event` at `--threads 1` | the serialize+enqueue hot path |
| **Per-event cost incl. drain** | `cpu ns/event` = (user+sys)/events | thread-count independent; charges the background service/drain thread too |
| **Scaling / contention** | `wall ns/event` across thread counts | does the shared buffer contend under load? |
| **Trace size on disk** | `trace_bytes` | bytes/event of the wire format (Perfetto) |
| **Peak memory** | `maxrss` | bytes (macOS) / KiB (Linux) |

**It cannot** produce one universal overhead number — that's not a limitation of
the harness, it's the nature of the thing. Report the config alongside the number.

### Wall vs CPU per event — read this before interpreting

With `N` parallel worker threads the regions run concurrently, so
`wall_ns / total_events` ≈ *per-thread cost ÷ N* — it measures **throughput**,
not per-event cost. Two clean reads:

* **`--threads 1`, `wall ns/event`** → the pure single-thread producer cost.
* **`cpu ns/event`** (any thread count) → per-event CPU cost, *including* the
  backend's background drain thread. This is the fair cross-thread metric and is
  roughly flat across thread counts (it rises only with real contention).

---

## Why Perfetto costs more per event (the thing under test)

This benchmark exists to put numbers on a known qualitative gap. Perfetto's
per-event hot path does more work than Tracy's, by design:

1. **Wire format** — Perfetto serializes each event as a self-describing
   **protobuf** `TracePacket` (protozero: varint tags, nested submessages).
   Tracy enqueues a compact fixed-layout POD referencing a compile-time
   source-location record.
2. **Timestamp source** — Perfetto defaults to a POSIX monotonic clock (needed
   to align with kernel ftrace / other processes). Tracy inlines `RDTSC`.
3. **Interning** — Perfetto interns names (cheap for static names, a hashmap hit
   for dynamic ones). Tracy emits a pointer to a precomputed struct.
4. **Shared-memory service** — Perfetto routes events through chunked SMB to a
   producer/service split that supports multi-process, system-wide capture.
   Tracy uses a single-producer lock-free queue.

Those costs buy Perfetto things Tracy doesn't do (system-wide traces, SQL,
cross-process, one format everywhere). This harness quantifies the price.

**Fairness guards baked in** (so the comparison isn't accidentally rigged):

* Only **string literals** as event names → both tools hit their static-name
  fast path (dynamic names would unfairly tax Perfetto's interner).
* Perfetto uses the **in-process backend**, not the `traced` system socket
  (the lighter, profiler-style path, comparable to Tracy).
* Tracy is built **without `TRACY_ON_DEMAND`** so its producer path is always
  exercised (on-demand would short-circuit to ~0 with no client and mislead).
* A discarded **warmup** run pays one-time process/icache costs.

---

## Layout

```
src/
  bench_trace.hpp     neutral macros -> Perfetto / Tracy / no-op (compile-time)
  workload.hpp        deterministic, non-elidable xorshift work (tunable size)
  harness.hpp         config, timing, getrusage, thread naming/pinning
  trace_backend.hpp   session lifecycle interface
  trace_backend.cpp   per-backend start()/stop() (the only #if-heavy file)
  main.cpp            arg parsing, barrier-synced worker pool, CSV row
third_party/perfetto/ vendored amalgamated Perfetto SDK (perfetto.h/.cc)
third_party/tracy/     Tracy (wolfpld) as a git submodule
scripts/run_matrix.py  driver: sweep, repeat, median, baseline subtraction
scripts/bench_linux.sh low-jitter Linux runner (governor + taskset + perf)
scripts/fetch_perfetto_sdk.sh  refresh the vendored Perfetto SDK from a release
CMakeLists.txt         builds bench_none / bench_perfetto / bench_tracy
```

Perfetto is **vendored** (amalgamated SDK) so the repo builds standalone. As of
~v54 Perfetto ships the amalgamated SDK as a release artifact
(`perfetto-cpp-sdk-src.zip`) rather than checking it into the source tree, so to
refresh to a newer version just run `scripts/fetch_perfetto_sdk.sh` (needs only
`curl` + `unzip`, no GN/clang toolchain) and commit the updated files. Tracy is a
**git submodule** at `third_party/tracy`, tracking `master`, pinned to commit
`9a785976` (self-reports as 0.13.4-dev — the commit all current numbers were
taken against).

---

## Build

Requires CMake ≥ 3.16 and a C++17 compiler. **Clone with submodules** so Tracy
comes along:

```bash
git clone --recursive git@github.com:manuts/trace-bench.git
# or, if you already cloned without --recursive:
git submodule update --init --recursive
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

That builds all three variants — `bench_none`, `bench_perfetto`, `bench_tracy` —
because `TRACY_SOURCE_DIR` defaults to the bundled `third_party/tracy` submodule.
(If the submodule isn't initialized, `bench_tracy` is skipped with a warning.)
To use a Tracy checkout elsewhere, pass `-DTRACY_SOURCE_DIR=/path/to/tracy`.

### Build options

| Option | Default | Effect |
|---|---|---|
| `WITH_PERFETTO` | ON | build `bench_perfetto` |
| `WITH_TRACY` | ON | build `bench_tracy` |
| `TRACY_SOURCE_DIR` | `third_party/tracy` | Tracy checkout (default: the submodule) |
| `TRACY_ON_DEMAND` | OFF | build Tracy with on-demand profiling (see below) |
| `TRACY_DU_PROFILE` | OFF | build Tracy exactly as the aqua DU does (see below) |

**`TRACY_ON_DEMAND`** gives you a third data point. Build a separate tree with
it ON and compare against the default:

```bash
cmake -S . -B build-ondemand -DCMAKE_BUILD_TYPE=Release -DTRACY_ON_DEMAND=ON
cmake --build build-ondemand -j
```

* **OFF (default):** Tracy serializes every zone from program start regardless
  of whether a client is connected — the **live producer cost**, the fair
  comparison vs Perfetto's always-on session.
* **ON, with no client connected:** the zone macros short-circuit — this
  measures Tracy's **compiled-in-but-idle** cost, the right comparison against
  Perfetto with its category *disabled*. Illustrative (1 thread, work=0):
  `none ≈ 0.2`, `on-demand ON ≈ 2`, `on-demand OFF ≈ 8`, `perfetto ≈ 97` ns/event.

### The aqua DU profile (`TRACY_DU_PROFILE`)

The aqua DU compiles Tracy in an on-demand, "lowest overhead" configuration
(on-demand + every background feature off + low-res timer fallback).
`-DTRACY_DU_PROFILE=ON` replicates that flag set exactly, so the benchmark
measures the overhead Tracy *actually* incurs in the DU rather than a default
build:

```bash
cmake -S . -B build-du -DCMAKE_BUILD_TYPE=Release \
      -DTRACY_SOURCE_DIR=/path/to/tracy -DTRACY_DU_PROFILE=ON
cmake --build build-du -j
```

It forces: `TRACY_ON_DEMAND`, `TRACY_NO_CONTEXT_SWITCH`, `TRACY_NO_EXIT`,
`TRACY_NO_SAMPLING`, `TRACY_NO_VSYNC_CAPTURE`, `TRACY_NO_FRAME_IMAGE`,
`TRACY_NO_SYSTEM_TRACING`, `TRACY_NO_CRASH_HANDLER`, `TRACY_TIMER_FALLBACK` ON,
and leaves callstack support available. Keep this block in sync with the DU's
`External/tracy` setup. It overrides the standalone `TRACY_ON_DEMAND` option.

Because the DU runs **on-demand**, there are two regimes worth measuring:

| Regime | How to run | What it tells you |
|---|---|---|
| **Idle** (no profiler attached) | `build-du/bench_tracy` standalone | what the DU pays ~all the time: ~1–3 ns/event (just the short-circuit check) |
| **Active** (profiler attached) | run with `--tracy-capture` | full producer cost during a live session |

The fair Perfetto comparison is therefore: **idle DU-Tracy vs category-disabled
Perfetto**, and **active DU-Tracy vs enabled Perfetto**.

> **`TRACY_NO_EXIT` is safe here.** That flag makes the client block at exit
> until a server drains it — but in on-demand mode with no client, no session is
> active, so there is nothing to flush and the process exits cleanly. Verified:
> a standalone `build-du/bench_tracy` run returns immediately (exit 0), it does
> not hang. (If you attach a client mid-run, exit *will* wait for the drain.)

Illustrative medians (M-series macOS, unpinned, 1M events/thread):

```
                          1 thread, work=0      4 threads, work=0
none baseline                 ~0.2 ns               ~0.06 ns
tracy DU-profile (idle)       ~1.7 ns/ev            ~0.8 ns/ev   <- the DU's normal cost
tracy default  (live)         ~8.2 ns/ev            ~2.7 ns/ev
perfetto (in-process)        ~99.7 ns/ev wall      ~29.9 ns/ev wall  (~122 ns cpu)
```

> If you push this to GitLab, add Tracy as a git submodule and set
> `TRACY_SOURCE_DIR` to it, or keep passing a local path. Tracy ≥ 0.13 is fine.

---

## Run

### One binary directly

```bash
build/bench_perfetto --threads 4 --events 1000000 --work 0
# -> backend,threads,events_per_thread,...,ns_per_event,checksum
```

Flags: `--threads`, `--events` (per thread), `--work` (xorshift rounds/region),
`--counters`, `--no-warmup`, `--no-pin`, `--buffer-kb`, `--trace-file`.

### The full sweep (recommended)

```bash
python3 scripts/run_matrix.py --build \
        --tracy-dir /path/to/tracy \
        --reps 7 --threads 1,4,8 --work 0,50,500 --events 1000000 \
        --raw-csv results.csv
```

Example output (Apple M-series, macOS, unpinned — *illustrative, not canonical*):

```
=== per-event instrumentation overhead vs `none` baseline ===
threads  work | perfetto wall perfetto cpu |    tracy wall    tracy cpu |
      1     0 |         98.37       119.68 |         15.09        25.04 |
      4     0 |         30.14       121.05 |          4.74        20.77 |
      4   200 |         26.94       122.33 |         12.02        54.40 |
```

Reading it: at `threads=1, work=0`, Perfetto adds ~98 ns/event vs Tracy's ~15
(producer hot path); the CPU column (~120 vs ~25 ns/event) is flat across thread
counts and includes each tool's drain thread. As `--work` grows, both shrink as
a *fraction* of the region — the practical takeaway for where instrumentation
stops mattering.

---

## Capturing a Tracy trace

By default `bench_tracy` records into Tracy's in-memory queue but writes no
file (Tracy needs a connected client). To save a `.tracy`, build Tracy's capture
tool and let the driver drive it over loopback:

```bash
# build tracy-capture from your Tracy checkout (see Tracy docs), then:
python3 scripts/run_matrix.py ... --tracy-capture /path/to/tracy-capture
```

Without a client, Tracy's queue grows in RAM (no stall) — fine for bounded
`--events`, but watch `maxrss` for very large runs.

---

## Reducing measurement jitter

ns/event jitter comes from CPU frequency scaling (turbo), the scheduler moving
threads between cores, background load, and — on Apple Silicon — threads landing
on efficiency vs performance cores. The baseline subtraction amplifies it. Two
rules tame most of it:

**1. Trust the right statistic.** Noise is *one-sided* — it only ever makes a
run slower. So across many reps the **minimum** is the least-disturbed estimate,
and the **stdev / coefficient-of-variation** tells you whether to believe the
number at all. Pull these from `--raw-csv`:

```python
# min is the robust estimator; trust it only when CV is small
import csv, statistics as st
xs = [float(r['ns_per_event']) for r in csv.DictReader(open('results.csv'))
      if r['backend']=='perfetto']
print(min(xs), st.median(xs), st.pstdev(xs)/st.mean(xs))
```

**2. On Linux, pin everything.** macOS gives wide error bars (no hard affinity,
turbo, P/E cores); for canonical numbers use an idle Linux box:

```bash
sudo cpupower frequency-set -g performance        # pin governor
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo  # kill turbo (Intel)
# dedicate cores at boot: isolcpus=2-9 nohz_full=2-9 on the kernel cmdline, then:
taskset -c 2-9 python3 scripts/run_matrix.py --reps 15 --threads 1,4 \
        --work 0,1000 --events 2000000 --raw-csv results.csv
# best of all: count CYCLES, immune to frequency drift:
perf stat -e cycles,instructions build/bench_perfetto --threads 1 --events 5000000
```

`scripts/bench_linux.sh` wraps the governor + `taskset` + optional `perf
cycles/event` in one command (`sudo scripts/bench_linux.sh --cycles`). Worker
threads also self-pin to cores on Linux (`--no-pin` to disable).

A few more knobs: raise `--events` (longer runs average out scheduler blips),
keep `--threads ≤` physical cores (leave a core free for the backend's drain
thread), and use `--threads 1` when you want the clean per-event producer cost
without cross-thread contention.

---

## Caveats

* The two tools are **not perfectly symmetric**: Perfetto writes a file
  in-process; Tracy streams to a client. Both represent "tool fully operational"
  but the I/O paths differ — the producer hot path (the headline metric) is the
  comparable part.
* `maxrss` units differ by OS (bytes on macOS, KiB on Linux).
* `trace_bytes` includes warmup events unless you pass `--no-warmup`.
* Results are sensitive to compiler, flags, and machine — always publish the config.
