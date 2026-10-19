#include "codecs/endpoint.h"
#include "codecs/batches.h"
#include "codecs/decisions.h"
#include "codecs/embeddings.h"
#include "codecs/files.h"
#include "codecs/generations.h"
#include "codecs/images.h"
#include "codecs/jobs.h"
#include "codecs/moderation.h"
#include "codecs/rerank.h"
#include "codecs/speech.h"
#include "codecs/tokens.h"
#include "codecs/transcription.h"

namespace sp::endpoints {
// RFC 3339 (`2026-10-19T13:02:22.150902Z`, or an offset) as Unix seconds.
std::optional<std::uint64_t> unix_seconds(std::string_view text) {
  const auto number = [&](std::size_t at, std::size_t digits) -> std::optional<int> {
    if (at + digits > text.size()) return std::nullopt;
    int value = 0;
    for (std::size_t i = at; i < at + digits; ++i) {
      if (text[i] < '0' || text[i] > '9') return std::nullopt;
      value = value * 10 + (text[i] - '0');
    }
    return value;
  };
  const auto year = number(0, 4), month = number(5, 2), day = number(8, 2), hour = number(11, 2), minute = number(14, 2), second = number(17, 2);
  if (!year || !month || !day || !hour || !minute || !second || text.size() < 20 || text[4] != '-' || text[7] != '-' ||
      (text[10] != 'T' && text[10] != 't') || text[13] != ':' || text[16] != ':' || *month < 1 || *month > 12 || *day < 1 ||
      *day > 31 || *hour > 23 || *minute > 59 || *second > 60 || *year < 1970)
    return std::nullopt;
  std::size_t at = 19;
  if (text[at] == '.') {
    ++at;
    const auto start = at;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at;
    if (at == start) return std::nullopt;
  }
  std::int64_t offset = 0;
  if (at < text.size() && (text[at] == 'Z' || text[at] == 'z')) ++at;
  else if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
    const auto hours = number(at + 1, 2), minutes = number(at + 4, 2);
    if (!hours || !minutes || at + 6 > text.size() || text[at + 3] != ':') return std::nullopt;
    offset = (text[at] == '+' ? 1 : -1) * (std::int64_t{*hours} * 3600 + std::int64_t{*minutes} * 60);
    at += 6;
  } else return std::nullopt;
  if (at != text.size()) return std::nullopt;
  // Days from the civil date (Howard Hinnant's algorithm).
  const int y = *year - (*month <= 2), era = y / 400, yoe = y - era * 400;
  const int doy = (153 * (*month + (*month > 2 ? -3 : 9)) + 2) / 5 + *day - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const std::int64_t days = static_cast<std::int64_t>(era) * 146097 + doe - 719468;
  const std::int64_t seconds = days * 86400 + std::int64_t{*hour} * 3600 + std::int64_t{*minute} * 60 + *second - offset;
  if (seconds < 0) return std::nullopt;
  return static_cast<std::uint64_t>(seconds);
}
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
  // Batch results are JSONL, whatever type the provider gives them.
  if (name == "content.document") return std::make_unique<jobs::ContentCodec>(accumulator, limits, true);
  if (name == "generations") return std::make_unique<generations::Codec>(descriptor, accumulator, limits);
  if (name == "tokens") return std::make_unique<tokens::Codec>(descriptor, accumulator, limits);
  if (name == "batches") return std::make_unique<batches::Codec>(descriptor, accumulator, batches::Codec::Kind::Batch, std::move(requested), limits);
  if (name == "batches.delete") return std::make_unique<batches::Codec>(descriptor, accumulator, batches::Codec::Kind::Deleted, std::move(requested), limits);
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
