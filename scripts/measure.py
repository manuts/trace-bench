#!/usr/bin/env python3
# trace-bench measurement helper. Two modes:
#
#   run mode:  measure.py <reps> <label> -- <command...>
#       Runs <command> <reps> times, parses the trace-bench CSV row (last stdout
#       line of each run), and prints robust stats.
#
#   agg mode:  <something emitting CSV rows> | measure.py --agg <label>
#       Reads already-produced CSV rows from stdin and prints the same stats.
#       Used when the caller orchestrates each run itself (e.g. Tracy capture,
#       which needs a fresh client per run).
#
# Reports min + median for wall ns/event and CPU ns/event. min is the robust
# estimator (jitter is one-sided); CV flags whether to trust it.
import subprocess, sys, statistics as st

# CSV columns: backend,threads,events_per_thread,work,counters,total_events,
#              wall_ns,cpu_user_ns,cpu_sys_ns,maxrss,trace_bytes,ns_per_event,checksum
def parse(line):
    f = line.split(",")
    total = float(f[5])
    wall = float(f[11])
    cpu = (float(f[7]) + float(f[8])) / total if total else 0.0
    return wall, cpu, int(f[10])

def stats(label, rows):
    if not rows:
        print(f"{label}  NO DATA"); return
    wall, cpu, tb = [], [], 0
    for r in rows:
        w, c, b = parse(r); wall.append(w); cpu.append(c); tb = b
    cv = (st.pstdev(wall) / st.mean(wall)) if st.mean(wall) else 0
    print(f"{label}  wall ns/ev min={min(wall):8.3f} med={st.median(wall):8.3f}  "
          f"cpu ns/ev min={min(cpu):8.3f} med={st.median(cpu):8.3f}  "
          f"cv={cv*100:4.1f}%  trace_bytes={tb}")

def is_csv(line):
    return line.count(",") >= 10

if sys.argv[1] == "--agg":
    label = sys.argv[2]
    rows = [l.strip() for l in sys.stdin if is_csv(l)]
    stats(label, rows)
else:
    reps = int(sys.argv[1]); label = sys.argv[2]
    assert sys.argv[3] == "--"
    cmd = sys.argv[4:]
    rows = []
    for _ in range(reps):
        r = subprocess.run(cmd, capture_output=True, text=True)
        out = [l for l in r.stdout.strip().splitlines() if is_csv(l)]
        if not out:
            print(f"{label}  NO CSV (stderr: {r.stderr.strip()[:100]})"); sys.exit(1)
        rows.append(out[-1])
    stats(label, rows)
