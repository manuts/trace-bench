//
// Backend-specific implementation of TraceBackend. Which branch compiles is
// decided by BENCH_BACKEND (see bench_trace.hpp).
//
#include "trace_backend.hpp"
#include "bench_trace.hpp"

#if BENCH_BACKEND == BENCH_BACKEND_PERFETTO
// ----------------------------------------------------------------------------
// Perfetto. Two topologies, chosen at run time (--perfetto-backend):
//
//   system (default) -- PRODUCER-ONLY, the Pharos/L1 model. We connect to the
//     external `traced` daemon and only emit events; the tracing session is
//     owned by a separate `perfetto` consumer process (which writes the file).
//     We create no session and read nothing back, exactly like an app built
//     with perfettoSystemBackend=true. This is what makes the measured CPU
//     reflect ONLY the application's producer cost -- the consumer's
//     serialize/drain work lives in `traced`/`perfetto`, not in this process.
//
//   inprocess        -- producer AND consumer in one process. We create the
//     session, start it, and read the trace back at stop(). Self-contained
//     (no daemon), but the consumer cost is then paid in-process -- a local
//     upper bound, NOT representative of the app's real overhead.
// ----------------------------------------------------------------------------
#include <fstream>
#include <memory>
#include <vector>

// Storage for the track-event data source. Exactly one TU must do this.
PERFETTO_TRACK_EVENT_STATIC_STORAGE();

namespace {
std::unique_ptr<perfetto::TracingSession> g_session;  // in-process only
}

namespace bench {

void TraceBackend::start(const std::string& file, int buffer_kb, bool system_backend, bool fast) {
  file_ = file;

  perfetto::TracingInitArgs args;
  args.backends =
      system_backend ? perfetto::kSystemBackend : perfetto::kInProcessBackend;

  if (fast) {
    // Cut the producer->service cost. The SMB sits between the producer and the
    // service in BOTH backends (in-process service thread, or the `traced`
    // daemon over a socket), so these apply either way -- though the IPC saving
    // is largest for the system backend.
    args.shmem_size_hint_kb = 4096;            // larger SMB: absorb write bursts, less backpressure
    args.shmem_page_size_hint_kb = 32;         // bigger chunks: fewer page handoffs
    args.shmem_batch_commits_duration_ms = 2;  // batch commit notifications: fewer IPCs to traced
    args.shmem_direct_patching_enabled = true; // patch chunk sizes in place (if service supports)
  }

  perfetto::Tracing::Initialize(args);
  perfetto::TrackEvent::Register();

  if (system_backend) {
    // Producer-only: an external `perfetto` consumer (talking to `traced`)
    // owns the session and writes the file. Nothing else to set up here. For
    // events to be recorded -- and thus for the producer to do real work --
    // that external session must be actively tracing "track_event" while we
    // run; otherwise the category is disabled and the macros short-circuit.
    return;
  }

  // In-process: we are also the consumer.
  perfetto::TraceConfig cfg;
  cfg.add_buffers()->set_size_kb(buffer_kb);
  auto* ds = cfg.add_data_sources()->mutable_config();
  ds->set_name("track_event");

  g_session = perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
  g_session->Setup(cfg);
  g_session->StartBlocking();
}

void TraceBackend::stop() {
  // Flush this producer's buffered events to the service. In the producer-only
  // case this is how the external consumer sees our data; it runs after the
  // timed region, so it never pollutes the measurement.
  perfetto::TrackEvent::Flush();
  if (!g_session) return;  // producer-only: the external consumer owns the file

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
// Tracy: the client self-starts on the first zone. By default the binary is
// built with the aqua DU production profile (on-demand), so with no client
// connected the zone macros short-circuit; build with TRACY_LIVE=ON for the
// full producer path. A .tracy file is produced only if a capture client is
// connected over loopback while we run -- see README "Capturing a Tracy trace".
// ----------------------------------------------------------------------------
namespace bench {
void TraceBackend::start(const std::string&, int, bool, bool) {}
void TraceBackend::stop() {}
}  // namespace bench

#else
// ----------------------------------------------------------------------------
// none: baseline, nothing to do.
// ----------------------------------------------------------------------------
namespace bench {
void TraceBackend::start(const std::string&, int, bool, bool) {}
void TraceBackend::stop() {}
}  // namespace bench

#endif
