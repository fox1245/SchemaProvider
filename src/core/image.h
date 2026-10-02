#pragma once
#include <memory>
#include <string>
#include <string_view>

namespace sp {
enum class ImageDetail { Auto, Low, High, Original };
struct Image {
  std::string mime = "image/png";
  std::shared_ptr<const std::string> data; // canonical base64; shared immutable ownership
  ImageDetail detail = ImageDetail::Auto;
};
// Structural base64/MIME/magic and decoded-size admission, not a pixel decoder.
// No file reads, URLs, downloads or full-image temporary decoding.
bool valid_image(const Image&) noexcept;
std::string_view image_detail_name(ImageDetail) noexcept;
} // namespace sp
