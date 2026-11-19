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
// The operation of the videos fact a request performs (`submit`): the encoder sends the request to that operation's
// path, and the policy states its billing under it.
std::string_view operation(const Request&) noexcept;
// What a request bills by (core/endpoint_facts.h): the videos fact and `submit`, with the quantities `item:second`,
// `duration`, and not stated when it is unset; `item:input_image`, the references, as many as there are; `input`, the
// input tokens up to the model's input limit or the host's measured count, and `input_image`, the image tokens among
// them, bounded the same way, when the request has references; and `output`, the output tokens up to the model's output
// limit; and the dimensions `aspect_ratio`, `audio` (`true` or `false`), `resolution` and `size` when they are set, with
// the request's extensions (endpoints::dimensions).
EndpointFacts endpoint_facts(const descriptor::ValidatedDescriptor&, const Request&);
}  // namespace sp::videos
