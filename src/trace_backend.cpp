//
// Backend-specific implementation of TraceBackend. Which branch compiles is
// decided by BENCH_BACKEND (see bench_trace.hpp).
//
#include "trace_backend.hpp"
#include "bench_trace.hpp"

#if BENCH_BACKEND == BENCH_BACKEND_PERFETTO
// ----------------------------------------------------------------------------
// Perfetto: in-process backend, single track_event data source, ring buffer.
// We deliberately use the IN-PROCESS backend (not the `traced` system socket)
// so we measure the lighter, profiler-style path -- the fair comparison point
// against Tracy. The socket/IPC backend is a heavier regime studied separately.
// ----------------------------------------------------------------------------
#include <fstream>
#include <memory>
#include <vector>

// Storage for the track-event data source. Exactly one TU must do this.
PERFETTO_TRACK_EVENT_STATIC_STORAGE();

namespace {
std::unique_ptr<perfetto::TracingSession> g_session;
}

namespace bench {

void TraceBackend::start(const std::string& file, int buffer_kb) {
  file_ = file;

  perfetto::TracingInitArgs args;
  args.backends = perfetto::kInProcessBackend;
  perfetto::Tracing::Initialize(args);
  perfetto::TrackEvent::Register();

  perfetto::TraceConfig cfg;
  cfg.add_buffers()->set_size_kb(buffer_kb);
  auto* ds = cfg.add_data_sources()->mutable_config();
  ds->set_name("track_event");

  g_session = perfetto::Tracing::NewTrace();
  g_session->Setup(cfg);
  g_session->StartBlocking();
}

void TraceBackend::stop() {
  if (!g_session) return;
  perfetto::TrackEvent::Flush();
  g_session->StopBlocking();
  std::vector<char> data = g_session->ReadTraceBlocking();
  std::ofstream out(file_, std::ios::binary);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  trace_bytes_ = data.size();
  g_session.reset();
}

}  // namespace bench

#elif BENCH_BACKEND == BENCH_BACKEND_TRACY
// ----------------------------------------------------------------------------
// Tracy: the client self-starts collecting on the first zone (unless built
// with TRACY_ON_DEMAND, which we do NOT, so the producer hot path is always
// exercised). A .tracy file is produced only if a capture client is connected
// over loopback while we run -- see README "Capturing a Tracy trace".
// ----------------------------------------------------------------------------
namespace bench {
void TraceBackend::start(const std::string&, int) {}
void TraceBackend::stop() {}
}  // namespace bench

#else
// ----------------------------------------------------------------------------
// none: baseline, nothing to do.
// ----------------------------------------------------------------------------
namespace bench {
void TraceBackend::start(const std::string&, int) {}
void TraceBackend::stop() {}
}  // namespace bench

#endif
