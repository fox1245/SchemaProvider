#pragma once
// Image generation and edits (D6): OpenAI `/v1/images/generations` and `/v1/images/edits`, and OpenRouter
// `/api/v1/images`. A request is dispatched by `runtime::Client::prepare(images::Request)`; the policy's endpoint
// facts name the origins, paths and admitted members.
#include "codecs/endpoint.h"
#include "core/value.h"
#include "descriptor/descriptor.h"
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sp::reason { struct Reason; }
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

using Encoded = endpoints::Encoded;
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Request&);
// The operation of the images fact `fact` a request performs: `edit` at the `openai` shape when it has references,
// otherwise `generate`. The encoder sends the request to that operation's path, and the policy states its billing
// under it.
std::string_view operation(const descriptor::EndpointFact& fact, const Request&) noexcept;
// What a request bills by (core/endpoint_facts.h): the images fact and the operation above, with the quantities
// - `item:image`: `count`, and not stated when it is unset, since the provider's default count is not known;
// - `item:input_image`: the references, as many as there are;
// - `input`: the input tokens, up to the model's input limit or the host's measured count; and `input_image`, the image
//   tokens among them, bounded the same way, when the request has references;
// - `output`: the output tokens, up to the model's output limit;
// and the dimensions `background`, `output_compression`, `output_format`, `quality` and `size` when they are set, with
// the request's extensions (endpoints::dimensions).
EndpointFacts endpoint_facts(const descriptor::ValidatedDescriptor&, const Request&);

using Close = endpoints::Close;
// Decodes one buffered response into accumulator events: one assistant message with a Media part per image.
class Codec final : public endpoints::Codec {
 public:
  // `requested` is the model the request named; the response does not report one.
  Codec(const descriptor::ValidatedDescriptor&, Accumulator&, std::string requested, SemanticLimits = {});
  bool buffered(std::string_view body, Close close) override;
 private:
  bool fail(ErrorKind, const reason::Reason&);
  bool emit(const Event&);
  const descriptor::ValidatedDescriptor& descriptor_;
  Accumulator& accumulator_;
  std::string requested_;
  SemanticLimits limits_;
};
// The typed result of an internal outcome the codec produced.
Outcome typed(const sp::Outcome&);
}  // namespace sp::images
