#pragma once

// Private test hooks around the host seam. Not part of an installed interface.
//
// The executor and the HTTP port are the public host seam (runtime/host.h); the names below are the ones this
// header had before the seam was published, and every test that implements them still compiles. What stays private
// is ClientAccess: the injectable RNG, the real-transport factory and the stat and control hooks of the tests.
#include "runtime/client.h"
#include "runtime/host.h"

namespace sp::runtime::detail {

using Executor = runtime::Executor;
using Attempt = runtime::HostAttempt;
using AttemptTransport = runtime::HostTransport;

// Production pool, exposed only to private behavioral scheduling tests. A build without threads
// (SP_THREADS=OFF) has no pool: this throws descriptor::ConfigError at "/executor", as does a Client made
// without an executor, so the host must supply one.
std::shared_ptr<Executor> make_pool_executor(std::size_t workers);

using RealTransportFactory = std::shared_ptr<AttemptTransport> (*)(transport::TransportOptions);
// The libcurl backend. Defined by sp_transport, which sp_runtime does not link, so a program that
// only uses an existing Client never loads libcurl.
std::shared_ptr<AttemptTransport> make_real_attempt_transport(transport::TransportOptions);
struct OperationStats {
  std::size_t queued_bytes = 0;
  std::size_t peak_queued_bytes = 0;
  std::size_t queued_chunks = 0;
  std::uint64_t pauses = 0;
  std::uint32_t attempts = 0;
  bool backing_off = false;
};

class ClientAccess {
 public:
  // An empty transport is built by `real_transport`; with neither, the call is rejected. Empty
  // random01 uses production RNG.
  static Client make(descriptor::ValidatedDescriptor, Options,
                     std::shared_ptr<Executor>,
                     std::shared_ptr<AttemptTransport> = {},
                     std::function<double()> random01 = {},
                     RealTransportFactory real_transport = nullptr);
  static OperationStats stats(const Operation&);
  // Test-only state fence; a completed peer response does not prove retry backoff has begun.
  static bool wait_for_backoff(const Operation&, std::chrono::milliseconds timeout);
  // Private qualification seam: freeze a bounded body-only negative control
  // before spending admission. Changed native requests cannot mint replay seals.
  static PreparedRequest prepare_control(Client&, Request, RunOptions,
                                         std::function<bool(std::string&)>);
};

}  // namespace sp::runtime::detail
