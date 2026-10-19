#pragma once
// Image generation and edits (D6): OpenAI `/v1/images/generations` and `/v1/images/edits`, and OpenRouter
// `/api/v1/images`. A request is dispatched by `runtime::Client::prepare(images::Request)`; the policy's endpoint
// facts name the origins, paths and admitted members.
#include "codecs/accumulator.h"
#include "core/value.h"
#include "descriptor/descriptor.h"
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sp::images {
struct Request {
  std::string model, prompt;
  // Source images: with any, OpenAI edits them (`/v1/images/edits`) and OpenRouter takes them as references.
  // Image media, inline (sent as a data URL), by URL, or by provider file id (OpenAI).
  std::vector<Media> references{};
  // How many images to make (`n`); providers may return fewer.
  std::optional<std::uint32_t> count{};
  // Tokens the provider defines (`1024x1024`, `high`, `transparent`, `webp`); the SDK checks their shape only.
  std::optional<std::string> size{}, quality{}, background{}, output_format{};
  std::optional<std::uint32_t> output_compression{};
  // Members the endpoint's policy fact admits (OpenAI `moderation`, OpenRouter `aspect_ratio`), sent as given.
  std::shared_ptr<const json::Document> extensions{};
};
struct Result {
  // The images in the order the provider gave them: inline base64 with its MIME type, or a URL.
  std::vector<Media> images;
  // Per image, the prompt the provider rewrote, empty where it gave none.
  std::vector<std::string> revised_prompts;
  std::optional<std::uint64_t> created;
  Usage usage;
  ServedModel model;
  AttemptEvidence attempt{};
  // The response document, kept whole.
  std::vector<RawWire> raw_events;
};
using Outcome = std::variant<Result, Failure>;

struct Encoded {
  std::string path, body, model;
  bool charge_on_accept = false;
  std::string auth;
};
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Request&);

struct Close { bool normal = true; ErrorKind error = ErrorKind::Truncated; };
// Decodes one buffered response into accumulator events: one assistant message with a Media part per image.
class Codec {
 public:
  // `requested` is the model the request named; the response does not report one.
  Codec(const descriptor::ValidatedDescriptor&, Accumulator&, std::string requested, SemanticLimits = {});
  bool buffered(std::string_view body, Close close);
 private:
  bool fail(ErrorKind, std::string);
  bool emit(const Event&);
  const descriptor::ValidatedDescriptor& descriptor_;
  Accumulator& accumulator_;
  std::string requested_;
  SemanticLimits limits_;
};
// The typed result of an internal outcome the codec produced.
Outcome typed(const sp::Outcome&);
}  // namespace sp::images
