#pragma once
// Moderation (D6): OpenAI `/v1/moderations` flags text and images against the provider's content categories. A
// request is dispatched by `runtime::Client::prepare(moderation::Request)`; the policy's endpoint facts name the
// origins.
#include "codecs/endpoint.h"
#include "core/value.h"
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace sp::moderation {
// One input to check: text, images, or both.
struct Input {
  std::string text;
  std::vector<Media> media{};
};
struct Request {
  std::string model;
  std::vector<Input> inputs;
  std::shared_ptr<const json::Document> extensions{};
};
struct Verdict {
  bool flagged = false;
  // Each category the provider checked, whether it applies, and its score, in the provider's order.
  std::vector<std::pair<std::string, bool>> categories;
  std::vector<std::pair<std::string, double>> scores;
};
struct Result {
  // One verdict per checked input; with images, the provider may judge the inputs as one.
  std::vector<Verdict> verdicts;
  std::string id;
  Usage usage;
  ServedModel model;
  AttemptEvidence attempt{};
  std::vector<RawWire> raw_events;
};
using Outcome = std::variant<Result, Failure>;

struct Encoded {
  std::string path, body, model;
  bool charge_on_accept = false;
  std::string auth;
};
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
}  // namespace sp::moderation
