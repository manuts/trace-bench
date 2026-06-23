#!/usr/bin/env bash
#
# trace-bench: five-way instrumentation-overhead comparison.
#
#   1. no profiling                          (bench_none)
#   2. perfetto enabled, no consumer running (system backend, producer short-circuits)
#   3. perfetto enabled, with consumer       (traced + perfetto CLI capturing)
#   4. tracy enabled, no capture running      (DU on-demand build, idle)
#   5. tracy enabled, with capture running    (tracy-capture attached per run)
#
# RUN THIS IN A NORMAL (UNSANDBOXED) SHELL. Cases 3 and 5 start the `traced`
# daemon and a Tracy capture, which bind unix/TCP sockets -- a sandbox blocks
# that with EPERM.
#
# Overrides via env: TB, PD, CAPTURE, REPS, EVENTS, THREADS, WORK.
set -u

# Repo root: parent of this script's dir (portable). Override with TB=...
TB="${TB:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
# Perfetto daemon + consumer (only case 3 needs them). Two ways to point at them:
#   TRACEBOX=/path/to/tracebox   -- single prebuilt multitool (easiest; no build):
#                                     curl -LO https://get.perfetto.dev/tracebox
#   PD=/path/to/out/dir          -- a source build's dir with separate `traced`
#                                     and `perfetto` binaries (e.g. out/linux)
TRACEBOX="${TRACEBOX:-}"
PD="${PD:-/Users/manu/workspaces/perfetto/out/mac_release}"
if [ -n "$TRACEBOX" ]; then
  TRACED=("$TRACEBOX" traced);  PERFETTO=("$TRACEBOX" perfetto)
else
  TRACED=("$PD/traced");        PERFETTO=("$PD/perfetto")
fi
CAPTURE="${CAPTURE:-tracy-capture}"          # headless Tracy capture (brew: tracy-capture)
REPS="${REPS:-11}"
EVENTS="${EVENTS:-200000}"
THREADS="${THREADS:-1}"
WORK="${WORK:-7500}"          # xorshift rounds/region; ~7500 ~= 10 us (~1.33 ns/iter)
ARGS=(--threads "$THREADS" --events "$EVENTS" --work "$WORK" --no-pin)
M="python3 $TB/scripts/measure.py"

export PERFETTO_PRODUCER_SOCK_NAME=/tmp/pf-prod.sock
export PERFETTO_CONSUMER_SOCK_NAME=/tmp/pf-cons.sock

PERF_PID=""; TRACED_PID=""
cleanup() {
  [ -n "$PERF_PID" ]   && kill "$PERF_PID"   2>/dev/null
  [ -n "$TRACED_PID" ] && kill "$TRACED_PID" 2>/dev/null
}
trap cleanup EXIT

echo "trace-bench five-way comparison  (reps=$REPS, ${ARGS[*]})"
echo "================================================================================"

# ---- 1. no profiling -------------------------------------------------------
$M "$REPS" "1. none (no profiling)    " -- "$TB/build-du/bench_none" "${ARGS[@]}"

# ---- 2. perfetto enabled, NO consumer --------------------------------------
# No active session -> the "bench" category is disabled, macros short-circuit.
$M "$REPS" "2. perfetto, no consumer  " -- "$TB/build-du/bench_perfetto" "${ARGS[@]}" --perfetto-backend system

# ---- 3. perfetto enabled, WITH consumer ------------------------------------
# traced + perfetto CLI hold one session active across all reps.
rm -f "$PERFETTO_PRODUCER_SOCK_NAME" "$PERFETTO_CONSUMER_SOCK_NAME"
"${TRACED[@]}" >/tmp/traced.log 2>&1 & TRACED_PID=$!
sleep 1.5
if ! kill -0 "$TRACED_PID" 2>/dev/null; then
  echo "  ERROR: traced failed to start:"; sed 's/^/    /' /tmp/traced.log; exit 1
fi
"${PERFETTO[@]}" -c "$TB/scripts/perfetto_track_event.cfg" --txt -o /tmp/trace_out >/tmp/perfetto.log 2>&1 & PERF_PID=$!
sleep 1.5
if ! kill -0 "$PERF_PID" 2>/dev/null; then
  echo "  ERROR: perfetto consumer failed to start:"; sed 's/^/    /' /tmp/perfetto.log; exit 1
fi
$M "$REPS" "3. perfetto, with consumer" -- "$TB/build-du/bench_perfetto" "${ARGS[@]}" --perfetto-backend system --wait-client 15
kill "$PERF_PID"   2>/dev/null; wait "$PERF_PID"   2>/dev/null; PERF_PID=""
kill "$TRACED_PID" 2>/dev/null; wait "$TRACED_PID" 2>/dev/null; TRACED_PID=""

# ---- 4. tracy enabled, NO capture ------------------------------------------
# DU build is on-demand: with no client the zone macros short-circuit.
$M "$REPS" "4. tracy, no capture      " -- "$TB/build-du/bench_tracy" "${ARGS[@]}"

# ---- 5. tracy enabled, WITH capture ----------------------------------------
# tracy-capture grabs ONE app session then exits, so we start a fresh capture
# per rep; --wait-client makes the bench block until that capture connects, so
# the whole timed region is collected (otherwise a short run races the connect).
if ! command -v "$CAPTURE" >/dev/null 2>&1; then
  echo "5. tracy, with capture     SKIP ('$CAPTURE' not on PATH; set CAPTURE=/path/to/tracy-capture)"
else
  for _ in $(seq 1 "$REPS"); do
    "$CAPTURE" -f -a 127.0.0.1 -o /tmp/cap.tracy >/tmp/capture.log 2>&1 &
    CAP=$!
    "$TB/build-du/bench_tracy" "${ARGS[@]}" --wait-client 15   # CSV->stdout, warnings->stderr
    wait "$CAP" 2>/dev/null
  done | $M --agg "5. tracy, with capture    "
fi

echo "================================================================================"
echo "min = robust estimator (jitter is one-sided).  overhead = <row> - <case 1 none>."
echo "If case 3 reads ~1-2 ns/ev, the perfetto session wasn't active."
echo "If case 5 reads ~ case 4, the capture didn't connect (check protocol version /"
echo "  '$CAPTURE' vs the Tracy submodule, and /tmp/capture.log)."
