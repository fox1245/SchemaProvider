#include "codecs/endpoint.h"
#include "codecs/decisions.h"
#include "codecs/embeddings.h"
#include "codecs/files.h"
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
  // A file, a page of files, or a deletion; `requested` is the file id the call named.
  if (name == "files") return std::make_unique<files::Codec>(descriptor, accumulator, files::Codec::Kind::File, std::move(requested), limits);
  if (name == "files.list") return std::make_unique<files::Codec>(descriptor, accumulator, files::Codec::Kind::Page, std::move(requested), limits);
  // A step of a chunked upload; `requested` is the session it advances.
  using Step = files::SessionCodec::Step;
  for (const auto& [step_name, step] : {std::pair{"files.start", Step::Start}, std::pair{"files.part", Step::Part},
                                        std::pair{"files.complete", Step::Complete}, std::pair{"files.query", Step::Query}, std::pair{"files.cancel", Step::Cancel}})
    if (name == step_name) return std::make_unique<files::SessionCodec>(descriptor, accumulator, step, std::move(requested), limits);
  if (name == "files.delete") return std::make_unique<files::Codec>(descriptor, accumulator, files::Codec::Kind::Deleted, std::move(requested), limits);
  return nullptr;
}
}  // namespace sp::endpoints
