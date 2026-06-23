#pragma once
//
// Neutral instrumentation layer for trace-bench.
//
// Exactly one backend is selected at compile time via -DBENCH_BACKEND=N:
//   0  none     -> every macro compiles to nothing (the baseline build)
//   1  perfetto -> maps to Perfetto TrackEvent macros
//   2  tracy    -> maps to Tracy zone/plot macros
//
// IMPORTANT: only string *literals* may be passed as event/counter names.
// That keeps the comparison apples-to-apples: Perfetto interns a static name
// once, and Tracy records a compile-time source-location struct. Passing a
// dynamic (runtime) string would push Perfetto onto its interning-hashmap
// path and unfairly inflate its measured overhead -- which is the single most
// common way published Tracy-vs-Perfetto benchmarks become misleading.
//
#define BENCH_BACKEND_NONE     0
#define BENCH_BACKEND_PERFETTO 1
#define BENCH_BACKEND_TRACY    2

#ifndef BENCH_BACKEND
#define BENCH_BACKEND BENCH_BACKEND_NONE
#endif

#if BENCH_BACKEND == BENCH_BACKEND_PERFETTO
  #include <perfetto.h>

  // Category declaration lives in this header so every TU that traces can see
  // it; the matching PERFETTO_TRACK_EVENT_STATIC_STORAGE() is in exactly one
  // TU (trace_backend.cpp). This is the documented multi-file pattern.
  PERFETTO_DEFINE_CATEGORIES(
      perfetto::Category("bench").SetDescription("trace-bench synthetic events"));

  #define BENCH_BACKEND_NAME            "perfetto"
  #define BENCH_TRACE_SCOPE(lit)        TRACE_EVENT("bench", lit)
  #define BENCH_TRACE_COUNTER(lit, val) TRACE_COUNTER("bench", lit, val)
  // OS-level thread naming (done separately in the worker) is what Perfetto
  // picks up, so the per-event macro is a no-op here.
  #define BENCH_SET_THREAD_NAME(name)   ((void)(name))

#elif BENCH_BACKEND == BENCH_BACKEND_TRACY
  #include <tracy/Tracy.hpp>

  #define BENCH_BACKEND_NAME            "tracy"
  #define BENCH_TRACE_SCOPE(lit)        ZoneScopedN(lit)
  #define BENCH_TRACE_COUNTER(lit, val) TracyPlot(lit, static_cast<double>(val))
  #define BENCH_SET_THREAD_NAME(name)   ::tracy::SetThreadName(name)

#else  // BENCH_BACKEND_NONE
  #define BENCH_BACKEND_NAME            "none"
  #define BENCH_TRACE_SCOPE(lit)        ((void)0)
  #define BENCH_TRACE_COUNTER(lit, val) ((void)0)
  #define BENCH_SET_THREAD_NAME(name)   ((void)(name))
#endif

// Wait (up to timeout_s) until tracing is actually active before the timed
// region, so the whole run is collected instead of racing the connection:
//   Tracy    -- until a capture client connects (on-demand DU build).
//   Perfetto -- until the "bench" category is enabled by a session (i.e. an
//               external consumer is recording, the producer-only/system case).
// Returns true once active, false on timeout. No-op (false) for none.
#include <chrono>
#include <thread>
inline bool bench_wait_for_client(double timeout_s) {
#if BENCH_BACKEND == BENCH_BACKEND_NONE
  (void)timeout_s;
  return false;
#else
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(timeout_s));
  for (;;) {
  #if BENCH_BACKEND == BENCH_BACKEND_TRACY
    if (TracyIsConnected) return true;
  #else  // BENCH_BACKEND_PERFETTO
    if (TRACE_EVENT_CATEGORY_ENABLED("bench")) return true;
  #endif
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
#endif
}
