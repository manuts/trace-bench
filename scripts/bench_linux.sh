#!/usr/bin/env bash
#
# Low-jitter benchmark runner for Linux. Pins CPU frequency, isolates the run
# to a fixed set of physical cores, and (optionally) reports cycle counts via
# perf -- cycles/event is immune to frequency drift, unlike ns/event.
#
# Usage:
#   sudo scripts/bench_linux.sh                 # full sweep, governor pinned
#   sudo scripts/bench_linux.sh --cycles        # also print perf cycles/event
#   CORES=2-9 sudo scripts/bench_linux.sh        # choose the core set
#
# Notes:
#   * Needs root for cpupower (frequency governor). The actual benchmark is run
#     under your own user via `sudo -u` so output files aren't root-owned.
#   * For the steadiest numbers, also boot with `isolcpus=2-9 nohz_full=2-9`
#     so the kernel keeps other work off those cores, and disable turbo:
#       echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo   (Intel)
#   * Worker threads pin themselves to cores on Linux (see harness.hpp); taskset
#     here bounds the whole process (incl. the backend drain thread) to CORES.
#
set -euo pipefail

CORES="${CORES:-2-9}"
BUILD_DIR="${BUILD_DIR:-build}"
RUN_USER="${SUDO_USER:-$(id -un)}"
REPS="${REPS:-15}"
THREADS="${THREADS:-1,4,8}"
WORK="${WORK:-0,100,1000,10000}"
EVENTS="${EVENTS:-2000000}"
DO_CYCLES=0
[[ "${1:-}" == "--cycles" ]] && DO_CYCLES=1

echo "== pinning CPU governor to 'performance' =="
if command -v cpupower >/dev/null; then
  cpupower frequency-set -g performance >/dev/null || echo "  (cpupower failed; continuing)"
else
  echo "  cpupower not found; install linux-tools for frequency pinning"
fi

echo "== sweep on cores ${CORES} (reps=${REPS}, events=${EVENTS}) =="
taskset -c "${CORES}" sudo -u "${RUN_USER}" \
  python3 scripts/run_matrix.py --build-dir "${BUILD_DIR}" \
    --reps "${REPS}" --threads "${THREADS}" --work "${WORK}" \
    --events "${EVENTS}" --raw-csv results_linux.csv

if [[ "${DO_CYCLES}" == "1" ]] && command -v perf >/dev/null; then
  echo "== perf cycles/event (1 thread, work=0; frequency-independent) =="
  for b in none perfetto tracy; do
    [[ -x "${BUILD_DIR}/bench_${b}" ]] || continue
    printf '%-9s ' "${b}"
    taskset -c "${CORES%%,*}" sudo -u "${RUN_USER}" \
      perf stat -x, -e cycles "${BUILD_DIR}/bench_${b}" \
        --threads 1 --events "${EVENTS}" --work 0 2>&1 \
      | awk -F, '/cycles/{printf "%.2f cycles/event\n", $1/'"${EVENTS}"'}'
  done
fi
