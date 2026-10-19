#pragma once
// Token counting (D7): how many input tokens a chat request would use, without running it. Anthropic
// `/v1/messages/count_tokens` counts a Messages request, OpenAI `/v1/responses/input_tokens` a Responses request,
// Google `models/{model}:countTokens` a Generate request. The request is encoded as the family would send it; the
// policy's `tokens` fact keeps the members the count takes (`keep`) and drops the rest (`max_tokens`, `stream`).
#include "codecs/endpoint.h"
#include "codecs/family_request.h"
#include "core/value.h"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace sp::tokens {
struct Count { FamilyRequest request; };
struct Result {
  std::uint64_t input = 0;
  // Google: tokens served from a cached content, and the count of each input modality.
  std::optional<std::uint64_t> cached{};
  std::vector<std::pair<std::string, std::uint64_t>> modalities{};
  AttemptEvidence attempt{};
  std::vector<RawWire> raw_events;
};
using Outcome = std::variant<Result, Failure>;

using Encoded = endpoints::Encoded;
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Count&);

using Close = endpoints::Close;
class Codec final : public endpoints::Codec {
 public:
  Codec(const descriptor::ValidatedDescriptor&, Accumulator&, SemanticLimits = {});
  bool buffered(std::string_view body, Close close) override;
 private:
  bool fail(ErrorKind, std::string);
  bool emit(const Event&);
  const descriptor::ValidatedDescriptor& descriptor_;
  Accumulator& accumulator_;
  SemanticLimits limits_;
};
Outcome typed(const sp::Outcome&);
}  // namespace sp::tokens
