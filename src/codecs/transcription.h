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

namespace sp::reason { struct Reason; }
namespace sp::transcription {
struct Request {
  std::string model;
  // The speech: inline audio (OpenAI, OpenRouter) or a URL (OpenRouter).
  Media audio;
  // The spoken language (`en`, `ko`), when known.
  std::optional<std::string> language{};
  // Text that guides the transcript (OpenAI).
  std::optional<std::string> prompt{};
  // Translate the speech into English text instead (OpenAI `/v1/audio/translations`, which takes no language).
  bool translate = false;
  // Members the endpoint's policy fact admits, sent as given: JSON members on OpenRouter, form fields on OpenAI. At
  // OpenAI `response_format` is `json` unless this names `verbose_json`, as `timestamp_granularities` does.
  std::shared_ptr<const json::Document> extensions{};
};
// The Usage::extra counter of a result's reported audio length in whole seconds, rounded up: of `duration` (OpenRouter,
// and OpenAI in `verbose_json`) and `usage.seconds` (OpenAI), each that gives a count, the larger. A result that reports no
// length, or one that is not a number from 0 to 2^64 - 1, records none, and so does one at or above 2^53 that is not an
// integer, whose double may be below the reported length.
inline constexpr std::string_view seconds_counter = "seconds";
struct Result {
  std::string text;
  // The language and the audio's length in seconds, when the provider reports them (OpenAI does in `verbose_json`).
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
// The operation of the transcription fact a request performs: `translate` with `translate`, otherwise `transcribe`.
// The encoder sends the request to that operation's path, and the policy states its billing under it.
std::string_view operation(const Request&) noexcept;
// What a request bills by (core/endpoint_facts.h): the transcription fact and the operation above, with the quantities
// `item:second`, the audio's length in whole seconds, which the request does not hold, so a bound takes it from the
// host (EndpointQuantity::Source::StatedSeconds); `input`, the input tokens up to the model's input limit or the host's
// measured count; and `output`, the output tokens up to the model's output limit; and the dimension `language` when it
// is set, with the request's extensions (endpoints::dimensions).
EndpointFacts endpoint_facts(const descriptor::ValidatedDescriptor&, const Request&);

using Close = endpoints::Close;
class Codec final : public endpoints::Codec {
 public:
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
Outcome typed(const sp::Outcome&);
}  // namespace sp::transcription
