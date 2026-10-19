#include "codecs/endpoint.h"
#include "codecs/decisions.h"
#include "codecs/embeddings.h"
#include "codecs/images.h"
#include "codecs/moderation.h"
#include "codecs/rerank.h"

namespace sp::endpoints {
std::unique_ptr<Codec> make_codec(std::string_view name, const descriptor::ValidatedDescriptor& descriptor, Accumulator& accumulator,
                                  std::string requested, SemanticLimits limits) {
  if (name == "images") return std::make_unique<images::Codec>(descriptor, accumulator, std::move(requested), limits);
  if (name == "decisions") return std::make_unique<decisions::Codec>(descriptor, accumulator, std::move(requested), limits);
  if (name == "embeddings") return std::make_unique<embeddings::Codec>(descriptor, accumulator, std::move(requested), limits);
  if (name == "rerank") return std::make_unique<rerank::Codec>(descriptor, accumulator, std::move(requested), limits);
  if (name == "moderation") return std::make_unique<moderation::Codec>(descriptor, accumulator, std::move(requested), limits);
  return nullptr;
}
}  // namespace sp::endpoints
