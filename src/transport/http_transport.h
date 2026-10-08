// Transport boundary: one HTTP attempt over libcurl's multi_socket interface,
// driven by a private standalone-Asio event loop (decision D1, docs/decisions/D1-transport.md).
//
// This header is free of Asio and libcurl types so the installed include-direction
// gate (DESIGN.md section 2.4) can hold. It is shipped under SchemaProvider/transport
// by the unstable SDK package; backend types stay private and no stability is promised.
//
// Contract (DESIGN.md sections 2.1 rule 3, 7; CONFORMANCE.md 5.2, 5.12, 5.15, 5.21):
//  * One start() is one logical attempt. The transport never retries and refuses libcurl's own
//    silent resend on a dead reused connection (FailureKind::ResendRefused).
//  * Exactly one on_done per operation, never before the last on_head/on_body returned and never
//    followed by another callback. Callbacks are serialized per operation and run on I/O threads.
//  * It reports what happened on the wire (AttemptObservation); it never interprets semantics.
//  * A response is a success only if the HTTP framing ended normally. A body delimited only by
//    connection close is NOT a normal end (Failed/Truncated).
//  * Cancellation, the deadline and the stall bounds complete without peer progress.
//  * The absolute deadline is the only mandatory time bound. Three OPTIONAL stall bounds, all
//    disabled by default (TransportOptions, HttpRequest), end an attempt whose peer stops making
//    progress while the deadline is still far away: connect_timeout, first_byte_timeout and
//    idle_timeout. Each ends the attempt as Status::Failed with its own FailureKind; the earliest
//    bound wins, and the deadline (Status::DeadlineExceeded) wins when it is the earliest.
#pragma once
#include "sp/config_defaults.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sp::transport {

// Private runtime guard; no backend/executor type crosses this boundary.
bool on_io_thread() noexcept;

enum class HttpVersion : std::uint8_t {
  Auto,                 // HTTP/2 via ALPN on https, HTTP/1.1 otherwise
  Http1_1,              // force HTTP/1.1
  Http2PriorKnowledge,  // cleartext HTTP/2 (tests and loopback only)
  Http3Preferred,       // HTTPS: prefer QUIC when linked libcurl supports it, else normal H2/H1
  Http3Only,            // HTTPS only, fresh QUIC connection, explicit failure instead of downgrade
};

struct Header {
  std::string name;
  std::string value;
};

struct HttpRequest {
  std::string method = "POST";
  std::string url;                 // http(s) only; redirects are never followed
  std::vector<Header> headers;     // curl's own default headers are suppressed
  std::string body;
  // Absolute monotonic deadline covering DNS, connect, TLS, send and the whole response.
  // time_point::max() means "none"; every production call site must set one. The optional stall
  // bounds below never extend it: a bound that would expire at or after the deadline never fires.
  std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
  // Per-attempt overrides of the TransportOptions defaults (std::nullopt = use the default;
  // zero or negative = disabled). Each bound is measured from the start of this attempt.
  std::optional<std::chrono::milliseconds> connect_timeout;
  std::optional<std::chrono::milliseconds> first_byte_timeout;
  std::optional<std::chrono::milliseconds> idle_timeout;
  HttpVersion http_version = HttpVersion::Auto;
  std::string ca_file;             // optional extra trust anchor file (tests, private CAs)
};

enum class ResponseVersion : std::uint8_t { Unknown, Http1_0, Http1_1, Http2, Http3 };

// How the response body is delimited on the wire.
enum class BodyFraming : std::uint8_t {
  None,             // no body (HEAD, 204, 304, content-length 0 handled as ContentLength)
  ContentLength,
  Chunked,
  CloseDelimited,   // delimited only by connection close: never a normal end
  Stream,           // HTTP/2 END_STREAM or HTTP/3 stream FIN; libcurl validates stream framing
};

struct ResponseHead {
  int status = 0;
  ResponseVersion version = ResponseVersion::Unknown;
  BodyFraming framing = BodyFraming::None;
  std::vector<Header> headers;  // names lower-cased; bounded by TransportOptions::max_head_bytes
};

// Furthest point the attempt provably reached, derived from libcurl's timing marks (zero for
// phases that did not happen) and the header callback. libcurl records the connect time only once
// the whole connection chain is up (TCP and, for https, TLS), so a stall in the TCP connect and a
// stall in the TLS handshake are both reported as Resolved; the distinction is not observable here.
// RequestStarted is the conservative "request bytes MAY have left the client" mark; anything below
// it guarantees that nothing was written (retry safety NotSent).
enum class Stage : std::uint8_t {
  Queued,            // no name lookup finished yet
  Resolved,          // name resolved; TCP connect and TLS handshake not both finished
  Connected,         // TCP and TLS (if any) established; nothing written yet
  RequestStarted,    // pre-transfer reached: request bytes MAY have left the client
  ResponseStarted,   // the first response header line arrived
};

struct AttemptObservation {
  Stage reached = Stage::Queued;
  // Body bytes libcurl reports as uploaded (CURLINFO_SIZE_UPLOAD_T). Header bytes are not reported:
  // libcurl's request size also counts an inline POST body, so it cannot be split reliably.
  std::int64_t request_body_bytes = 0;
  // The FIRST request write of this operation went onto a connection that already existed. A
  // reused connection that dies before any response byte is what makes a resend tempting.
  bool connection_reused = false;
  bool response_head_seen = false;
  // Number of request writes beyond the first that libcurl tried on this operation. The default
  // policy refuses them, so a nonzero value always comes with FailureKind::ResendRefused.
  std::uint32_t transport_internal_resends = 0;
  ResponseVersion version = ResponseVersion::Unknown;
};

enum class Status : std::uint8_t { Completed, Cancelled, DeadlineExceeded, Failed };

enum class FailureKind : std::uint8_t {
  None,
  Resolve,          // DNS
  Connect,          // TCP or QUIC connection establishment
  Tls,              // handshake or certificate verification
  Send,
  Receive,          // reset, EOF before any response, stream error
  Truncated,        // abnormal end of the body (no normal close)
  Protocol,         // malformed HTTP / HTTP/2 / HTTP/3, or unsupported requested protocol
  ResendRefused,    // libcurl wanted to resend on a fresh connection; refused
  ResponseTooLarge, // response head exceeded max_head_bytes
  CallbackError,    // a user callback threw
  Misuse,           // contract violation by the caller (for example join() on an I/O thread)
  ConnectTimeout,   // connect_timeout expired before the connection (TCP and TLS) was established
  FirstByteTimeout, // first_byte_timeout expired before the first response header line arrived
  IdleTimeout,      // idle_timeout: no byte moved in either direction for that long
  Other,
};

struct Result {
  Status status = Status::Failed;
  FailureKind failure = FailureKind::None;
  int http_status = 0;         // 0 when no response head arrived
  int curl_code = 0;           // CURLcode; diagnostic only
  std::string detail;          // safe text: never a URL, header value or body
  AttemptObservation attempt;
  std::chrono::steady_clock::duration elapsed{};
};

struct Callbacks {
  // Final response head (1xx responses are skipped). May be empty.
  std::function<void(const ResponseHead&)> on_head;
  // Offered body bytes, valid only during the call. Return true if consumed. Returning false
  // means "not consumed": the transport stops reading this transfer (TCP backpressure on
  // HTTP/1.1), libcurl retains the offered bytes and redelivers them after Operation::resume().
  // May be empty (everything is consumed).
  std::function<bool(std::string_view)> on_body;
  // Exactly once. Required.
  std::function<void(const Result&)> on_done;
};

// Blocking name lookup used by the transport's resolver pool: returns numeric addresses in
// preference order, empty on failure. The default uses getaddrinfo. A custom function is called
// concurrently from up to resolver_threads threads and may outlive the Transport (a lookup that is
// stuck when the transport is destroyed finishes on its own thread), so it must be thread-safe and
// must not reference the Transport.
using ResolveFn = std::function<std::vector<std::string>(const std::string& host)>;

struct TransportOptions {
  unsigned io_threads = static_cast<unsigned>(config_defaults::defaults_io_threads);
  long max_host_connections = static_cast<long>(config_defaults::defaults_max_host_connections); // 0 = unlimited
  std::size_t max_head_bytes = config_defaults::defaults_max_head_bytes;
  // Name resolution is done by the transport, not by libcurl, because libcurl's threaded resolver
  // starts one thread per concurrent lookup (measured: 72 simultaneous lookups -> +72 threads),
  // which breaks the "threads independent of concurrency" gate. Lookups are single-flight per
  // host, cached for dns_ttl, and run on at most resolver_threads threads. IP literals skip it.
  unsigned resolver_threads = static_cast<unsigned>(config_defaults::defaults_resolver_threads);
  std::chrono::seconds dns_ttl{config_defaults::defaults_dns_ttl_seconds};
  ResolveFn resolve;                    // empty: getaddrinfo
  // Optional stall bounds, applied to every attempt unless HttpRequest overrides them. Zero (the
  // default) disables a bound, so a transport built without them behaves exactly as before: the
  // absolute HttpRequest::deadline is then the only time bound. Each is measured from the start of
  // the attempt, with its own timer on the transport's event loop, and ends the attempt without
  // waiting for peer progress.
  //  * connect_timeout: attempt start until the connection is established. libcurl records
  //    "connected" only once the whole chain (TCP and, for https, TLS) is up, so a stalled TCP
  //    connect and a stalled TLS handshake are both bounded by this one value and reported as
  //    FailureKind::ConnectTimeout (the distinction is not observable, see Stage). Name lookup
  //    time counts. A reused connection satisfies it at once.
  //  * first_byte_timeout: attempt start until the first response header line arrives. It
  //    includes connect_timeout's span, the request upload and the peer's think time.
  //  * idle_timeout: no byte moved for this long, in either direction, counted from the start of
  //    the attempt and re-armed by every request header sent, request body byte sent, response header
  //    line or body byte received (SSE comments and keep-alives count; HTTP/2 activity is per transfer,
  //    not per connection). TLS handshake bytes and HTTP framing that carries no payload are not
  //    counted. Time the application keeps a transfer paused (on_body returned false) is not
  //    peer silence and is excluded. This is the bound to set when the deadline is raised for a
  //    long generation.
  std::chrono::milliseconds connect_timeout{0};
  std::chrono::milliseconds first_byte_timeout{0};
  std::chrono::milliseconds idle_timeout{0};
};

struct RuntimeInfo {
  std::string curl_version;
  std::string ssl_backend;
  bool http2 = false;
  bool http3 = false; // linked CURL_VERSION_HTTP3 feature, not a header/version-number guess
  bool async_dns = false;
};

class Transport;
struct OperationState;
struct TransportCore;

// Move-only handle over shared operation state (DESIGN.md section 7).
//  * Destroying a non-terminal handle performs a non-blocking cancel; the state still delivers
//    exactly one outcome (Cancelled) to the owned callbacks. detach() releases without cancel.
//  * join() blocks until the outcome was delivered and its callback returned. Called from an I/O
//    thread it returns FailureKind::Misuse immediately instead of deadlocking.
//  * Like std::thread, one Operation object is not safe for concurrent mutation: cancel(),
//    resume(), detach(), move-assignment and destruction of the SAME object must not race with
//    each other or with join() on it. Different handles are independent, and the state behind a
//    handle is safe from any thread.
class Operation {
 public:
  Operation() noexcept = default;
  Operation(Operation&&) noexcept;
  Operation& operator=(Operation&&) noexcept;
  Operation(const Operation&) = delete;
  Operation& operator=(const Operation&) = delete;
  ~Operation();

  bool valid() const noexcept { return state_ != nullptr; }
  void cancel() noexcept;   // non-blocking, idempotent
  void resume() noexcept;   // non-blocking: redeliver bytes refused by on_body
  void detach() noexcept;   // keep running without a handle
  Result join();            // single outcome; callable repeatedly

 private:
  friend class Transport;
  explicit Operation(std::shared_ptr<OperationState> state) noexcept : state_(std::move(state)) {}
  std::shared_ptr<OperationState> state_;
};

class Transport {
 public:
  explicit Transport(TransportOptions options = {});
  ~Transport();  // cancels every active operation (one Cancelled outcome each) and joins threads
  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;

  // Thread-safe. The returned handle owns the callbacks' lifetime through the shared state.
  Operation start(HttpRequest request, Callbacks callbacks);

  RuntimeInfo runtime_info() const;
  unsigned io_threads() const noexcept;

 private:
  std::shared_ptr<TransportCore> core_;
};

}  // namespace sp::transport
