#pragma once
// RFC 6455 client framing: the opening handshake, masked client frames and the decoder for
// server frames. Pure and libcurl-free like SseFramer; the WebSocket transport drives it.
// No extension and no subprotocol is negotiated, so every reserved bit must be zero.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sp::transport {

enum class WsOpcode : std::uint8_t { Continuation = 0, Text = 1, Binary = 2, Close = 8, Ping = 9, Pong = 10 };

// Close codes (RFC 6455 7.4.1). NoStatus and Abnormal are reported locally and never sent.
namespace ws_close {
inline constexpr std::uint16_t Normal = 1000;
inline constexpr std::uint16_t GoingAway = 1001;
inline constexpr std::uint16_t ProtocolError = 1002;
inline constexpr std::uint16_t UnsupportedData = 1003;
inline constexpr std::uint16_t NoStatus = 1005;
inline constexpr std::uint16_t Abnormal = 1006;
inline constexpr std::uint16_t InvalidPayload = 1007;
inline constexpr std::uint16_t PolicyViolation = 1008;
inline constexpr std::uint16_t TooBig = 1009;
inline constexpr std::uint16_t InternalError = 1011;
}  // namespace ws_close

// True for a code a peer may put in a Close frame: 1000-1003, 1007-1014 and 3000-4999.
bool ws_close_code_valid(std::uint16_t code) noexcept;
// True when `text` is complete, valid UTF-8, as a text message must be (RFC 6455 5.6).
bool ws_utf8_valid(std::string_view text) noexcept;

struct WsLimits {
  // Reassembled message bytes, and the payload of one frame; a larger one fails with TooBig.
  std::size_t max_message_bytes = 64u * 1024u * 1024u;
};

struct WsEvent {
  enum class Kind : std::uint8_t { Text, Binary, Ping, Pong, Close };
  Kind kind;
  // A whole message, a control payload, or the close reason. Valid only during the sink call.
  std::string_view payload;
  // Close only: the peer's code, or ws_close::NoStatus when the frame carried none.
  std::uint16_t close_code = 0;
};

enum class WsError : std::uint8_t { None, ProtocolError, InvalidUtf8, TooBig, Misuse };

// Decodes the server-to-client byte stream. Messages are reassembled from fragments; control
// frames may arrive between fragments and are delivered at once. Text is validated as UTF-8 as
// it arrives, so an invalid sequence fails before the message ends. A masked server frame, a
// reserved bit, an unknown opcode, a fragmented or oversized control frame, a continuation
// without a message, or an invalid Close payload is a ProtocolError. After a Close event the
// decoder is stopped. Chunk partitioning never changes the events or the error.
// Not thread-safe; a false sink stops it without an error.
class WsDecoder {
 public:
  using Sink = std::function<bool(const WsEvent&)>;

  explicit WsDecoder(WsLimits limits = {});

  // True while open. False once stopped: a Close event, a false sink, or an error.
  bool feed(std::string_view bytes, const Sink& sink);

  WsError error() const { return error_; }
  bool stopped() const { return stopped_; }
  // The code the client sends in its Close frame for error(): 1002, 1007 or 1009.
  std::uint16_t close_code_for_error() const;

 private:
  enum class Stage : std::uint8_t { Head, Length, Payload };
  bool fail(WsError error);
  bool begin_payload(const Sink& sink);
  bool end_frame(const Sink& sink);
  bool accept_text(std::string_view bytes);

  WsLimits limits_;
  WsError error_ = WsError::None;
  bool stopped_ = false;
  Stage stage_ = Stage::Head;
  std::array<unsigned char, 10> head_{};
  std::size_t head_size_ = 0, head_need_ = 2;
  bool fin_ = false;
  WsOpcode opcode_ = WsOpcode::Continuation;
  std::uint64_t remaining_ = 0;
  std::string control_;
  // The message being reassembled; message_kind_ is Continuation when none is open.
  WsOpcode message_kind_ = WsOpcode::Continuation;
  std::string message_;
  // Incremental UTF-8 state of the open text message.
  unsigned utf8_pending_ = 0;
  unsigned char utf8_low_ = 0x80, utf8_high_ = 0xBF;
};

// One client frame, always masked with `mask` (RFC 6455 5.3). The mask must come from a strong
// random source, fresh for every frame.
std::string ws_encode_frame(WsOpcode opcode, std::string_view payload, const std::array<unsigned char, 4>& mask,
                            bool fin = true);
// The payload of a Close frame: the code in network order, then the UTF-8 reason (at most 123 bytes).
std::string ws_close_payload(std::uint16_t code, std::string_view reason = {});

// base64(SHA-1(key + the RFC 6455 GUID)), the Sec-WebSocket-Accept value for `key`.
std::string ws_accept_value(std::string_view key);
// base64 of 16 bytes, the Sec-WebSocket-Key for a handshake.
std::string ws_key_from(const std::array<unsigned char, 16>& nonce);

// The GET request of the opening handshake (RFC 6455 4.1). `target` is the path and query.
std::string ws_handshake_request(std::string_view host, std::string_view target, std::string_view key,
                                 const std::vector<std::pair<std::string, std::string>>& headers);

struct WsHandshakeResponse {
  int status = 0;
  std::vector<std::pair<std::string, std::string>> headers;
  // Empty when the response completes the handshake, otherwise the failed check.
  std::string error;
};
// Checks the response head (the status line and headers, up to and excluding the blank line):
// status 101, Upgrade websocket, Connection upgrade, the Accept value for `key`, and no
// extension or subprotocol, since none was offered.
WsHandshakeResponse ws_check_handshake(std::string_view head, std::string_view key);

}  // namespace sp::transport
