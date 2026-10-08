// Family x lifecycle matrix over the real transport.
// Usage: sp_matrix_tests <node> <lifecycle_server.mjs> <openssl> <family> [--list | --only <name-substring>]
//   family: chat | responses | messages | gemini | interactions
//
// Every cell drives one public sp::runtime::Client through a real libcurl/Asio attempt against the
// local peer in tests/support/lifecycle_server.mjs and asserts: exactly one terminal outcome, the
// callback ran once and nothing ran after it, the typed ErrorKind/RetryClass/RetrySafety/http_status/
// retry_after, the number of requests the peer saw, and that nothing hung. The same cell table runs
// for all five families; where a family legitimately differs the difference is encoded in
// `shape()` and cites config/error-policy.json.
#include "runtime/client.h"
#include "runtime/testing.h"
#include "codecs/chat.h"
#include "codecs/gemini_request.h"
#include "codecs/interactions_request.h"
#include "codecs/messages_request.h"
#include "codecs/responses_request.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include "support/runtime_peer.h"

#include "support/portable.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;
using sp::ErrorKind;
using sp::RetryClass;
using sp::RetrySafety;
using sp::runtime::Result;
using runtime_test::require;

constexpr std::string_view marker = "MATRIX_SECRET_MARKER_5d1e90";
// Upper bounds are generous (CI under load), lower bounds strict.
constexpr Ms deadline_cell = 600ms;     // deadline for every cell that expects DeadlineExceeded
constexpr Ms no_hang = 20s;             // any outcome later than this is a hang
constexpr Ms late_bound = 5s;           // deadline/cancel outcome may trail its trigger by at most this

// ---------------------------------------------------------------------------------------------
// Families
enum class Fam { Chat, Responses, Messages, Gemini, Interactions };
const char* label(Fam f) {
  switch (f) {
    case Fam::Chat: return "chat"; case Fam::Responses: return "responses"; case Fam::Messages: return "messages";
    case Fam::Gemini: return "gemini"; case Fam::Interactions: return "interactions";
  }
  return "?";
}
const char* peer_family(Fam f) { return label(f); }
const char* family_id(Fam f) {
  switch (f) {
    case Fam::Chat: return "openai.chat"; case Fam::Responses: return "openai.responses";
    case Fam::Messages: return "anthropic.messages"; case Fam::Gemini: return "google.generate";
    case Fam::Interactions: return "google.interactions";
  }
  return "?";
}

std::string descriptor_source(Fam f, const std::string& base, const std::string& model) {
  const auto head = [&](std::string_view paths, std::string_view bindings, std::string_view extra_connection,
                        std::string_view tail) {
    return "{\"descriptor_version\":1,\"revision\":1,\"id\":\"matrix-loopback\",\"family\":" + sp::json::quote(family_id(f))
      + ",\"evidence\":{\"urls\":[\"https://example.com/spec\"],\"verified_at\":\"2026-10-01\"},\"connection\":{\"base_url\":"
      + sp::json::quote(base) + ",\"paths\":" + std::string(paths) + std::string(extra_connection) + "},\"bindings\":"
      + std::string(bindings) + std::string(tail) + "}";
  };
  const auto one = [](std::string_view path) {
    return "{\"buffered\":" + sp::json::quote(path) + ",\"streaming\":" + sp::json::quote(path) + "}";
  };
  switch (f) {
    case Fam::Chat:
      return head(one("/v1/chat/completions"), R"({"model":"model","messages":"messages","stream":"stream","max_output_tokens":"max_tokens","usage":["usage"]})",
                  "", R"(,"stop_reasons":{"stop":"EndTurn","end_turn":"EndTurn","tool_use":"ToolUse"})");
    case Fam::Messages:
      return head(one("/v1/messages"), R"({"model":"model","messages":"messages","stream":"stream","max_output_tokens":"max_tokens","usage":["usage"]})",
                  R"(,"headers":{"anthropic-version":"2023-06-01"})", R"(,"stop_reasons":{"stop":"EndTurn","end_turn":"EndTurn","tool_use":"ToolUse"})");
    case Fam::Responses:
      return head(one("/v1/responses"), R"({"model":"model","messages":"input","stream":"stream","max_output_tokens":"max_output_tokens","usage":["usage"]})", "", "");
    case Fam::Interactions:
      return head(one("/v1beta/interactions"), R"({"model":"model","messages":"input","stream":"stream","max_output_tokens":"max_output_tokens","usage":["usage"]})", "", "");
    case Fam::Gemini: {
      const auto path = "/v1beta/models/" + model;
      return head("{\"buffered\":" + sp::json::quote(path + ":generateContent") + ",\"streaming\":"
                  + sp::json::quote(path + ":streamGenerateContent?alt=sse") + "}",
                  R"({"model":"model","messages":"contents","stream":"stream","max_output_tokens":"maxOutputTokens","usage":["usageMetadata"]})", "", "");
    }
  }
  throw std::logic_error("family");
}
sp::descriptor::ValidatedDescriptor make_descriptor(Fam f, const std::string& base, const std::string& model) {
  auto loaded = sp::descriptor::load(descriptor_source(f, base, model));
  require(std::holds_alternative<sp::descriptor::ValidatedDescriptor>(loaded), "matrix descriptor rejected");
  return std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded));
}
sp::Message user_message() { sp::Message m; m.role = sp::Role::User; m.parts.emplace_back(sp::Text{"synthetic"}); return m; }
sp::runtime::Request make_request(Fam f, const std::string& model) {
  switch (f) {
    case Fam::Chat: { sp::chat::Request r; r.model = model; r.messages.push_back({sp::Role::User, "synthetic"}); return r; }
    case Fam::Messages: { sp::messages::Request r; r.model = model; r.account_scope = "synthetic-account"; r.messages.push_back(user_message()); return r; }
    case Fam::Responses: { sp::responses::Request r; r.model = model; r.account_scope = "synthetic-account"; r.messages.push_back(user_message()); return r; }
    case Fam::Gemini: { sp::gemini::Request r; r.model = model; r.account_scope = "synthetic-account"; r.messages.push_back(user_message()); return r; }
    case Fam::Interactions: { sp::interactions::Request r; r.model = model; r.account_scope = "synthetic-account"; r.messages.push_back(user_message()); return r; }
  }
  throw std::logic_error("family");
}

// ---------------------------------------------------------------------------------------------
// Wires. `h2` / `tls` are what the peer must have observed, not what the client asked for.
struct Wire {
  const char* name;
  bool tls, h2;
  sp::transport::HttpVersion version;
};
constexpr Wire h1{"h1", false, false, sp::transport::HttpVersion::Auto};
constexpr Wire tls1{"tls-h1", true, false, sp::transport::HttpVersion::Http1_1};
constexpr Wire h2c{"h2c", false, true, sp::transport::HttpVersion::Http2PriorKnowledge};
constexpr Wire tls2{"tls-h2", true, true, sp::transport::HttpVersion::Auto};

// ---------------------------------------------------------------------------------------------
// Peer access
struct Lab {
  runtime_test::Peer peer;
  std::uint64_t plain = 0, h2c_port = 0, tls = 0, black = 0;
  std::string ca;
  std::size_t sequence = 0;
  Lab(const char* node, const char* script) : peer(node, script) {
    auto info = peer.command("{\"info\":true}");
    auto root = info.root();
    plain = root.get("plain").as_uint(); h2c_port = root.get("h2c").as_uint(); tls = root.get("tls").as_uint();
    black = root.get("blackhole").as_uint(); ca = std::string(root.get("ca").as_string());
    require(plain && h2c_port && tls && black && !ca.empty(), "peer info incomplete");
  }
  std::uint64_t port(const Wire& w) const { return w.tls ? tls : w.h2 ? h2c_port : plain; }
  std::string arm(Fam f, std::string_view plan, bool streaming) {
    const auto model = "matrix-" + std::to_string(++sequence);
    peer.command("{\"arm\":" + sp::json::quote(model) + ",\"family\":" + sp::json::quote(peer_family(f))
                 + ",\"streaming\":" + (streaming ? "true" : "false") + ",\"plan\":" + std::string(plan) + "}");
    return model;
  }
  sp::json::Document stats(const std::string& model) { return peer.stats(model); }
  std::uint64_t counter(const std::string& model, const char* key) { return stats(model).root().get(key).as_uint(); }
};

// ---------------------------------------------------------------------------------------------
// Operation capture: one terminal outcome, nothing after it, nothing semantic that is terminal.
struct Capture {
  std::mutex mutex;
  std::condition_variable cv;
  Result result;
  std::size_t outcomes = 0, deltas = 0, terminals = 0, late = 0;
  std::string text;
  Clock::time_point ended{};
  sp::runtime::Callbacks callbacks() {
    return {[this](const sp::Event& event) {
      std::lock_guard lock(mutex);
      if (outcomes) ++late;
      if (const auto* delta = std::get_if<sp::PartDelta>(&event)) {
        if (delta->payload.kind == sp::PartKind::Text && delta->payload.channel == sp::DeltaChannel::Content) {
          ++deltas; text.append(delta->payload.bytes);
        }
      }
      if (std::holds_alternative<sp::Commit>(event) || std::holds_alternative<sp::Fail>(event)) ++terminals;
      cv.notify_all();
    }, [this](Result value) {
      std::lock_guard lock(mutex);
      ended = Clock::now(); result = std::move(value); ++outcomes; cv.notify_all();
    }};
  }
};

struct Opts {
  Wire wire = h1;
  bool streaming = true;
  Ms deadline{0};                     // 0: runtime default (15 s)
  bool retry = false, risk = false;
  std::uint32_t attempts = 3;
  std::uint64_t port = 0;             // 0: the wire's listener
  bool no_ca = false;                 // TLS without the trust anchor
};

const char* name(ErrorKind k) {
  switch (k) {
    case ErrorKind::InvalidConfig: return "InvalidConfig"; case ErrorKind::InvalidRequest: return "InvalidRequest";
    case ErrorKind::Unsupported: return "Unsupported"; case ErrorKind::Transport: return "Transport";
    case ErrorKind::ProtocolCorrupt: return "ProtocolCorrupt"; case ErrorKind::Truncated: return "Truncated";
    case ErrorKind::RemoteFailure: return "RemoteFailure"; case ErrorKind::Cancelled: return "Cancelled";
    case ErrorKind::DeadlineExceeded: return "DeadlineExceeded"; case ErrorKind::ResourceLimit: return "ResourceLimit";
    case ErrorKind::Misuse: return "Misuse"; case ErrorKind::ReplayIneligible: return "ReplayIneligible";
    case ErrorKind::Authentication: return "Authentication"; case ErrorKind::Permission: return "Permission";
    case ErrorKind::NotFound: return "NotFound"; case ErrorKind::RateLimited: return "RateLimited";
    case ErrorKind::QuotaExhausted: return "QuotaExhausted"; case ErrorKind::LimitUnknown: return "LimitUnknown";
    case ErrorKind::Overloaded: return "Overloaded";
  }
  return "?";
}
const char* name(RetryClass c) {
  switch (c) { case RetryClass::Never: return "Never"; case RetryClass::Transient: return "Transient";
    case RetryClass::AfterReset: return "AfterReset"; case RetryClass::Unknown: return "Unknown"; }
  return "?";
}
const char* name(RetrySafety s) {
  switch (s) { case RetrySafety::NotSent: return "NotSent"; case RetrySafety::PossiblyAccepted: return "PossiblyAccepted";
    case RetrySafety::RejectedBeforeOutput: return "RejectedBeforeOutput"; case RetrySafety::OutputObserved: return "OutputObserved"; }
  return "?";
}

// What a failing cell must report. Unset optionals are deliberately not asserted (a race the
// public API cannot close, documented at the cell).
struct Want {
  ErrorKind kind;
  RetryClass retry;
  RetrySafety safety;
  std::optional<int> status = 0;                  // http_status; unset: not asserted
  std::string vendor;                             // vendor_code
  std::optional<bool> head;                       // attempt.response_head_seen
  bool left = true;                               // attempt.request_may_have_left
  std::uint32_t attempts = 1;
  std::optional<std::pair<Ms, Ms>> retry_after;   // unset: must be absent
  std::optional<std::string> text;                // partial output text
  std::optional<std::uint32_t> resends = 0;       // transport_internal_resends
  Want(ErrorKind k, RetryClass r, RetrySafety s) : kind(k), retry(r), safety(s) {}
  Want& http(int v) { status = v; return *this; }
  Want& any_status() { status.reset(); return *this; }
  Want& code(std::string v) { vendor = std::move(v); return *this; }
  Want& head_seen(bool v) { head = v; return *this; }
  Want& sent(bool v) { left = v; return *this; }
  Want& tries(std::uint32_t v) { attempts = v; return *this; }
  Want& after(std::pair<Ms, Ms> range) { retry_after = range; return *this; }
  Want& output(std::string v) { text = std::move(v); return *this; }
  Want& resent(std::uint32_t v) { resends = v; return *this; }
};

const sp::Failure& expect_failure(const Result& result, const Want& w) {
  require(result && std::holds_alternative<sp::Failure>(*result), "expected a failure outcome");
  const auto& f = std::get<sp::Failure>(*result);
  const auto& e = f.error;
  std::string diff;
  const auto note = [&](std::string_view what, const std::string& got, const std::string& want) {
    diff += std::string(what) + " got " + got + " want " + want + "; ";
  };
  if (e.kind != w.kind) note("kind", name(e.kind), name(w.kind));
  if (e.retry_class != w.retry) note("retry_class", name(e.retry_class), name(w.retry));
  if (e.retry_safety != w.safety) note("retry_safety", name(e.retry_safety), name(w.safety));
  if (w.status && e.http_status != *w.status) note("http_status", std::to_string(e.http_status), std::to_string(*w.status));
  if (e.vendor_code != w.vendor) note("vendor_code", e.vendor_code, w.vendor);
  if (w.head && e.attempt.response_head_seen != *w.head) note("response_head_seen", e.attempt.response_head_seen ? "true" : "false", *w.head ? "true" : "false");
  if (e.attempt.request_may_have_left != w.left) note("request_may_have_left", e.attempt.request_may_have_left ? "true" : "false", w.left ? "true" : "false");
  if (e.attempt.attempts != w.attempts) note("attempts", std::to_string(e.attempt.attempts), std::to_string(w.attempts));
  if (w.resends && e.attempt.transport_internal_resends != *w.resends) note("transport_internal_resends", std::to_string(e.attempt.transport_internal_resends), std::to_string(*w.resends));
  if (w.retry_after) {
    if (!e.retry_after) note("retry_after", "absent", "present");
    else if (*e.retry_after < w.retry_after->first || *e.retry_after > w.retry_after->second)
      note("retry_after_ms", std::to_string(e.retry_after->count()), "[" + std::to_string(w.retry_after->first.count()) + "," + std::to_string(w.retry_after->second.count()) + "]");
  } else if (e.retry_after) note("retry_after", std::to_string(e.retry_after->count()) + "ms", "absent");
  if (e.safe_message.find(marker) != std::string::npos || e.vendor_code.find(marker) != std::string::npos) diff += "secret leaked through error metadata; ";
  if (w.text) {
    std::string text;
    for (const auto& m : f.partial.messages) for (const auto& p : m.parts) if (const auto* t = std::get_if<sp::Text>(&p)) text += t->value;
    if (text != *w.text) note("partial text", text, *w.text);
  }
  require(diff.empty(), diff);
  return f;
}

// ---------------------------------------------------------------------------------------------
// One Client against one armed model. Multiple sequential runs share the Client (reuse cells).
class Cell {
 public:
  Cell(Lab& lab, Fam fam, std::string_view plan, Opts opts) : lab_(lab), fam_(fam), opts_(opts) {
    model = lab.arm(fam, plan, opts.streaming);
    sp::runtime::Options o;
    o.api_key = std::string(marker);
    o.default_timeout = 15s;
    o.retry_tokens_per_second = 0;
    o.http_version = opts.wire.version;
    if (opts.wire.tls && !opts.no_ca) o.ca_file = lab.ca;
    const auto base = std::string(opts.wire.tls ? "https" : "http") + "://127.0.0.1:" + std::to_string(opts.port ? opts.port : lab.port(opts.wire));
    client.emplace(make_descriptor(fam, base, model), o);
  }
  ~Cell() {
    stop.request_stop();
    if (op.valid()) { op.cancel(); op.join(); }
  }
  void start() {
    cap = std::make_unique<Capture>();
    stop = std::stop_source();
    sp::runtime::RunOptions r;
    r.streaming = opts_.streaming;
    r.stop_token = stop.get_token();
    r.retry = sp::runtime::RetryPolicy{opts_.retry, opts_.risk, opts_.attempts, 0ms, 0ms};
    began = Clock::now();
    if (opts_.deadline.count()) { deadline = began + opts_.deadline; r.deadline = deadline; }
    op = client->start(make_request(fam_, model), r, cap->callbacks());
  }
  // Waits for the single outcome (bounded), then fences with join and checks the callback contract.
  Result finish(Ms limit = no_hang) {
    {
      std::unique_lock lock(cap->mutex);
      if (!cap->cv.wait_for(lock, limit, [&] { return cap->outcomes != 0; })) {
        lock.unlock(); op.cancel();
        throw std::runtime_error("HANG: no outcome within " + std::to_string(limit.count()) + " ms");
      }
    }
    auto result = op.join();
    std::lock_guard lock(cap->mutex);
    require(result == cap->result, "join returned a different outcome than the callback");
    require(cap->outcomes == 1, "outcome callback ran " + std::to_string(cap->outcomes) + " times");
    require(cap->terminals == 0, "terminal event leaked as a semantic event");
    require(cap->late == 0, "semantic events delivered after the outcome");
    return result;
  }
  void wait_peer(const char* key, std::size_t n = 1) { lab_.peer.wait(model, key, n); }
  void wait_delta() {
    std::unique_lock lock(cap->mutex);
    require(cap->cv.wait_for(lock, 10s, [&] { return !cap->text.empty(); }), "nonempty semantic output never arrived");
  }
  // Peer accounting: exact request count, injected faults, protocol actually spoken, nothing invalid.
  void peer(std::uint64_t count, std::uint64_t faults) {
    auto report = lab_.stats(model); auto s = report.root();
    std::string diff;
    const auto eq = [&](const char* key, std::uint64_t want) {
      if (s.get(key).as_uint() != want) diff += std::string(key) + "=" + std::to_string(s.get(key).as_uint()) + " want " + std::to_string(want) + "; ";
    };
    eq("count", count); eq("faults", faults); eq("invalid", 0); eq("unexpected", 0);
    eq("h2", opts_.wire.h2 ? count : 0); eq("tls", opts_.wire.tls ? count : 0);
    require(diff.empty(), "peer accounting: " + diff);
  }
  std::uint64_t value(const char* key) { return lab_.counter(model, key); }
  // The peer must learn that the client gave up. HTTP/1.1: the client closes the socket at once. HTTP/2: libcurl
  // (observed with the transport alone, see docs/CONFORMANCE.md "Lifecycle matrix") does not send RST_STREAM when an
  // attempt is cancelled or times out while the connection stays alive, so the stream is only released when the
  // Client (and with it the connection) is destroyed; that is the property pinned here.
  void peer_closed() {
    if (opts_.wire.h2) client.reset();
    wait_peer("closed");
  }
  // Outcome time relative to the absolute deadline.
  Ms past_deadline() { return std::chrono::duration_cast<Ms>(cap->ended - deadline); }
  Lab& lab() { return lab_; }
  Fam fam() const { return fam_; }
  std::string model;
  std::optional<sp::runtime::Client> client;
  sp::runtime::Operation op;
  std::unique_ptr<Capture> cap;
  std::stop_source stop;
  Clock::time_point began{}, deadline{};
 private:
  Lab& lab_;
  Fam fam_;
  Opts opts_;
};

void succeeded(const Result& result, std::string_view expected = "hello", std::uint32_t attempts = 1) {
  require(result && std::holds_alternative<sp::Completion>(*result), "expected a completion");
  const auto& c = std::get<sp::Completion>(*result);
  std::string text;
  for (const auto& m : c.messages) for (const auto& p : m.parts) if (const auto* t = std::get_if<sp::Text>(&p)) text += t->value;
  require(text == expected, "completed text was '" + text + "'");
  require(c.stop.kind == sp::StopKind::EndTurn, "stop reason was not EndTurn");
  require(c.attempt.transport_internal_resends == 0, "completion reports a transport-internal resend");
  require(c.attempt.attempts == attempts, "completion reports " + std::to_string(c.attempt.attempts) + " attempts, expected " + std::to_string(attempts));
}

// ---------------------------------------------------------------------------------------------
// Family-specific classification of HTTP error statuses. Everything not listed is the shared
// status table of config/error-policy.json ("statuses"): 400 InvalidRequest, 401 Authentication,
// 403 Permission, 404 NotFound (all Never); 429 LimitUnknown (Unknown); 500/503/529 Overloaded (Transient).
struct Shape { ErrorKind kind; RetryClass retry; std::string vendor; };
Shape shape(Fam f, int status, bool family_body) {
  Shape s{ErrorKind::Overloaded, RetryClass::Transient, ""};
  switch (status) {
    case 400: s = {ErrorKind::InvalidRequest, RetryClass::Never, ""}; break;
    case 401: s = {ErrorKind::Authentication, RetryClass::Never, ""}; break;
    case 403: s = {ErrorKind::Permission, RetryClass::Never, ""}; break;
    case 404: s = {ErrorKind::NotFound, RetryClass::Never, ""}; break;
    case 429: s = {ErrorKind::LimitUnknown, RetryClass::Unknown, ""}; break;
    default: break;
  }
  if (!family_body) return s;
  // error-policy.json "families": codes are matched against the fields listed per family.
  if (f == Fam::Chat || f == Fam::Responses) {
    // code_fields [type, code]; peer sends type=requests/code=rate_limit_exceeded, code=invalid_api_key, code=model_not_found.
    if (status == 401) s.vendor = "invalid_api_key";
    if (status == 404) s.vendor = "model_not_found";
    if (status == 429) s = {ErrorKind::RateLimited, RetryClass::AfterReset, "rate_limit_exceeded"};
  } else if (f == Fam::Messages) {
    // code_fields [type]; Anthropic error types. api_error is RemoteFailure/Transient, so a 500 with
    // an api_error body is NOT Overloaded here, unlike every other family.
    if (status == 400) s.vendor = "invalid_request_error";
    if (status == 401) s.vendor = "authentication_error";
    if (status == 403) s.vendor = "permission_error";
    if (status == 404) s.vendor = "not_found_error";
    if (status == 429) s = {ErrorKind::RateLimited, RetryClass::AfterReset, "rate_limit_error"};
    if (status == 500) s = {ErrorKind::RemoteFailure, RetryClass::Transient, "api_error"};
    if (status == 503 || status == 529) s.vendor = "overloaded_error";
  }
  // google.generate / google.interactions: code_fields [details.reason]; the only rule is API_KEY_INVALID,
  // so these families use the status table alone (429 stays LimitUnknown/Unknown, no vendor code).
  return s;
}
const char* err_for(int status) {
  switch (status) {
    case 400: return "invalid_request"; case 401: return "auth"; case 403: return "permission"; case 404: return "not_found";
    case 429: return "rate_limit"; case 500: return "server"; default: return "overloaded";
  }
}

enum class Body { Family, Empty, Html, Array };
const char* body_name(Body b) { switch (b) { case Body::Family: return "family-body"; case Body::Empty: return "bare"; case Body::Html: return "html"; case Body::Array: return "array-envelope"; } return "?"; }

std::string status_plan(int status, Body body, const std::string& extra = {}) {
  const char* b = body == Body::Empty ? "empty" : body == Body::Html ? "html" : body == Body::Array ? "array" : "family";
  return "[{\"a\":\"status\",\"status\":" + std::to_string(status) + ",\"err\":\"" + err_for(status) + "\",\"body\":\"" + b + "\"" + extra + "}]";
}

// ---------------------------------------------------------------------------------------------
// Cell implementations
using Fn = std::function<void()>;
struct Matrix {
  Lab& lab;
  Fam fam;
};

void normal(Matrix& m, Wire w, bool streaming) {
  Cell c(m.lab, m.fam, R"([{"a":"ok"}])", {.wire = w, .streaming = streaming});
  c.start();
  succeeded(c.finish());
  c.peer(1, 0);
}

void http_status(Matrix& m, Wire w, bool streaming, int status, Body body, const std::string& extra = {},
                 std::optional<std::pair<Ms, Ms>> retry_after = std::nullopt) {
  Cell c(m.lab, m.fam, status_plan(status, body, extra), {.wire = w, .streaming = streaming});
  c.start();
  const auto result = c.finish();
  const auto s = shape(m.fam, status, body == Body::Family || body == Body::Array);
  Want want(s.kind, s.retry, RetrySafety::PossiblyAccepted);
  want.http(status).code(s.vendor).head_seen(true);
  if (retry_after) want.after(*retry_after);
  expect_failure(result, want);
  c.peer(1, 1);
}

enum class Stall { BeforeHead, AfterHead, MidBody, PartialSse, Trickle };
void deadline_stall(Matrix& m, Wire w, Stall stall, bool streaming) {
  const char* plan = stall == Stall::BeforeHead ? R"([{"a":"stall-head"}])" : stall == Stall::AfterHead ? R"([{"a":"stall-after-head"}])"
      : stall == Stall::MidBody ? R"([{"a":"stall-mid"}])" : stall == Stall::PartialSse ? R"([{"a":"stall-mid","partial_sse":true}])"
      : R"([{"a":"trickle","interval_ms":40}])";
  Cell c(m.lab, m.fam, plan, {.wire = w, .streaming = streaming, .deadline = deadline_cell});
  c.start();
  const auto result = c.finish(deadline_cell + no_hang);
  const auto late = c.past_deadline();
  require(late >= -2ms, "deadline outcome arrived " + std::to_string(-late.count()) + " ms before the deadline");
  require(late <= late_bound, "deadline outcome trailed the deadline by " + std::to_string(late.count()) + " ms");
  const bool output = streaming && (stall == Stall::MidBody || stall == Stall::PartialSse || stall == Stall::Trickle);
  Want want(ErrorKind::DeadlineExceeded, RetryClass::Never, output ? RetrySafety::OutputObserved : RetrySafety::PossiblyAccepted);
  want.http(stall == Stall::BeforeHead ? 0 : 200).head_seen(stall != Stall::BeforeHead);
  if (output && stall != Stall::Trickle) want.output("hello");
  const auto& f = expect_failure(result, want);
  if (stall == Stall::Trickle && streaming) {
    // Progress does not extend an absolute deadline: valid events kept arriving yet the deadline held.
    std::string text;
    for (const auto& msg : f.partial.messages) for (const auto& p : msg.parts) if (const auto* t = std::get_if<sp::Text>(&p)) text += t->value;
    require(text.starts_with("hello") && text.size() > 5 + 3 && text.find_first_not_of('x', 5) == std::string::npos,
            "trickled output was not retained as a valid prefix: '" + text + "'");
  }
  c.peer(1, 0);
  c.peer_closed();
}

void never_respond(Matrix& m) {
  Cell c(m.lab, m.fam, R"([{"a":"ok"}])", {.streaming = true, .deadline = deadline_cell, .port = m.lab.black});
  const auto before = c.value("blackhole");
  c.start();
  const auto result = c.finish(deadline_cell + no_hang);
  const auto late = c.past_deadline();
  require(late >= -2ms && late <= late_bound, "never-respond deadline outcome off by " + std::to_string(late.count()) + " ms");
  expect_failure(result, Want(ErrorKind::DeadlineExceeded, RetryClass::Never, RetrySafety::PossiblyAccepted).head_seen(false));
  c.peer(0, 0);
  require(c.value("blackhole") == before + 1, "black hole did not see exactly one connection");
}

enum class State { BeforeHead, AfterHead, MidBody };
enum class Mech { Handle, Token };
void cancel(Matrix& m, Wire w, State state, Mech mech, bool streaming) {
  const char* plan = state == State::BeforeHead ? R"([{"a":"stall-head"}])" : state == State::AfterHead ? R"([{"a":"stall-after-head"}])" : R"([{"a":"stall-mid"}])";
  Cell c(m.lab, m.fam, plan, {.wire = w, .streaming = streaming});
  c.start();
  c.wait_peer("held");
  const bool output = state == State::MidBody && streaming;
  if (output) c.wait_delta();
  const auto fired = Clock::now();
  if (mech == Mech::Handle) c.op.cancel(); else c.stop.request_stop();
  const auto result = c.finish(late_bound);
  const auto took = std::chrono::duration_cast<Ms>(c.cap->ended - fired);
  require(took < late_bound, "cancel took " + std::to_string(took.count()) + " ms");
  // After the head, or buffered mid-body: whether the client had read the head when cancel fired is a race
  // the public API cannot observe, so response_head_seen and http_status are asserted only where the peer
  // makes the state certain (before the head, or after a delivered semantic delta).
  Want want(ErrorKind::Cancelled, RetryClass::Never, output ? RetrySafety::OutputObserved : RetrySafety::PossiblyAccepted);
  want.any_status();
  if (state == State::BeforeHead) want.http(0).head_seen(false);
  if (output) want.http(200).head_seen(true).output("hello");
  expect_failure(result, want);
  c.peer(1, 0);
  c.peer_closed();
}

void cancel_in_backoff(Matrix& m, Mech mech) {
  // 503 is Transient for every family, including Google where a 429 is LimitUnknown/Unknown and
  // cannot be retried. The five-second Retry-After leaves a controlled backoff cancellation window.
  // The plan repeats its only action, so a (wrongly) sent retry would be counted by the peer.
  Cell c(m.lab, m.fam, status_plan(503, Body::Family, ",\"retry_after\":5"), {.streaming = false, .retry = true, .risk = true});
  c.start();
  c.wait_peer("faults");
  c.wait_peer("closed");
  // Fence the actor's actual backoff transition, not merely the peer's response completion.
  require(sp::runtime::detail::ClientAccess::wait_for_backoff(c.op, 3s),
          "operation did not enter retry backoff before cancellation");
  const auto fired = Clock::now();
  if (mech == Mech::Handle) c.op.cancel(); else c.stop.request_stop();
  const auto result = c.finish(late_bound);
  require(std::chrono::duration_cast<Ms>(c.cap->ended - fired) < 3s, "cancel during backoff was not prompt");
  require(result && std::holds_alternative<sp::Failure>(*result), "backoff cancel did not fail");
  // stop() retains the prior attempt/safety, but deliberately clears the prior HTTP error metadata.
  expect_failure(result, Want(ErrorKind::Cancelled, RetryClass::Never, RetrySafety::PossiblyAccepted).head_seen(true));
  c.peer(1, 1);
}

void reset(Matrix& m, Wire w, bool streaming, bool at_body, Want want) {
  const char* plan = !at_body ? R"([{"a":"reset","at":"head"}])" : streaming ? R"([{"a":"reset","at":"body","hold":true}])" : R"([{"a":"reset","at":"body"}])";
  Cell c(m.lab, m.fam, plan, {.wire = w, .streaming = streaming});
  c.start();
  if (at_body && streaming) { c.wait_peer("held"); c.wait_delta(); c.lab().peer.release(c.model); }
  const auto result = c.finish();
  expect_failure(result, want);
  c.peer(1, 1);
}

void close_after_head(Matrix& m, Wire w, bool streaming, Want want) {
  Cell c(m.lab, m.fam, R"([{"a":"close-after-head"}])", {.wire = w, .streaming = streaming});
  c.start();
  expect_failure(c.finish(), want);
  c.peer(1, 1);
}

// A port that is bound but never listens: the kernel refuses every connection.
class RefusedPort {
 public:
  RefusedPort() : socket_(::socket(AF_INET, SOCK_STREAM, 0)) {
    require(static_cast<bool>(socket_), "cannot reserve a refused port");
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::bind(socket_.get(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0, "cannot bind the refused port");
    portable::socklen size = sizeof(a);
    require(::getsockname(socket_.get(), reinterpret_cast<sockaddr*>(&a), &size) == 0, "cannot inspect the refused port");
    port = ntohs(a.sin_port);
  }
  std::uint16_t port = 0;
 private:
  portable::Socket socket_;
};
void connect_refused(Matrix& m, bool retry) {
  RefusedPort refused;
  Cell c(m.lab, m.fam, R"([{"a":"ok"}])", {.streaming = false, .retry = retry, .attempts = 3, .port = refused.port});
  c.start();
  expect_failure(c.finish(), Want(ErrorKind::Transport, RetryClass::Transient, RetrySafety::NotSent).head_seen(false).sent(false).tries(retry ? 3U : 1U));
  c.peer(0, 0);
}

void tls_untrusted(Matrix& m) {
  // Self-signed peer without the trust anchor: the handshake fails before any request byte, the peer sees nothing.
  Cell c(m.lab, m.fam, R"([{"a":"ok"}])", {.wire = tls1, .streaming = false, .no_ca = true});
  c.start();
  expect_failure(c.finish(), Want(ErrorKind::Transport, RetryClass::Transient, RetrySafety::NotSent).head_seen(false).sent(false));
  c.peer(0, 0);
}

void retry_after_honoured(Matrix& m, bool date, bool streaming) {
  const std::string ra = date ? ",\"retry_after_date_s\":2" : ",\"retry_after\":1";
  // Use 503 for the shared retry contract: Google deliberately leaves 429 Unknown and never retries it.
  Cell c(m.lab, m.fam, "[{\"a\":\"status\",\"status\":503,\"err\":\"overloaded\",\"body\":\"family\"" + ra + "},{\"a\":\"ok\"}]",
         {.streaming = streaming, .retry = true, .risk = true});
  c.start();
  const auto result = c.finish();
  succeeded(result, "hello", 2);
  c.peer(2, 1);
  auto report = c.lab().stats(c.model);
  auto times = report.root().get("times");
  const auto gap = times.at(1).as_double() - times.at(0).as_double();
  // HTTP-date has one-second resolution: +2 s truncates to between 1 and 2 s ahead.
  require(gap >= (date ? 800.0 : 980.0), "retry dispatched " + std::to_string(gap) + " ms after the 503, before Retry-After");
  require(gap < 8000.0, "retry waited far longer than Retry-After");
  if (date) {
    // The exact advertised instant, not the loose floor above: a retry before it ignored the Retry-After date.
    const auto not_before = report.root().get("not_before").as_double();
    require(times.at(1).as_double() >= not_before - 50.0, "retry dispatched before the advertised Retry-After date");
  }
}

void retry_after_beyond_deadline(Matrix& m) {
  // Retry-After (30 s) cannot fit in the 3 s deadline: the 503 is returned, no retry is sent.
  Cell c(m.lab, m.fam, status_plan(503, Body::Family, ",\"retry_after\":30"), {.streaming = false, .deadline = 3s, .retry = true, .risk = true});
  c.start();
  const auto s = shape(m.fam, 503, true);
  expect_failure(c.finish(), Want(s.kind, s.retry, RetrySafety::PossiblyAccepted).http(503).code(s.vendor).head_seen(true).after({Ms(25000), Ms(30000)}));
  c.peer(1, 1);
}
void explicit_retry(Matrix& m, bool risk, bool family_429) {
  const int status = family_429 ? 429 : 503;
  Cell c(m.lab, m.fam, "[{\"a\":\"status\",\"status\":" + std::to_string(status)
         + ",\"err\":\"" + err_for(status) + "\",\"retry_after\":0},{\"a\":\"ok\"}]",
         {.streaming = false, .retry = true, .risk = risk, .attempts = 2});
  c.start();
  const auto result = c.finish();
  const auto s = shape(m.fam, status, true);
  if (risk && s.retry != RetryClass::Unknown) {
    succeeded(result, "hello", 2);
    c.peer(2, 1);
  } else {
    expect_failure(result, Want(s.kind, s.retry, RetrySafety::PossiblyAccepted).http(status).code(s.vendor).head_seen(true).after({0ms, 0ms}));
    c.peer(1, 1);
  }
}

void retry_attempt_cap(Matrix& m) {
  Cell c(m.lab, m.fam, status_plan(503, Body::Family, ",\"retry_after\":0"),
         {.streaming = false, .retry = true, .risk = true, .attempts = 2});
  c.start();
  const auto s = shape(m.fam, 503, true);
  expect_failure(c.finish(), Want(s.kind, s.retry, RetrySafety::PossiblyAccepted).http(503).code(s.vendor).head_seen(true).tries(2).after({0ms, 0ms}));
  c.peer(2, 2);
}

void retry_total_deadline(Matrix& m) {
  // Spend most of the original budget in backoff so refreshing the total deadline on retry
  // cannot hide inside timing tolerance. One second of late scheduling slack remains generous.
  constexpr Ms total = 2600ms;
  Cell c(m.lab, m.fam, R"([{"a":"status","status":503,"err":"overloaded","retry_after":2},{"a":"stall-head"}])",
         {.streaming = false, .deadline = total, .retry = true, .risk = true});
  c.start();
  expect_failure(c.finish(total + no_hang),
                 Want(ErrorKind::DeadlineExceeded, RetryClass::Never, RetrySafety::PossiblyAccepted).head_seen(false).tries(2));
  const auto late = c.past_deadline();
  require(late >= -2ms && late <= 1s, "retry changed the original total deadline");
  c.peer(2, 1);
  c.peer_closed();
}

void retry_after_output(Matrix& m) {
  Cell c(m.lab, m.fam, R"([{"a":"reset","at":"body","hold":true},{"a":"ok"}])", {.retry = true, .risk = true});
  c.start(); c.wait_peer("held"); c.wait_delta(); c.lab().peer.release(c.model);
  expect_failure(c.finish(), Want(ErrorKind::Truncated, RetryClass::Transient, RetrySafety::OutputObserved).http(200).head_seen(true).output("hello"));
  c.peer(1, 1);
}


// Connection reuse: sequential requests on one Client.
void reuse_two(Matrix& m, Wire w) {
  Cell c(m.lab, m.fam, R"([{"a":"ok"},{"a":"ok"}])", {.wire = w, .streaming = true});
  c.start(); succeeded(c.finish());
  c.start(); succeeded(c.finish());
  c.peer(2, 0);
  require(c.value("conns") == 1 && c.value("reused") == 1, "second request did not reuse the first connection (conns=" + std::to_string(c.value("conns")) + ")");
}
void reuse_idle_reset(Matrix& m, Wire w) {
  // The idle keep-alive connection dies between requests (RST; HTTP/2: connection dropped). Depending on
  // when libcurl notices the dead connection, the next request either uses a fresh connection or fails
  // at the resend guard. Both forbid a duplicate peer dispatch. src/transport/http_transport.cpp,
  // OperationState::prereq_cb aborts the second proposal before writing; TransportCore::finish reports
  // prereq_count - 1 as transport_internal_resends. Thus a refused proposal is counted as 1, not 0.
  Cell c(m.lab, m.fam, R"([{"a":"ok","then":"reset-idle"},{"a":"ok"}])", {.wire = w, .streaming = true});
  c.start(); succeeded(c.finish());
  // The response is consumed before the peer resets the now-idle connection.
  c.lab().peer.command("{\"model\":" + sp::json::quote(c.model) + ",\"reset_idle\":true}");
  c.wait_peer("faults");
  c.start();
  const auto result = c.finish();
  if (std::holds_alternative<sp::Completion>(*result)) {
    succeeded(result);
    require(c.value("conns") == 2 && c.value("count") == 2, "request after an idle reset did not use exactly one fresh connection");
    c.peer(2, 1);
  } else {
    expect_failure(result, Want(ErrorKind::Transport, RetryClass::Transient, RetrySafety::PossiblyAccepted).tries(2).resent(1));
    require(c.value("count") == 1, "request after an idle reset reached the peer twice");
    c.peer(1, 1);
  }
}
void reuse_reset_second(Matrix& m, Wire w, Want want) {
  // The reused connection is reset while serving the second request: libcurl would transparently resend on a
  // new connection; the transport must refuse, so the peer sees the second request exactly once.
  Cell c(m.lab, m.fam, R"([{"a":"ok"},{"a":"reset","at":"head"}])", {.wire = w, .streaming = true});
  c.start(); succeeded(c.finish());
  c.start();
  // HTTP/1.1: OperationState::prereq_cb refuses libcurl's second write proposal; TransportCore::finish
  // counts that refused proposal as transport_internal_resends=1, and OperationState::done in
  // src/runtime/client.cpp adds it to attempts. It is NOT a second peer request (pinned below).
  // HTTP/2: RST_STREAM kills the stream only; libcurl does not propose a resend (counter remains 0).
  if (!w.h2) want.tries(2).resent(1);
  expect_failure(c.finish(), want);
  c.peer(2, 1);
  require(c.value("conns") == 1 && c.value("reused") == 1, "the reset was not served on the reused connection");
}


// ---------------------------------------------------------------------------------------------
// Observed behaviour of a peer that resets or closes: derived from the current Chat client, then required of
// every family. HTTP/2 differs from HTTP/1.1 only in the wire event (RST_STREAM / END_STREAM, not RST / FIN).
Want reset_want(Wire w, bool at_body, bool streaming) {
  (void)w;
  if (!at_body) return Want(ErrorKind::Transport, RetryClass::Transient, RetrySafety::PossiblyAccepted).head_seen(false);
  if (streaming) return Want(ErrorKind::Truncated, RetryClass::Transient, RetrySafety::OutputObserved).http(200).head_seen(true).output("hello");
  return Want(ErrorKind::Truncated, RetryClass::Transient, RetrySafety::PossiblyAccepted).any_status();
}
// HTTP/1.1 closes mid-body (FIN without the promised bytes): an abnormal end, retryable class. HTTP/2 ends the
// stream normally (END_STREAM after HEADERS) with an empty body: the codec reports Truncated (no terminal event) but
// the transport saw a normal end, so the class is Never.
Want close_want(Wire w, bool streaming) {
  (void)streaming;
  return Want(ErrorKind::Truncated, w.h2 ? RetryClass::Never : RetryClass::Transient, RetrySafety::PossiblyAccepted).http(200).head_seen(true);
}

struct Entry { std::string name; std::function<void(Matrix&)> fn; };
std::vector<Entry> build(Fam fam) {
  std::vector<Entry> table;
  const auto add = [&](const std::string& name, std::function<void(Matrix&)> fn) {
    table.push_back({std::string(label(fam)) + "/" + name, std::move(fn)});
  };
  const auto mode = [](bool streaming) { return streaming ? "stream" : "buffered"; };
  const std::pair<Ms, Ms> seconds{590000ms, 600000ms}, date{1180000ms, 1200000ms};
  const std::pair<Ms, Ms> unparsable{Ms::max(), Ms::max()};

  // ---- HTTP/1.1 cleartext: the full lifecycle matrix -------------------------------------------
  for (bool s : {true, false}) add(std::string("h1/ok/") + mode(s), [=](Matrix& m) { normal(m, h1, s); });
  for (bool s : {true, false}) for (int status : {400, 401, 403, 404, 429, 500, 503, 529}) {
    for (Body body : {Body::Family, Body::Empty}) {
      add("h1/status-" + std::to_string(status) + "/" + body_name(body) + "/" + mode(s), [=](Matrix& m) { http_status(m, h1, s, status, body); });
    }
  }
  for (bool s : {true, false}) for (int status : {429, 500}) {
    add("h1/status-" + std::to_string(status) + "/" + body_name(Body::Array) + "/" + mode(s), [=](Matrix& m) { http_status(m, h1, s, status, Body::Array); });
  }
  for (bool s : {true, false}) add(std::string("h1/status-503/html/") + mode(s), [=](Matrix& m) { http_status(m, h1, s, 503, Body::Html); });
  for (bool s : {true, false}) {
    add(std::string("h1/retry-after/429-seconds/") + mode(s), [=](Matrix& m) { http_status(m, h1, s, 429, Body::Family, ",\"retry_after\":600", seconds); });
    add(std::string("h1/retry-after/429-http-date/") + mode(s), [=](Matrix& m) { http_status(m, h1, s, 429, Body::Family, ",\"retry_after_date_s\":1200", date); });
    add(std::string("h1/retry-after/503-seconds/") + mode(s), [=](Matrix& m) { http_status(m, h1, s, 503, Body::Family, ",\"retry_after\":600", seconds); });
    add(std::string("h1/retry-after/529-http-date/") + mode(s), [=](Matrix& m) { http_status(m, h1, s, 529, Body::Family, ",\"retry_after_date_s\":1200", date); });
    add(std::string("h1/retry-after/429-unparsable/") + mode(s), [=](Matrix& m) { http_status(m, h1, s, 429, Body::Family, ",\"retry_after\":\"soon\"", unparsable); });
  }
  add("h1/retry-after/honoured-seconds/stream", [](Matrix& m) { retry_after_honoured(m, false, true); });
  add("h1/retry-after/honoured-http-date/buffered", [](Matrix& m) { retry_after_honoured(m, true, false); });
  add("h1/retry-after/beyond-deadline/buffered", [](Matrix& m) { retry_after_beyond_deadline(m); });

  add("h1/retry/risk-not-authorized", [](Matrix& m) { explicit_retry(m, false, false); });
  add("h1/retry/family-429-policy", [](Matrix& m) { explicit_retry(m, true, true); });
  add("h1/retry/attempt-cap", [](Matrix& m) { retry_attempt_cap(m); });
  add("h1/retry/total-deadline-carried", [](Matrix& m) { retry_total_deadline(m); });
  add("h1/retry/no-retry-after-output", [](Matrix& m) { retry_after_output(m); });
  for (bool s : {false, true}) {
    add(std::string("h1/deadline/before-head/") + mode(s), [=](Matrix& m) { deadline_stall(m, h1, Stall::BeforeHead, s); });
    add(std::string("h1/deadline/after-head/") + mode(s), [=](Matrix& m) { deadline_stall(m, h1, Stall::AfterHead, s); });
    add(std::string("h1/deadline/mid-body/") + mode(s), [=](Matrix& m) { deadline_stall(m, h1, Stall::MidBody, s); });
  }
  add("h1/deadline/trickle/stream", [](Matrix& m) { deadline_stall(m, h1, Stall::Trickle, true); });
  add("h1/deadline/partial-sse/stream", [](Matrix& m) { deadline_stall(m, h1, Stall::PartialSse, true); });
  add("h1/deadline/never-respond/stream", [](Matrix& m) { never_respond(m); });

  const char* states[] = {"before-head", "after-head", "mid-body"};
  for (int st = 0; st < 3; ++st) {
    add(std::string("h1/cancel/") + states[st] + "/handle/stream", [=](Matrix& m) { cancel(m, h1, static_cast<State>(st), Mech::Handle, true); });
    add(std::string("h1/cancel/") + states[st] + "/stop-token/stream", [=](Matrix& m) { cancel(m, h1, static_cast<State>(st), Mech::Token, true); });
    add(std::string("h1/cancel/") + states[st] + "/handle/buffered", [=](Matrix& m) { cancel(m, h1, static_cast<State>(st), Mech::Handle, false); });
  }
  add("h1/cancel/retry-backoff/handle", [](Matrix& m) { cancel_in_backoff(m, Mech::Handle); });
  add("h1/cancel/retry-backoff/stop-token", [](Matrix& m) { cancel_in_backoff(m, Mech::Token); });

  for (bool s : {true, false}) {
    add(std::string("h1/reset/before-head/") + mode(s), [=](Matrix& m) { reset(m, h1, s, false, reset_want(h1, false, s)); });
    add(std::string("h1/reset/mid-body/") + mode(s), [=](Matrix& m) { reset(m, h1, s, true, reset_want(h1, true, s)); });
    add(std::string("h1/close-after-head/") + mode(s), [=](Matrix& m) { close_after_head(m, h1, s, close_want(h1, s)); });
  }
  add("h1/connect-refused/no-retry", [](Matrix& m) { connect_refused(m, false); });
  add("h1/connect-refused/retry", [](Matrix& m) { connect_refused(m, true); });

  // ---- TLS (self-signed peer + ca_file), HTTP/2 cleartext, HTTP/2 over TLS ALPN ---------------
  add("tls-h1/untrusted-peer", [](Matrix& m) { tls_untrusted(m); });
  for (const Wire w : {tls1, h2c, tls2}) {
    const std::string p = std::string(w.name) + "/";
    add(p + "ok/stream", [=](Matrix& m) { normal(m, w, true); });
    add(p + "ok/buffered", [=](Matrix& m) { normal(m, w, false); });
    add(p + "status-503-retry-after/stream", [=](Matrix& m) { http_status(m, w, true, 503, Body::Family, ",\"retry_after\":600", seconds); });
    add(p + "status-429-retry-after-date/buffered", [=](Matrix& m) { http_status(m, w, false, 429, Body::Family, ",\"retry_after_date_s\":1200", date); });
    add(p + "status-401/bare/buffered", [=](Matrix& m) { http_status(m, w, false, 401, Body::Empty); });
    add(p + "deadline/before-head/buffered", [=](Matrix& m) { deadline_stall(m, w, Stall::BeforeHead, false); });
    add(p + "deadline/mid-body/stream", [=](Matrix& m) { deadline_stall(m, w, Stall::MidBody, true); });
    add(p + "deadline/trickle/stream", [=](Matrix& m) { deadline_stall(m, w, Stall::Trickle, true); });
    add(p + "cancel/before-head/handle/stream", [=](Matrix& m) { cancel(m, w, State::BeforeHead, Mech::Handle, true); });
    add(p + "cancel/after-head/handle/buffered", [=](Matrix& m) { cancel(m, w, State::AfterHead, Mech::Handle, false); });
    add(p + "cancel/mid-body/stop-token/stream", [=](Matrix& m) { cancel(m, w, State::MidBody, Mech::Token, true); });
    add(p + "reset/before-head/stream", [=](Matrix& m) { reset(m, w, true, false, reset_want(w, false, true)); });
    add(p + "reset/mid-body/stream", [=](Matrix& m) { reset(m, w, true, true, reset_want(w, true, true)); });
    add(p + "close-after-head/stream", [=](Matrix& m) { close_after_head(m, w, true, close_want(w, true)); });
  }

  // ---- connection reuse, every wire ------------------------------------------------------------
  for (const Wire w : {h1, tls1, h2c, tls2}) {
    const std::string p = std::string(w.name) + "/reuse/";
    add(p + "two-requests-one-connection", [=](Matrix& m) { reuse_two(m, w); });
    add(p + "idle-connection-reset-between-requests", [=](Matrix& m) { reuse_idle_reset(m, w); });
    add(p + "reset-on-reused-connection", [=](Matrix& m) { reuse_reset_second(m, w, reset_want(w, false, true)); });
  }
  return table;
}

std::optional<Fam> parse_family(std::string_view value) {
  for (const Fam f : {Fam::Chat, Fam::Responses, Fam::Messages, Fam::Gemini, Fam::Interactions}) if (value == label(f)) return f;
  return std::nullopt;
}
#ifdef _WIN32
// No alarm(2): a watchdog thread ends the process if the matrix is still running after 280 s.
struct AlarmGuard {
  std::jthread watchdog{[](std::stop_token stop) {
    std::mutex mutex;
    std::condition_variable_any ready;
    std::unique_lock lock(mutex);
    ready.wait_for(lock, stop, 280s, [] { return false; });
    if (!stop.stop_requested()) ::TerminateProcess(::GetCurrentProcess(), 142);
  }};
};
#else
struct AlarmGuard {
  AlarmGuard() { alarm(280); }
  ~AlarmGuard() { alarm(0); }
};
#endif
}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  runtime_test::Arguments arguments(argc, argv);
  argc = arguments.argc(); argv = arguments.argv();
#endif
  if (argc < 5) { std::cerr << "usage: sp_matrix_tests <node> <lifecycle_server.mjs> <openssl> <family> [--list]\n"; return 2; }
  const auto fam = parse_family(argv[4]);
  if (!fam) { std::cerr << "unknown family\n"; return 2; }
  const auto table = build(*fam);
  const std::string only = argc > 6 && std::string_view(argv[5]) == "--only" ? argv[6] : "";
  if (argc > 5 && std::string_view(argv[5]) == "--list") {
    for (const auto& entry : table) std::cout << entry.name << '\n';
    return 0;
  }
#ifndef _WIN32
  std::signal(SIGPIPE, SIG_IGN);
#endif
  AlarmGuard alarm_guard;
  portable::set_env("SP_OPENSSL", argv[3]);
  std::size_t failed = 0;
  std::size_t ran = 0;
  try {
    Lab lab(argv[1], argv[2]);
    Matrix matrix{lab, *fam};
    for (const auto& entry : table) {
      if (!only.empty() && entry.name.find(only) == std::string::npos) continue;
      ++ran;
      const auto start = Clock::now();
      try {
        entry.fn(matrix);
        std::cout << "PASS " << entry.name << " (" << std::chrono::duration_cast<Ms>(Clock::now() - start).count() << " ms)\n";
      } catch (const std::exception& error) {
        ++failed;
        std::cout << "FAIL " << entry.name << ": " << error.what() << '\n';
      }
      std::cout.flush();
    }
  } catch (const std::exception& error) {
    std::cerr << "matrix setup failed: " << error.what() << '\n';
    return 1;
  }
  if (ran == 0) { std::cerr << "no matrix cells matched the filter\n"; return 2; }
  std::cout << "matrix " << label(*fam) << ": " << ran << "/" << table.size() << " cells, " << failed << " failed\n";
  return failed == 0 ? 0 : 1;
}
