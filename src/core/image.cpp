#include "core/image.h"
#include <array>

namespace sp {
namespace {
int sextet(unsigned char c) noexcept {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
}
std::string_view image_detail_name(ImageDetail detail) noexcept {
  switch (detail) {
    case ImageDetail::Auto: return "auto";
    case ImageDetail::Low: return "low";
    case ImageDetail::High: return "high";
    case ImageDetail::Original: return "original";
  }
  return {};
}
bool valid_image(const Image& image, std::size_t decoded_limit) noexcept {
  if (!image.data || image_detail_name(image.detail).empty()) return false;
  const auto& data = *image.data;
  if (data.empty() || data.size() % 4 || data.size() > ((decoded_limit + 2) / 3) * 4) return false;
  const size_t padding = data.back() == '=' ? (data[data.size() - 2] == '=' ? 2 : 1) : 0;
  const size_t characters = data.size() - padding;
  for (size_t i = 0; i < characters; ++i) if (sextet(static_cast<unsigned char>(data[i])) < 0) return false;
  if (padding == 1 && (sextet(static_cast<unsigned char>(data[characters - 1])) & 3)) return false;
  if (padding == 2 && (sextet(static_cast<unsigned char>(data[characters - 1])) & 15)) return false;
  const size_t decoded = data.size() / 4 * 3 - padding;
  if (decoded > decoded_limit) return false;
  std::array<unsigned char, 12> head{};
  size_t filled = 0;
  for (size_t i = 0; i + 3 < data.size() && filled < head.size(); i += 4) {
    const auto a = static_cast<unsigned>(sextet(static_cast<unsigned char>(data[i])));
    const auto b = static_cast<unsigned>(sextet(static_cast<unsigned char>(data[i + 1])));
    const auto c = data[i + 2] == '=' ? 0U : static_cast<unsigned>(sextet(static_cast<unsigned char>(data[i + 2])));
    const auto d = data[i + 3] == '=' ? 0U : static_cast<unsigned>(sextet(static_cast<unsigned char>(data[i + 3])));
    if (filled < decoded && filled < head.size()) head[filled++] = static_cast<unsigned char>((a << 2) | (b >> 4));
    if (filled < decoded && filled < head.size()) head[filled++] = static_cast<unsigned char>((b << 4) | (c >> 2));
    if (filled < decoded && filled < head.size()) head[filled++] = static_cast<unsigned char>((c << 6) | d);
  }
  if (image.mime == "image/png") {
    constexpr std::array<unsigned char, 8> magic{137, 80, 78, 71, 13, 10, 26, 10};
    if (decoded < magic.size()) return false;
    for (size_t i = 0; i < magic.size(); ++i) if (head[i] != magic[i]) return false;
    return true;
  }
  if (image.mime == "image/jpeg") return decoded >= 3 && head[0] == 255 && head[1] == 216 && head[2] == 255;
  if (image.mime == "image/gif") return decoded >= 6 && head[0] == 'G' && head[1] == 'I' && head[2] == 'F' &&
      head[3] == '8' && (head[4] == '7' || head[4] == '9') && head[5] == 'a';
  if (image.mime == "image/webp") return decoded >= 12 && head[0] == 'R' && head[1] == 'I' && head[2] == 'F' && head[3] == 'F' &&
      head[8] == 'W' && head[9] == 'E' && head[10] == 'B' && head[11] == 'P';
  return false;
}
} // namespace sp
