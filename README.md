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

**Defaults reflect production configs, not a symmetric comparison.** Out of the
box each tool is built/run the way it actually ships:

* Only **string literals** as event names → both tools hit their static-name
  fast path (dynamic names would unfairly tax Perfetto's interner).
* Perfetto uses the **system backend** (`traced` socket) by default — the
  system-wide capture regime. Pass `--perfetto-backend inprocess` for the
  lighter, profiler-style path that's directly comparable to Tracy.
* Tracy is built with the **aqua DU production profile** by default (on-demand,
  lowest overhead), so with no client connected it short-circuits. Build with
  `-DTRACY_LIVE=ON` for the full producer path (always exercised, all features).
* A discarded **warmup** run pays one-time process/icache costs.

For the *symmetric* "both tools fully operational, lightest path" comparison,
use `-DTRACY_LIVE=ON` together with `--perfetto-backend inprocess`.

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
| `TRACY_LIVE` | OFF | build full live Tracy instead of the default aqua-DU profile |

> The Perfetto backend is **not** a build option — it's chosen at run time with
> `--perfetto-backend system|inprocess` (default `system`), so a single binary
> covers both. See [Run](#run).

### Tracy build profile (`TRACY_LIVE`)

By **default** `bench_tracy` is built exactly as the aqua DU production target
compiles Tracy — on-demand + every background feature off + low-res timer
fallback ("lowest overhead"). That's the config whose cost actually ships, so
it's what the benchmark measures unless you ask otherwise.

`-DTRACY_LIVE=ON` instead builds a **full default Tracy** (on-demand off, all
features on), which serializes every zone from program start whether or not a
client is connected — the heavier producer path, and the right thing to compare
against an enabled Perfetto session:

```bash
cmake -S . -B build-live -DCMAKE_BUILD_TYPE=Release -DTRACY_LIVE=ON
cmake --build build-live -j
```

The default profile forces `TRACY_ON_DEMAND`, `TRACY_NO_CONTEXT_SWITCH`,
`TRACY_NO_EXIT`, `TRACY_NO_SAMPLING`, `TRACY_NO_VSYNC_CAPTURE`,
`TRACY_NO_FRAME_IMAGE`, `TRACY_NO_SYSTEM_TRACING`, `TRACY_NO_CRASH_HANDLER`,
`TRACY_TIMER_FALLBACK` ON, and leaves callstack support available. Keep that
block in `CMakeLists.txt` in sync with the DU's `External/tracy` setup.

Because the default profile runs **on-demand**, there are two regimes worth
measuring:

| Regime | How to run | What it tells you |
|---|---|---|
| **Idle** (no profiler attached) | `build/bench_tracy` standalone | what the DU pays ~all the time: ~1–3 ns/event (just the short-circuit check) |
| **Active** (profiler attached) | run with `--tracy-capture` | full producer cost during a live session |

So the comparisons are: **idle default-Tracy vs system/category-disabled
Perfetto**, and **`TRACY_LIVE` Tracy vs in-process enabled Perfetto**.

> **`TRACY_NO_EXIT` and short runs.** This flag makes the Tracy client block at
> process exit until a profiler connects and drains — intended so short programs
> don't lose data. On macOS a standalone idle `bench_tracy` happened to exit
> anyway, but on Linux it does what the flag says and **hangs at exit**. Since
> the benchmark only needs the timing (already printed) and not the trace,
> `main()` does `fflush(stdout); _Exit(0)` right after emitting the CSV, which
> bypasses the atexit handler. `NO_EXIT` is exit-only and has no effect on the
> measured per-event overhead, so this changes no numbers.

Illustrative medians (M-series macOS, unpinned, 1M events/thread):

```
                              1 thread, work=0      4 threads, work=0
none baseline                     ~0.2 ns               ~0.06 ns
tracy default (DU, idle)          ~1.7 ns/ev            ~0.8 ns/ev   <- the DU's normal cost
tracy TRACY_LIVE (live)           ~8.2 ns/ev            ~2.7 ns/ev
perfetto --perfetto-backend inprocess  ~99.7 ns/ev wall  ~29.9 ns/ev wall  (~122 ns cpu)
```

> Note the headline numbers above were taken with Perfetto **in-process**. With
> the default **system** backend the producer hot path is similar, but the
> serialize/drain cost moves into `traced`, so this process's CPU column drops
> (see Caveats).

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
`--counters`, `--no-warmup`, `--no-pin`, `--buffer-kb`,
`--perfetto-backend system|inprocess` (default `system`), `--trace-file`.

> The **system** backend talks to the `traced` daemon. If `traced` is **not**
> running (e.g. a stock macOS box), the run does **not** hang or error — the
> consumer connection fails fast, the process exits cleanly, and you get a
> **0-byte trace** with a misleadingly-low ~1 ns/event (that's just the
> not-connected gating check in the TrackEvent macro, *not* real overhead).
> Treat a suspiciously-cheap system-backend number as "no daemon connected".
> For self-contained measurement use `--perfetto-backend inprocess`, which needs
> nothing external. To exercise the real system path, start `traced` first
> (`traced &` on Linux, or run under `tracebox`).

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

## Five-way comparison (`scripts/compare5.sh`)

The headline experiment: how much does instrumentation cost the **application
under test**, idle vs actively captured, for each tool? Five runs:

1. no profiling (`bench_none`)
2. perfetto enabled, **no consumer** — compiled in but nothing recording
3. perfetto enabled, **with consumer** — `traced` + a `perfetto` consumer capturing
4. tracy enabled, **no capture** — on-demand DU build, idle
5. tracy enabled, **with capture** — `tracy-capture` attached

This mirrors the Pharos/L1 topology: `bench_perfetto --perfetto-backend system`
is a **producer only** (like L1 with `perfettoSystemBackend=true`); an external
`perfetto` consumer owns the session and writes the file, so the consumer's
serialize/drain cost is *not* charged to the app's `RUSAGE`.

**Prerequisites**

* Only **case 3** needs the `traced` daemon + a `perfetto` consumer. Easiest is
  the prebuilt multitool (no source build):
  ```bash
  curl -LO https://get.perfetto.dev/tracebox && chmod +x tracebox
  ```
  Or build from source for separate `traced`/`perfetto` binaries
  (<https://perfetto.dev/docs/quickstart/linux-tracing>):
  ```bash
  tools/install-build-deps
  tools/gn gen out/linux --args='is_debug=false'
  tools/ninja -C out/linux tracebox traced perfetto
  ```
  Keep Perfetto roughly **version-aligned** with the vendored SDK (`perfetto.cc`
  reports `PERFETTO_VERSION_STRING`; IPC is compatible across nearby versions).
  To refresh the vendored copy, use Perfetto's **released, portable** SDK zip:
  ```bash
  bash scripts/fetch_perfetto_sdk.sh          # latest; or PERFETTO_VERSION=v57.0
  ```
  Do **not** vendor a locally `gen_amalgamated`-generated SDK: that bakes in only
  the *host* OS's system includes (e.g. a macOS-generated `perfetto.cc` omits
  `<linux/vm_sockets.h>` and won't compile on Linux). The released zip is
  generated portably and builds on macOS and Linux alike.
* A `tracy-capture` whose **protocol version matches** the Tracy submodule
  (this repo pins `third_party/tracy` to **v0.13.1**; use a 0.13.1 capture).

**Run** (needs a real shell — `traced`/Tracy capture bind sockets a sandbox blocks).
Point at the Perfetto binaries with **`TRACEBOX=`** (single prebuilt binary) or
**`PD=`** (a build dir with separate `traced`/`perfetto`):
```bash
TRACEBOX=./tracebox bash scripts/compare5.sh           # prebuilt multitool
PD=~/devel/perfetto/out/linux bash scripts/compare5.sh # or a source build dir
# knobs: REPS=11 EVENTS=200000 THREADS=1 WORK=0 CAPTURE=tracy-capture
```
`WORK` is xorshift rounds per region (~1.33 ns each, so `WORK=7500` ≈ 10 µs/region
for a realistic "instrument a chunky region" scenario; `WORK=0` isolates pure
per-event overhead). The script prints, per case, min/median wall and CPU
ns/event; overhead = case − case 1.

**Illustrative result** (Apple M-series, macOS, unpinned, 1 thread, work=0 —
ordering is the point, not the absolute figures):

| # | scenario | wall ns/ev | app overhead | cpu ns/ev |
|---|---|--:|--:|--:|
| 1 | none | 0.20 | — | 0.21 |
| 2 | perfetto, no consumer | 0.82 | +0.6 | 0.86 |
| 3 | perfetto, capturing | 102.6 | **+102** | 129.3 |
| 4 | tracy, no capture | 1.58 | +1.4 | 2.20 |
| 5 | tracy, capturing | 33.5 | **+33** | 48.8 |

Takeaways: compiled-in-but-idle is ~1 ns either way; while **actively
capturing, Tracy costs the app ~3× less** than Perfetto (~33 vs ~102 ns/event
hot-path). `cpu > wall` is the backend's in-process background thread (Perfetto's
muxer shipping to `traced`, Tracy's net-send thread) — real, and charged to the
app. At `WORK=7500` (~10 µs/region) these overheads are a sub-1% share and the
difference method gets noisy; instrument regions ≥ ~1 µs (Perfetto) / ~300 ns
(Tracy) for the cost to stay negligible.

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

* The two tools are **not perfectly symmetric**: Perfetto routes events to a
  service (the `traced` daemon by default, or an in-process service with
  `--perfetto-backend inprocess`); Tracy streams to a client. Both represent
  "tool fully operational" but the I/O paths differ — the producer hot path (the
  headline metric) is the comparable part.
* **The system backend moves cost off this process.** With the default
  `--perfetto-backend system`, serialization/drain happens inside `traced`, so
  `RUSAGE_SELF` (the CPU column) understates the real work — it looks cheaper
  than it is. Use `--perfetto-backend inprocess` to keep all cost in-process and
  visible to `getrusage`.
* `maxrss` units differ by OS (bytes on macOS, KiB on Linux).
* `trace_bytes` includes warmup events unless you pass `--no-warmup`.
* Results are sensitive to compiler, flags, and machine — always publish the config.
