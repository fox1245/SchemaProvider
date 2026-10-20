#pragma once
// The host seam: what a program that supplies its own event loop and its own HTTP implementation gives a Client
// (`Client(descriptor, options, executor, transport)`, runtime/client.h). A browser, a Worker, Node or a GUI
// thread implements these two interfaces over its fetch, its timers and its message loop. Nothing here needs
// libcurl, Asio or threads, and the header is installed with sp_runtime.
//
// Both interfaces use types the rest of the SDK already speaks: transport::HttpRequest and transport::Callbacks
// (transport/http_transport.h) carry a request in and the response head, body chunks and the end of the attempt out.
// Incremental SSE and UTF-8 framing stay in the runtime (transport::SseFramer), so a host hands over the bytes as
// they arrive, cut anywhere.
#include "runtime/policy.h"
#include "transport/http_transport.h"
#include "transport/ws_transport.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace sp::runtime {

// Where the runtime runs its work and its timers. One executor serves one Client.
//  * `post` and a timer's task never run inline: they run later, on a turn the executor owns. A throwing `post`
//    means the task was not accepted.
//  * Tasks of one Client may run on several threads or on one; the runtime serializes each operation itself.
//  * `in_thread` is true on a thread while it runs one of this executor's tasks. The runtime refuses a blocking call
//    (`Client::complete`, `Operation::join`, ...) made from such a thread, because the call would wait for work that
//    only that thread can do.
//  * `host_pumped` declares that the host runs the executor from its own loop, on the thread that would otherwise
//    block. A blocking call on a Client with such an executor is refused at once, wherever it is made: no thread
//    exists that could finish the work while the caller waits, and the runtime never runs a loop of its own.
//  * `in_thread` and `host_pumped` are read from any thread, so they must be safe to call concurrently.
class Executor {
 public:
  using Task = std::function<void()>;
  using Timer = std::uint64_t;
  virtual ~Executor() = default;
  virtual void post(Task) = 0;
  // A nonzero id, or 0 when the executor is stopping and took nothing. `cancel` of an id that already ran or was
  // cancelled does nothing.
  virtual Timer schedule(SteadyTime, Task) = 0;
  virtual void cancel(Timer) noexcept = 0;
  virtual SteadyTime now() const noexcept = 0;
  virtual WallTime wall_now() const noexcept = 0;
  virtual bool in_thread() const noexcept = 0;
  // Called once, when the Client has no operation left. Drops work that has not run.
  virtual void shutdown() noexcept = 0;
  virtual bool host_pumped() const noexcept { return false; }
};

// One HTTP attempt on the host.
class HostAttempt {
 public:
  virtual ~HostAttempt() = default;
  // Non-blocking and idempotent. An attempt that has not ended ends with one `on_done` whose status is Cancelled.
  virtual void cancel() noexcept = 0;
  // Offers again the bytes `on_body` returned false for, and then reads on.
  virtual void resume() noexcept = 0;
};

// A bidirectional byte stream opened by an HTTP upgrade (WebSocket). RESERVED: declared so that adding WebSocket
// support does not change this interface, and not implemented. No SchemaProvider code opens one yet; a host
// transport that has none keeps the default `open_stream` and returns nullptr.
class HostStream {
 public:
  virtual ~HostStream() = default;
  virtual bool send(transport::WsMessageKind, std::string message) = 0;
  virtual void close(std::uint16_t code, std::string reason) = 0;
  virtual void cancel() noexcept = 0;
};

// The host's HTTP. `start` sends one request and reports its response through `callbacks`, with the contract of
// transport/http_transport.h:
//  * `on_head` once, with the status and the headers (names lower-cased); `on_body` for each chunk, in order, cut
//    anywhere; `on_done` exactly once, last, with the outcome. A response is `Completed` only if the body ended
//    normally; a stream that broke is `Failed`.
//  * No callback runs before `start` returns, and none runs from inside another: a host delivers each from a turn of
//    its own loop (the executor is the natural place to post them), so that a completion is never delivered inline.
//  * `on_body` returning false means "not consumed": stop reading, keep the chunk, and offer it again after
//    `HostAttempt::resume()`. That is the only backpressure.
//  * `HttpRequest::deadline` is enforced by the client, which cancels the attempt at it. The stall bounds
//    (`connect_timeout`, `first_byte_timeout`, `idle_timeout`) are the transport's to honor when it can; `http_version`
//    and `ca_file` are the host's own business and may be ignored. A request with a `file_body` can be refused with
//    `FailureKind::UploadSource` where the host has no files.
//  * Nothing is retried, redirected or resent: one `start` is one attempt.
// The return value is the handle of the attempt; an attempt that cannot even start reports through `on_done`, from a
// later turn, and still returns a handle.
class HostTransport {
 public:
  virtual ~HostTransport() = default;
  virtual std::unique_ptr<HostAttempt> start(transport::HttpRequest, transport::Callbacks) = 0;
  // RESERVED, see HostStream.
  virtual std::unique_ptr<HostStream> open_stream(transport::WsRequest, transport::WsCallbacks) { return nullptr; }
  // Called once, after every attempt has ended.
  virtual void shutdown() noexcept = 0;
};

// The reasons (`Error::reason`) of a blocking call that the runtime refuses. Both come with `ErrorKind::Misuse`.
inline constexpr std::string_view blocking_host_pumped_reason = "runtime.blocking.host_pumped";
inline constexpr std::string_view blocking_in_thread_reason = "runtime.blocking.in_thread";

}  // namespace sp::runtime
