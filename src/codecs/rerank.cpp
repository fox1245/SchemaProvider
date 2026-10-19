#include "codecs/rerank.h"
#include "codecs/provider_cost.h"
#include "codecs/reason.h"
#include "codecs/served_model.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <cmath>

namespace sp::rerank {
namespace {
std::optional<Count> counter(json::Value value) {
  if (!value.is_uint()) return std::nullopt;
  return Count{value.as_uint(), Evidence::Reported};
}
}  // namespace

Codec::Codec(const descriptor::ValidatedDescriptor& descriptor, Accumulator& accumulator, std::string requested, SemanticLimits limits)
    : descriptor_(descriptor), accumulator_(accumulator), requested_(std::move(requested)), limits_(limits) {}
bool Codec::emit(const Event& event) { return accumulator_.accept(event); }
bool Codec::fail(ErrorKind kind, std::string text) {
  Error error{kind, std::move(text)};
  error.reason = reason::code(reason::Source::Rerank, error.safe_message);
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
  if (!emit(RawWire{"rerank.response", wire})) return false;
  if (root.get("error").valid() && !root.get("error").is_null()) return fail(ErrorKind::RemoteFailure, "remote error property");
  const auto results = root.get("results");
  if (!results.is_array()) return fail(ErrorKind::ProtocolCorrupt, "rerank results required");
  for (auto item : results.elements())
    if (!item.is_object() || !item.get("index").is_uint() || !item.get("relevance_score").is_number() || !std::isfinite(item.get("relevance_score").as_double()))
      return fail(ErrorKind::ProtocolCorrupt, "a rerank result has an index and a score");
  const auto served = root.get("model");
  const std::string_view name = served.is_string() ? served.as_string() : std::string_view{};
  accumulator_.served_model({requested_, std::string(name), served::relation(descriptor_, requested_, name)});
  const auto id = root.get("id");
  if (!emit(Begin{id.is_string() ? std::string(id.as_string()) : std::string()}) || !emit(MessageBegin{{0}, std::nullopt, Role::Assistant, nullptr})) return false;
  Usage usage;
  if (const auto reported = root.get("usage"); reported.is_object()) {
    usage.total = counter(reported.get("total_tokens"));
    if (auto units = counter(reported.get("search_units"))) usage.extra["search_units"] = *units;
    usage.provider_cost = codecs::provider_cost(reported, descriptor_);
    usage.stage = UsageStage::Final;
  }
  return emit(UsageUpdate{std::move(usage)}) && emit(Stop{StopReason{StopKind::EndTurn, "completed"}}) &&
         emit(MessageSeal{{0}}) && emit(Commit{"rerank.results+normal_close"});
}

Outcome typed(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  Result result;
  for (const auto& raw : completion.raw_events) {
    if (raw.type != "rerank.response" || !raw.payload) continue;
    const auto root = raw.payload->root();
    for (auto item : root.get("results").elements())
      result.ranked.push_back({item.get("index").as_uint(), item.get("relevance_score").as_double()});
    if (root.get("id").is_string()) result.id = std::string(root.get("id").as_string());
    if (root.get("provider").is_string()) result.provider = std::string(root.get("provider").as_string());
  }
  result.usage = completion.usage;
  result.model = completion.model;
  result.attempt = completion.attempt;
  result.raw_events = completion.raw_events;
  return result;
}
}  // namespace sp::rerank
