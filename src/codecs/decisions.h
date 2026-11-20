#pragma once
// Decisions (D6): classification and scoring questions about an input, answered with probabilities. OpenAI
// `/v1/decisions` and OpenRouter `/api/alpha/decisions`. A request is dispatched by
// `runtime::Client::prepare(decisions::Request)`; the policy's endpoint facts name the origins and admitted members.
#include "codecs/endpoint.h"
#include "core/value.h"
#include "descriptor/descriptor.h"
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace sp::reason { struct Reason; }
namespace sp::decisions {
// What a question asks: how likely a statement is (OpenRouter calls it `noul`), which option applies, which level a
// score reaches, or (OpenRouter) where something is in an image.
enum class Kind { Predicate, Choice, Score, Box };
// A choice's value or a score level's label, with guidance that describes it: a string, or on OpenRouter a JSON
// object or array.
struct Option {
  std::string value;
  std::shared_ptr<const json::Document> guidance{};
};
struct Question {
  // Required on OpenRouter, which keys questions by name; optional on OpenAI.
  std::string name;
  Kind kind = Kind::Predicate;
  // A string; OpenRouter also takes a JSON object or array.
  std::shared_ptr<const json::Document> instructions;
  // A choice's options, or a score's levels from lowest to highest; at least two.
  std::vector<Option> options{};
  // A predicate's guidance for a true and a false answer, both or neither (OpenRouter).
  std::shared_ptr<const json::Document> when_true{}, when_false{};
};
// An image sent once under the OpenRouter top-level `assets` member, keyed by `key`. The service resolves the key
// wherever `input` (the `state`) or a question's guidance writes it, as a whole string and, at the models that read
// them, inside longer text; the SDK does not read the text, so it does not check that a key is referred to, and the
// service refuses a request with an asset nothing refers to. The image is inline data of a format that the endpoint's
// policy fact (`asset_formats`) names.
struct Asset {
  std::string key;
  Media media;
};
struct Request {
  std::string model;
  // The evidence: OpenAI `input` (a string or message items), OpenRouter `state` (a string, object or array).
  std::shared_ptr<const json::Document> input;
  std::vector<Question> questions;
  // Members the endpoint's policy fact admits (OpenAI `safety_identifier`, OpenRouter `provider`), sent as given.
  std::shared_ptr<const json::Document> extensions{};
  // OpenRouter only; empty sends no `assets` member. Keys are non-empty and unique.
  std::vector<Asset> assets{};
};
struct Answer {
  std::string name;
  // Empty for a refusal, which names no kind.
  std::optional<Kind> kind{};
  bool refused = false;
  std::optional<double> probability{}, score{}, confidence{};
  std::string choice{};
  // The probability of each choice value or score level, as the provider listed them.
  std::vector<std::pair<std::string, double>> probabilities{};
  // The answer as the provider gave it, with a box's edges or a score's legend.
  std::shared_ptr<const json::Document> raw{};
};
struct Result {
  // In question order (OpenAI), or in the order the provider listed them (OpenRouter).
  std::vector<Answer> answers;
  std::string id, provider;
  Usage usage;
  ServedModel model;
  AttemptEvidence attempt{};
  // The response document, kept whole.
  std::vector<RawWire> raw_events;
};
using Outcome = std::variant<Result, Failure>;

using Encoded = endpoints::Encoded;
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Request&);
// The operation of the decisions fact a request performs (`decide`): the encoder sends the request to that operation's
// path, and the policy states its billing under it.
std::string_view operation(const Request&) noexcept;
// What a request bills by (core/endpoint_facts.h): the decisions fact and `decide`, with the quantities `input`, the
// input tokens up to the model's input limit or the host's measured count, and `output`, the output tokens up to the
// model's output limit; and the request's extensions as dimensions (endpoints::dimensions).
EndpointFacts endpoint_facts(const descriptor::ValidatedDescriptor&, const Request&);

using Close = endpoints::Close;
// Decodes one buffered response into accumulator events: one assistant message with an Opaque part per answer.
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
// The typed result of an internal outcome the codec produced.
Outcome typed(const sp::Outcome&);
}  // namespace sp::decisions
