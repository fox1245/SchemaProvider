#include "codecs/media_output.h"

namespace sp::media_output {
bool parse_data_uri(std::string_view uri, std::string_view& mime, std::string_view& payload) noexcept {
  constexpr std::string_view scheme = "data:";
  constexpr std::string_view marker = ";base64,";
  if (uri.size() <= scheme.size() || uri.substr(0, scheme.size()) != scheme) return false;
  const auto comma = uri.find(',');
  if (comma == std::string_view::npos || comma < scheme.size() + marker.size() - 1) return false;
  const auto header = uri.substr(scheme.size(), comma - scheme.size());
  if (header.size() < marker.size() - 1 || header.substr(header.size() - (marker.size() - 1)) != marker.substr(0, marker.size() - 1))
    return false;
  mime = header.substr(0, header.size() - (marker.size() - 1));
  payload = uri.substr(comma + 1);
  return !mime.empty();
}

MediaKind kind_of(std::string_view mime) noexcept {
  // Shared classification retains generateContent's documented audio/timestamp alias semantics;
  // describe() below keeps the reported spelling rather than normalizing it to another MIME.
  const auto kind = mime_kind(mime);
  if (kind) return *kind;
  // Generated output with an unusual MIME spelling still classifies by its top-level type.
  const auto essence = mime_essence(mime);
  const auto top = essence.substr(0, essence.find('/'));
  auto equal = [&](std::string_view name) {
    if (top.size() != name.size()) return false;
    for (std::size_t i = 0; i < top.size(); ++i) {
      auto c = top[i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
      if (c != name[i]) return false;
    }
    return true;
  };
  if (equal("image")) return MediaKind::Image;
  if (equal("audio")) return MediaKind::Audio;
  if (equal("video")) return MediaKind::Video;
  return MediaKind::Document;
}

Media describe(std::string_view mime, MediaSource source, std::string_view reference) {
  Media media;
  media.kind = kind_of(mime);
  media.mime = std::string(mime);
  media.source = source;
  media.reference = std::string(reference);
  return media;
}

void append_audio(std::string& joined, std::string_view chunk) {
  if (joined.empty() || joined.back() != '=') { joined.append(chunk); return; }
  constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  auto sextet = [](unsigned char c) -> unsigned {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : 63;
  };
  // Reopen only the padded last quartet. Its last 2/4 bits belong to the next chunk.
  const auto start = joined.size() - 4;
  const unsigned count = joined[joined.size() - 2] == '=' ? 2 : 3;
  unsigned bits = 0, available = 0;
  for (unsigned i = 0; i < count; ++i) { bits = (bits << 6) | sextet(joined[start + i]); available += 6; }
  const unsigned padding_bits = count == 2 ? 4 : 2;
  bits >>= padding_bits; available -= padding_bits;
  joined.resize(start);
  while (available >= 6) { available -= 6; joined.push_back(alphabet[(bits >> available) & 63]); }
  const auto chunk_count = chunk.size() - (chunk.back() == '=' ? (chunk[chunk.size() - 2] == '=' ? 2 : 1) : 0);
  for (std::size_t i = 0; i < chunk_count; ++i) {
    unsigned value = sextet(chunk[i]), width = 6;
    if (i + 1 == chunk_count && chunk.back() == '=') width = chunk[chunk.size() - 2] == '=' ? 2 : 4, value >>= 6 - width;
    bits = (bits << width) | value; available += width;
    while (available >= 6) { available -= 6; joined.push_back(alphabet[(bits >> available) & 63]); }
  }
  if (available) joined.push_back(alphabet[(bits << (6 - available)) & 63]);
  while (joined.size() % 4) joined.push_back('=');
}
} // namespace sp::media_output
