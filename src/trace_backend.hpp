#pragma once
//
// Tracing session lifecycle, abstracted across backends.
//
//   start() -> begin recording (Perfetto: init + session on the chosen backend
//              -- system `traced` socket or in-process; Tracy: no-op, it
//              self-starts; none: no-op)
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
  // system_backend selects the Perfetto backend at run time: true = system
  // (`traced` socket), false = in-process. Ignored by the Tracy/none backends.
  void start(const std::string& file, int buffer_kb, bool system_backend);
  void stop();
  std::size_t trace_bytes() const { return trace_bytes_; }

 private:
  std::size_t trace_bytes_ = 0;
  std::string file_;
};

}  // namespace bench
