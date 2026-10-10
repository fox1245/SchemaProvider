#include "core/media.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace sp {
namespace {
using Byte = unsigned char;

constexpr std::array<std::int8_t, 256> inverse = [] {
  std::array<std::int8_t, 256> table{};
  for (auto& value : table) value = -1;
  for (int i = 0; i < 26; ++i) {
    table['A' + i] = static_cast<std::int8_t>(i);
    table['a' + i] = static_cast<std::int8_t>(26 + i);
  }
  for (int i = 0; i < 10; ++i) table['0' + i] = static_cast<std::int8_t>(52 + i);
  table['+'] = 62;
  table['/'] = 63;
  return table;
}();
constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool token_char(Byte c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '!' || c == '#' ||
         c == '$' || c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' || c == '-' || c == '.' ||
         c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
}
bool token(std::string_view text) noexcept {
  if (text.empty()) return false;
  for (const char c : text) if (!token_char(static_cast<Byte>(c))) return false;
  return true;
}
char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }
bool lower_equal(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) if (lower(a[i]) != lower(b[i])) return false;
  return true;
}
bool printable(std::string_view text, std::size_t maximum) noexcept {
  if (text.empty() || text.size() > maximum) return false;
  for (const char c : text) {
    const auto b = static_cast<Byte>(c);
    if (b < 0x21 || b > 0x7e || c == '"' || c == '\\') return false;
  }
  return true;
}
bool text_safe(std::string_view text, std::size_t maximum) noexcept {
  if (text.size() > maximum) return false;
  for (const char c : text) if (static_cast<Byte>(c) < 0x20 || c == 0x7f) return false;
  return true;
}

// Decodes up to `want` bytes starting at decoded byte `offset` of a canonical base64 string
// without materializing the payload, so signature checks cost O(want), not O(size).
std::size_t decode_at(std::string_view text, std::size_t offset, Byte* out, std::size_t want) noexcept {
  const std::size_t total = base64_decoded_size(text);
  if (offset >= total) return 0;
  want = std::min(want, total - offset);
  std::size_t produced = 0;
  const auto* p = reinterpret_cast<const Byte*>(text.data());
  for (std::size_t quartet = offset / 3; produced < want; ++quartet) {
    const unsigned a = static_cast<unsigned>(inverse[p[4 * quartet]]);
    const unsigned b = static_cast<unsigned>(inverse[p[4 * quartet + 1]]);
    const unsigned c = p[4 * quartet + 2] == '=' ? 0U : static_cast<unsigned>(inverse[p[4 * quartet + 2]]);
    const unsigned d = p[4 * quartet + 3] == '=' ? 0U : static_cast<unsigned>(inverse[p[4 * quartet + 3]]);
    const Byte group[3]{static_cast<Byte>((a << 2) | (b >> 4)), static_cast<Byte>((b << 4) | (c >> 2)),
                        static_cast<Byte>((c << 6) | d)};
    for (std::size_t k = quartet == offset / 3 ? offset % 3 : 0; k < 3 && produced < want; ++k) out[produced++] = group[k];
  }
  return produced;
}

bool starts(const Byte* bytes, std::size_t size, std::string_view prefix, std::size_t at = 0) noexcept {
  return size >= at + prefix.size() && std::memcmp(bytes + at, prefix.data(), prefix.size()) == 0;
}
std::uint32_t be32(const Byte* b) noexcept {
  return (static_cast<std::uint32_t>(b[0]) << 24) | (static_cast<std::uint32_t>(b[1]) << 16) |
         (static_cast<std::uint32_t>(b[2]) << 8) | b[3];
}
// HEIF starts with a FileTypeBox. Read the declared box, not an arbitrary prefix:
// a compatible brand may occur past byte 64, and ISO-BMFF permits a 64-bit box size.
// This remains a structural check, not an image decoder.
bool ftyp(std::string_view canonical, std::initializer_list<std::string_view> brands) noexcept {
  Byte head[24];
  const auto n = decode_at(canonical, 0, head, sizeof head);
  if (n < 16 || !starts(head, n, "ftyp", 4)) return false;
  const std::size_t total = base64_decoded_size(canonical);
  std::uint64_t declared = be32(head);
  std::size_t header = 8;
  if (declared == 1) {
    if (n < 24) return false;
    declared = (static_cast<std::uint64_t>(be32(head + 8)) << 32) | be32(head + 12);
    header = 16;
  } else if (declared == 0) declared = total;
  if (declared < header + 8 || declared > total || (declared - header - 8) % 4) return false;
  auto matches = [&](const Byte* brand) {
    for (const auto expected : brands) if (std::memcmp(brand, expected.data(), 4) == 0) return true;
    return false;
  };
  if (matches(head + header)) return true;
  Byte compatible[64];
  for (std::size_t at = header + 8; at < declared;) {
    const auto count = decode_at(canonical, at, compatible,
        std::min<std::size_t>(sizeof compatible, static_cast<std::size_t>(declared) - at));
    for (std::size_t k = 0; k < count; k += 4) if (matches(compatible + k)) return true;
    at += count;
  }
  return false;
}

enum class Signature : std::uint8_t { None, Png, Jpeg, Gif, Webp, Heif, Avif, Bmp, Tiff };
struct Entry { std::string_view essence; Signature signature; };
// Only image formats have mandatory cheap signatures here. Audio, video and document
// containers have valid variants that a short magic-number probe cannot reliably classify.
constexpr Entry registry[] = {
  {"image/png", Signature::Png}, {"image/jpeg", Signature::Jpeg}, {"image/jpg", Signature::Jpeg},
  {"image/gif", Signature::Gif}, {"image/webp", Signature::Webp}, {"image/heic", Signature::Heif},
  {"image/heif", Signature::Heif}, {"image/avif", Signature::Avif}, {"image/bmp", Signature::Bmp},
  {"image/tiff", Signature::Tiff},
};

Signature signature_of(std::string_view essence) noexcept {
  for (const auto& entry : registry) if (lower_equal(entry.essence, essence)) return entry.signature;
  return Signature::None;
}
} // namespace

std::string_view image_detail_name(ImageDetail detail) noexcept {
  switch (detail) {
    case ImageDetail::Auto: return "auto";
    case ImageDetail::Low: return "low";
    case ImageDetail::High: return "high";
    case ImageDetail::Original: return "original";
  }
  return {};
}
std::string_view media_kind_name(MediaKind kind) noexcept {
  switch (kind) {
    case MediaKind::Image: return "image";
    case MediaKind::Audio: return "audio";
    case MediaKind::Video: return "video";
    case MediaKind::Document: return "document";
  }
  return {};
}
std::string_view media_source_name(MediaSource source) noexcept {
  switch (source) {
    case MediaSource::Inline: return "inline";
    case MediaSource::Url: return "url";
    case MediaSource::File: return "file";
  }
  return {};
}

std::string_view mime_essence(std::string_view mime) noexcept { return mime.substr(0, mime.find(';')); }
bool mime_is_generate_alias(std::string_view mime) noexcept {
  const auto essence = mime_essence(mime);
  return lower_equal(essence, "video/audio/s16le") || lower_equal(essence, "video/audio/wav") ||
         lower_equal(essence, "video/text/timestamp");
}
bool mime_syntax(std::string_view mime) noexcept {
  if (mime.empty() || mime.size() > 255) return false;
  const auto semicolon = mime.find(';');
  const auto essence = mime.substr(0, semicolon);
  const auto slash = essence.find('/');
  if (!mime_is_generate_alias(essence) &&
      (slash == std::string_view::npos || !token(essence.substr(0, slash)) || !token(essence.substr(slash + 1))))
    return false;
  if (semicolon == std::string_view::npos) return true;
  auto rest = mime.substr(semicolon);
  while (!rest.empty()) {
    rest.remove_prefix(1);  // the leading semicolon
    const auto next = rest.find(';');
    const auto parameter = rest.substr(0, next);
    const auto equals = parameter.find('=');
    if (equals == std::string_view::npos || !token(parameter.substr(0, equals)) || !token(parameter.substr(equals + 1)))
      return false;
    rest = next == std::string_view::npos ? std::string_view{} : rest.substr(next);
  }
  return true;
}
bool mime_equal(std::string_view a, std::string_view b) noexcept { return lower_equal(mime_essence(a), mime_essence(b)); }
std::optional<MediaKind> mime_kind(std::string_view mime) noexcept {
  if (!mime_syntax(mime)) return std::nullopt;
  const auto essence = mime_essence(mime);
  if (lower_equal(essence, "video/audio/s16le") || lower_equal(essence, "video/audio/wav")) return MediaKind::Audio;
  if (lower_equal(essence, "video/text/timestamp")) return MediaKind::Document;
  const auto top = essence.substr(0, essence.find('/'));
  if (lower_equal(top, "image")) return MediaKind::Image;
  if (lower_equal(top, "audio")) return MediaKind::Audio;
  if (lower_equal(top, "video")) return MediaKind::Video;
  return MediaKind::Document;
}
bool url_syntax(std::string_view url) noexcept {
  constexpr std::string_view scheme = "https://";
  return url.size() > scheme.size() && url.substr(0, scheme.size()) == scheme && printable(url, 4096);
}
bool reference_syntax(std::string_view reference) noexcept { return printable(reference, 1024); }

bool canonical_base64(std::string_view text) noexcept {
  if (text.empty() || text.size() % 4) return false;
  const std::size_t padding = text.back() == '=' ? (text[text.size() - 2] == '=' ? 2 : 1) : 0;
  const std::size_t count = text.size() - padding;
  const auto* p = reinterpret_cast<const Byte*>(text.data());
  Byte bad = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const Byte c = p[i];
    const bool valid = (static_cast<Byte>(c - 'A') < 26) | (static_cast<Byte>(c - 'a') < 26) |
                       (static_cast<Byte>(c - '0') < 10) | (c == '+') | (c == '/');
    bad |= static_cast<Byte>(!valid);
  }
  if (bad) return false;
  if (padding == 1 && (inverse[p[count - 1]] & 3)) return false;
  if (padding == 2 && (inverse[p[count - 1]] & 15)) return false;
  return true;
}
std::size_t base64_decoded_size(std::string_view text) noexcept {
  if (text.size() < 4) return 0;
  return text.size() / 4 * 3 - (text.back() == '=' ? (text[text.size() - 2] == '=' ? 2 : 1) : 0);
}
std::string base64_encode(std::string_view bytes) {
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  const auto* p = reinterpret_cast<const Byte*>(bytes.data());
  std::size_t i = 0;
  for (; i + 3 <= bytes.size(); i += 3) {
    const unsigned n = (static_cast<unsigned>(p[i]) << 16) | (static_cast<unsigned>(p[i + 1]) << 8) | p[i + 2];
    out.push_back(alphabet[(n >> 18) & 63]);
    out.push_back(alphabet[(n >> 12) & 63]);
    out.push_back(alphabet[(n >> 6) & 63]);
    out.push_back(alphabet[n & 63]);
  }
  if (const std::size_t rest = bytes.size() - i) {
    const unsigned n = (static_cast<unsigned>(p[i]) << 16) | (rest == 2 ? static_cast<unsigned>(p[i + 1]) << 8 : 0U);
    out.push_back(alphabet[(n >> 18) & 63]);
    out.push_back(alphabet[(n >> 12) & 63]);
    out.push_back(rest == 2 ? alphabet[(n >> 6) & 63] : '=');
    out.push_back('=');
  }
  return out;
}
bool base64_decode(std::string_view text, std::string& out) {
  if (!canonical_base64(text)) return false;
  const std::size_t size = base64_decoded_size(text);
  out.resize(size);
  const auto* p = reinterpret_cast<const Byte*>(text.data());
  auto* q = reinterpret_cast<Byte*>(out.data());
  std::size_t at = 0;
  for (std::size_t i = 0; i < text.size(); i += 4) {
    const unsigned a = static_cast<unsigned>(inverse[p[i]]), b = static_cast<unsigned>(inverse[p[i + 1]]);
    const unsigned c = p[i + 2] == '=' ? 0U : static_cast<unsigned>(inverse[p[i + 2]]);
    const unsigned d = p[i + 3] == '=' ? 0U : static_cast<unsigned>(inverse[p[i + 3]]);
    if (at < size) q[at++] = static_cast<Byte>((a << 2) | (b >> 4));
    if (at < size) q[at++] = static_cast<Byte>((b << 4) | (c >> 2));
    if (at < size) q[at++] = static_cast<Byte>((c << 6) | d);
  }
  return true;
}

bool media_signature_matches(std::string_view essence, std::string_view canonical) noexcept {
  const auto signature = signature_of(essence);
  if (signature == Signature::None) return true;
  if (signature == Signature::Heif)
    return ftyp(canonical, {"heic", "heix", "heim", "heis", "hevc", "hevx", "hevm", "hevs", "mif1", "mif2", "msf1"});
  if (signature == Signature::Avif) return ftyp(canonical, {"avif", "avis"});
  Byte head[64];
  const auto n = decode_at(canonical, 0, head, sizeof head);
  switch (signature) {
    case Signature::Png: return starts(head, n, "\x89PNG\r\n\x1a\n");
    case Signature::Jpeg: return n >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF;
    case Signature::Gif: return starts(head, n, "GIF87a") || starts(head, n, "GIF89a");
    case Signature::Webp: return starts(head, n, "RIFF") && starts(head, n, "WEBP", 8);
    case Signature::Heif:
    case Signature::Avif: return false; // handled above without truncating the FileTypeBox
    case Signature::Bmp: return starts(head, n, "BM");
    case Signature::Tiff:
      return starts(head, n, std::string_view("II*\0", 4)) || starts(head, n, std::string_view("MM\0*", 4)) ||
             starts(head, n, std::string_view("II+\0", 4)) || starts(head, n, std::string_view("MM\0+", 4));
    case Signature::None: return true;
  }
  return false;
}

MediaFault check_media(const Media& media, std::size_t decoded_limit) noexcept {
  if (static_cast<unsigned>(media.kind) >= media_kind_count) return MediaFault::Kind;
  if (static_cast<unsigned>(media.source) > static_cast<unsigned>(MediaSource::File)) return MediaFault::Source;
  if (image_detail_name(media.detail).empty()) return MediaFault::Detail;
  if (!text_safe(media.name, 512) || !text_safe(media.id, 512)) return MediaFault::Text;
  const bool has_mime = !media.mime.empty();
  if (has_mime) {
    if (!mime_syntax(media.mime)) return MediaFault::Mime;
    const auto kind = mime_kind(media.mime);
    if (!kind || *kind != media.kind) return MediaFault::Kind;
  }
  switch (media.source) {
    case MediaSource::Inline: {
      if (!has_mime) return MediaFault::Mime;
      if (!media.data || !media.reference.empty()) return MediaFault::Source;
      const auto& data = *media.data;
      if (data.empty() || data.size() % 4) return MediaFault::Base64;
      if (!canonical_base64(data)) return MediaFault::Base64;
      if (base64_decoded_size(data) > decoded_limit) return MediaFault::Size;
      return media_signature_matches(mime_essence(media.mime), data) ? MediaFault::None : MediaFault::Signature;
    }
    case MediaSource::Url:
      if (media.data || !url_syntax(media.reference)) return MediaFault::Source;
      return MediaFault::None;
    case MediaSource::File:
      if (media.data || !reference_syntax(media.reference)) return MediaFault::Source;
      return MediaFault::None;
  }
  return MediaFault::Source;
}
const char* media_fault_name(MediaFault fault) noexcept {
  switch (fault) {
    case MediaFault::None: return "none";
    case MediaFault::Kind: return "media kind does not match its MIME type";
    case MediaFault::Mime: return "invalid or missing media MIME type";
    case MediaFault::Source: return "invalid media source";
    case MediaFault::Base64: return "media payload is not canonical base64";
    case MediaFault::Size: return "media payload exceeds the decoded size limit";
    case MediaFault::Signature: return "media payload does not match its MIME type";
    case MediaFault::Detail: return "invalid image detail";
    case MediaFault::Text: return "invalid media name or identity";
  }
  return "invalid media";
}
} // namespace sp
