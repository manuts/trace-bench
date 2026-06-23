//
// trace-bench: measure the per-event overhead of Perfetto vs Tracy
// instrumentation in a multi-threaded C++ workload.
//
// One binary per backend (bench_none / bench_perfetto / bench_tracy), built
// from these same sources with a different -DBENCH_BACKEND. Each run prints a
// single CSV row to stdout; scripts/run_matrix.py drives the sweep and does
// the baseline subtraction.
//
#include "bench_trace.hpp"
#include "harness.hpp"
#include "trace_backend.hpp"
#include "workload.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace bench;

namespace {

// Cache-line padded so the per-thread checksum sink never false-shares.
struct alignas(64) Sink {
  uint64_t v = 0;
  char pad[64 - sizeof(uint64_t)];
};

void worker(int tid, const Config& c, Sink* sinks,
            std::atomic<int>* ready, std::atomic<bool>* go) {
  char name[32];
  std::snprintf(name, sizeof(name), "worker-%d", tid);
  set_os_thread_name(name);
  BENCH_SET_THREAD_NAME(name);
  if (c.pin) pin_to_core(tid);

  // Barrier: announce readiness, then spin until the main thread releases all
  // workers at once. This keeps thread-creation cost out of the timed region.
  ready->fetch_add(1, std::memory_order_release);
  while (!go->load(std::memory_order_acquire)) { /* spin */ }

  uint64_t acc = 0x9e3779b97f4a7c15ull ^ static_cast<uint64_t>(tid + 1);
  for (uint64_t i = 0; i < c.events; ++i) {
    BENCH_TRACE_SCOPE("task");            // <-- the instrumentation under test
    acc = work(acc, c.work_iters);
    if (c.counters)
      BENCH_TRACE_COUNTER("acc", static_cast<int64_t>(acc & 0xffff));
  }
  sinks[tid].v = acc;
}

// Runs one full parallel region; returns a checksum (forces use of results)
// and writes the wall-clock duration of just the work into *out_wall_ns.
uint64_t run_once(const Config& c, std::vector<Sink>& sinks, uint64_t* out_wall_ns) {
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  threads.reserve(c.threads);
  for (int t = 0; t < c.threads; ++t)
    threads.emplace_back(worker, t, std::cref(c), sinks.data(), &ready, &go);

  while (ready.load(std::memory_order_acquire) < c.threads) { /* wait */ }
  uint64_t t0 = now_ns();
  go.store(true, std::memory_order_release);
  for (auto& th : threads) th.join();
  uint64_t t1 = now_ns();

  *out_wall_ns = t1 - t0;
  uint64_t checksum = 0;
  for (int t = 0; t < c.threads; ++t) checksum ^= sinks[t].v;
  return checksum;
}

void usage() {
  std::printf(
      "trace-bench (%s)\n"
      "  --threads N        worker threads            (default 4)\n"
      "  --events N         traced regions per thread (default 1000000)\n"
      "  --work N           xorshift rounds / region  (default 0)\n"
      "  --counters         also emit a counter per region\n"
      "  --no-warmup        skip the discarded warmup run\n"
      "  --no-pin           do not pin worker threads to cores\n"
      "  --buffer-kb N      Perfetto ring buffer size (default 262144)\n"
      "  --trace-file PATH  output trace path         (default trace_bench.pftrace)\n"
      "\nPrints one CSV row:\n"
      "  backend,threads,events_per_thread,work_iters,counters,total_events,"
      "wall_ns,cpu_user_ns,cpu_sys_ns,maxrss,trace_bytes,ns_per_event,checksum\n",
      BENCH_BACKEND_NAME);
}

}  // namespace

int main(int argc, char** argv) {
  Config c;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
    if      (a == "--threads")    c.threads    = std::stoi(next());
    else if (a == "--events")     c.events     = std::stoull(next());
    else if (a == "--work")       c.work_iters = std::stoi(next());
    else if (a == "--counters")   c.counters   = true;
    else if (a == "--no-warmup")  c.warmup     = false;
    else if (a == "--no-pin")     c.pin        = false;
    else if (a == "--buffer-kb")  c.buffer_kb  = std::stoi(next());
    else if (a == "--trace-file") c.trace_file = next();
    else if (a == "--help")       { usage(); return 0; }
    else { std::fprintf(stderr, "unknown arg: %s (try --help)\n", a.c_str()); return 2; }
  }

  std::vector<Sink> sinks(c.threads);

  TraceBackend backend;
  backend.start(c.trace_file, c.buffer_kb);

  // Warmup pays one-time process-level costs (session bring-up, icache, first
  // allocations) before the measured run. Per-thread TLS init is not warmed
  // here -- the measured run spawns fresh threads -- but with a large --events
  // it amortises to nothing over the loop.
  if (c.warmup) {
    Config w = c;
    w.events = 5000;
    uint64_t junk = 0;
    volatile uint64_t s = run_once(w, sinks, &junk);
    (void)s;
  }

  CpuTimes cpu0 = cpu_times();
  uint64_t wall_ns = 0;
  volatile uint64_t checksum = run_once(c, sinks, &wall_ns);
  CpuTimes cpu1 = cpu_times();

  backend.stop();

  uint64_t total_events = static_cast<uint64_t>(c.threads) * c.events;
  double ns_per_event = total_events ? static_cast<double>(wall_ns) /
                                           static_cast<double>(total_events)
                                     : 0.0;

  std::printf("%s,%d,%llu,%d,%d,%llu,%llu,%llu,%llu,%ld,%zu,%.4f,%llu\n",
              BENCH_BACKEND_NAME, c.threads,
              static_cast<unsigned long long>(c.events), c.work_iters,
              c.counters ? 1 : 0,
              static_cast<unsigned long long>(total_events),
              static_cast<unsigned long long>(wall_ns),
              static_cast<unsigned long long>(cpu1.user_ns - cpu0.user_ns),
              static_cast<unsigned long long>(cpu1.sys_ns - cpu0.sys_ns),
              cpu1.maxrss, backend.trace_bytes(), ns_per_event,
              static_cast<unsigned long long>(checksum & 0xffffffffull));
  return 0;
}
