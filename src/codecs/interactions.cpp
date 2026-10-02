#include "codecs/interactions.h"
#include "core/native.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include <algorithm>
#include <limits>

namespace sp::interactions {
namespace {
bool unsigned_count(json::Value v) { return v.is_uint() || (v.is_int() && v.as_int() >= 0); }
uint64_t number(json::Value v) { return v.is_uint() ? v.as_uint() : static_cast<uint64_t>(v.as_int()); }
bool nonempty(json::Value v) { return v.is_string() && !v.as_string().empty(); }
bool prefix(std::string_view original, std::string_view final) { return final.starts_with(original); }
}
Codec::Codec(const descriptor::ValidatedDescriptor& descriptor, Mode mode, Accumulator& accumulator,
             std::shared_ptr<const NativeContext> context, SemanticLimits limits)
    : descriptor_(descriptor), mode_(mode), accumulator_(accumulator), context_(std::move(context)), limits_(limits) {}
bool Codec::emit(const Event& event) { return accumulator_.accept(event, buffered_failure_ ? &*buffered_failure_ : nullptr); }
bool Codec::fail(ErrorKind kind, std::string label) {
  if (buffered_failure_ && !closed_) return false;
  emit(Fail{{kind, std::move(label)}}); return false;
}
void Codec::unknown(json::Value v, std::initializer_list<std::string_view> keys) {
  for (auto m : v.members()) if (std::find(keys.begin(), keys.end(), m.key) == keys.end()) ++diagnostics_.unknown_properties;
}
std::shared_ptr<const json::Document> Codec::own(json::Value value) {
  std::string bytes; json::BoundedWriter writer({limits_.max_content_bytes, limits_.max_json_depth}, &bytes);
  writer.value(value);
  if (!writer.ok()) { fail(ErrorKind::ResourceLimit, "retained JSON limit"); return {}; }
  auto parsed = json::parse(bytes, {limits_.max_content_bytes, limits_.max_json_depth});
  if (auto doc = std::get_if<json::Document>(&parsed)) return std::make_shared<const json::Document>(std::move(*doc));
  fail(ErrorKind::ProtocolCorrupt, "invalid retained JSON"); return {};
}
bool Codec::buffered(std::string_view body, Close close) {
  if (closed_ || accumulator_.terminal()) return false;
  if (mode_ != Mode::Buffered || body_seen_) return fail(ErrorKind::Misuse, "unexpected buffered interaction");
  body_seen_ = true;
  if (!close.normal) buffered_failure_ = Error{close.error, "abnormal response close"};
  bool decoded = document(body, {}, false); finish(close);
  return decoded && accumulator_.outcome() && std::holds_alternative<Completion>(*accumulator_.outcome());
}
bool Codec::frame(std::string_view event, std::string_view data) {
  if (closed_ || accumulator_.terminal()) return false;
  if (mode_ != Mode::Sse) return fail(ErrorKind::Misuse, "unexpected interaction frame");
  if (event == "error") return fail(ErrorKind::RemoteFailure, "remote interaction error");
  if (event == "done" && data == "[DONE]") {
    if (!terminal_ || done_) return fail(ErrorKind::ProtocolCorrupt, "unexpected stream sentinel");
    done_ = true; return true;
  }
  return document(data, event, true);
}
bool Codec::identity(json::Value resource, bool initial) {
  auto id = resource.get("id");
  if (!resource.is_object() || !nonempty(resource.get("status")) ||
      (id.valid() && !id.is_null() && !id.is_string()))
    return fail(ErrorKind::ProtocolCorrupt, "invalid interaction identity or status");
  if (resource.get("agent").valid() || resource.get("environment").valid() || resource.get("environment_id").valid())
    return fail(ErrorKind::Unsupported, "agent or environment interaction unsupported");
  if (resource.get("error").valid() && !resource.get("error").is_null()) return fail(ErrorKind::RemoteFailure, "remote interaction error");
  auto model = resource.get("model");
  if ((model.valid() && (!nonempty(model) || model.as_string() != context_->model())) ||
      (!initial && !generation_.empty() && id.is_string() && id.as_string() != generation_))
    return fail(ErrorKind::ProtocolCorrupt, "interaction identity changed");
  unknown(resource, {"id", "model", "object", "created", "updated", "status", "steps", "usage", "service_tier", "error"});
  return true;
}
bool Codec::document(std::string_view bytes, std::string_view event, bool streaming) {
  if (!context_ || descriptor_.family() != "google.interactions" || !context_->matches_descriptor(descriptor_))
    return fail(ErrorKind::InvalidConfig, "Interactions request context required");
  if (bytes.size() > limits_.max_content_bytes - std::min(input_bytes_, limits_.max_content_bytes))
    return fail(ErrorKind::ResourceLimit, "interaction response byte limit");
  input_bytes_ += bytes.size();
  auto parsed = json::parse(bytes, {limits_.max_content_bytes, limits_.max_json_depth});
  if (auto error = std::get_if<json::ParseError>(&parsed)) return fail(error->code == json::ParseCode::SizeExceeded ||
      error->code == json::ParseCode::DepthExceeded ? ErrorKind::ResourceLimit : ErrorKind::ProtocolCorrupt, "invalid interaction JSON");
  auto root = std::get<json::Document>(parsed).root();
  if (!root.is_object()) return fail(ErrorKind::ProtocolCorrupt, "interaction object required");
  if (root.get("error").valid() && !root.get("error").is_null()) return fail(ErrorKind::RemoteFailure, "remote interaction error");
  if (!streaming) {
    if (!nonempty(root.get("model"))) return fail(ErrorKind::ProtocolCorrupt, "buffered interaction model required");
    if (!identity(root, true)) return false;
    generation_ = root.get("id").as_string(); begun_ = true;
    if (!emit(Begin{generation_}) || !emit(MessageBegin{{0}, generation_.empty() ? std::optional<std::string>{} : generation_, Role::Assistant, context_})) return false;
    auto steps = root.get("steps");
    if (steps.valid() && !steps.is_array()) return fail(ErrorKind::ProtocolCorrupt, "invalid buffered interaction steps");
    uint64_t index = 0;
    if (steps.valid()) for (auto step : steps.elements()) { if (!start(step, index) || !stop(index)) return false; ++index; }
    return terminal(root, false);
  }
  auto type = root.get("event_type");
  if (!type.is_string() || (!event.empty() && event != type.as_string())) return fail(ErrorKind::ProtocolCorrupt, "interaction event discriminator mismatch");
  auto name = type.as_string();
  if (name == "error") return fail(ErrorKind::RemoteFailure, "remote interaction error");
  if (terminal_ || done_) return fail(ErrorKind::ProtocolCorrupt, "event after terminal interaction");
  if (name == "interaction.created") {
    unknown(root, {"event_type", "event_id", "interaction"});
    if (begun_ || !identity(root.get("interaction"), true)) return begun_ ? fail(ErrorKind::ProtocolCorrupt, "duplicate interaction creation") : false;
    auto resource = root.get("interaction");
    if (resource.get("status").as_string() != "in_progress") return fail(ErrorKind::ProtocolCorrupt, "new interaction must be in progress");
    if (resource.get("steps").valid() && (!resource.get("steps").is_array() || resource.get("steps").size()))
      return fail(ErrorKind::ProtocolCorrupt, "creation contains unexpected steps");
    generation_ = resource.get("id").as_string(); begun_ = true;
    return emit(Begin{generation_}) && emit(MessageBegin{{0}, generation_.empty() ? std::optional<std::string>{} : generation_, Role::Assistant, context_}) && usage(resource.get("usage"), false);
  }
  if (!begun_) return fail(ErrorKind::ProtocolCorrupt, "event before interaction creation");
  if (name == "interaction.status_update") {
    unknown(root, {"event_type", "event_id", "interaction_id", "status"});
    const auto id = root.get("interaction_id");
    if (!nonempty(root.get("status")) || (id.valid() && !id.is_null() && !id.is_string()) ||
        (!generation_.empty() && id.is_string() && id.as_string() != generation_))
      return fail(ErrorKind::ProtocolCorrupt, "invalid interaction status update");
    auto status = root.get("status").as_string();
    if (status == "failed") return fail(ErrorKind::RemoteFailure, "interaction failure status");
    if (status == "cancelled") return fail(ErrorKind::Cancelled, "interaction cancelled status");
    if (status != "in_progress" && status != "requires_action" && status != "completed" && status != "incomplete")
      return fail(ErrorKind::Unsupported, "unsupported interaction status update");
    return true;
  }
  if (name == "interaction.completed") { unknown(root, {"event_type", "event_id", "interaction"}); return terminal(root.get("interaction"), true); }
  if (name != "step.start" && name != "step.delta" && name != "step.stop") return fail(ErrorKind::Unsupported, "unsupported interaction event");
  auto index = root.get("index");
  if (!unsigned_count(index) || number(index) > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
    return fail(ErrorKind::ProtocolCorrupt, "invalid interaction step index");
  if (name == "step.start") { unknown(root, {"event_type", "event_id", "index", "step"}); return start(root.get("step"), number(index)); }
  if (name == "step.delta") {
    unknown(root, {"event_type", "event_id", "index", "delta", "metadata"});
    auto metadata = root.get("metadata");
    if (metadata.valid() && !metadata.is_object()) return fail(ErrorKind::ProtocolCorrupt, "invalid step delta metadata");
    if (metadata.valid() && !usage(metadata.get("total_usage"), false)) return false;
    return delta(root.get("delta"), number(index));
  }
  unknown(root, {"event_type", "event_id", "index", "usage", "step_usage"});
  return usage(root.get("usage"), false) && stop(number(index));
}
bool Codec::start(json::Value value, uint64_t index) {
  if (!value.is_object() || !value.get("type").is_string()) return fail(ErrorKind::ProtocolCorrupt, "step type required");
  if (index >= limits_.max_parts || steps_.contains(index)) return fail(index >= limits_.max_parts ? ErrorKind::ResourceLimit : ErrorKind::ProtocolCorrupt, "invalid or repeated step index");
  auto type = value.get("type").as_string();
  if (type != "thought" && type != "function_call" && type != "model_output") return fail(ErrorKind::Unsupported, "unsupported critical interaction step");
  if (value.get("error").valid() && !value.get("error").is_null()) return fail(ErrorKind::RemoteFailure, "remote step error");
  Step step; step.type = type; step.original = own(value); if (!step.original) return false;
  auto begin_part = [&](PartKind kind, PartHeader header, uint64_t offset) {
    if (next_part_ >= limits_.max_parts || next_part_ == std::numeric_limits<uint32_t>::max()) return fail(ErrorKind::ResourceLimit, "interaction part limit");
    LocalId id{next_part_++}; step.parts.push_back(id);
    return emit(PartBegin{{0}, id, kind, std::move(header), index * (static_cast<uint64_t>(limits_.max_parts) + 1) + offset});
  };
  if (type == "thought") {
    unknown(value, {"type", "signature", "summary"});
    auto signature = value.get("signature"), summary = value.get("summary");
    if (signature.valid()) { if (!signature.is_string()) return fail(ErrorKind::ProtocolCorrupt, "invalid thought signature"); step.signature = signature.as_string(); }
    if (summary.valid() && !summary.is_array()) return fail(ErrorKind::ProtocolCorrupt, "invalid thought summary");
    if (!begin_part(PartKind::Thought, {}, 0)) return false;
    if (summary.valid()) for (auto content : summary.elements()) {
      if (!content.is_object() || !content.get("type").is_string()) return fail(ErrorKind::ProtocolCorrupt, "invalid thought summary content");
      if (content.get("type").as_string() != "text") return fail(ErrorKind::Unsupported, "nontext thought summary unsupported");
      if (!content.get("text").is_string()) return fail(ErrorKind::ProtocolCorrupt, "thought summary text required");
      auto doc = own(content); if (!doc) return false; step.summary.push_back(std::move(doc));
      if (!emit(PartDelta{step.parts[0], {PartKind::Thought, content.get("text").as_string()}})) return false;
    }
  } else if (type == "function_call") {
    unknown(value, {"type", "id", "name", "arguments"});
    auto id = value.get("id"), name = value.get("name"), arguments = value.get("arguments");
    if (!nonempty(id) || !nonempty(name) || !arguments.is_object() || !calls_.insert(std::string(id.as_string())).second)
      return fail(ErrorKind::ProtocolCorrupt, "invalid function step identity");
    if (!context_->client_tool_declared(name.as_string())) return fail(ErrorKind::Unsupported, "undeclared interaction function");
    PartHeader header; header.wire_id = id.as_string(); header.name = name.as_string(); header.wire_type = "function_call";
    if (!begin_part(PartKind::ToolCall, std::move(header), 0)) return false;
  } else {
    unknown(value, {"type", "content", "error"}); auto content = value.get("content");
    if (content.valid() && !content.is_array()) return fail(ErrorKind::ProtocolCorrupt, "invalid model output content");
    if (content.valid()) for (auto item : content.elements()) {
      if (!item.is_object() || !item.get("type").is_string()) return fail(ErrorKind::ProtocolCorrupt, "invalid model output block");
      if (item.get("type").as_string() != "text") return fail(ErrorKind::Unsupported, "nontext model output unsupported");
      if (!item.get("text").is_string()) return fail(ErrorKind::ProtocolCorrupt, "model output text required");
      unknown(item, {"type", "text", "annotations"}); step.texts.emplace_back(item.get("text").as_string());
      if (!begin_part(PartKind::Text, {}, step.texts.size() - 1) || !emit(PartDelta{step.parts.back(), {PartKind::Text, item.get("text").as_string()}})) return false;
    }
  }
  steps_.emplace(index, std::move(step)); return true;
}
bool Codec::delta(json::Value value, uint64_t index) {
  auto found = steps_.find(index);
  if (found == steps_.end() || found->second.closed || !value.is_object() || !value.get("type").is_string()) return fail(ErrorKind::ProtocolCorrupt, "delta without open step");
  auto& step = found->second; auto type = value.get("type").as_string();
  if (step.signature_delta) return fail(ErrorKind::ProtocolCorrupt, "delta after final thought signature");
  if (type == "thought_signature" && step.type == "thought") {
    unknown(value, {"type", "signature"}); auto signature = value.get("signature");
    if (signature.valid() && !signature.is_string()) return fail(ErrorKind::ProtocolCorrupt, "invalid thought signature delta");
    if (signature.valid()) step.signature = signature.as_string();
    step.signature_delta = true; return true;
  }
  if (type == "thought_summary" && step.type == "thought") {
    unknown(value, {"type", "content"}); auto content = value.get("content");
    if (!content.valid()) return true;
    if (!content.is_object() || !content.get("type").is_string()) return fail(ErrorKind::ProtocolCorrupt, "thought summary content required");
    if (content.get("type").as_string() != "text") return fail(ErrorKind::Unsupported, "nontext thought summary unsupported");
    if (!content.get("text").is_string()) return fail(ErrorKind::ProtocolCorrupt, "thought summary text required");
    auto doc = own(content); if (!doc) return false; step.summary.push_back(std::move(doc));
    return emit(PartDelta{step.parts[0], {PartKind::Thought, content.get("text").as_string()}});
  }
  if (type == "text" && step.type == "model_output") {
    unknown(value, {"type", "text"}); auto text = value.get("text");
    if (!text.is_string()) return fail(ErrorKind::ProtocolCorrupt, "text delta required");
    if (step.parts.empty()) {
      if (next_part_ >= limits_.max_parts) return fail(ErrorKind::ResourceLimit, "interaction part limit");
      LocalId id{next_part_++}; step.parts.push_back(id); step.texts.emplace_back();
      if (!emit(PartBegin{{0}, id, PartKind::Text, {}, index * (static_cast<uint64_t>(limits_.max_parts) + 1)})) return false;
    }
    step.texts.back().append(text.as_string()); return emit(PartDelta{step.parts.back(), {PartKind::Text, text.as_string()}});
  }
  if (type == "arguments_delta" && step.type == "function_call") {
    unknown(value, {"type", "arguments"}); auto arguments = value.get("arguments");
    if (arguments.valid() && !arguments.is_string()) return fail(ErrorKind::ProtocolCorrupt, "invalid arguments delta");
    if (!arguments.valid()) return true;
    if (!step.argument_delta && step.original->root().get("arguments").size()) return fail(ErrorKind::ProtocolCorrupt, "arguments delta conflicts with complete initial object");
    if (arguments.as_string().size() > limits_.max_tool_bytes - std::min(step.arguments.size(), limits_.max_tool_bytes)) return fail(ErrorKind::ResourceLimit, "function argument limit");
    step.argument_delta = true; step.arguments.append(arguments.as_string());
    return emit(PartDelta{step.parts[0], {PartKind::ToolCall, arguments.as_string()}});
  }
  return fail(ErrorKind::Unsupported, "unsupported or mismatched critical step delta");
}
bool Codec::stop(uint64_t index) {
  auto found = steps_.find(index);
  if (found == steps_.end() || found->second.closed) return fail(ErrorKind::ProtocolCorrupt, "stop without open step");
  found->second.closed = true; return true;
}
std::shared_ptr<const json::Document> Codec::assembled(const Step& step) {
  std::string bytes; json::BoundedWriter writer({limits_.max_content_bytes, limits_.max_json_depth}, &bytes);
  auto original = step.original->root(); writer.raw("{"); bool comma = false;
  auto key = [&](std::string_view name) { if (comma) writer.raw(","); comma = true; writer.quoted(name).raw(":"); };
  for (auto member : original.members()) {
    if ((step.type == "thought" && (member.key == "summary" || member.key == "signature")) ||
        (step.type == "model_output" && member.key == "content") || (step.type == "function_call" && member.key == "arguments")) continue;
    key(member.key); writer.value(member.value, 1);
  }
  if (step.type == "thought") {
    if (step.signature) { key("signature"); writer.quoted(*step.signature); }
    if (original.get("summary").valid() || !step.summary.empty()) {
      key("summary"); writer.raw("["); bool separator = false;
      for (const auto& item : step.summary) { if (separator) writer.raw(","); separator = true; writer.value(item->root(), 2); }
      writer.raw("]");
    }
  } else if (step.type == "model_output") {
    if (original.get("content").valid() || !step.texts.empty()) {
      key("content"); writer.raw("[");
      for (size_t i = 0; i < step.texts.size(); ++i) {
        if (i) writer.raw(",");
        writer.raw("{"); bool item_comma = false;
        auto item = original.get("content").at(i);
        for (auto member : item.members()) if (member.key != "text") {
          if (item_comma) writer.raw(",");
          item_comma = true; writer.quoted(member.key).raw(":").value(member.value, 3);
        }
        if (!item.valid()) { writer.raw("\"type\":\"text\""); item_comma = true; }
        if (item_comma) writer.raw(",");
        writer.raw("\"text\":").quoted(step.texts[i]).raw("}");
      }
      writer.raw("]");
    }
  } else {
    key("arguments");
    if (step.argument_delta) writer.raw(step.arguments); else writer.value(original.get("arguments"), 1);
  }
  writer.raw("}");
  if (!writer.ok()) { fail(ErrorKind::ResourceLimit, "assembled step limit"); return {}; }
  auto parsed = json::parse(bytes, {limits_.max_content_bytes, limits_.max_json_depth});
  if (auto doc = std::get_if<json::Document>(&parsed)) return std::make_shared<const json::Document>(std::move(*doc));
  fail(ErrorKind::ProtocolCorrupt, "invalid assembled function arguments"); return {};
}
bool Codec::reconcile(Step& step, json::Value final) {
  if (!final.is_object() || !final.get("type").is_string() || final.get("type").as_string() != step.type)
    return fail(ErrorKind::ProtocolCorrupt, "final step type mismatch");
  if (final.get("error").valid() && !final.get("error").is_null()) return fail(ErrorKind::RemoteFailure, "final step error");
  if (step.type == "thought") {
    auto summary = final.get("summary"), signature = final.get("signature");
    if ((summary.valid() && !summary.is_array()) || (signature.valid() && !signature.is_string())) return fail(ErrorKind::ProtocolCorrupt, "invalid final thought");
    if ((!summary.valid() && !step.summary.empty()) || (summary.valid() && summary.size() < step.summary.size())) return fail(ErrorKind::ProtocolCorrupt, "final thought summary lost");
    if (summary.valid()) for (size_t i = 0; i < summary.size(); ++i) {
      auto item = summary.at(i);
      if (!item.is_object() || !item.get("type").is_string()) return fail(ErrorKind::ProtocolCorrupt, "invalid final thought content");
      if (item.get("type").as_string() != "text") return fail(ErrorKind::Unsupported, "nontext final thought unsupported");
      if (!item.get("text").is_string() || (i < step.summary.size() && !json::equal(item, step.summary[i]->root())))
        return fail(ErrorKind::ProtocolCorrupt, "final thought summary mismatch");
    }
    // The final resource signature replaces the streamed value; it is not prose.
    return true;
  }
  if (step.type == "function_call") {
    auto original = step.original->root();
    if (!nonempty(final.get("id")) || !nonempty(final.get("name")) || final.get("id").as_string() != original.get("id").as_string() ||
        final.get("name").as_string() != original.get("name").as_string() || !final.get("arguments").is_object())
      return fail(ErrorKind::ProtocolCorrupt, "final function ownership mismatch");
    if (step.argument_delta) {
      auto parsed = json::parse(step.arguments, {limits_.max_tool_bytes, limits_.max_json_depth});
      if (auto doc = std::get_if<json::Document>(&parsed)) {
        if (!json::equal(doc->root(), final.get("arguments"))) return fail(ErrorKind::ProtocolCorrupt, "final function arguments mismatch");
      } else return fail(ErrorKind::ProtocolCorrupt, "invalid streamed function arguments");
    } else if (!json::equal(original.get("arguments"), final.get("arguments"))) return fail(ErrorKind::ProtocolCorrupt, "final function arguments mismatch");
    return true;
  }
  auto content = final.get("content");
  if (!content.valid() && step.texts.empty()) return true;
  if (!content.is_array() || content.size() != step.texts.size()) return fail(ErrorKind::ProtocolCorrupt, "final output content mismatch");
  for (size_t i = 0; i < content.size(); ++i) {
    auto item = content.at(i);
    if (!item.is_object() || !item.get("type").is_string()) return fail(ErrorKind::ProtocolCorrupt, "invalid final output block");
    if (item.get("type").as_string() != "text") return fail(ErrorKind::Unsupported, "nontext final output unsupported");
    if (!item.get("text").is_string() || !prefix(step.texts[i], item.get("text").as_string())) return fail(ErrorKind::ProtocolCorrupt, "final output prefix mismatch");
  }
  return true;
}
bool Codec::seal(Step& step, json::Value final) {
  if (step.type == "thought") { auto metadata = own(final); return metadata && emit(PartSeal{step.parts[0], {}, std::move(metadata)}); }
  if (step.type == "function_call") {
    auto arguments = final.get("arguments");
    if (!arguments.is_object()) return fail(ErrorKind::ProtocolCorrupt, "function arguments must be object");
    std::string bytes = step.argument_delta ? step.arguments : arguments.dump(); auto metadata = own(final);
    return metadata && emit(PartSeal{step.parts[0], bytes, std::move(metadata)});
  }
  for (size_t i = 0; i < step.parts.size(); ++i) if (!emit(PartSeal{step.parts[i], final.get("content").at(i).get("text").as_string()})) return false;
  return true;
}
bool Codec::terminal(json::Value resource, bool streaming) {
  if (!identity(resource, false)) return false;
  auto status = resource.get("status").as_string();
  if (status == "failed") return fail(ErrorKind::RemoteFailure, "interaction failed");
  if (status == "cancelled") return fail(ErrorKind::Cancelled, "interaction cancelled");
  StopKind kind;
  if (status == "completed") kind = StopKind::EndTurn;
  else if (status == "requires_action") kind = StopKind::ToolUse;
  else if (status == "incomplete") kind = StopKind::MaxTokens;
  else return fail(ErrorKind::Unsupported, "unsupported terminal interaction status");
  if ((kind == StopKind::ToolUse && calls_.empty()) || (kind == StopKind::EndTurn && !calls_.empty()))
    return fail(ErrorKind::ProtocolCorrupt, "terminal status disagrees with client action");
  auto final_steps = resource.get("steps");
  if (final_steps.valid() && (!final_steps.is_array() || final_steps.size() != steps_.size())) return fail(ErrorKind::ProtocolCorrupt, "terminal step count mismatch");
  std::string bytes; json::BoundedWriter writer({limits_.max_content_bytes, limits_.max_json_depth}, &bytes); writer.raw("[");
  uint64_t expected = 0;
  for (auto& [index, step] : steps_) {
    if (index != expected++ || !step.closed) return fail(ErrorKind::ProtocolCorrupt, "terminal with missing or open steps");
    std::shared_ptr<const json::Document> assembled_step;
    json::Value final;
    if (final_steps.valid()) final = final_steps.at(index);
    else {
      assembled_step = assembled(step); if (!assembled_step) return false;
      final = assembled_step->root();
    }
    if (!reconcile(step, final) || !seal(step, final)) return false;
    if (index) writer.raw(",");
    writer.value(final, 1);
  }
  writer.raw("]");
  if (!writer.ok()) return fail(ErrorKind::ResourceLimit, "atomic interaction output limit");
  auto parsed = json::parse(bytes, {limits_.max_content_bytes, limits_.max_json_depth});
  auto doc = std::get_if<json::Document>(&parsed); if (!doc) return fail(ErrorKind::ProtocolCorrupt, "invalid atomic interaction output");
  if (!usage(resource.get("usage"), true) || !emit(MessageSeal{{0}, std::make_shared<const json::Document>(std::move(*doc))}) ||
      !emit(Stop{{kind, std::string(status)}})) return false;
  terminal_ = true; (void)streaming; return true;
}
void Codec::conflict(std::string_view key, std::string_view detail) {
  usage_.quality = UsageQuality::Inconsistent;
  usage_.conflicts.push_back({std::string(key), std::string(detail)});
}
bool Codec::count(json::Value value, std::optional<Count>& target, std::string_view name) {
  target.reset(); if (!value.valid() || value.is_null()) return true;
  if (!unsigned_count(value)) return fail(ErrorKind::ProtocolCorrupt, "invalid interaction usage count");
  target = Count{number(value), Evidence::Reported}; (void)name; return true;
}
bool Codec::leaves(json::Value value, std::string path) {
  if (value.is_object()) {
    for (auto member : value.members()) if (!leaves(member.value, path.empty() ? std::string(member.key) : path + "." + std::string(member.key))) return false;
  } else if (value.is_array()) {
    for (size_t i = 0; i < value.size(); ++i) if (!leaves(value.at(i), path + "." + std::to_string(i))) return false;
  } else if (value.is_number()) {
    if (!unsigned_count(value)) return fail(ErrorKind::ProtocolCorrupt, "invalid numeric usage leaf");
    usage_.extra.emplace(std::move(path), Count{number(value), Evidence::Reported});
  }
  return true;
}
bool Codec::usage(json::Value value, bool final) {
  if (!value.valid() || value.is_null()) {
    if (final) { usage_ = Usage{}; return emit(UsageUpdate{usage_}); }
    if (usage_.stage != UsageStage::Missing) usage_.stage = UsageStage::Partial;
    return true;
  }
  if (!value.is_object()) return fail(ErrorKind::ProtocolCorrupt, "interaction usage object required");
  usage_ = Usage{}; usage_.stage = final ? UsageStage::Final : UsageStage::Partial;
  std::optional<Count> response, tool;
  if (!count(value.get("total_input_tokens"), usage_.input_total, "input") ||
      !count(value.get("total_output_tokens"), response, "output") || !count(value.get("total_thought_tokens"), usage_.reasoning, "thought") ||
      !count(value.get("total_cached_tokens"), usage_.cache_read, "cached") || !count(value.get("total_tokens"), usage_.provider_reported_total, "total") ||
      !count(value.get("total_tool_use_tokens"), tool, "tool") || !leaves(value, {})) return false;
  auto add = [&](uint64_t a, uint64_t b, std::optional<Count>& out, std::string_view name) {
    if (b > std::numeric_limits<uint64_t>::max() - a) conflict(name, "usage arithmetic overflow");
    else out = Count{a + b, Evidence::Derived};
  };
  if (response && usage_.reasoning) add(response->value, usage_.reasoning->value, usage_.output_total, "output_total");
  if (usage_.input_total && usage_.cache_read) {
    if (usage_.cache_read->value > usage_.input_total->value) conflict("cache_read", "cached input exceeds input total");
    else usage_.input_uncached = Count{usage_.input_total->value - usage_.cache_read->value, Evidence::Derived};
  }
  if (tool && usage_.input_total && tool->value > usage_.input_total->value) conflict("total_tool_use_tokens", "tool-use prompt subset exceeds input");
  if (usage_.input_total && usage_.output_total) add(usage_.input_total->value, usage_.output_total->value, usage_.total, "total");
  if (!usage_.total && usage_.provider_reported_total) usage_.total = usage_.provider_reported_total;
  if (usage_.total && usage_.provider_reported_total && usage_.total->value != usage_.provider_reported_total->value)
    conflict("total", "reported total disagrees with input plus responses plus thoughts");
  return emit(UsageUpdate{usage_});
}
void Codec::finish(Close close) {
  if (closed_ || accumulator_.terminal()) return;
  closed_ = true;
  if (!close.normal) { emit(Fail{{close.error, "abnormal interaction close"}}); return; }
  if (!terminal_) { fail(ErrorKind::Truncated, "interaction lacks terminal resource"); return; }
  emit(Commit{"normal interaction close"});
}
} // namespace sp::interactions
