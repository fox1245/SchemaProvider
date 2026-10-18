#include "transport/ws_frame.h"

#include <algorithm>
#include <cstring>

namespace sp::transport {
namespace {

constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::size_t kMaxControlPayload = 125;

// SHA-1 only verifies the server's Sec-WebSocket-Accept, as RFC 6455 requires; it protects nothing.
std::array<unsigned char, 20> sha1(std::string_view input) {
  std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  std::string data(input);
  const std::uint64_t bits = static_cast<std::uint64_t>(input.size()) * 8u;
  data.push_back(static_cast<char>(0x80));
  while (data.size() % 64 != 56) data.push_back('\0');
  for (int i = 7; i >= 0; --i) data.push_back(static_cast<char>((bits >> (i * 8)) & 0xFFu));
  const auto rotl = [](std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
  for (std::size_t block = 0; block < data.size(); block += 64) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      const auto* p = reinterpret_cast<const unsigned char*>(data.data() + block + i * 4);
      w[i] = (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
    }
    for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
      const std::uint32_t t = rotl(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rotl(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  std::array<unsigned char, 20> out{};
  for (int i = 0; i < 5; ++i)
    for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<unsigned char>((h[i] >> (24 - j * 8)) & 0xFFu);
  return out;
}

std::string base64(const unsigned char* data, std::size_t size) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  for (std::size_t i = 0; i < size; i += 3) {
    const std::uint32_t chunk = (std::uint32_t{data[i]} << 16) | (i + 1 < size ? std::uint32_t{data[i + 1]} << 8 : 0u) |
                                (i + 2 < size ? std::uint32_t{data[i + 2]} : 0u);
    out.push_back(kAlphabet[(chunk >> 18) & 63]);
    out.push_back(kAlphabet[(chunk >> 12) & 63]);
    out.push_back(i + 1 < size ? kAlphabet[(chunk >> 6) & 63] : '=');
    out.push_back(i + 2 < size ? kAlphabet[chunk & 63] : '=');
  }
  return out;
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}
std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}
bool has_token(std::string_view list, std::string_view token) {
  while (!list.empty()) {
    const auto comma = list.find(',');
    if (iequals(trim(list.substr(0, comma)), token)) return true;
    if (comma == std::string_view::npos) break;
    list.remove_prefix(comma + 1);
  }
  return false;
}
bool header_safe(std::string_view s) {
  return std::none_of(s.begin(), s.end(), [](char c) { return c == '\r' || c == '\n' || c == '\0'; });
}

// Validates a complete UTF-8 string (a close reason).
bool utf8_complete(std::string_view text) {
  unsigned pending = 0;
  unsigned char low = 0x80, high = 0xBF;
  for (unsigned char c : text) {
    if (pending) {
      if (c < low || c > high) return false;
      --pending; low = 0x80; high = 0xBF;
    } else if (c >= 0x80) {
      if (c >= 0xC2 && c <= 0xDF) pending = 1;
      else if (c == 0xE0) { pending = 2; low = 0xA0; }
      else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) pending = 2;
      else if (c == 0xED) { pending = 2; high = 0x9F; }
      else if (c == 0xF0) { pending = 3; low = 0x90; }
      else if (c >= 0xF1 && c <= 0xF3) pending = 3;
      else if (c == 0xF4) { pending = 3; high = 0x8F; }
      else return false;
    }
  }
  return pending == 0;
}

}  // namespace

bool ws_utf8_valid(std::string_view text) noexcept { return utf8_complete(text); }

bool ws_close_code_valid(std::uint16_t code) noexcept {
  return (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1014) || (code >= 3000 && code <= 4999);
}

WsDecoder::WsDecoder(WsLimits limits) : limits_(limits) {}

std::uint16_t WsDecoder::close_code_for_error() const {
  switch (error_) {
    case WsError::InvalidUtf8: return ws_close::InvalidPayload;
    case WsError::TooBig: return ws_close::TooBig;
    default: return ws_close::ProtocolError;
  }
}

bool WsDecoder::fail(WsError error) {
  if (error_ == WsError::None) error_ = error;
  stopped_ = true;
  return false;
}

bool WsDecoder::accept_text(std::string_view bytes) {
  for (unsigned char c : bytes) {
    if (utf8_pending_) {
      if (c < utf8_low_ || c > utf8_high_) return false;
      --utf8_pending_; utf8_low_ = 0x80; utf8_high_ = 0xBF;
    } else if (c >= 0x80) {
      if (c >= 0xC2 && c <= 0xDF) utf8_pending_ = 1;
      else if (c == 0xE0) { utf8_pending_ = 2; utf8_low_ = 0xA0; }
      else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) utf8_pending_ = 2;
      else if (c == 0xED) { utf8_pending_ = 2; utf8_high_ = 0x9F; }
      else if (c == 0xF0) { utf8_pending_ = 3; utf8_low_ = 0x90; }
      else if (c >= 0xF1 && c <= 0xF3) utf8_pending_ = 3;
      else if (c == 0xF4) { utf8_pending_ = 3; utf8_high_ = 0x8F; }
      else return false;
    }
  }
  return true;
}

bool WsDecoder::begin_payload(const Sink& sink) {
  const bool control = static_cast<std::uint8_t>(opcode_) >= 8;
  if (!control) {
    if (remaining_ > limits_.max_message_bytes || message_.size() > limits_.max_message_bytes - remaining_)
      return fail(WsError::TooBig);
    if (opcode_ != WsOpcode::Continuation) message_kind_ = opcode_;
  }
  stage_ = Stage::Payload;
  return remaining_ == 0 ? end_frame(sink) : true;
}

bool WsDecoder::end_frame(const Sink& sink) {
  stage_ = Stage::Head;
  head_size_ = 0;
  head_need_ = 2;
  bool keep = true;
  if (opcode_ == WsOpcode::Ping || opcode_ == WsOpcode::Pong) {
    keep = sink(WsEvent{opcode_ == WsOpcode::Ping ? WsEvent::Kind::Ping : WsEvent::Kind::Pong, control_, 0});
    control_.clear();
  } else if (opcode_ == WsOpcode::Close) {
    std::uint16_t code = ws_close::NoStatus;
    std::string_view reason;
    if (control_.size() == 1) return fail(WsError::ProtocolError);
    if (control_.size() >= 2) {
      code = static_cast<std::uint16_t>((static_cast<unsigned char>(control_[0]) << 8) |
                                        static_cast<unsigned char>(control_[1]));
      if (!ws_close_code_valid(code)) return fail(WsError::ProtocolError);
      reason = std::string_view(control_).substr(2);
      if (!utf8_complete(reason)) return fail(WsError::InvalidUtf8);
    }
    stopped_ = true;
    sink(WsEvent{WsEvent::Kind::Close, reason, code});
    return false;
  } else if (fin_) {
    const bool text = message_kind_ == WsOpcode::Text;
    if (text && utf8_pending_) return fail(WsError::InvalidUtf8);
    keep = sink(WsEvent{text ? WsEvent::Kind::Text : WsEvent::Kind::Binary, message_, 0});
    message_.clear();
    message_kind_ = WsOpcode::Continuation;
    utf8_pending_ = 0; utf8_low_ = 0x80; utf8_high_ = 0xBF;
  }
  if (!keep) stopped_ = true;
  return keep;
}

bool WsDecoder::feed(std::string_view bytes, const Sink& sink) {
  if (stopped_) return false;
  if (!sink) return fail(WsError::Misuse);
  while (!bytes.empty()) {
    if (stage_ == Stage::Payload) {
      const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(remaining_, bytes.size()));
      const auto chunk = bytes.substr(0, take);
      bytes.remove_prefix(take);
      remaining_ -= take;
      if (static_cast<std::uint8_t>(opcode_) >= 8) control_.append(chunk);
      else {
        if (message_kind_ == WsOpcode::Text && !accept_text(chunk)) return fail(WsError::InvalidUtf8);
        message_.append(chunk);
      }
      if (remaining_ == 0 && !end_frame(sink)) return false;
      continue;
    }
    const auto take = std::min(head_need_ - head_size_, bytes.size());
    std::memcpy(head_.data() + head_size_, bytes.data(), take);
    head_size_ += take;
    bytes.remove_prefix(take);
    if (head_size_ < head_need_) break;
    if (stage_ == Stage::Head) {
      const unsigned char b0 = head_[0], b1 = head_[1];
      if (b0 & 0x70) return fail(WsError::ProtocolError);    // reserved bits without an extension
      if (b1 & 0x80) return fail(WsError::ProtocolError);    // a server never masks
      fin_ = (b0 & 0x80) != 0;
      const unsigned op = b0 & 0x0F;
      if (op != 0 && op != 1 && op != 2 && op != 8 && op != 9 && op != 10) return fail(WsError::ProtocolError);
      opcode_ = static_cast<WsOpcode>(op);
      const unsigned length = b1 & 0x7F;
      if (op >= 8) {
        if (!fin_ || length > kMaxControlPayload) return fail(WsError::ProtocolError);
      } else if (op == 0 ? message_kind_ == WsOpcode::Continuation : message_kind_ != WsOpcode::Continuation) {
        return fail(WsError::ProtocolError);   // a continuation without a message, or a new one inside it
      }
      if (length < 126) {
        remaining_ = length;
        if (!begin_payload(sink)) return false;
      } else {
        stage_ = Stage::Length;
        head_need_ = length == 126 ? 4 : 10;
      }
      continue;
    }
    // Stage::Length
    std::uint64_t length = 0;
    for (std::size_t i = 2; i < head_need_; ++i) length = (length << 8) | head_[i];
    if (head_need_ == 10 && (head_[2] & 0x80)) return fail(WsError::ProtocolError);
    remaining_ = length;
    if (!begin_payload(sink)) return false;
  }
  return !stopped_;
}

std::string ws_encode_frame(WsOpcode opcode, std::string_view payload, const std::array<unsigned char, 4>& mask,
                            bool fin) {
  std::string out;
  out.reserve(payload.size() + 14);
  out.push_back(static_cast<char>((fin ? 0x80 : 0x00) | static_cast<unsigned>(opcode)));
  const auto size = static_cast<std::uint64_t>(payload.size());
  if (size < 126) {
    out.push_back(static_cast<char>(0x80 | size));
  } else if (size <= 0xFFFF) {
    out.push_back(static_cast<char>(0x80 | 126));
    out.push_back(static_cast<char>((size >> 8) & 0xFF));
    out.push_back(static_cast<char>(size & 0xFF));
  } else {
    out.push_back(static_cast<char>(0x80 | 127));
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((size >> (i * 8)) & 0xFF));
  }
  out.append(reinterpret_cast<const char*>(mask.data()), mask.size());
  for (std::size_t i = 0; i < payload.size(); ++i)
    out.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]));
  return out;
}

std::string ws_close_payload(std::uint16_t code, std::string_view reason) {
  std::string out;
  out.push_back(static_cast<char>((code >> 8) & 0xFF));
  out.push_back(static_cast<char>(code & 0xFF));
  out.append(reason.substr(0, kMaxControlPayload - 2));
  return out;
}

std::string ws_accept_value(std::string_view key) {
  std::string input(key);
  input.append(kGuid);
  const auto digest = sha1(input);
  return base64(digest.data(), digest.size());
}

std::string ws_key_from(const std::array<unsigned char, 16>& nonce) { return base64(nonce.data(), nonce.size()); }

std::string ws_handshake_request(std::string_view host, std::string_view target, std::string_view key,
                                 const std::vector<std::pair<std::string, std::string>>& headers) {
  if (!header_safe(host) || !header_safe(target) || !header_safe(key) || target.empty() || target.front() != '/')
    return {};
  std::string out = "GET ";
  out.append(target).append(" HTTP/1.1\r\nHost: ").append(host);
  out.append("\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ").append(key);
  out.append("\r\nSec-WebSocket-Version: 13\r\n");
  for (const auto& [name, value] : headers) {
    if (name.empty() || !header_safe(name) || !header_safe(value) || name.find(':') != std::string::npos) return {};
    out.append(name).append(": ").append(value).append("\r\n");
  }
  out.append("\r\n");
  return out;
}

WsHandshakeResponse ws_check_handshake(std::string_view head, std::string_view key) {
  WsHandshakeResponse result;
  const auto line_end = head.find("\r\n");
  const auto status_line = head.substr(0, line_end);
  if (status_line.size() < 12 || status_line.substr(0, 5) != "HTTP/" || status_line.find(' ') == std::string_view::npos) {
    result.error = "malformed status line";
    return result;
  }
  const auto code = status_line.substr(status_line.find(' ') + 1, 3);
  if (code.size() != 3 || !std::all_of(code.begin(), code.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    result.error = "malformed status code";
    return result;
  }
  result.status = (code[0] - '0') * 100 + (code[1] - '0') * 10 + (code[2] - '0');
  auto rest = line_end == std::string_view::npos ? std::string_view{} : head.substr(line_end + 2);
  while (!rest.empty()) {
    const auto end = rest.find("\r\n");
    const auto line = rest.substr(0, end);
    rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 2);
    if (line.empty()) break;
    const auto colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) {
      result.error = "malformed header line";
      return result;
    }
    result.headers.emplace_back(std::string(line.substr(0, colon)), std::string(trim(line.substr(colon + 1))));
  }
  if (result.status != 101) {
    result.error = "the server did not switch protocols";
    return result;
  }
  const auto value = [&](std::string_view name) -> const std::string* {
    for (const auto& [n, v] : result.headers)
      if (iequals(n, name)) return &v;
    return nullptr;
  };
  const auto* upgrade = value("Upgrade");
  const auto* connection = value("Connection");
  const auto* accept = value("Sec-WebSocket-Accept");
  if (!upgrade || !iequals(*upgrade, "websocket")) result.error = "missing Upgrade: websocket";
  else if (!connection || !has_token(*connection, "upgrade")) result.error = "missing Connection: upgrade";
  else if (!accept || *accept != ws_accept_value(key)) result.error = "wrong Sec-WebSocket-Accept";
  else if (value("Sec-WebSocket-Extensions")) result.error = "the server chose an extension that was not offered";
  else if (value("Sec-WebSocket-Protocol")) result.error = "the server chose a subprotocol that was not offered";
  return result;
}

}  // namespace sp::transport
