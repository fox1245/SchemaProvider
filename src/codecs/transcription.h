#pragma once
// Transcription (D6): OpenAI `/v1/audio/transcriptions` (a multipart upload) and OpenRouter
// `/api/v1/audio/transcriptions` (JSON with the audio inline or by URL) turn speech into text. A request is
// dispatched by `runtime::Client::prepare(transcription::Request)`.
#include "codecs/endpoint.h"
#include "core/value.h"
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sp::transcription {
struct Request {
  std::string model;
  // The speech: inline audio (OpenAI, OpenRouter) or a URL (OpenRouter).
  Media audio;
  // The spoken language (`en`, `ko`), when known.
  std::optional<std::string> language{};
  // Text that guides the transcript (OpenAI).
  std::optional<std::string> prompt{};
  // Members the endpoint's policy fact admits, sent as given: JSON members on OpenRouter, form fields on OpenAI.
  std::shared_ptr<const json::Document> extensions{};
};
struct Result {
  std::string text;
  // The language and the audio's length in seconds, when the provider reports them.
  std::string language;
  std::optional<double> seconds{};
  Usage usage;
  ServedModel model;
  AttemptEvidence attempt{};
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
}  // namespace sp::transcription
