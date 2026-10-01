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

struct OperationStats {
  std::size_t queued_bytes = 0;
  std::size_t peak_queued_bytes = 0;
  std::size_t queued_chunks = 0;
  std::uint64_t pauses = 0;
  std::uint32_t attempts = 0;
};

class ClientAccess {
 public:
  // Empty transport selects the real backend. Empty random01 uses production RNG.
  static Client make(descriptor::ValidatedDescriptor, Options,
                     std::shared_ptr<Executor>,
                     std::shared_ptr<AttemptTransport> = {},
                     std::function<double()> random01 = {});
  static OperationStats stats(const Operation&);
};

}  // namespace sp::runtime::detail
