#include "codecs/images.h"
#include "codecs/provider_cost.h"
#include "codecs/reason.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <algorithm>

namespace sp::images {
namespace {
// A token the provider defines (`1024x1024`, `high`, `16:9`): bounded, printable, no separators.
bool token(std::string_view value) {
  return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ':' || c == '.' || c == '_' || c == '-';
  });
}
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
  error.reason = reason::code(reason::Source::Images, error.safe_message);
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
  if (!emit(RawWire{"images.response", wire})) return false;
  if (root.get("error").valid() && !root.get("error").is_null()) return fail(ErrorKind::RemoteFailure, "remote error property");
  const auto data = root.get("data");
  if (!data.is_array()) return fail(ErrorKind::ProtocolCorrupt, "image data required");
  if (data.size() >= limits_.max_parts) return fail(ErrorKind::ResourceLimit, "image count limit");
  // Neither provider names the model in an image response.
  accumulator_.served_model({requested_, {}, ServedRelation::NotReported});
  if (!emit(Begin{}) || !emit(MessageBegin{{0}, std::nullopt, Role::Assistant, nullptr})) return false;
  // OpenAI names the format once for every image, OpenRouter on each image.
  const auto format = root.get("output_format");
  const std::string shared_mime = format.is_string() && token(format.as_string()) ? "image/" + std::string(format.as_string()) : std::string();
  std::uint32_t index = 0;
  for (auto item : data.elements()) {
    if (!item.is_object()) return fail(ErrorKind::ProtocolCorrupt, "image item must be object");
    const auto b64 = item.get("b64_json"), url = item.get("url"), type = item.get("media_type");
    PartHeader header;
    header.media.kind = MediaKind::Image;
    header.media.mime = type.is_string() ? std::string(type.as_string()) : shared_mime;
    const bool inline_image = b64.is_string() && !b64.as_string().empty();
    if (inline_image) header.media.source = MediaSource::Inline;
    else if (url.is_string() && !url.as_string().empty()) { header.media.source = MediaSource::Url; header.media.reference = std::string(url.as_string()); }
    else return fail(ErrorKind::ProtocolCorrupt, "image item has no data");
    const LocalId local{++index};
    if (!emit(PartBegin{{0}, local, PartKind::Media, std::move(header), index - 1})) return false;
    if (inline_image && !emit(PartDelta{local, {PartKind::Media, b64.as_string()}})) return false;
    if (!emit(PartSeal{local, {}})) return false;
  }
  Usage usage;
  if (const auto reported = root.get("usage"); reported.is_object()) {
    // OpenAI counts input and output tokens; OpenRouter prompt and completion tokens.
    const bool openai = reported.get("input_tokens").valid();
    usage.input_total = counter(reported.get(openai ? "input_tokens" : "prompt_tokens"));
    usage.output_total = counter(reported.get(openai ? "output_tokens" : "completion_tokens"));
    usage.total = counter(reported.get("total_tokens"));
    for (const char* key : {"input_tokens_details", "output_tokens_details", "prompt_tokens_details", "completion_tokens_details"})
      if (const auto details = reported.get(key); details.is_object())
        for (auto member : details.members())
          if (auto count = counter(member.value)) usage.extra[std::string(key) + "." + std::string(member.key)] = *count;
    usage.provider_cost = codecs::provider_cost(reported, descriptor_);
    usage.stage = UsageStage::Final;
  }
  return emit(UsageUpdate{std::move(usage)}) && emit(Stop{StopReason{StopKind::EndTurn, "completed"}}) &&
         emit(MessageSeal{{0}}) && emit(Commit{"images.data+normal_close"});
}

Outcome typed(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  Result result;
  for (const auto& message : completion.messages)
    for (const auto& part : message.parts)
      if (const auto* media = std::get_if<Media>(&part)) result.images.push_back(*media);
  result.revised_prompts.assign(result.images.size(), {});
  for (const auto& raw : completion.raw_events) {
    if (raw.type != "images.response" || !raw.payload) continue;
    const auto root = raw.payload->root();
    if (root.get("created").is_uint()) result.created = root.get("created").as_uint();
    std::size_t index = 0;
    for (auto item : root.get("data").elements()) {
      if (index < result.revised_prompts.size() && item.get("revised_prompt").is_string())
        result.revised_prompts[index] = std::string(item.get("revised_prompt").as_string());
      ++index;
    }
  }
  result.usage = completion.usage;
  result.model = completion.model;
  result.attempt = completion.attempt;
  result.raw_events = completion.raw_events;
  return result;
}
}  // namespace sp::images
