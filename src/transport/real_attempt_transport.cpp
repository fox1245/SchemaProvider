// The libcurl backend of the runtime client. Everything here is the only place the runtime meets
// libcurl, so sp_runtime itself stays free of it: a program that only uses an existing Client (the
// graph engine, for one) never loads libcurl or the libraries it pulls in. Constructing a Client
// through the public constructor requires linking this library.
#include "runtime/testing.h"
#include "transport/http_transport.h"

namespace sp::runtime::detail {
namespace {
class RealAttempt final : public Attempt {
  transport::Operation operation_;
 public:
  explicit RealAttempt(transport::Operation operation) : operation_(std::move(operation)) {}
  void cancel() noexcept override { operation_.cancel(); }
  void resume() noexcept override { operation_.resume(); }
};
class RealTransport final : public AttemptTransport {
  std::unique_ptr<transport::Transport> transport_;
 public:
  explicit RealTransport(transport::TransportOptions options)
      : transport_(std::make_unique<transport::Transport>(std::move(options))) {}
  std::unique_ptr<Attempt> start(transport::HttpRequest request, transport::Callbacks callbacks) override {
    return std::make_unique<RealAttempt>(transport_->start(std::move(request), std::move(callbacks)));
  }
  void shutdown() noexcept override { transport_.reset(); }
};
}  // namespace

std::shared_ptr<AttemptTransport> make_real_attempt_transport(transport::TransportOptions options) {
  return std::make_shared<RealTransport>(std::move(options));
}
}  // namespace sp::runtime::detail

namespace sp::runtime {
Client::Client(descriptor::ValidatedDescriptor descriptor, Options options)
    : Client((require_interface_contract(EXPECTED_INTERFACE_REVISION, capability::RequiredProvider),
              detail::ClientAccess::make(std::move(descriptor), std::move(options), {}, {}, {},
                                         &detail::make_real_attempt_transport))) {}
}  // namespace sp::runtime
