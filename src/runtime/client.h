#pragma once

#include "core/interface_contract.h"
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

// Out-of-line capability gate checks the actually linked runtime and core.
InterfaceContract interface_contract() noexcept;
class InterfaceContractError final : public std::exception {
 public:
  const char* what() const noexcept override;
};
void require_interface_contract(std::uint32_t expected_revision, std::uint64_t required_capabilities);

using Request = std::variant<chat::Request, messages::Request, responses::Request, gemini::Request, interactions::Request>;
// Immutable outcome owner; messages/raw JSON/native carry can outlive Client.
using Result = std::shared_ptr<const Outcome>;

struct RunOptions {
  bool streaming = true;
  // Absolute monotonic deadline, fixed at preparation; includes encoding/waits.
  // If omitted, prepare computes now + Options::default_timeout once.
  std::optional<SteadyTime> deadline;
  std::stop_token stop_token;
  std::optional<RetryPolicy> retry;
  // Per-run overrides of the client-wide stall bounds in Options::transport (transport::
  // TransportOptions documents each). Unset = the client default; zero = disabled for this run;
  // otherwise a positive duration no longer than the admitted ceiling (the default_timeout_ms
  // admission ceiling), else the run starts as an InvalidRequest failure. The bounds apply to every
  // attempt and never extend `deadline`. Typical use: raise `deadline` for a long generation and set
  // idle_timeout so a stalled stream is still noticed in seconds.
  std::optional<std::chrono::milliseconds> connect_timeout;
  std::optional<std::chrono::milliseconds> first_byte_timeout;
  std::optional<std::chrono::milliseconds> idle_timeout;
};

struct Limits {
  std::size_t max_operations = config_defaults::defaults_max_operations;
  std::size_t queued_body_chunks = config_defaults::defaults_queued_body_chunks;
  std::size_t queued_body_bytes = config_defaults::defaults_queued_body_bytes;
  std::size_t max_response_bytes = config_defaults::defaults_max_response_bytes;
  std::size_t max_error_bytes = config_defaults::defaults_max_error_bytes;
  SemanticLimits semantic;
  transport::SseLimits sse;
};

struct Options {
  Options(configuration::PolicySnapshot = configuration::builtin_runtime_policy());
  configuration::PolicySnapshot policy;
  std::size_t workers = config_defaults::defaults_workers;
  transport::TransportOptions transport;
  transport::HttpVersion http_version = transport::HttpVersion::Auto;
  std::string ca_file;
  // Host-provided credential; family code selects the auth header. Never log it.
  std::string api_key;
  std::chrono::milliseconds default_timeout{config_defaults::defaults_default_timeout_ms};
  std::chrono::milliseconds slow_callback_threshold{config_defaults::defaults_slow_callback_threshold_ms};
  std::size_t retry_tokens = config_defaults::defaults_retry_tokens;
  double retry_tokens_per_second = config_defaults::defaults_retry_tokens_per_second;
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
  // Destruction requests nonblocking cancellation; detach relinquishes without it.
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

// Move-only pre-dispatch admission. Encoding/native validation happens once;
// no executor callback, timer, transport or provider request runs before start().
// The bounded preparation slot is released on abandonment. Views are borrowed
// from this handle; encoded_body may contain sensitive native provider state.
class PreparedRequest {
 public:
  PreparedRequest() = default;
  ~PreparedRequest();
  PreparedRequest(PreparedRequest&&) noexcept;
  PreparedRequest& operator=(PreparedRequest&&) noexcept;
  PreparedRequest(const PreparedRequest&) = delete;
  PreparedRequest& operator=(const PreparedRequest&) = delete;
  // valid() means owned state, not successful preflight; inspect error() as well.
  bool valid() const noexcept;
  const Error* error() const noexcept;
  std::string_view family() const noexcept;
  std::string_view model() const noexcept;
  std::string_view encoded_body() const noexcept;
  SteadyTime deadline() const noexcept;
  const descriptor::ValidatedDescriptor* descriptor() const noexcept;
  const RetryPolicy* retry_policy() const noexcept;
  std::optional<std::uint64_t> max_output_tokens() const noexcept;
  const Limits* limits() const noexcept;
  const NativeContext* native_context() const noexcept;
  std::optional<std::uint64_t> model_invocation_limit() const noexcept;
 private:
  explicit PreparedRequest(std::shared_ptr<detail::OperationState>);
  void release() noexcept;
  std::shared_ptr<detail::OperationState> state_;
  friend class Client;
  friend class detail::ClientAccess;
};

class Client {
 public:
  // One immutable descriptor/credential-origin and one shared retry budget.
  // Invalid options throw ConfigError with fixed, non-sensitive diagnostics.
  explicit Client(descriptor::ValidatedDescriptor, Options = {});
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) noexcept;
  Client& operator=(Client&&) noexcept;
  // Holds a bounded slot and the original absolute deadline. An initial error
  // is observable before a durable dispatch receipt or money reservation.
  PreparedRequest prepare(Request, RunOptions = {});
  // Consumes a preparation belonging to this Client; never re-encodes it.
  Operation start(PreparedRequest, Callbacks = {});
  // Accepted operations always deliver on the executor, including preflight failures.
  // Capacity/shutdown/moved-client rejection throws AdmissionError; no callback runs.
  Operation start(Request, RunOptions = {}, Callbacks = {});
  // AdmissionError is translated to the same owned Failure result.
  Result complete(Request, RunOptions = {});
  Result complete(PreparedRequest);
  Diagnostics diagnostics() const noexcept;
 private:
  explicit Client(std::shared_ptr<detail::ClientState>);
  std::shared_ptr<detail::ClientState> state_;
  friend class detail::ClientAccess;
};

}  // namespace sp::runtime
