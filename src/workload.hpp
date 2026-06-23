#pragma once
//
// Workload: deterministic, non-elidable CPU work with a tunable cost.
//
#include <cstdint>

namespace bench {

// A dependency-chained xorshift. Each iteration depends on the previous, so
// the compiler cannot vectorise or reorder it away, and the caller stores the
// result into a sink so the whole thing cannot be dead-code-eliminated.
//
// Cost scales linearly with `iters`, which is the "region size" knob: it lets
// us measure instrumentation overhead as a fraction of real work per traced
// region (iters=0 isolates the pure per-event overhead).
inline uint64_t work(uint64_t x, int iters) {
  for (int i = 0; i < iters; ++i) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
  }
  return x ? x : 1;  // never let the state collapse to 0
}

}  // namespace bench
