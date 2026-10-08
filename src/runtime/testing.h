#pragma once

// Private scheduling/transport seam. Not part of an installed interface.
#include "runtime/client.h"

namespace sp::runtime::detail {

class Executor {
 public:
  using Task = std::function<void()>;
  using Timer = std::uint64_t;
  virtual ~Executor() = default;
  // Never invoke inline. Throwing means the task was not enqueued.
  virtual void post(Task) = 0;
  virtual Timer schedule(SteadyTime, Task) = 0;
  virtual void cancel(Timer) noexcept = 0;
  virtual SteadyTime now() const noexcept = 0;
  virtual WallTime wall_now() const noexcept = 0;
  virtual bool in_thread() const noexcept = 0;
  // Quiescent shutdown; production implementation must handle its own worker.
  virtual void shutdown() noexcept = 0;
};

class Attempt {
 public:
  virtual ~Attempt() = default;
  virtual void cancel() noexcept = 0;
  virtual void resume() noexcept = 0;
};

class AttemptTransport {
 public:
  virtual ~AttemptTransport() = default;
  virtual std::unique_ptr<Attempt> start(transport::HttpRequest, transport::Callbacks) = 0;
  virtual void shutdown() noexcept = 0;
};

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
  // Private qualification seam: freeze a bounded body-only negative control
  // before spending admission. Changed native requests cannot mint replay seals.
  static PreparedRequest prepare_control(Client&, Request, RunOptions,
                                         std::function<bool(std::string&)>);
};

}  // namespace sp::runtime::detail
