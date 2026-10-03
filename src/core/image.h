#pragma once
#include "sp/config_defaults.h"
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
bool valid_image(const Image&, std::size_t decoded_limit = config_defaults::codec_resources_image_decoded_bytes) noexcept;
std::string_view image_detail_name(ImageDetail) noexcept;
} // namespace sp
