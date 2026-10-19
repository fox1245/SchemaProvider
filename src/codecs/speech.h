#pragma once
// Speech (D6): OpenAI `/v1/audio/speech` and OpenRouter `/api/v1/audio/speech` turn text into audio. The response
// body is the audio itself. A request is dispatched by `runtime::Client::prepare(speech::Request)`.
#include "codecs/endpoint.h"
#include "core/value.h"
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sp::speech {
struct Request {
  std::string model, input, voice;
  // How to speak (OpenAI `gpt-4o-mini-tts` and later, OpenRouter models that take it).
  std::optional<std::string> instructions{};
  // The audio format, a token the provider defines (`mp3`, `wav`, `opus`, `pcm`).
  std::optional<std::string> format{};
  std::optional<double> speed{};
  std::shared_ptr<const json::Document> extensions{};
};
struct Result {
  // The audio, inline, with the MIME type the response named.
  Media audio;
  // OpenRouter's generation id (`X-Generation-Id`), empty elsewhere.
  std::string id;
  Usage usage;
  ServedModel model;
  AttemptEvidence attempt{};
  // The body is audio, not a document: `speech.response` records its type, size and SHA-256.
  std::vector<RawWire> raw_events;
};
using Outcome = std::variant<Result, Failure>;

using Encoded = endpoints::Encoded;
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Request&);

using Close = endpoints::Close;
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
}  // namespace sp::speech
