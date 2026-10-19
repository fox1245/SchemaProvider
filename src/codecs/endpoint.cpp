#include "codecs/endpoint.h"
#include "codecs/decisions.h"
#include "codecs/embeddings.h"
#include "codecs/images.h"
#include "codecs/jobs.h"
#include "codecs/moderation.h"
#include "codecs/rerank.h"
#include "codecs/speech.h"
#include "codecs/transcription.h"

namespace sp::endpoints {
std::unique_ptr<Codec> make_codec(std::string_view name, const descriptor::ValidatedDescriptor& descriptor, Accumulator& accumulator,
                                  std::string requested, SemanticLimits limits) {
  if (name == "images") return std::make_unique<images::Codec>(descriptor, accumulator, std::move(requested), limits);
  if (name == "decisions") return std::make_unique<decisions::Codec>(descriptor, accumulator, std::move(requested), limits);
  if (name == "embeddings") return std::make_unique<embeddings::Codec>(descriptor, accumulator, std::move(requested), limits);
  if (name == "rerank") return std::make_unique<rerank::Codec>(descriptor, accumulator, std::move(requested), limits);
  if (name == "moderation") return std::make_unique<moderation::Codec>(descriptor, accumulator, std::move(requested), limits);
  if (name == "speech") return std::make_unique<speech::Codec>(descriptor, accumulator, std::move(requested), limits);
  if (name == "transcription") return std::make_unique<transcription::Codec>(descriptor, accumulator, std::move(requested), limits);
  // A job's state, from a submit or a poll, and one output it made.
  if (name == "videos") return std::make_unique<jobs::StateCodec>(descriptor, accumulator, std::move(requested), std::string(name), limits);
  if (name == "content") return std::make_unique<jobs::ContentCodec>(accumulator, limits);
  return nullptr;
}
}  // namespace sp::endpoints
