// WebSocket client sessions (RFC 6455) on the HTTP transport's event loop.
//
// This header is free of Asio and libcurl types, like http_transport.h. A session borrows the
// transport's resolver, I/O threads and TLS settings: libcurl opens the TCP and TLS connection
// (CONNECT_ONLY, HTTP/1.1 ALPN), and the session writes the opening handshake and every frame
// itself through ws_frame.h. A session costs no thread of its own.
//
// Contract:
//  * Exactly one on_done per session, never followed by another callback. Callbacks are
//    serialized per session and run on I/O threads; they must not block.
//  * on_open runs once the server switched protocols. Messages sent before it are queued and
//    written after the handshake.
//  * A Ping is answered with a Pong of the same payload. A Pong is not delivered.
//  * The session completes (Status::Completed) only through a closing handshake, started by
//    either side; WsResult::clean tells whether both Close frames were exchanged. Losing the
//    connection without a Close frame is Status::Failed with close_code 1006 (Abnormal).
//  * A protocol violation by the server fails the session after sending a Close frame with
//    1002, 1007 or 1009.
//  * Cancellation, the deadline and the optional bounds complete without peer progress.
#pragma once
#include "transport/http_transport.h"
#include "transport/ws_frame.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sp::transport {

struct WsRequest {
  std::string url;              // ws:// or wss://; the path and query form the request target
  std::vector<Header> headers;  // sent in the opening handshake (authorization, origin, ...)
  // Absolute monotonic deadline of the whole session, handshake included. time_point::max()
  // means none; a realtime session sets it to the longest the session may run.
  std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
  std::string ca_file;          // optional extra trust anchor file (tests, private CAs)
  // Optional bounds (std::nullopt = the TransportOptions default; zero or negative = disabled),
  // measured from the start of the session like the HTTP stall bounds:
  //  * connect_timeout: until TCP and TLS are established;
  //  * handshake_timeout: until the server's 101 head has arrived;
  //  * idle_timeout: no byte moved in either direction for this long.
  std::optional<std::chrono::milliseconds> connect_timeout;
  std::optional<std::chrono::milliseconds> handshake_timeout;
  std::optional<std::chrono::milliseconds> idle_timeout;
  // How long close() waits for the server's Close frame before the session ends uncleanly.
  std::chrono::milliseconds close_timeout{5000};
  WsLimits limits;
};

enum class WsMessageKind : std::uint8_t { Text, Binary };

struct WsResult {
  // Completed after a closing handshake; otherwise the HTTP transport's status and failure kinds.
  Result transport;
  // The handshake response status (101 when the server switched protocols), 0 before a response.
  int http_status = 0;
  // The peer's close code and reason, or the code the client sent when it closed or failed the
  // session. ws_close::Abnormal (1006) when no Close frame was exchanged.
  std::uint16_t close_code = ws_close::Abnormal;
  std::string close_reason;
  bool closed_by_peer = false;  // the server sent the first Close frame
  bool clean = false;           // both Close frames were exchanged
};

struct WsCallbacks {
  // The server switched protocols; the head carries its 101 status and headers (names lower-cased).
  std::function<void(const ResponseHead&)> on_open;
  // One whole message, valid only during the call. Text is valid UTF-8.
  std::function<void(WsMessageKind, std::string_view)> on_message;
  // The server's Close frame arrived; the session still answers it before on_done.
  std::function<void(std::uint16_t code, std::string_view reason)> on_close;
  // Exactly once. Required.
  std::function<void(const WsResult&)> on_done;
};

struct WsSessionState;

// Move-only handle, like Operation. Destroying a session that is still open cancels it; detach()
// releases the handle without cancelling. Sends and close are thread-safe and non-blocking.
class WsSession {
 public:
  WsSession() noexcept = default;
  WsSession(WsSession&&) noexcept;
  WsSession& operator=(WsSession&&) noexcept;
  WsSession(const WsSession&) = delete;
  WsSession& operator=(const WsSession&) = delete;
  ~WsSession();

  bool valid() const noexcept { return state_ != nullptr; }
  // Queue one message. False once the session is closing or done; the message is then dropped.
  bool send_text(std::string message);
  bool send_binary(std::string message);
  // Start the closing handshake with `code` (ws_close_code_valid) and a reason of at most 123 bytes.
  void close(std::uint16_t code = ws_close::Normal, std::string reason = {});
  void cancel() noexcept;  // non-blocking, idempotent: ends the session without a closing handshake
  void detach() noexcept;
  WsResult join();         // single outcome; callable repeatedly; Misuse from an I/O thread

 private:
  friend WsSession open_websocket(Transport&, WsRequest, WsCallbacks);
  explicit WsSession(std::shared_ptr<WsSessionState> state) noexcept : state_(std::move(state)) {}
  bool send(WsOpcode opcode, std::string message);
  std::shared_ptr<WsSessionState> state_;
};

// Thread-safe. The transport must outlive the session's I/O; destroying it cancels the session.
WsSession open_websocket(Transport& transport, WsRequest request, WsCallbacks callbacks);

}  // namespace sp::transport
