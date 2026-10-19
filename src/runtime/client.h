#pragma once

#include "core/interface_contract.h"
#include "codecs/chat.h"
#include "codecs/messages_request.h"
#include "codecs/responses_request.h"
#include "codecs/gemini_request.h"
#include "codecs/interactions_request.h"
#include "codecs/images.h"
#include "codecs/decisions.h"
#include "codecs/embeddings.h"
#include "codecs/moderation.h"
#include "codecs/rerank.h"
#include "codecs/speech.h"
#include "codecs/transcription.h"
#include "codecs/videos.h"
#include "codecs/files.h"
#include "codecs/batches.h"
#include "descriptor/presets.h"
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
using sp::as_completion;
using sp::as_error;
using sp::as_failure;
using sp::is_completion;
using sp::is_failure;
using sp::joined_text;

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
  static Options with_api_key(
      std::string key,
      configuration::PolicySnapshot runtime_policy = configuration::builtin_runtime_policy()) {
    Options options(std::move(runtime_policy));
    options.api_key = std::move(key);
    return options;
  }
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
  // Ordering: a PartDelta is in the retained partial output before on_event receives it, so every delta a
  // callback was shown is in Failure::partial of a call that then fails, is cancelled or times out. A cancel,
  // stop request or deadline is acted on when the running callback returns (also a cancel made from inside
  // it), and no later event is delivered; a cancel made from another thread between two events lets at most
  // one more event through, and that event is retained too. Wire content received but not yet delivered when
  // the call is cut is not reported. A text PartDelta with empty text is applied but never delivered, so the
  // first text delta of a part carries text; the part's PartBegin and PartSeal are delivered either way.
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
  // The non-chat endpoint a prepared call reaches (`images`, D6), empty for a chat request.
  std::string_view endpoint() const noexcept;
 private:
  explicit PreparedRequest(std::shared_ptr<detail::OperationState>);
  void release() noexcept;
  std::shared_ptr<detail::OperationState> state_;
  friend class Client;
  friend class detail::ClientAccess;
};

// A prepared non-chat endpoint call (D6) whose outcome is typed. It holds the same admission slot and absolute
// deadline as a PreparedRequest, and a host journals it the same way, from `prepared()`.
template <class Outcome>
class PreparedEndpoint {
 public:
  PreparedEndpoint() = default;
  const PreparedRequest& prepared() const noexcept { return prepared_; }
  bool valid() const noexcept { return prepared_.valid(); }
  const Error* error() const noexcept { return prepared_.error(); }
 private:
  explicit PreparedEndpoint(PreparedRequest prepared) : prepared_(std::move(prepared)) {}
  PreparedRequest prepared_;
  friend class Client;
};

namespace detail {
inline images::Outcome typed_outcome(const sp::Outcome& outcome, const images::Outcome*) { return images::typed(outcome); }
inline decisions::Outcome typed_outcome(const sp::Outcome& outcome, const decisions::Outcome*) { return decisions::typed(outcome); }
inline embeddings::Outcome typed_outcome(const sp::Outcome& outcome, const embeddings::Outcome*) { return embeddings::typed(outcome); }
inline rerank::Outcome typed_outcome(const sp::Outcome& outcome, const rerank::Outcome*) { return rerank::typed(outcome); }
inline moderation::Outcome typed_outcome(const sp::Outcome& outcome, const moderation::Outcome*) { return moderation::typed(outcome); }
inline speech::Outcome typed_outcome(const sp::Outcome& outcome, const speech::Outcome*) { return speech::typed(outcome); }
inline transcription::Outcome typed_outcome(const sp::Outcome& outcome, const transcription::Outcome*) { return transcription::typed(outcome); }
inline jobs::Outcome typed_outcome(const sp::Outcome& outcome, const jobs::Outcome*) { return jobs::typed(outcome); }
inline jobs::DownloadOutcome typed_outcome(const sp::Outcome& outcome, const jobs::DownloadOutcome*) { return jobs::typed_content(outcome); }
inline files::Outcome typed_outcome(const sp::Outcome& outcome, const files::Outcome*) { return files::typed(outcome); }
inline files::PageOutcome typed_outcome(const sp::Outcome& outcome, const files::PageOutcome*) { return files::typed_page(outcome); }
inline files::DeleteOutcome typed_outcome(const sp::Outcome& outcome, const files::DeleteOutcome*) { return files::typed_deleted(outcome); }
inline files::ProgressOutcome typed_outcome(const sp::Outcome& outcome, const files::ProgressOutcome*) { return files::typed_progress(outcome); }
inline batches::Outcome typed_outcome(const sp::Outcome& outcome, const batches::Outcome*) { return batches::typed(outcome); }
inline batches::DeleteOutcome typed_outcome(const sp::Outcome& outcome, const batches::DeleteOutcome*) { return batches::typed_deleted(outcome); }
}  // namespace detail
// A started non-chat endpoint call. Like an Operation, destruction requests cancellation and detach relinquishes it;
// join waits for the typed outcome, the one the callback received.
template <class Outcome>
class EndpointCall {
 public:
  EndpointCall() = default;
  void cancel() noexcept { operation_.cancel(); }
  void detach() noexcept { operation_.detach(); }
  bool valid() const noexcept { return operation_.valid(); }
  Outcome join() const { return detail::typed_outcome(*operation_.join(), static_cast<const Outcome*>(nullptr)); }
 private:
  explicit EndpointCall(Operation operation) : operation_(std::move(operation)) {}
  Operation operation_;
  friend class Client;
};

class Client {
 public:
  // One immutable descriptor/credential-origin and one shared retry budget.
  // Invalid options throw ConfigError with fixed, non-sensitive diagnostics.
  explicit Client(descriptor::ValidatedDescriptor, Options = {});
  Client(descriptor::ValidatedDescriptor descriptor, std::string api_key)
      : Client(std::move(descriptor), Options::with_api_key(std::move(api_key))) {}
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
  // Non-chat endpoints (D6). A call uses this client's origin, credential and descriptor headers, and shares its
  // admission slots, retry budget, deadline handling and error classification. The policy's endpoint fact for the
  // origin admits it. A call the provider charges for once it accepts it is not retried once it may have reached
  // the provider. Calls are buffered.
  PreparedEndpoint<images::Outcome> prepare(images::Request, RunOptions = {});
  images::Outcome complete(images::Request, RunOptions = {});
  images::Outcome complete(PreparedEndpoint<images::Outcome>);
  EndpointCall<images::Outcome> start(PreparedEndpoint<images::Outcome>, std::function<void(images::Outcome)> on_outcome = {});
  PreparedEndpoint<decisions::Outcome> prepare(decisions::Request, RunOptions = {});
  decisions::Outcome complete(decisions::Request, RunOptions = {});
  decisions::Outcome complete(PreparedEndpoint<decisions::Outcome>);
  EndpointCall<decisions::Outcome> start(PreparedEndpoint<decisions::Outcome>, std::function<void(decisions::Outcome)> on_outcome = {});
  PreparedEndpoint<embeddings::Outcome> prepare(embeddings::Request, RunOptions = {});
  embeddings::Outcome complete(embeddings::Request, RunOptions = {});
  embeddings::Outcome complete(PreparedEndpoint<embeddings::Outcome>);
  EndpointCall<embeddings::Outcome> start(PreparedEndpoint<embeddings::Outcome>, std::function<void(embeddings::Outcome)> on_outcome = {});
  PreparedEndpoint<rerank::Outcome> prepare(rerank::Request, RunOptions = {});
  rerank::Outcome complete(rerank::Request, RunOptions = {});
  rerank::Outcome complete(PreparedEndpoint<rerank::Outcome>);
  EndpointCall<rerank::Outcome> start(PreparedEndpoint<rerank::Outcome>, std::function<void(rerank::Outcome)> on_outcome = {});
  PreparedEndpoint<moderation::Outcome> prepare(moderation::Request, RunOptions = {});
  moderation::Outcome complete(moderation::Request, RunOptions = {});
  moderation::Outcome complete(PreparedEndpoint<moderation::Outcome>);
  EndpointCall<moderation::Outcome> start(PreparedEndpoint<moderation::Outcome>, std::function<void(moderation::Outcome)> on_outcome = {});
  PreparedEndpoint<speech::Outcome> prepare(speech::Request, RunOptions = {});
  speech::Outcome complete(speech::Request, RunOptions = {});
  speech::Outcome complete(PreparedEndpoint<speech::Outcome>);
  EndpointCall<speech::Outcome> start(PreparedEndpoint<speech::Outcome>, std::function<void(speech::Outcome)> on_outcome = {});
  PreparedEndpoint<transcription::Outcome> prepare(transcription::Request, RunOptions = {});
  transcription::Outcome complete(transcription::Request, RunOptions = {});
  transcription::Outcome complete(PreparedEndpoint<transcription::Outcome>);
  EndpointCall<transcription::Outcome> start(PreparedEndpoint<transcription::Outcome>, std::function<void(transcription::Outcome)> on_outcome = {});
  // Provider jobs: a video submit answers with the job's state. Poll its handle until the job finishes, then
  // download each output. A poll and a download are never charged, so they retry as ordinary reads do.
  PreparedEndpoint<jobs::Outcome> prepare(videos::Request, RunOptions = {});
  PreparedEndpoint<jobs::Outcome> prepare(jobs::Poll, RunOptions = {});
  PreparedEndpoint<jobs::DownloadOutcome> prepare(jobs::Download, RunOptions = {});
  jobs::Outcome complete(videos::Request, RunOptions = {});
  jobs::Outcome complete(jobs::Poll, RunOptions = {});
  jobs::DownloadOutcome complete(jobs::Download, RunOptions = {});
  jobs::Outcome complete(PreparedEndpoint<jobs::Outcome>);
  jobs::DownloadOutcome complete(PreparedEndpoint<jobs::DownloadOutcome>);
  EndpointCall<jobs::Outcome> start(PreparedEndpoint<jobs::Outcome>, std::function<void(jobs::Outcome)> on_outcome = {});
  EndpointCall<jobs::DownloadOutcome> start(PreparedEndpoint<jobs::DownloadOutcome>, std::function<void(jobs::DownloadOutcome)> on_outcome = {});
  // Provider files (D7) at this client's origin: upload, get, list, delete, and download the content of a file the
  // provider made. None is charged. An upload is not retried once it may have reached the provider, unless the
  // retry policy accepts duplicate risk; reads are.
  PreparedEndpoint<files::Outcome> prepare(files::Upload, RunOptions = {});
  PreparedEndpoint<files::Outcome> prepare(files::Get, RunOptions = {});
  PreparedEndpoint<files::PageOutcome> prepare(files::List, RunOptions = {});
  PreparedEndpoint<files::DeleteOutcome> prepare(files::Delete, RunOptions = {});
  PreparedEndpoint<jobs::DownloadOutcome> prepare(files::Download, RunOptions = {});
  files::Outcome complete(files::Upload, RunOptions = {});
  files::Outcome complete(files::Get, RunOptions = {});
  files::PageOutcome complete(files::List, RunOptions = {});
  files::DeleteOutcome complete(files::Delete, RunOptions = {});
  jobs::DownloadOutcome complete(files::Download, RunOptions = {});
  files::Outcome complete(PreparedEndpoint<files::Outcome>);
  files::PageOutcome complete(PreparedEndpoint<files::PageOutcome>);
  files::DeleteOutcome complete(PreparedEndpoint<files::DeleteOutcome>);
  EndpointCall<files::Outcome> start(PreparedEndpoint<files::Outcome>, std::function<void(files::Outcome)> on_outcome = {});
  EndpointCall<files::PageOutcome> start(PreparedEndpoint<files::PageOutcome>, std::function<void(files::PageOutcome)> on_outcome = {});
  EndpointCall<files::DeleteOutcome> start(PreparedEndpoint<files::DeleteOutcome>, std::function<void(files::DeleteOutcome)> on_outcome = {});
  // A chunked upload, one step per call; each answers with the advanced session, and the file once it is whole.
  PreparedEndpoint<files::ProgressOutcome> prepare(files::Start, RunOptions = {});
  PreparedEndpoint<files::ProgressOutcome> prepare(files::Part, RunOptions = {});
  PreparedEndpoint<files::ProgressOutcome> prepare(files::Complete, RunOptions = {});
  PreparedEndpoint<files::ProgressOutcome> prepare(files::Query, RunOptions = {});
  PreparedEndpoint<files::ProgressOutcome> prepare(files::Cancel, RunOptions = {});
  files::ProgressOutcome complete(files::Start, RunOptions = {});
  files::ProgressOutcome complete(files::Part, RunOptions = {});
  files::ProgressOutcome complete(files::Complete, RunOptions = {});
  files::ProgressOutcome complete(files::Query, RunOptions = {});
  files::ProgressOutcome complete(files::Cancel, RunOptions = {});
  files::ProgressOutcome complete(PreparedEndpoint<files::ProgressOutcome>);
  EndpointCall<files::ProgressOutcome> start(PreparedEndpoint<files::ProgressOutcome>, std::function<void(files::ProgressOutcome)> on_outcome = {});
  // Batches (D7) at this client's origin. A submit is charged once the provider accepts it; a get, a cancel, a
  // deletion and a download of the results are not.
  PreparedEndpoint<batches::Outcome> prepare(batches::Submit, RunOptions = {});
  PreparedEndpoint<batches::Outcome> prepare(batches::Get, RunOptions = {});
  PreparedEndpoint<batches::Outcome> prepare(batches::Cancel, RunOptions = {});
  PreparedEndpoint<batches::DeleteOutcome> prepare(batches::Delete, RunOptions = {});
  PreparedEndpoint<jobs::DownloadOutcome> prepare(batches::Results, RunOptions = {});
  batches::Outcome complete(batches::Submit, RunOptions = {});
  batches::Outcome complete(batches::Get, RunOptions = {});
  batches::Outcome complete(batches::Cancel, RunOptions = {});
  batches::DeleteOutcome complete(batches::Delete, RunOptions = {});
  jobs::DownloadOutcome complete(batches::Results, RunOptions = {});
  batches::Outcome complete(PreparedEndpoint<batches::Outcome>);
  batches::DeleteOutcome complete(PreparedEndpoint<batches::DeleteOutcome>);
  EndpointCall<batches::Outcome> start(PreparedEndpoint<batches::Outcome>, std::function<void(batches::Outcome)> on_outcome = {});
  EndpointCall<batches::DeleteOutcome> start(PreparedEndpoint<batches::DeleteOutcome>, std::function<void(batches::DeleteOutcome)> on_outcome = {});
  Diagnostics diagnostics() const noexcept;
 private:
  explicit Client(std::shared_ptr<detail::ClientState>);
  // The admission and option checks every call shares; `encode` fills the operation's wire request and model.
  PreparedRequest prepare_call(RunOptions, const std::function<void(detail::OperationState&, std::optional<Error>&)>& encode);
  // An endpoint call's wire request from its encoder's result (D6).
  template <class Encoded> PreparedRequest prepare_endpoint(RunOptions, std::string_view endpoint, int reason_source,
                                                           const std::function<std::variant<Encoded, Error>()>& encode);
  // `complete` and `start` for a prepared endpoint call of any typed outcome.
  template <class Outcome> Outcome complete_endpoint(PreparedEndpoint<Outcome>);
  template <class Outcome> EndpointCall<Outcome> start_endpoint(PreparedEndpoint<Outcome>, std::function<void(Outcome)>);
  std::shared_ptr<detail::ClientState> state_;
  friend class detail::ClientAccess;
};

}  // namespace sp::runtime
