// Hand-authored framing oracles from RFC 6455 (sections 1.3, 5.2-5.7 and 7.4), not snapshots generated
// by the implementation. No providers, clocks, sockets or randomness.
#include "transport/ws_frame.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace sp::transport;
using namespace std::string_literals;

namespace {
int failures = 0;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      ++failures;                                                               \
      std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__,       \
                   #condition);                                                 \
    }                                                                           \
  } while (0)

struct Event {
  WsEvent::Kind kind;
  std::string payload;
  std::uint16_t code = 0;
  bool operator==(const Event&) const = default;
};

struct Outcome {
  std::vector<Event> events;
  WsError error = WsError::None;
  bool stopped = false;
};

// Feeds `wire` in pieces of `step` bytes (0 means all at once).
Outcome decode(std::string_view wire, std::size_t step = 0, WsLimits limits = {}) {
  WsDecoder decoder(limits);
  Outcome outcome;
  const auto sink = [&](const WsEvent& event) {
    outcome.events.push_back({event.kind, std::string(event.payload), event.close_code});
    return true;
  };
  if (step == 0) decoder.feed(wire, sink);
  else
    for (std::size_t i = 0; i < wire.size() && !decoder.stopped(); i += step) decoder.feed(wire.substr(i, step), sink);
  outcome.error = decoder.error();
  outcome.stopped = decoder.stopped();
  return outcome;
}

// Every partition of the wire gives the same events and error.
void each_partition(std::string_view wire, const std::vector<Event>& events, WsError error = WsError::None) {
  for (std::size_t step : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{7}}) {
    const auto outcome = decode(wire, step);
    CHECK(outcome.events == events);
    CHECK(outcome.error == error);
  }
}

std::string frame(unsigned char b0, std::string_view payload) {
  std::string out(1, static_cast<char>(b0));
  if (payload.size() < 126) out.push_back(static_cast<char>(payload.size()));
  else if (payload.size() <= 0xFFFF) {
    out.push_back(126);
    out.push_back(static_cast<char>(payload.size() >> 8));
    out.push_back(static_cast<char>(payload.size() & 0xFF));
  } else {
    out.push_back(127);
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> (i * 8)) & 0xFF));
  }
  return out.append(payload);
}

void rfc_examples() {
  // RFC 6455 5.7: a single-frame unmasked text message.
  each_partition("\x81\x05Hello"s, {{WsEvent::Kind::Text, "Hello"}});
  // A fragmented unmasked text message.
  each_partition("\x01\x03Hel\x80\x02lo"s, {{WsEvent::Kind::Text, "Hello"}});
  // Unmasked Ping and Pong.
  each_partition("\x89\x05Hello"s, {{WsEvent::Kind::Ping, "Hello"}});
  each_partition("\x8a\x05Hello"s, {{WsEvent::Kind::Pong, "Hello"}});
  // 256 bytes and 65536 bytes of binary data in a single unmasked frame.
  const std::string small(256, 'x'), large(65536, 'y');
  each_partition(frame(0x82, small), {{WsEvent::Kind::Binary, small}});
  each_partition(frame(0x82, large), {{WsEvent::Kind::Binary, large}});
  // A masked client text frame "Hello" with masking key 37 fa 21 3d.
  CHECK(ws_encode_frame(WsOpcode::Text, "Hello", {0x37, 0xfa, 0x21, 0x3d}) ==
        "\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58"s);
  // Masked frames use the 16- and 64-bit lengths at 126 and 65536 bytes.
  const auto medium = ws_encode_frame(WsOpcode::Binary, std::string(126, 'a'), {0, 0, 0, 0});
  CHECK(medium.substr(0, 4) == "\x82\xfe\x00\x7e"s && medium.size() == 4 + 4 + 126);
  const auto big = ws_encode_frame(WsOpcode::Binary, large, {0, 0, 0, 0});
  CHECK(big.substr(0, 10) == "\x82\xff\x00\x00\x00\x00\x00\x01\x00\x00"s);
  CHECK(ws_encode_frame(WsOpcode::Text, "x", {1, 2, 3, 4}, false)[0] == '\x01');
}

void handshake() {
  // RFC 6455 1.3 and 4.2.2: the example key and its accept value.
  CHECK(ws_accept_value("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
  CHECK(ws_key_from({'t', 'h', 'e', ' ', 's', 'a', 'm', 'p', 'l', 'e', ' ', 'n', 'o', 'n', 'c', 'e'}) ==
        "dGhlIHNhbXBsZSBub25jZQ==");
  const auto request = ws_handshake_request("server.example.com", "/chat?x=1", "dGhlIHNhbXBsZSBub25jZQ==",
                                            {{"Authorization", "Bearer t"}});
  CHECK(request ==
        "GET /chat?x=1 HTTP/1.1\r\nHost: server.example.com\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\nAuthorization: Bearer t\r\n\r\n");
  // The headers the handshake sets itself are not repeated, whatever their case; the caller's others follow.
  CHECK(ws_handshake_request("server.example.com", "/chat", "dGhlIHNhbXBsZSBub25jZQ==",
                             {{"upgrade", "h2c"}, {"SEC-WEBSOCKET-KEY", "other"}, {"Sec-WebSocket-Protocol", "chat"},
                              {"sec-websocket-extensions", "permessage-deflate"}, {"Sec-WebSocket-Version", "8"}, {"CONNECTION", "close"},
                              {"Host", "elsewhere"}, {"X-Kept", "1"}}) ==
        "GET /chat HTTP/1.1\r\nHost: server.example.com\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\nX-Kept: 1\r\n\r\n");
  CHECK(ws_handshake_request("h", "/", "k", {{"X", "a\r\nInjected: 1"}}).empty());
  CHECK(ws_handshake_request("h", "relative", "k", {}).empty());

  const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
  const std::string good = "HTTP/1.1 101 Switching Protocols\r\nupgrade: WebSocket\r\nConnection: keep-alive, Upgrade\r\n"
                           "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
  const auto accepted = ws_check_handshake(good, key);
  CHECK(accepted.error.empty() && accepted.status == 101 && accepted.headers.size() == 3);
  CHECK(ws_check_handshake("HTTP/1.1 401 Unauthorized\r\nContent-Type: text/plain", key).status == 401);
  CHECK(!ws_check_handshake("HTTP/1.1 401 Unauthorized", key).error.empty());
  CHECK(!ws_check_handshake("HTTP/1.1 101 OK\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: x",
                            key).error.empty());
  CHECK(!ws_check_handshake(good + "\r\nSec-WebSocket-Extensions: permessage-deflate", key).error.empty());
  CHECK(!ws_check_handshake(good + "\r\nSec-WebSocket-Protocol: chat", key).error.empty());
  CHECK(!ws_check_handshake("HTTP/1.1 101 OK\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
                            key).error.empty());
  CHECK(!ws_check_handshake("garbage", key).error.empty());
}

void control_frames_between_fragments() {
  // RFC 6455 5.4: control frames may be injected in the middle of a fragmented message.
  each_partition("\x01\x03Hel\x89\x01p\x80\x02lo"s, {{WsEvent::Kind::Ping, "p"}, {WsEvent::Kind::Text, "Hello"}});
  // Empty fragments and an empty message.
  each_partition("\x01\x00\x00\x00\x80\x00"s, {{WsEvent::Kind::Text, ""}});
  each_partition("\x82\x00"s, {{WsEvent::Kind::Binary, ""}});
}

void close_frames() {
  each_partition("\x88\x00"s, {{WsEvent::Kind::Close, "", ws_close::NoStatus}});
  each_partition("\x88\x02\x03\xe8"s, {{WsEvent::Kind::Close, "", 1000}});
  each_partition("\x88\x05\x03\xe8" "bye"s, {{WsEvent::Kind::Close, "bye", 1000}});
  each_partition("\x88\x02\x0f\xa0"s, {{WsEvent::Kind::Close, "", 4000}});
  // A Close stops the decoder: bytes after it are not read.
  const auto after = decode("\x88\x00\x81\x01x"s);
  CHECK(after.events.size() == 1 && after.stopped && after.error == WsError::None);
  // A 1-byte payload, and codes that may not be sent (RFC 6455 7.4.1-7.4.2).
  each_partition("\x88\x01\x03"s, {}, WsError::ProtocolError);
  for (unsigned code : {0u, 999u, 1004u, 1005u, 1006u, 1015u, 1016u, 2999u, 5000u}) {
    std::string wire = "\x88\x02"s;
    wire.push_back(static_cast<char>(code >> 8));
    wire.push_back(static_cast<char>(code & 0xFF));
    each_partition(wire, {}, WsError::ProtocolError);
  }
  for (std::uint16_t code : {1000, 1003, 1007, 1011, 1012, 1014, 3000, 4999}) CHECK(ws_close_code_valid(code));
  // An invalid UTF-8 reason.
  each_partition("\x88\x04\x03\xe8\xc0\xaf"s, {}, WsError::InvalidUtf8);
  CHECK(ws_close_payload(1000, "bye") == "\x03\xe8" "bye"s);
  CHECK(ws_close_payload(1001, std::string(200, 'r')).size() == 125);
}

void protocol_errors() {
  each_partition("\xc1\x01x"s, {}, WsError::ProtocolError);          // RSV1 without an extension
  each_partition("\xa1\x01x"s, {}, WsError::ProtocolError);          // RSV2
  each_partition("\x91\x01x"s, {}, WsError::ProtocolError);          // RSV3
  each_partition("\x83\x00"s, {}, WsError::ProtocolError);           // reserved data opcode
  each_partition("\x8b\x00"s, {}, WsError::ProtocolError);           // reserved control opcode
  each_partition("\x81\x81\x00\x00\x00\x00x"s, {}, WsError::ProtocolError);   // a masked server frame
  each_partition("\x09\x00"s, {}, WsError::ProtocolError);           // a fragmented ping
  each_partition(frame(0x89, std::string(126, 'p')), {}, WsError::ProtocolError);   // ping over 125 bytes
  each_partition("\x80\x01x"s, {}, WsError::ProtocolError);          // continuation without a message
  each_partition("\x01\x01x\x81\x01y"s, {}, WsError::ProtocolError); // a new message inside a fragmented one
  // A 64-bit length with the most significant bit set.
  each_partition("\x82\x7f\x80\x00\x00\x00\x00\x00\x00\x01"s, {}, WsError::ProtocolError);
  // A message before the error is still delivered.
  each_partition("\x81\x01" "a\xc1\x00"s, {{WsEvent::Kind::Text, "a"}}, WsError::ProtocolError);
}

void utf8() {
  // Valid multi-byte text split across fragments: κόσμε (Autobahn 6.2).
  const std::string kosme = "\xce\xba\xe1\xbd\xb9\xcf\x83\xce\xbc\xce\xb5";
  each_partition(frame(0x81, kosme), {{WsEvent::Kind::Text, kosme}});
  each_partition(frame(0x01, kosme.substr(0, 3)) + frame(0x80, kosme.substr(3)), {{WsEvent::Kind::Text, kosme}});
  // Invalid sequences: overlong, surrogate, beyond U+10FFFF, stray continuation, truncated at the end.
  for (const auto& bad : {"\xc0\xaf"s, "\xed\xa0\x80"s, "\xf4\x90\x80\x80"s, "\x80"s, "\xce"s, "\xf8\x88\x80\x80\x80"s})
    each_partition(frame(0x81, bad), {}, WsError::InvalidUtf8);
  // Fail fast: an invalid first fragment fails before the message ends.
  const auto early = decode(frame(0x01, "\xce\xba\xff"s));
  CHECK(early.error == WsError::InvalidUtf8);
  // Binary data is not UTF-8 checked.
  each_partition(frame(0x82, "\xff\xfe"s), {{WsEvent::Kind::Binary, "\xff\xfe"s}});
}

void limits() {
  WsLimits limits;
  limits.max_message_bytes = 8;
  CHECK(decode(frame(0x82, std::string(9, 'x')), 0, limits).error == WsError::TooBig);
  CHECK(decode(frame(0x02, std::string(5, 'x')) + frame(0x80, std::string(5, 'x')), 0, limits).error ==
        WsError::TooBig);
  CHECK(decode(frame(0x82, std::string(8, 'x')), 0, limits).error == WsError::None);
  WsDecoder decoder(limits);
  decoder.feed(frame(0x82, std::string(9, 'x')), [](const WsEvent&) { return true; });
  CHECK(decoder.close_code_for_error() == ws_close::TooBig);
}

void sink_stop() {
  WsDecoder decoder;
  int seen = 0;
  CHECK(!decoder.feed("\x81\x01" "a\x81\x01" "b"s, [&](const WsEvent&) { ++seen; return false; }));
  CHECK(seen == 1 && decoder.stopped() && decoder.error() == WsError::None);
  WsDecoder misuse;
  CHECK(!misuse.feed("\x81\x01" "a"s, {}) && misuse.error() == WsError::Misuse);
}
}  // namespace

int main() {
  rfc_examples();
  handshake();
  control_frames_between_fragments();
  close_frames();
  protocol_errors();
  utf8();
  limits();
  sink_stop();
  if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
