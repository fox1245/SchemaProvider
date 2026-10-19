#include "codecs/decisions.h"
#include "codecs/provider_cost.h"
#include "codecs/reason.h"
#include "codecs/served_model.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <cmath>

namespace sp::decisions {
namespace {
std::optional<Count> counter(json::Value value) {
  if (!value.is_uint()) return std::nullopt;
  return Count{value.as_uint(), Evidence::Reported};
}
bool number(json::Value value) { return value.is_number() && std::isfinite(value.as_double()); }
// A choice value is a string, or (OpenAI) a boolean.
std::optional<std::string> value_text(json::Value value) {
  if (value.is_string()) return std::string(value.as_string());
  if (value.is_bool()) return std::string(value.as_bool() ? "true" : "false");
  return std::nullopt;
}
}  // namespace

Codec::Codec(const descriptor::ValidatedDescriptor& descriptor, Accumulator& accumulator, std::string requested, SemanticLimits limits)
    : descriptor_(descriptor), accumulator_(accumulator), requested_(std::move(requested)), limits_(limits) {}
bool Codec::emit(const Event& event) { return accumulator_.accept(event); }
bool Codec::fail(ErrorKind kind, std::string text) {
  Error error{kind, std::move(text)};
  error.reason = reason::code(reason::Source::Decisions, error.safe_message);
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
  if (!emit(RawWire{"decisions.response", wire})) return false;
  if (root.get("error").valid() && !root.get("error").is_null()) return fail(ErrorKind::RemoteFailure, "remote error property");
  // OpenAI lists answers in question order; OpenRouter keys them by question name.
  const auto answers = root.get("answers");
  if (!answers.is_array() && !answers.is_object()) return fail(ErrorKind::ProtocolCorrupt, "decision answers required");
  if (answers.size() >= limits_.max_parts) return fail(ErrorKind::ResourceLimit, "decision answer count limit");
  const auto served = root.get("model");
  if (served.valid() && !served.is_string()) return fail(ErrorKind::ProtocolCorrupt, "decision model must be string");
  const std::string_view served_name = served.is_string() ? served.as_string() : std::string_view{};
  accumulator_.served_model({requested_, std::string(served_name), served::relation(descriptor_, requested_, served_name)});
  const auto id = root.get("id");
  if (!emit(Begin{id.is_string() ? std::string(id.as_string()) : std::string()}) ||
      !emit(MessageBegin{{0}, id.is_string() ? std::optional<std::string>(std::string(id.as_string())) : std::nullopt, Role::Assistant, nullptr}))
    return false;
  std::uint32_t index = 0;
  auto answer = [&](json::Value item, std::optional<std::string_view> key) {
    if (!item.is_object() || !item.get("type").is_string() || item.get("type").as_string().empty())
      return fail(ErrorKind::ProtocolCorrupt, "decision answer needs a type");
    const auto type = item.get("type").as_string();
    for (const char* member : {"probability", "noul", "score", "confidence"})
      if (item.get(member).valid() && !item.get(member).is_null() && !number(item.get(member)))
        return fail(ErrorKind::ProtocolCorrupt, "decision probabilities and scores are numbers");
    // An OpenRouter answer carries its question's name, which keys it, as `name`.
    std::shared_ptr<const json::Document> metadata;
    if (key) {
      std::string text = "{\"name\":" + json::quote(*key);
      for (auto member : item.members()) if (member.key != "name") text += "," + json::quote(member.key) + ":" + member.value.dump();
      auto rebuilt = json::parse(text + "}", {limits_.max_content_bytes, limits_.max_json_depth});
      if (!std::holds_alternative<json::Document>(rebuilt)) return fail(ErrorKind::ResourceLimit, "decision answer limit");
      metadata = std::make_shared<const json::Document>(std::move(std::get<json::Document>(rebuilt)));
    } else {
      auto copied = json::parse(item.dump(), {limits_.max_content_bytes, limits_.max_json_depth});
      if (!std::holds_alternative<json::Document>(copied)) return fail(ErrorKind::ResourceLimit, "decision answer limit");
      metadata = std::make_shared<const json::Document>(std::move(std::get<json::Document>(copied)));
    }
    PartHeader header;
    header.wire_type = std::string(type);
    header.wire_metadata = std::move(metadata);
    const LocalId local{++index};
    return emit(PartBegin{{0}, local, PartKind::Opaque, std::move(header), index - 1}) && emit(PartSeal{local, {}});
  };
  if (answers.is_array()) { for (auto item : answers.elements()) if (!answer(item, std::nullopt)) return false; }
  else for (auto member : answers.members()) if (!answer(member.value, member.key)) return false;
  Usage usage;
  if (const auto reported = root.get("usage"); reported.is_object()) {
    usage.input_total = counter(reported.get("input_tokens"));
    usage.output_total = counter(reported.get("output_tokens"));
    usage.total = counter(reported.get("total_tokens"));
    const auto input = reported.get("input_tokens_details"), output = reported.get("output_tokens_details");
    usage.cache_read = counter(input.get("cached_tokens"));
    usage.cache_write = counter(input.get("cache_write_tokens"));
    usage.reasoning = counter(output.get("reasoning_tokens"));
    usage.provider_cost = codecs::provider_cost(reported, descriptor_);
    usage.stage = UsageStage::Final;
  }
  return emit(UsageUpdate{std::move(usage)}) && emit(Stop{StopReason{StopKind::EndTurn, "completed"}}) &&
         emit(MessageSeal{{0}}) && emit(Commit{"decisions.answers+normal_close"});
}

Outcome typed(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  Result result;
  for (const auto& message : completion.messages)
    for (const auto& part : message.parts) {
      const auto* opaque = std::get_if<Opaque>(&part);
      if (!opaque || !opaque->wire_metadata) continue;
      const auto item = opaque->wire_metadata->root();
      Answer answer;
      answer.raw = opaque->wire_metadata;
      if (item.get("name").is_string()) answer.name = std::string(item.get("name").as_string());
      const auto& type = opaque->wire_type;
      if (type == "predicate" || type == "noul") answer.kind = Kind::Predicate;
      else if (type == "choice") answer.kind = Kind::Choice;
      else if (type == "score") answer.kind = Kind::Score;
      else if (type == "box") answer.kind = Kind::Box;
      answer.refused = type == "refusal";
      const auto value = [&](const char* key) { const auto v = item.get(key); return v.is_number() ? std::optional<double>(v.as_double()) : std::nullopt; };
      answer.probability = value(type == "noul" ? "noul" : "probability");
      answer.score = value("score");
      answer.confidence = value("confidence");
      if (auto choice = value_text(item.get("choice"))) answer.choice = std::move(*choice);
      // OpenAI lists {value, probability} (with a score level's label); OpenRouter maps each value to its probability.
      const auto probabilities = item.get("probabilities");
      if (probabilities.is_array()) {
        for (auto entry : probabilities.elements()) {
          if (!entry.get("probability").is_number()) continue;
          auto label = entry.get("label").is_string() ? std::optional<std::string>(std::string(entry.get("label").as_string())) : value_text(entry.get("value"));
          if (!label && entry.get("value").is_number()) label = entry.get("value").dump();
          if (label) answer.probabilities.emplace_back(std::move(*label), entry.get("probability").as_double());
        }
      } else if (probabilities.is_object()) {
        for (auto entry : probabilities.members())
          if (entry.value.is_number()) answer.probabilities.emplace_back(std::string(entry.key), entry.value.as_double());
      }
      result.answers.push_back(std::move(answer));
    }
  for (const auto& raw : completion.raw_events) {
    if (raw.type != "decisions.response" || !raw.payload) continue;
    const auto root = raw.payload->root();
    if (root.get("id").is_string()) result.id = std::string(root.get("id").as_string());
    if (root.get("provider").is_string()) result.provider = std::string(root.get("provider").as_string());
  }
  result.usage = completion.usage;
  result.model = completion.model;
  result.attempt = completion.attempt;
  result.raw_events = completion.raw_events;
  return result;
}
}  // namespace sp::decisions
