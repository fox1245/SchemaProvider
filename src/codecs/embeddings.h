#pragma once
// Embeddings (D6): OpenAI and OpenRouter `/embeddings`, and Google `models/{model}:batchEmbedContents`. A request is
// dispatched by `runtime::Client::prepare(embeddings::Request)`; the policy's endpoint facts name the origins.
#include "codecs/endpoint.h"
#include "core/value.h"
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sp::embeddings {
// One thing to embed: text, images (Google and OpenRouter multimodal models), or both.
struct Input {
  std::string text;
  std::vector<Media> media{};
};
struct Request {
  std::string model;
  std::vector<Input> inputs;
  // Output dimensions, for models that can shorten their vectors.
  std::optional<std::uint32_t> dimensions{};
  // What the vectors are for, a token the provider defines: Google `taskType` (`RETRIEVAL_QUERY`), OpenRouter
  // `input_type` (`query`). OpenAI takes none.
  std::optional<std::string> task{};
  // Members the endpoint's policy fact admits, sent as given.
  std::shared_ptr<const json::Document> extensions{};
};
struct Result {
  // One vector per input, in input order.
  std::vector<std::vector<float>> vectors;
  Usage usage;
  ServedModel model;
  AttemptEvidence attempt{};
  // The response document, kept whole.
  std::vector<RawWire> raw_events;
};
using Outcome = std::variant<Result, Failure>;

using Encoded = endpoints::Encoded;
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Request&);

using Close = endpoints::Close;
// Checks one buffered response: one vector per input, numbers only. The vectors stay in the response document.
class Codec final : public endpoints::Codec {
 public:
  Codec(const descriptor::ValidatedDescriptor&, Accumulator&, std::string requested, SemanticLimits = {});
  bool buffered(std::string_view body, Close close) override;
 private:
  bool fail(ErrorKind, std::string);
  bool emit(const Event&);
  const descriptor::ValidatedDescriptor& descriptor_;
  Accumulator& accumulator_;
  std::string requested_;
  SemanticLimits limits_;
};
Outcome typed(const sp::Outcome&);
}  // namespace sp::embeddings
