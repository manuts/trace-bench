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
