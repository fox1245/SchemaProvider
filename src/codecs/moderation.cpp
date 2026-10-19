#include "codecs/moderation.h"
#include "codecs/reason.h"
#include "codecs/served_model.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <cmath>

namespace sp::moderation {
Codec::Codec(const descriptor::ValidatedDescriptor& descriptor, Accumulator& accumulator, std::string requested, SemanticLimits limits)
    : descriptor_(descriptor), accumulator_(accumulator), requested_(std::move(requested)), limits_(limits) {}
bool Codec::emit(const Event& event) { return accumulator_.accept(event); }
bool Codec::fail(ErrorKind kind, std::string text) {
  Error error{kind, std::move(text)};
  error.reason = reason::code(reason::Source::Moderation, error.safe_message);
  emit(Fail{std::move(error)});
  return false;
}
bool Codec::buffered(std::string_view body, Close close) {
  if (!close.normal) return fail(close.error, "abnormal response close");
  auto parsed = json::parse(body, {limits_.max_content_bytes, limits_.max_json_depth});
  if (auto* error = std::get_if<json::ParseError>(&parsed))
    return fail(error->code == json::ParseCode::SizeExceeded || error->code == json::ParseCode::DepthExceeded
                    ? ErrorKind::ResourceLimit : ErrorKind::ProtocolCorrupt, "invalid response JSON");
  auto wire = std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
  const auto root = wire->root();
  if (!root.is_object()) return fail(ErrorKind::ProtocolCorrupt, "response must be object");
  if (!emit(RawWire{"moderation.response", wire})) return false;
  if (root.get("error").valid() && !root.get("error").is_null()) return fail(ErrorKind::RemoteFailure, "remote error property");
  const auto results = root.get("results");
  if (!results.is_array()) return fail(ErrorKind::ProtocolCorrupt, "moderation results required");
  for (auto item : results.elements()) {
    if (!item.is_object() || !item.get("flagged").is_bool()) return fail(ErrorKind::ProtocolCorrupt, "a moderation result says whether it is flagged");
    for (auto member : item.get("categories").members())
      if (!member.value.is_bool()) return fail(ErrorKind::ProtocolCorrupt, "moderation categories are booleans");
    for (auto member : item.get("category_scores").members())
      if (!member.value.is_number() || !std::isfinite(member.value.as_double())) return fail(ErrorKind::ProtocolCorrupt, "moderation scores are numbers");
  }
  const auto served = root.get("model");
  const std::string_view name = served.is_string() ? served.as_string() : std::string_view{};
  accumulator_.served_model({requested_, std::string(name), served::relation(descriptor_, requested_, name)});
  const auto id = root.get("id");
  if (!emit(Begin{id.is_string() ? std::string(id.as_string()) : std::string()}) || !emit(MessageBegin{{0}, std::nullopt, Role::Assistant, nullptr})) return false;
  return emit(UsageUpdate{Usage{}}) && emit(Stop{StopReason{StopKind::EndTurn, "completed"}}) &&
         emit(MessageSeal{{0}}) && emit(Commit{"moderation.results+normal_close"});
}

Outcome typed(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  Result result;
  for (const auto& raw : completion.raw_events) {
    if (raw.type != "moderation.response" || !raw.payload) continue;
    const auto root = raw.payload->root();
    for (auto item : root.get("results").elements()) {
      auto& verdict = result.verdicts.emplace_back();
      verdict.flagged = item.get("flagged").as_bool();
      for (auto member : item.get("categories").members()) verdict.categories.emplace_back(std::string(member.key), member.value.as_bool());
      for (auto member : item.get("category_scores").members()) verdict.scores.emplace_back(std::string(member.key), member.value.as_double());
    }
    if (root.get("id").is_string()) result.id = std::string(root.get("id").as_string());
  }
  result.usage = completion.usage;
  result.model = completion.model;
  result.attempt = completion.attempt;
  result.raw_events = completion.raw_events;
  return result;
}
}  // namespace sp::moderation
