#include "codecs/transcription.h"
#include "codecs/provider_cost.h"
#include "codecs/reason.h"
#include "codecs/served_model.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <cmath>

namespace sp::transcription {
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
  error.reason = reason::code(reason::Source::Transcription, error.safe_message);
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
  if (!emit(RawWire{"transcription.response", wire})) return false;
  if (root.get("error").valid() && !root.get("error").is_null()) return fail(ErrorKind::RemoteFailure, "remote error property");
  if (!root.get("text").is_string()) return fail(ErrorKind::ProtocolCorrupt, "transcript text required");
  for (const char* key : {"duration", "seconds"})
    if (root.get(key).valid() && (!root.get(key).is_number() || !std::isfinite(root.get(key).as_double())))
      return fail(ErrorKind::ProtocolCorrupt, "an audio duration is a number");
  accumulator_.served_model({requested_, {}, ServedRelation::NotReported});
  if (!emit(Begin{}) || !emit(MessageBegin{{0}, std::nullopt, Role::Assistant, nullptr})) return false;
  const LocalId local{1};
  if (!emit(PartBegin{{0}, local, PartKind::Text, {}, 0}) || !emit(PartDelta{local, {PartKind::Text, root.get("text").as_string()}}) ||
      !emit(PartSeal{local, {}})) return false;
  Usage usage;
  if (const auto reported = root.get("usage"); reported.is_object()) {
    usage.input_total = counter(reported.get("input_tokens"));
    usage.output_total = counter(reported.get("output_tokens"));
    usage.total = counter(reported.get("total_tokens"));
    usage.provider_cost = codecs::provider_cost(reported, descriptor_);
    usage.stage = UsageStage::Final;
  }
  return emit(UsageUpdate{std::move(usage)}) && emit(Stop{StopReason{StopKind::EndTurn, "completed"}}) &&
         emit(MessageSeal{{0}}) && emit(Commit{"transcription.text+normal_close"});
}

Outcome typed(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  Result result;
  result.text = completion.joined_text();
  for (const auto& raw : completion.raw_events) {
    if (raw.type != "transcription.response" || !raw.payload) continue;
    const auto root = raw.payload->root();
    if (root.get("language").is_string()) result.language = std::string(root.get("language").as_string());
    // OpenAI reports the length as `usage.seconds` (or `duration` in verbose form), OpenRouter as `duration`.
    for (const auto value : {root.get("duration"), root.get("usage").get("seconds")})
      if (value.is_number() && !result.seconds) result.seconds = value.as_double();
  }
  result.usage = completion.usage;
  result.model = completion.model;
  result.attempt = completion.attempt;
  result.raw_events = completion.raw_events;
  return result;
}
}  // namespace sp::transcription
