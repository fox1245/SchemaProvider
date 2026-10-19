#include "codecs/generations.h"
#include "codecs/provider_cost.h"
#include "codecs/reason.h"
#include "descriptor/policy.h"
#include "json/json.h"

namespace sp::generations {
namespace {
std::string text_of(json::Value value) { return value.is_string() ? std::string(value.as_string()) : std::string(); }
std::optional<std::uint64_t> number_of(json::Value value) { return value.is_uint() ? std::optional<std::uint64_t>(value.as_uint()) : std::nullopt; }
std::optional<Count> counter(json::Value value) {
  if (!value.is_uint()) return std::nullopt;
  return Count{value.as_uint(), Evidence::Reported};
}
}  // namespace

Codec::Codec(const descriptor::ValidatedDescriptor& descriptor, Accumulator& accumulator, SemanticLimits limits)
    : descriptor_(descriptor), accumulator_(accumulator), limits_(limits) {}
bool Codec::emit(const Event& event) { return accumulator_.accept(event); }
bool Codec::fail(ErrorKind kind, std::string text) {
  Error error{kind, std::move(text)};
  error.reason = reason::code(reason::Source::Generations, error.safe_message);
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
  const auto data = wire->root().get("data");
  if (!data.is_object() || !data.get("id").is_string()) return fail(ErrorKind::ProtocolCorrupt, "generation statistics name the generation");
  for (const char* key : {"native_tokens_prompt", "native_tokens_completion", "native_tokens_reasoning", "native_tokens_cached", "tokens_prompt", "tokens_completion"})
    if (data.get(key).valid() && !data.get(key).is_null() && !data.get(key).is_uint()) return fail(ErrorKind::ProtocolCorrupt, "generation token counts are numbers");
  if (!emit(RawWire{"generations.response", wire})) return false;
  Usage usage;
  usage.input_total = counter(data.get("native_tokens_prompt"));
  usage.output_total = counter(data.get("native_tokens_completion"));
  usage.reasoning = counter(data.get("native_tokens_reasoning"));
  usage.cache_read = counter(data.get("native_tokens_cached"));
  // The cost is in US dollars at OpenRouter's origins, as in its chat usage.
  const bool usd = descriptor::contains(descriptor_.family_policy().openrouter_origins, descriptor_.base_url());
  usage.provider_cost.source = usd ? CostSource::OpenRouterUsd : CostSource::UnknownCurrency;
  usage.provider_cost.status[0] = codecs::usd_amount(data.get("total_cost"), usd, usage.provider_cost.total);
  usage.provider_cost.status[1] = codecs::usd_amount(data.get("upstream_inference_cost"), usd, usage.provider_cost.upstream_total);
  if (data.get("is_byok").is_bool()) { usage.provider_cost.is_byok = data.get("is_byok").as_bool(); usage.provider_cost.byok_status = CostStatus::Available; }
  usage.stage = UsageStage::Final;
  accumulator_.served_model({{}, text_of(data.get("model")), ServedRelation::NotReported});
  if (!emit(Begin{text_of(data.get("id"))}) || !emit(MessageBegin{{0}, text_of(data.get("id")), Role::Assistant, nullptr})) return false;
  return emit(UsageUpdate{std::move(usage)}) && emit(Stop{StopReason{StopKind::EndTurn, "completed"}}) && emit(MessageSeal{{0}}) &&
         emit(Commit{"generations.response+normal_close"});
}

Outcome typed(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  Result result;
  for (const auto& raw : completion.raw_events) {
    if (raw.type != "generations.response" || !raw.payload) continue;
    const auto data = raw.payload->root().get("data");
    result.id = text_of(data.get("id"));
    result.model = text_of(data.get("model"));
    result.provider = text_of(data.get("provider_name"));
    result.upstream_id = text_of(data.get("upstream_id"));
    if (data.get("created_at").is_string()) result.created_at = endpoints::unix_seconds(data.get("created_at").as_string());
    result.tokens_prompt = number_of(data.get("tokens_prompt"));
    result.tokens_completion = number_of(data.get("tokens_completion"));
    result.latency_ms = number_of(data.get("latency"));
    result.generation_ms = number_of(data.get("generation_time"));
    result.finish_reason = text_of(data.get("finish_reason"));
    result.native_finish_reason = text_of(data.get("native_finish_reason"));
    result.streamed = data.get("streamed").is_bool() && data.get("streamed").as_bool();
    result.cancelled = data.get("cancelled").is_bool() && data.get("cancelled").as_bool();
    result.raw = raw.payload;
  }
  result.usage = completion.usage;
  result.attempt = completion.attempt;
  result.raw_events = completion.raw_events;
  return result;
}
}  // namespace sp::generations
