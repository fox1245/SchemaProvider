#include "codecs/tokens.h"
#include "codecs/reason.h"
#include "descriptor/policy.h"
#include "json/json.h"

namespace sp::tokens {
namespace {
// A count Google gives as a number or as a string of digits.
std::optional<std::uint64_t> count_of(json::Value value) {
  if (value.is_uint()) return value.as_uint();
  if (!value.is_string() || value.as_string().empty() || value.as_string().size() > 19) return std::nullopt;
  std::uint64_t out = 0;
  for (const char c : value.as_string()) { if (c < '0' || c > '9') return std::nullopt; out = out * 10 + static_cast<std::uint64_t>(c - '0'); }
  return out;
}
}  // namespace

Codec::Codec(const descriptor::ValidatedDescriptor& descriptor, Accumulator& accumulator, SemanticLimits limits)
    : descriptor_(descriptor), accumulator_(accumulator), limits_(limits) {}
bool Codec::emit(const Event& event) { return accumulator_.accept(event); }
bool Codec::fail(ErrorKind kind, const reason::Reason& text) {
  Error error = reason::error(kind, text);
  emit(Fail{std::move(error)});
  return false;
}
bool Codec::buffered(std::string_view body, Close close) {
  if (!close.normal) return fail(close.error, reason::transport::response_close);
  auto parsed = json::parse(body, {limits_.max_content_bytes, limits_.max_json_depth});
  if (auto* error = std::get_if<json::ParseError>(&parsed))
    return fail(error->code == json::ParseCode::SizeExceeded || error->code == json::ParseCode::DepthExceeded
                    ? ErrorKind::ResourceLimit : ErrorKind::ProtocolCorrupt, reason::tokens::response_json_invalid);
  auto wire = std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
  const auto root = wire->root();
  if (!root.is_object()) return fail(ErrorKind::ProtocolCorrupt, reason::tokens::response_not_object);
  if (!emit(RawWire{"tokens.response", wire})) return false;
  // Anthropic and OpenAI say `input_tokens`; Google `totalTokens`, which it leaves out when it is zero.
  const auto input = root.get("input_tokens").valid() ? root.get("input_tokens") : root.get("totalTokens");
  if (!(input.valid() ? count_of(input).has_value() : root.get("promptTokensDetails").valid() || root.size() == 0))
    return fail(ErrorKind::ProtocolCorrupt, reason::tokens::count_invalid);
  Usage usage;
  usage.input_total = sp::Count{input.valid() ? *count_of(input) : 0, Evidence::Reported};
  usage.stage = UsageStage::Final;
  if (!emit(Begin{}) || !emit(MessageBegin{{0}, std::nullopt, Role::Assistant, nullptr})) return false;
  return emit(UsageUpdate{std::move(usage)}) && emit(Stop{StopReason{StopKind::EndTurn, "completed"}}) && emit(MessageSeal{{0}}) &&
         emit(Commit{"tokens.response+normal_close"});
}

Outcome typed(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  Result result;
  if (completion.usage.input_total) result.input = completion.usage.input_total->value;
  for (const auto& raw : completion.raw_events) {
    if (raw.type != "tokens.response" || !raw.payload) continue;
    const auto root = raw.payload->root();
    result.cached = count_of(root.get("cachedContentTokenCount"));
    if (const auto details = root.get("promptTokensDetails"); details.is_array())
      for (auto item : details.elements())
        if (item.get("modality").is_string()) result.modalities.emplace_back(std::string(item.get("modality").as_string()), count_of(item.get("tokenCount")).value_or(0));
  }
  result.attempt = completion.attempt;
  result.raw_events = completion.raw_events;
  return result;
}
}  // namespace sp::tokens
