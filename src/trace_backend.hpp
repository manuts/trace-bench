#pragma once
//
// Tracing session lifecycle, abstracted across backends.
//
//   start() -> begin recording (Perfetto: init + in-process session;
//              Tracy: no-op, it self-starts; none: no-op)
//   stop()  -> flush + (Perfetto) write the trace file and record its size
//
// Only the parallel work region between start() and stop() is timed by the
// caller, so session setup/teardown never pollutes the measurement.
//
#include <string>
#include <cstddef>

namespace bench {

class TraceBackend {
 public:
  void start(const std::string& file, int buffer_kb);
  void stop();
  std::size_t trace_bytes() const { return trace_bytes_; }

 private:
  std::size_t trace_bytes_ = 0;
  std::string file_;
};

}  // namespace bench
