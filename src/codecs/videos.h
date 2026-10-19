#pragma once
// Video generation (D6): OpenRouter `/api/v1/videos` starts a job; poll it and download its output through
// `jobs::Poll` and `jobs::Download`. A request is dispatched by `runtime::Client::prepare(videos::Request)`.
#include "codecs/jobs.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sp::videos {
struct Request {
  std::string model, prompt;
  // Reference images, inline (sent as data URLs) or by URL.
  std::vector<Media> references{};
  // Seconds of video.
  std::optional<std::uint32_t> duration{};
  // Tokens the provider defines (`16:9`, `480p`, `1280x720`).
  std::optional<std::string> aspect_ratio{}, resolution{}, size{};
  std::optional<bool> audio{};
  std::shared_ptr<const json::Document> extensions{};
};
using Encoded = endpoints::Encoded;
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Request&);
}  // namespace sp::videos
