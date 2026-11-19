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
#include "descriptor/catalogue.h"
#include <algorithm>

namespace sp::endpoints {
std::variant<std::optional<std::pair<std::string, std::string>>, Error> credential(std::string_view scheme, std::string_view key) {
  if (key.empty()) return std::optional<std::pair<std::string, std::string>>{};
  auto header = descriptor::credential_header(scheme, key);
  if (!header) {
    Error refused{ErrorKind::InvalidConfig, "the credential scheme names no header"};
    refused.reason = "credential.scheme.no_header";
    return refused;
  }
  return header;
}
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
  // The days of the month, February by the Gregorian leap rule.
  const auto month_days = [](int y, int m) {
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 ? (leap ? 29 : 28) : (m == 4 || m == 6 || m == 9 || m == 11) ? 30 : 31;
  };
  if (!year || !month || !day || !hour || !minute || !second || text.size() < 20 || text[4] != '-' || text[7] != '-' ||
      (text[10] != 'T' && text[10] != 't') || text[13] != ':' || text[16] != ':' || *month < 1 || *month > 12 || *day < 1 ||
      *day > month_days(*year, *month) || *hour > 23 || *minute > 59 || *second > 60 || *year < 1970)
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
                                  std::string requested, const Context& context, SemanticLimits limits) {
  if (name == "images") return std::make_unique<images::Codec>(descriptor, accumulator, std::move(requested), limits);
  if (name == "decisions") return std::make_unique<decisions::Codec>(descriptor, accumulator, std::move(requested), limits);
  if (name == "embeddings") return std::make_unique<embeddings::Codec>(descriptor, accumulator, std::move(requested), limits, context.inputs);
  if (name == "rerank") return std::make_unique<rerank::Codec>(descriptor, accumulator, std::move(requested), limits, context.inputs, context.results);
  if (name == "moderation") return std::make_unique<moderation::Codec>(descriptor, accumulator, std::move(requested), limits);
  if (name == "speech") return std::make_unique<speech::Codec>(descriptor, accumulator, std::move(requested), limits);
  if (name == "transcription") return std::make_unique<transcription::Codec>(descriptor, accumulator, std::move(requested), limits);
  // A job's state, from a submit or a poll, and one output it made.
  if (name == "videos") return std::make_unique<jobs::StateCodec>(descriptor, accumulator, std::move(requested), std::string(name), limits, context.job);
  if (name == "content") return std::make_unique<jobs::ContentCodec>(accumulator, limits);
  // A file's content and batch results are documents: a text or JSON body is the content, whatever type the provider gives it.
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
    if (name == step_name) return std::make_unique<files::SessionCodec>(descriptor, accumulator, step, std::move(requested), limits, context.secret);
  if (name == "files.delete") return std::make_unique<files::Codec>(descriptor, accumulator, files::Codec::Kind::Deleted, std::move(requested), limits);
  return nullptr;
}
EndpointFacts call_facts(const descriptor::ValidatedDescriptor& descriptor, std::string_view fact, std::string_view operation, std::string_view model) {
  EndpointFacts facts;
  const auto& policy = *descriptor.policy();
  facts.endpoint = std::string(fact);
  facts.operation = std::string(operation);
  facts.billing = policy.endpoint_billing(fact, descriptor.base_url(), operation);
  if (facts.billing == descriptor::EndpointBilling::Carried) facts.carried_tier = std::string(policy.carried_tier(fact, descriptor.base_url()));
  facts.family = std::string(descriptor.family());
  facts.origin = std::string(descriptor.base_url());
  facts.model = std::string(model);
  facts.routed_gateway = descriptor::routed_gateway(descriptor);
  return facts;
}
std::vector<std::pair<std::string, std::string>> dimensions(
    std::string_view operation, std::initializer_list<std::pair<std::string_view, std::optional<std::string>>> typed,
    const std::shared_ptr<const json::Document>& extensions) {
  std::vector<std::pair<std::string, std::string>> out;
  // A member whose value the grammar cannot hold is offered without one: no row matches it, and a row that names its
  // key leaves the unit unpriced. A key outside the grammar cannot be named by a row.
  const auto offer = [&](std::string_view key, std::string_view value) {
    if (!descriptor::valid_dimension_key(key)) return;
    out.emplace_back(std::string(key), descriptor::valid_dimension_value(value) ? std::string(value) : std::string());
  };
  // The call's own operation, so a row qualified by an operation prices only that operation's calls.
  if (!operation.empty()) offer("operation", operation);
  for (const auto& [key, value] : typed)
    if (value) offer(key, *value);
  if (extensions && extensions->root().is_object())
    for (auto member : extensions->root().members()) {
      if (member.value.is_string()) offer(member.key, member.value.as_string());
      else if (member.value.is_bool()) offer(member.key, *boolean(member.value.as_bool()));
      else if (member.value.is_uint()) offer(member.key, *decimal(member.value.as_uint()));
      else offer(member.key, {});  // a number with a fraction or a sign, null, an array, an object
    }
  // Stable, so a key offered twice stays twice and the bound sees it.
  std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  return out;
}
std::optional<std::string> decimal(std::optional<std::uint64_t> value) {
  if (!value) return std::nullopt;
  return std::to_string(*value);
}
std::optional<std::string> boolean(std::optional<bool> value) {
  if (!value) return std::nullopt;
  return std::string(*value ? "true" : "false");
}
std::optional<std::string> unstated(bool set) {
  if (!set) return std::nullopt;
  return std::string();
}
EndpointQuantity requested(std::string unit, std::optional<std::uint64_t> count, bool exact) {
  EndpointQuantity quantity;
  quantity.unit = std::move(unit);
  quantity.at_most = count;
  quantity.exact = exact && count.has_value();
  return quantity;
}
EndpointQuantity input_limited(std::string unit, std::uint64_t per) {
  EndpointQuantity quantity;
  quantity.unit = std::move(unit);
  quantity.source = EndpointQuantity::Source::InputLimit;
  quantity.per = per;
  return quantity;
}
EndpointQuantity output_limited() {
  EndpointQuantity quantity;
  quantity.unit = "output";
  quantity.source = EndpointQuantity::Source::OutputLimit;
  return quantity;
}
}  // namespace sp::endpoints
