#pragma once

#include "codecs/chat.h"
#include "codecs/messages_request.h"
#include "codecs/responses_request.h"
#include "codecs/gemini_request.h"
#include "codecs/interactions_request.h"
#include "runtime/policy.h"
#include "transport/sse_framer.h"

#include <exception>
#include <functional>
#include <memory>
#include <stop_token>
#include <variant>

namespace sp::runtime {

using Request = std::variant<chat::Request, messages::Request, responses::Request, gemini::Request, interactions::Request>;
using Result = std::shared_ptr<const Outcome>;

struct RunOptions {
  bool streaming = true;
  std::optional<SteadyTime> deadline;
  std::stop_token stop_token;
  RetryPolicy retry;
};

struct Limits {
  std::size_t max_operations = 256;
  std::size_t queued_body_chunks = 16;
  std::size_t queued_body_bytes = 256 << 10;
  std::size_t max_response_bytes = 16 << 20;
  std::size_t max_error_bytes = 64 << 10;
  SemanticLimits semantic;
  transport::SseLimits sse;
};

struct Options {
  std::size_t workers = 2;
  transport::TransportOptions transport;
  transport::HttpVersion http_version = transport::HttpVersion::Auto;
  std::string ca_file;
  std::string api_key;
  std::chrono::milliseconds default_timeout{30000};
  std::chrono::milliseconds slow_callback_threshold{50};
  std::size_t retry_tokens = 16;
  double retry_tokens_per_second = 1;
  Limits limits;
};

struct Callbacks {
  // Nonterminal semantic events only. Views are borrowed; callbacks must not block.
  std::function<void(const Event&)> on_event;
  // Exactly once, after all events. Result may be retained beyond callback/join.
  std::function<void(Result)> on_outcome;
};

struct Diagnostics {
  std::uint64_t slow_callbacks = 0;
  std::uint64_t callback_exceptions = 0;
};

namespace detail {
struct ClientState;
struct OperationState;
class ClientAccess;
}

// No operation/callback is accepted when the runtime cannot reserve a slot.
class AdmissionError final : public std::exception {
 public:
  const char* what() const noexcept override;
  Result outcome() const noexcept { return outcome_; }
 private:
  explicit AdmissionError(ErrorKind);
  Result outcome_;
  friend class Client;
};

class Operation {
 public:
  Operation() = default;
  ~Operation();
  Operation(Operation&&) noexcept;
  Operation& operator=(Operation&&) noexcept;
  Operation(const Operation&) = delete;
  Operation& operator=(const Operation&) = delete;
  void cancel() noexcept;
  void detach() noexcept;
  bool valid() const noexcept;
  // Fences callback return and admission-slot release. Library I/O workers get Misuse.
  Result join() const;
 private:
  explicit Operation(std::shared_ptr<detail::OperationState>);
  std::shared_ptr<detail::OperationState> state_;
  friend class Client;
  friend class detail::ClientAccess;
};

class Client {
 public:
  // One immutable descriptor/credential-origin and one shared retry budget.
  // Invalid options throw a fixed, non-sensitive invalid_argument message.
  explicit Client(descriptor::ValidatedDescriptor, Options = {});
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) noexcept;
  Client& operator=(Client&&) noexcept;
  // Accepted operations always deliver on the executor, including preflight failures.
  // Capacity/shutdown/moved-client rejection throws AdmissionError; no callback runs.
  Operation start(Request, RunOptions = {}, Callbacks = {});
  // AdmissionError is translated to the same owned Failure result.
  Result complete(Request, RunOptions = {});
  Diagnostics diagnostics() const noexcept;
 private:
  explicit Client(std::shared_ptr<detail::ClientState>);
  std::shared_ptr<detail::ClientState> state_;
  friend class detail::ClientAccess;
};

}  // namespace sp::runtime
