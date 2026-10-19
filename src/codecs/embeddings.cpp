#include "codecs/embeddings.h"
#include "codecs/provider_cost.h"
#include "codecs/reason.h"
#include "codecs/served_model.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <algorithm>
#include <cmath>

namespace sp::embeddings {
namespace {
std::optional<Count> counter(json::Value value) {
  if (!value.is_uint()) return std::nullopt;
  return Count{value.as_uint(), Evidence::Reported};
}
// The vectors of a response: OpenAI and OpenRouter `data[].embedding` (by `index`), Google `embeddings[].values`.
std::vector<json::Value> vectors_of(json::Value root) {
  std::vector<std::pair<std::uint64_t, json::Value>> indexed;
  std::vector<json::Value> out;
  if (const auto data = root.get("data"); data.is_array()) {
    for (auto item : data.elements()) indexed.emplace_back(item.get("index").is_uint() ? item.get("index").as_uint() : indexed.size(), item.get("embedding"));
    std::stable_sort(indexed.begin(), indexed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [index, vector] : indexed) out.push_back(vector);
  } else if (const auto embeddings = root.get("embeddings"); embeddings.is_array()) {
    for (auto item : embeddings.elements()) out.push_back(item.get("values"));
  }
  return out;
}
}  // namespace

Codec::Codec(const descriptor::ValidatedDescriptor& descriptor, Accumulator& accumulator, std::string requested, SemanticLimits limits)
    : descriptor_(descriptor), accumulator_(accumulator), requested_(std::move(requested)), limits_(limits) {}
bool Codec::emit(const Event& event) { return accumulator_.accept(event); }
bool Codec::fail(ErrorKind kind, std::string text) {
  Error error{kind, std::move(text)};
  error.reason = reason::code(reason::Source::Embeddings, error.safe_message);
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
  if (!emit(RawWire{"embeddings.response", wire})) return false;
  if (root.get("error").valid() && !root.get("error").is_null()) return fail(ErrorKind::RemoteFailure, "remote error property");
  if (!root.get("data").is_array() && !root.get("embeddings").is_array()) return fail(ErrorKind::ProtocolCorrupt, "embedding vectors required");
  for (auto vector : vectors_of(root)) {
    if (!vector.is_array() || !vector.size()) return fail(ErrorKind::ProtocolCorrupt, "an embedding is an array of numbers");
    for (auto value : vector.elements())
      if (!value.is_number() || !std::isfinite(value.as_double())) return fail(ErrorKind::ProtocolCorrupt, "an embedding is an array of numbers");
  }
  const auto served = root.get("model");
  const std::string_view name = served.is_string() ? served.as_string() : std::string_view{};
  accumulator_.served_model({requested_, std::string(name), served::relation(descriptor_, requested_, name)});
  if (!emit(Begin{}) || !emit(MessageBegin{{0}, std::nullopt, Role::Assistant, nullptr})) return false;
  Usage usage;
  if (const auto reported = root.get("usage"); reported.is_object()) {
    usage.input_total = counter(reported.get("prompt_tokens"));
    usage.total = counter(reported.get("total_tokens"));
    if (const auto details = reported.get("prompt_tokens_details"); details.is_object())
      for (auto member : details.members())
        if (auto count = counter(member.value)) usage.extra["prompt_tokens_details." + std::string(member.key)] = *count;
    usage.provider_cost = codecs::provider_cost(reported, descriptor_);
    usage.stage = UsageStage::Final;
  } else if (const auto metadata = root.get("usageMetadata"); metadata.is_object()) {
    usage.input_total = counter(metadata.get("promptTokenCount"));
    usage.total = counter(metadata.get("totalTokenCount"));
    usage.stage = UsageStage::Final;
  }
  return emit(UsageUpdate{std::move(usage)}) && emit(Stop{StopReason{StopKind::EndTurn, "completed"}}) &&
         emit(MessageSeal{{0}}) && emit(Commit{"embeddings.vectors+normal_close"});
}

Outcome typed(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  Result result;
  for (const auto& raw : completion.raw_events) {
    if (raw.type != "embeddings.response" || !raw.payload) continue;
    for (auto vector : vectors_of(raw.payload->root())) {
      auto& out = result.vectors.emplace_back();
      out.reserve(vector.size());
      for (auto value : vector.elements()) out.push_back(static_cast<float>(value.as_double()));
    }
  }
  result.usage = completion.usage;
  result.model = completion.model;
  result.attempt = completion.attempt;
  result.raw_events = completion.raw_events;
  return result;
}
}  // namespace sp::embeddings
