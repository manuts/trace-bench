#!/usr/bin/env python3
"""
Drive the trace-bench matrix: run each backend across a sweep of thread counts
and per-region work sizes, take the median over repetitions, and report the
marginal per-event instrumentation overhead (backend minus the `none`
baseline).

Examples
--------
# build (if needed) and run the default sweep
python3 scripts/run_matrix.py --build --tracy-dir /Users/manu/workspaces/tracy

# custom sweep, more reps, write raw rows to a CSV
python3 scripts/run_matrix.py --reps 11 --threads 1,4,8 --work 0,50,500 \
        --events 1000000 --raw-csv results.csv

Notes
-----
* The marginal overhead is computed as ns_per_event(backend) - ns_per_event(none)
  at the SAME (threads, work, events) point. Run on an otherwise-idle machine;
  on Linux pin the CPU frequency and add isolated cores for the cleanest numbers.
* Tracy writes no trace file unless a capture client is connected. Pass
  --tracy-capture /path/to/tracy-capture to record one over loopback.
"""
import argparse
import os
import shutil
import statistics
import subprocess
import sys
import time

BACKENDS = ["none", "perfetto", "tracy"]
FIELDS = ["backend", "threads", "events_per_thread", "work_iters", "counters",
          "total_events", "wall_ns", "cpu_user_ns", "cpu_sys_ns", "maxrss",
          "trace_bytes", "ns_per_event", "checksum"]


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def configure_and_build(src, build_dir, tracy_dir):
    args = ["cmake", "-S", src, "-B", build_dir, "-DCMAKE_BUILD_TYPE=Release"]
    if tracy_dir:
        args.append(f"-DTRACY_SOURCE_DIR={tracy_dir}")
    sh(args)
    sh(["cmake", "--build", build_dir, "-j", str(os.cpu_count() or 4)])


def parse_row(stdout):
    """Return the CSV data row (dict) from a binary's stdout, ignoring any
    Perfetto log noise."""
    for line in reversed(stdout.strip().splitlines()):
        parts = line.split(",")
        if len(parts) == len(FIELDS) and parts[0] in BACKENDS:
            return dict(zip(FIELDS, parts))
    raise RuntimeError(f"no CSV row found in output:\n{stdout}")


def run_one(binary, threads, events, work, counters, extra, tracy_capture):
    cmd = [binary, "--threads", str(threads), "--events", str(events),
           "--work", str(work)]
    if counters:
        cmd.append("--counters")
    cmd += extra

    cap = None
    if tracy_capture and binary.endswith("bench_tracy"):
        # Launch the capture client; it waits for the app to connect on loopback.
        out = f"/tmp/trace_bench_{threads}t_{work}w.tracy"
        cap = subprocess.Popen([tracy_capture, "-o", out, "-f", "-a", "127.0.0.1"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.3)

    proc = subprocess.run(cmd, capture_output=True, text=True)
    if cap:
        try:
            cap.wait(timeout=10)
        except subprocess.TimeoutExpired:
            cap.terminate()
    if proc.returncode != 0:
        raise RuntimeError(f"{binary} failed:\n{proc.stderr}")
    return parse_row(proc.stdout)


def medians(rows):
    """Return (wall_ns_per_event, cpu_ns_per_event) medians.

    wall_ns_per_event with N threads reflects THROUGHPUT (regions run in
    parallel, so it scales ~1/N). cpu_ns_per_event = (user+sys)/total_events is
    thread-count independent and also charges a backend's background drain
    thread -- it is the fairer "cost per event" for multi-threaded runs.
    """
    total = int(rows[0]["total_events"])
    walls = [int(r["wall_ns"]) for r in rows]
    cpus = [int(r["cpu_user_ns"]) + int(r["cpu_sys_ns"]) for r in rows]
    return statistics.median(walls) / total, statistics.median(cpus) / total


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--src", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--build", action="store_true", help="(re)configure and build first")
    ap.add_argument("--tracy-dir", default="", help="Tracy checkout (for --build)")
    ap.add_argument("--tracy-capture", default="", help="path to tracy-capture (records a .tracy)")
    ap.add_argument("--reps", type=int, default=7)
    ap.add_argument("--threads", default="1,4,8", help="comma list of thread counts")
    ap.add_argument("--work", default="0,50,500", help="comma list of xorshift rounds/region")
    ap.add_argument("--events", type=int, default=1_000_000, help="regions per thread")
    ap.add_argument("--counters", action="store_true")
    ap.add_argument("--no-pin", action="store_true")
    ap.add_argument("--backends", default=",".join(BACKENDS))
    ap.add_argument("--raw-csv", default="", help="write every rep's row here")
    args = ap.parse_args()

    if args.build:
        configure_and_build(args.src, args.build_dir, args.tracy_dir)

    backends = [b for b in args.backends.split(",") if b]
    binaries = {}
    for b in backends:
        path = os.path.join(args.build_dir, f"bench_{b}")
        if os.path.exists(path):
            binaries[b] = path
        else:
            print(f"! bench_{b} not built; skipping", file=sys.stderr)

    thread_list = [int(x) for x in args.threads.split(",")]
    work_list = [int(x) for x in args.work.split(",")]
    extra = ["--no-pin"] if args.no_pin else []

    raw_rows = []
    summary = {}  # (threads, work) -> {backend: (ns_per_event, median_wall)}

    for threads in thread_list:
        for work in work_list:
            summary[(threads, work)] = {}
            for b, binary in binaries.items():
                rows = []
                for _ in range(args.reps):
                    r = run_one(binary, threads, args.events, work,
                                args.counters, extra, args.tracy_capture)
                    rows.append(r)
                    raw_rows.append(r)
                npe, cpe = medians(rows)
                summary[(threads, work)][b] = (npe, cpe)
                print(f"  ran {b:9s} threads={threads} work={work}: "
                      f"{npe:7.2f} ns/ev wall, {cpe:7.2f} ns/ev cpu "
                      f"(median of {args.reps})", file=sys.stderr)

    if args.raw_csv:
        with open(args.raw_csv, "w") as f:
            f.write(",".join(FIELDS) + "\n")
            for r in raw_rows:
                f.write(",".join(r[k] for k in FIELDS) + "\n")
        print(f"\nwrote raw rows -> {args.raw_csv}", file=sys.stderr)

    # ---- summary table ----
    # Overhead = backend minus the `none` baseline at the same point, reported
    # both as wall ns/event (throughput-ish) and CPU ns/event (per-event cost).
    print("\n=== per-event instrumentation overhead vs `none` baseline ===")
    print(f"    (median of {args.reps} reps, {args.events} events/thread; "
          "wall scales ~1/threads, cpu does not)")
    head = f"{'threads':>7} {'work':>5} | "
    for b in ("perfetto", "tracy"):
        if b in binaries:
            head += f"{b+' wall':>13} {b+' cpu':>12} | "
    print(head)
    for (threads, work), d in summary.items():
        base_wall, base_cpu = d.get("none", (0.0, 0.0))
        line = f"{threads:>7} {work:>5} | "
        for b in ("perfetto", "tracy"):
            if b in d:
                w, c = d[b]
                line += f"{w - base_wall:>13.2f} {c - base_cpu:>12.2f} | "
        print(line)


if __name__ == "__main__":
    main()
