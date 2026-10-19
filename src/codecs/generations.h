#pragma once
// Generation statistics (D7): what OpenRouter recorded about one generation, by the id its answer carried. The
// statistics appear some seconds after the generation; until then a get fails as `NotFound`.
#include "codecs/endpoint.h"
#include "core/value.h"
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sp::generations {
struct Get { std::string id; };
struct Result {
  std::string id;
  // The model and provider that served it, and the provider's own id for it.
  std::string model, provider, upstream_id;
  std::optional<std::uint64_t> created_at{};
  // The provider's own token counts (OpenRouter's `native_tokens_*`) and the cost, in `provider_cost`.
  Usage usage;
  // OpenRouter's normalized counts.
  std::optional<std::uint64_t> tokens_prompt{}, tokens_completion{};
  // Milliseconds to the first token, and of generation.
  std::optional<std::uint64_t> latency_ms{}, generation_ms{};
  std::string finish_reason, native_finish_reason;
  bool streamed = false, cancelled = false;
  std::shared_ptr<const json::Document> raw{};
  AttemptEvidence attempt{};
  std::vector<RawWire> raw_events;
};
using Outcome = std::variant<Result, Failure>;

using Encoded = endpoints::Encoded;
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Get&);

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
}  // namespace sp::generations
