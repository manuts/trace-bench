#pragma once
//
// Harness helpers: config, timing, CPU/memory accounting, thread utilities.
// Header-only so it adds no extra translation unit.
//
#include <cstdint>
#include <string>
#include <ctime>
#include <sys/resource.h>
#include <pthread.h>

namespace bench {

struct Config {
  int      threads    = 4;           // worker threads
  uint64_t events     = 1'000'000;   // traced regions PER thread
  int      work_iters = 0;           // xorshift rounds inside each region
  bool     counters   = false;       // also emit a TRACE_COUNTER per region
  bool     warmup     = true;        // discarded warmup run (process warm)
  int      buffer_kb  = 256 * 1024;  // Perfetto ring buffer (other backends ignore)
  bool     pin        = true;        // pin workers to cores (Linux only)
  bool     perfetto_system = true;   // Perfetto backend: true=system (traced), false=in-process
  bool     perfetto_fast   = false;  // Perfetto: apply SMB/commit-batching tuning (both backends)
  double   wait_client_sec = 0.0;    // >0: (Tracy) wait up to N s for a capture client before timing
  std::string trace_file = "trace_bench.pftrace";
};

// --- thread naming (best effort; makes traces readable) ---------------------
inline void set_os_thread_name(const char* name) {
#if defined(__APPLE__)
  pthread_setname_np(name);
#elif defined(__linux__)
  pthread_setname_np(pthread_self(), name);
#else
  (void)name;
#endif
}

// --- core pinning -----------------------------------------------------------
// Hard affinity is Linux-only. macOS has no portable equivalent (its affinity
// API is only a hint), so there we rely on many reps + median to fight jitter.
inline void pin_to_core(int core) {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
  (void)core;
#endif
}

// --- timing -----------------------------------------------------------------
inline uint64_t now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull +
         static_cast<uint64_t>(ts.tv_nsec);
}

// --- CPU + memory accounting ------------------------------------------------
// user_ns/sys_ns are summed across ALL threads in the process, so they capture
// the cost of a backend's background drain/service thread too -- not just the
// producer hot path that wall-clock sees.
struct CpuTimes {
  uint64_t user_ns;
  uint64_t sys_ns;
  long     maxrss;  // bytes on macOS, kibibytes on Linux (see README)
};

inline CpuTimes cpu_times() {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  auto to_ns = [](struct timeval t) {
    return static_cast<uint64_t>(t.tv_sec) * 1'000'000'000ull +
           static_cast<uint64_t>(t.tv_usec) * 1000ull;
  };
  return {to_ns(ru.ru_utime), to_ns(ru.ru_stime), ru.ru_maxrss};
}

}  // namespace bench
