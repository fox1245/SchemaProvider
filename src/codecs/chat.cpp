#include "codecs/chat.h"
#include "core/native.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include <algorithm>
#include <limits>

namespace sp::chat {
namespace {
bool unsigned_count(json::Value v) { return v.is_uint() || (v.is_int() && v.as_int() >= 0); }
uint64_t uint_value(json::Value v) { return v.is_uint() ? v.as_uint() : static_cast<uint64_t>(v.as_int()); }
StopKind stop_kind(descriptor::StopKind kind) {
  switch (kind) {
    case descriptor::StopKind::EndTurn: return StopKind::EndTurn;
    case descriptor::StopKind::MaxTokens: return StopKind::MaxTokens;
    case descriptor::StopKind::ToolUse: return StopKind::ToolUse;
    case descriptor::StopKind::ContentFilter: return StopKind::ContentFilter;
    case descriptor::StopKind::Refusal: return StopKind::Refusal;
    case descriptor::StopKind::PauseTurn: return StopKind::PauseTurn;
    case descriptor::StopKind::ContextLimit: return StopKind::ContextLimit;
    case descriptor::StopKind::StopSequence: return StopKind::StopSequence;
    case descriptor::StopKind::MalformedCall: return StopKind::MalformedCall;
    case descriptor::StopKind::Unknown: return StopKind::Unknown;
  }
  return StopKind::Unknown;
}
}
Codec::Codec(const descriptor::ValidatedDescriptor& descriptor, Mode mode, Accumulator& accumulator,
             SemanticLimits limits, std::shared_ptr<const NativeContext> context)
    : descriptor_(descriptor), mode_(mode), accumulator_(accumulator), context_(std::move(context)), limits_(limits) {
  if (descriptor_.family() != "openai.chat") fail(ErrorKind::InvalidConfig, "descriptor family does not match Chat codec");
  if (context_ && !context_->matches_descriptor(descriptor_))
    fail(ErrorKind::InvalidConfig, "Chat request context does not match descriptor");
}
bool Codec::emit(const Event& e) {
  return accumulator_.accept(e, buffered_failure_ ? &*buffered_failure_ : nullptr);
}
bool Codec::fail(ErrorKind kind, std::string message) {
  if (buffered_failure_ && !closed_) return false;
  emit(Fail{{kind, std::move(message)}}); return false;
}
void Codec::unknown(json::Value v, std::initializer_list<std::string_view> known) {
  for (auto member : v.members()) if (std::find(known.begin(), known.end(), member.key) == known.end()) ++diagnostics_.unknown_properties;
}
bool Codec::buffered(std::string_view body, Close close) {
  if (closed_ || accumulator_.terminal()) return false;
  if (mode_ != Mode::Buffered || body_seen_) return fail(ErrorKind::Misuse, "unexpected buffered document");
  body_seen_ = true;
  if (!close.normal) buffered_failure_ = Error{close.error, "abnormal response close"};
  const bool decoded = document(body, {}, false);
  finish(close);
  return decoded && accumulator_.outcome() && std::holds_alternative<Completion>(*accumulator_.outcome());
}
bool Codec::frame(std::string_view event, std::string_view data) {
  if (closed_ || accumulator_.terminal()) return false;
  if (mode_ != Mode::Sse) return fail(ErrorKind::Misuse, "unexpected stream frame");
  if (data == "[DONE]" && event != "error") {
    if (!event.empty() && event != "message") return fail(ErrorKind::Unsupported, "unknown stream event");
    if (done_) return fail(ErrorKind::ProtocolCorrupt, "duplicate DONE");
    if (!stopped_) return fail(ErrorKind::ProtocolCorrupt, "DONE without finish reason");
    done_ = true; return true;
  }
  return document(data, event, true);
}
bool Codec::document(std::string_view bytes, std::string_view event, bool streaming) {
  const bool named_error = streaming && event == "error";
  const bool unknown_event = streaming && !event.empty() && event != "message" && !named_error;
  if (bytes.size() > limits_.max_content_bytes - std::min(input_bytes_, limits_.max_content_bytes)) {
    if (unknown_event) return fail(ErrorKind::Unsupported, "unknown stream event");
    return fail(named_error ? ErrorKind::RemoteFailure : ErrorKind::ResourceLimit, named_error ? "remote error event" : "response byte limit");
  }
  input_bytes_ += bytes.size();
  auto parsed = json::parse(bytes, {limits_.max_content_bytes, limits_.max_json_depth});
  if (auto* error = std::get_if<json::ParseError>(&parsed)) {
    if (unknown_event) return fail(ErrorKind::Unsupported, "unknown stream event");
    return fail(named_error ? ErrorKind::RemoteFailure : error->code == json::ParseCode::SizeExceeded || error->code == json::ParseCode::DepthExceeded ? ErrorKind::ResourceLimit : ErrorKind::ProtocolCorrupt,
                named_error ? "remote error event" : "invalid response JSON");
  }
  auto wire = std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
  const auto root = wire->root();
  if (!root.is_object()) {
    if (unknown_event) return fail(ErrorKind::Unsupported, "unknown stream event");
    return fail(named_error ? ErrorKind::RemoteFailure : ErrorKind::ProtocolCorrupt, named_error ? "remote error event" : "response must be object");
  }
  const auto object = root.get("object");
  std::string wire_type = named_error ? std::string(event)
      : object.is_string() && !object.as_string().empty() ? std::string(object.as_string())
      : !event.empty() ? std::string(event)
      : streaming ? "chat.completion.chunk" : "chat.completion";
  if (named_error || unknown_event) {
    const Error classified{named_error ? ErrorKind::RemoteFailure : ErrorKind::Unsupported,
                           named_error ? "remote error event" : "unknown stream event"};
    if (!accumulator_.accept(RawWire{std::move(wire_type), wire}, &classified)) return false;
    return fail(classified.kind, classified.safe_message);
  }
  if (!emit(RawWire{std::move(wire_type), wire})) return false;
  if (root.get("error").valid()) return fail(ErrorKind::RemoteFailure, "remote error property");
  if (done_) return fail(ErrorKind::ProtocolCorrupt, "frame after DONE");
  for (auto name : {"id", "model", "object"}) if (!root.get(name).is_string()) return fail(ErrorKind::ProtocolCorrupt, "required response metadata must be string");
  if (root.get("object").as_string() != (streaming ? "chat.completion.chunk" : "chat.completion")) return fail(ErrorKind::Unsupported, "unknown response object tag");
  auto created = root.get("created");
  if (!unsigned_count(created)) return fail(ErrorKind::ProtocolCorrupt, "creation counter required");
  if (begun_ && (generation_ != root.get("id").as_string() || model_ != root.get("model").as_string() || created_ != uint_value(created)))
    return fail(ErrorKind::ProtocolCorrupt, "inconsistent response identity");
  unknown(root, {"id", "model", "object", "created", "choices", "usage", "error"});
  auto choices = root.get("choices");
  if (!choices.is_array()) return fail(ErrorKind::ProtocolCorrupt, "choices must be array");
  if (choices.size() > 1) return fail(ErrorKind::Unsupported, "multiple candidates unsupported");
  if (!begun_) {
    generation_ = root.get("id").as_string();
    model_ = root.get("model").as_string();
    created_ = uint_value(created);
    if (!emit(Begin{generation_}) || !emit(MessageBegin{{0}, generation_, Role::Assistant, context_})) return false;
    begun_ = true;
  }
  auto usage_value = root;
  for (const auto& key : descriptor_.usage_path()) {
    if (!usage_value.valid()) break;
    if (!usage_value.is_object()) return fail(ErrorKind::ProtocolCorrupt, "usage path crosses non-object");
    usage_value = usage_value.get(key);
  }
  if (choices.size() == 0) {
    if (!streaming || !usage_value.valid() || usage_value.is_null()) return fail(ErrorKind::ProtocolCorrupt, "empty choices without usage");
  } else {
    if (stopped_) return fail(ErrorKind::ProtocolCorrupt, "choice after finish reason");
    auto choice = choices.at(0);
    if (!choice.is_object()) return fail(ErrorKind::ProtocolCorrupt, "choice must be object");
    unknown(choice, {"index", "message", "delta", "finish_reason", "logprobs"});
    if (auto opposite = choice.get(streaming ? "message" : "delta"); opposite.valid())
      return fail(opposite.is_object() ? ErrorKind::Unsupported : ErrorKind::ProtocolCorrupt, "choice has wrong response shape");
    auto index = choice.get("index");
    if (!unsigned_count(index)) return fail(ErrorKind::ProtocolCorrupt, "choice index required");
    if (uint_value(index) != 0) return fail(ErrorKind::Unsupported, "nonzero candidate index");
    if (auto lp = choice.get("logprobs"); lp.valid() && !lp.is_null()) {
      if (!lp.is_object()) return fail(ErrorKind::ProtocolCorrupt, "invalid logprobs");
      return fail(ErrorKind::Unsupported, "logprobs outside first Chat cell");
    }
    auto reason = choice.get("finish_reason");
    if (!reason.valid() || (!reason.is_null() && !reason.is_string())) return fail(ErrorKind::ProtocolCorrupt, "invalid finish reason");
    if (!item(choice.get(streaming ? "delta" : "message"), streaming)) return false;
    if (reason.is_string()) {
      if (reason.as_string().empty()) return fail(ErrorKind::ProtocolCorrupt, "empty finish reason");
      if (reason.as_string() == "error") return fail(ErrorKind::RemoteFailure, "remote failure terminal");
      auto kind = stop_kind(descriptor_.stop_kind(reason.as_string()));
      if (streaming && !finish_reasoning_details()) return false;
      if (!emit(Stop{{kind, std::string(reason.as_string())}})) return false;
      stopped_ = true;
    }
  }
  if (usage_value.valid() && !usage_value.is_null() && !usage(usage_value)) return false;
  return true;
}
bool Codec::item(json::Value value, bool streaming) {
  if (!value.is_object()) return fail(ErrorKind::ProtocolCorrupt, "message or delta must be object");
  unknown(value, {"role", "content", "refusal", "tool_calls", "function_call", "annotations", "audio", "reasoning", "reasoning_content", "reasoning_details"});
  auto role = value.get("role");
  if ((!streaming && !role.valid()) || (role.valid() && (!role.is_string() || role.as_string() != "assistant"))) return fail(ErrorKind::ProtocolCorrupt, "assistant role required");
  for (auto key : {"function_call", "audio"}) {
    auto v = value.get(key);
    if (v.valid() && !v.is_null()) {
      return fail(v.is_object() ? ErrorKind::Unsupported : ErrorKind::ProtocolCorrupt, "unsupported known message feature");
    }
  }
  if (auto annotations = value.get("annotations"); annotations.valid()) {
    if (!annotations.is_array()) return fail(ErrorKind::ProtocolCorrupt, "annotations must be array");
    if (annotations.size()) return fail(ErrorKind::Unsupported, "annotations outside first Chat cell");
  }
  auto reasoning = value.get("reasoning"), reasoning_content = value.get("reasoning_content");
  for (auto fragment : {reasoning, reasoning_content})
    if (fragment.valid() && !fragment.is_null() && !fragment.is_string())
      return fail(ErrorKind::ProtocolCorrupt, "reasoning must be string or null");
  if (reasoning.is_string() && reasoning_content.is_string() && !json::equal(reasoning, reasoning_content))
    return fail(ErrorKind::Unsupported, "contradictory reasoning aliases");
  if (!text(reasoning_content.is_string() ? reasoning_content : reasoning, PartKind::Thinking, streaming) ||
      !reasoning_details(value.get("reasoning_details"), streaming)) return false;
  auto content = value.get("content");
  if (!streaming && !content.valid()) return fail(ErrorKind::ProtocolCorrupt, "buffered content required");
  if (!text(content, PartKind::Text, streaming) || !text(value.get("refusal"), PartKind::Refusal, streaming)) return false;
  if (auto calls = value.get("tool_calls"); calls.valid()) {
    if (!calls.is_array()) return fail(ErrorKind::ProtocolCorrupt, "tool_calls must be array");
    uint64_t pos = 0;
    for (auto call : calls.elements()) if (!tool(call, streaming, pos++)) return false;
  }
  return true;
}
bool Codec::text(json::Value value, PartKind kind, bool streaming) {
  if (!value.valid() || value.is_null()) return true;
  if (!value.is_string()) return fail(ErrorKind::ProtocolCorrupt, "text channels must be strings or null");
  auto& id = kind == PartKind::Text ? text_ : kind == PartKind::Thinking ? thinking_ : refusal_;
  if (!id) {
    id = LocalId{next_part_++};
    const uint64_t order = kind == PartKind::Thinking ? 0U :
        kind == PartKind::Text ? 1 + limits_.max_parts : 2 + limits_.max_parts;
    if (!emit(PartBegin{{0}, *id, kind, {}, order})) return false;
  }
  if (streaming) return emit(PartDelta{*id, {kind, value.as_string()}});
  return emit(PartSeal{*id, value.as_string()});
}
bool Codec::reasoning_details(json::Value value, bool streaming) {
  if (!value.valid() || value.is_null()) return true;
  if (!value.is_array()) return fail(ErrorKind::ProtocolCorrupt, "reasoning_details must be array");
  if (!descriptor::contains(descriptor_.family_policy().openrouter_origins, descriptor_.base_url()))
    return fail(ErrorKind::Unsupported, "reasoning_details requires declared OpenRouter origin");
  if (reasoning_details_count_ >= limits_.max_parts)
    return fail(ErrorKind::ResourceLimit, "reasoning details part limit");
  for (auto detail : value.elements())
    if (!detail.is_object()) return fail(ErrorKind::ProtocolCorrupt, "reasoning detail must be object");
  const json::Limits limits{limits_.max_content_bytes, limits_.max_json_depth};
  const std::string_view type = streaming ? "reasoning_details.frame" : "reasoning_details";
  auto write = [&](json::BoundedWriter& writer) {
    writer.raw("{\"type\":").quoted(type).raw(",\"details\":").value(value, 1).raw("}");
  };
  json::BoundedWriter measure(limits);
  write(measure);
  if (!measure.ok()) return fail(ErrorKind::ResourceLimit, "reasoning details metadata limit");
  std::string bytes;
  bytes.reserve(measure.size());
  json::BoundedWriter output(limits, &bytes);
  write(output);
  auto parsed = json::parse(bytes, limits);
  auto* document = std::get_if<json::Document>(&parsed);
  if (!document) return fail(ErrorKind::ResourceLimit, "reasoning details metadata cannot be retained");
  auto metadata = std::make_shared<const json::Document>(std::move(*document));
  const LocalId id{next_part_++};
  const uint64_t order = 1 + reasoning_details_count_++;
  if (!emit(PartBegin{{0}, id, PartKind::Opaque,
                      {{}, {}, ToolCallKind::ClientExecuted, std::string(type), metadata}, order}) ||
      !emit(PartSeal{id, {}})) return false;
  if (!streaming) return true;
  reasoning_frames_.push_back(std::move(metadata));
  // OpenRouter's official accumulator joins consecutive text/summary deltas;
  // encrypted objects are discrete blobs and are never concatenated.
  // https://github.com/OpenRouterTeam/ai-sdk-provider/blob/main/src/chat/index.ts
  for (auto detail : reasoning_frames_.back()->root().get("details").elements()) {
    const auto detail_type = detail.get("type");
    if (!detail_type.is_string() || detail_type.as_string().empty())
      return fail(ErrorKind::ProtocolCorrupt, "reasoning detail requires a type");
    const auto name = detail_type.as_string();
    const auto identity = detail.get("id"), index = detail.get("index"), format = detail.get("format");
    if ((identity.valid() && !identity.is_null() && !identity.is_string()) ||
        (index.valid() && !unsigned_count(index)) ||
        (format.valid() && !format.is_null() && !format.is_string()))
      return fail(ErrorKind::ProtocolCorrupt, "invalid reasoning detail identity");
    const std::string_view key = name == "reasoning.text" ? "text" : name == "reasoning.summary" ? "summary" : "";
    if (key.empty()) {
      if (name == "reasoning.encrypted" && !detail.get("data").is_string())
        return fail(ErrorKind::ProtocolCorrupt, "encrypted reasoning requires a complete opaque blob");
      if (reasoning_bindings_.size() >= limits_.max_parts)
        return fail(ErrorKind::ResourceLimit, "reasoning detail count limit");
      reasoning_bindings_.push_back({detail, {}, {}, {}});
      continue;
    }
    const auto fragment = detail.get(key), signature = detail.get("signature");
    if ((fragment.valid() && !fragment.is_null() && !fragment.is_string()) ||
        (signature.valid() && !signature.is_null() && !signature.is_string()))
      return fail(ErrorKind::ProtocolCorrupt, "invalid reasoning text or signature fragment");
    bool join = !reasoning_bindings_.empty() && reasoning_bindings_.back().payload_key == key;
    if (join) for (const auto identity_key : {"id", "index"}) {
      const auto previous = reasoning_bindings_.back().fields.find(identity_key);
      const auto next = detail.get(identity_key);
      if (previous != reasoning_bindings_.back().fields.end() && !previous->second.is_null() &&
          next.valid() && !next.is_null() && !json::equal(previous->second, next)) join = false;
    }
    if (!join) {
      if (reasoning_bindings_.size() >= limits_.max_parts)
        return fail(ErrorKind::ResourceLimit, "reasoning detail count limit");
      reasoning_bindings_.push_back({detail, {}, key, {}});
    }
    auto& binding = reasoning_bindings_.back();
    for (auto member : detail.members()) {
      if (member.key == key) continue;
      auto [found, inserted] = binding.fields.emplace(member.key, member.value);
      if (inserted || member.value.is_null()) continue;
      if (found->second.is_null() ||
          (found->second.is_string() && found->second.as_string().empty())) {
        found->second = member.value;
      } else if (!json::equal(found->second, member.value) &&
                 !(member.value.is_string() && member.value.as_string().empty())) {
        return fail(ErrorKind::ProtocolCorrupt, "contradictory reasoning detail metadata");
      }
    }
    if (fragment.is_string()) {
      const auto bytes = fragment.as_string();
      if (bytes.size() > limits_.max_content_bytes - std::min(binding.payload.size(), limits_.max_content_bytes))
        return fail(ErrorKind::ResourceLimit, "reasoning payload limit");
      binding.payload.append(bytes);
    }
  }
  return true;
}
bool Codec::finish_reasoning_details() {
  if (reasoning_frames_.empty()) return true;
  const json::Limits limits{limits_.max_content_bytes, limits_.max_json_depth};
  auto write = [&](json::BoundedWriter& writer) {
    writer.raw("{\"type\":\"reasoning_details\",\"details\":[");
    bool comma = false;
    for (const auto& binding : reasoning_bindings_) {
      if (comma) writer.raw(",");
      comma = true;
      if (binding.payload_key.empty()) writer.value(binding.original, 2);
      else {
        writer.raw("{");
        bool field_comma = false;
        for (const auto& [key, value] : binding.fields) {
          if (field_comma) writer.raw(",");
          field_comma = true;
          writer.quoted(key).raw(":").value(value, 3);
        }
        if (field_comma) writer.raw(",");
        writer.quoted(binding.payload_key).raw(":").quoted(binding.payload).raw("}");
      }
    }
    writer.raw("],\"frames\":[");
    comma = false;
    for (const auto& frame : reasoning_frames_) {
      if (comma) writer.raw(",");
      comma = true;
      writer.value(frame->root().get("details"), 2);
    }
    writer.raw("]}");
  };
  json::BoundedWriter measure(limits);
  write(measure);
  if (!measure.ok()) return fail(ErrorKind::ResourceLimit, "completed reasoning metadata limit");
  std::string bytes;
  bytes.reserve(measure.size());
  json::BoundedWriter output(limits, &bytes);
  write(output);
  auto parsed = json::parse(bytes, limits);
  auto* document = std::get_if<json::Document>(&parsed);
  if (!document) return fail(ErrorKind::ResourceLimit, "completed reasoning metadata cannot be retained");
  auto metadata = std::make_shared<const json::Document>(std::move(*document));
  const LocalId id{next_part_++};
  return emit(PartBegin{{0}, id, PartKind::Opaque,
                        {{}, {}, ToolCallKind::ClientExecuted, "reasoning_details", metadata},
                        1 + reasoning_details_count_}) && emit(PartSeal{id, {}});
}
bool Codec::tool(json::Value value, bool streaming, uint64_t position) {
  if (!value.is_object()) return fail(ErrorKind::ProtocolCorrupt, "tool call must be object");
  unknown(value, {"index", "id", "type", "function"});
  auto index = value.get("index"), id = value.get("id"), type = value.get("type"), function = value.get("function");
  if (index.valid() && !unsigned_count(index)) return fail(ErrorKind::ProtocolCorrupt, "invalid tool index");
  if (id.valid() && (!id.is_string() || id.as_string().empty())) return fail(ErrorKind::ProtocolCorrupt, "invalid tool id");
  if ((!streaming && !type.valid()) || (type.valid() && !type.is_string())) return fail(ErrorKind::ProtocolCorrupt, "invalid tool type");
  if (type.valid() && type.as_string() != "function") return fail(ErrorKind::Unsupported, "unsupported tool type");
  if ((!streaming && !function.valid()) || (function.valid() && !function.is_object())) return fail(ErrorKind::ProtocolCorrupt, "invalid tool function");
  if (function.valid()) unknown(function, {"name", "arguments"});
  auto name = function.get("name"), args = function.get("arguments");
  if (name.valid() && (!name.is_string() || name.as_string().empty())) return fail(ErrorKind::ProtocolCorrupt, "invalid function name");
  if ((!streaming && !args.valid()) || (args.valid() && !args.is_string())) return fail(ErrorKind::ProtocolCorrupt, "invalid arguments fragment");
  std::optional<size_t> found;
  const bool indexed = !streaming || index.valid();
  const uint64_t wire_index = index.valid() ? uint_value(index) : position;
  if (indexed && wire_index >= limits_.max_parts) return fail(ErrorKind::ResourceLimit, "tool index exceeds part limit");
  if (indexed && indexed_tools_ == false) return fail(ErrorKind::ProtocolCorrupt, "mixed tool ordering is ambiguous");
  if (!streaming || index.valid()) {
    auto it = indices_.find(wire_index);
    if (it != indices_.end()) found = it->second;
    if (found && id.valid() && tools_[*found].id != id.as_string()) found.reset();
  } else if (id.valid()) {
    for (size_t i = 0; i < tools_.size(); ++i) if (tools_[i].id == id.as_string()) found = i;
  } else {
    if (tools_.size() != 1) return fail(ErrorKind::ProtocolCorrupt, "ambiguous unindexed tool fragment");
    found = 0;
  }
  if (!found) {
    if (!id.valid() || !name.valid()) return fail(ErrorKind::ProtocolCorrupt, "new tool requires id and name");
    for (const auto& call : tools_) if (call.id == id.as_string()) return fail(ErrorKind::ProtocolCorrupt, "tool id rebound to another index");
    if (tools_.size() >= limits_.max_parts) return fail(ErrorKind::ResourceLimit, "tool count limit");
    if (indexed_tools_ && *indexed_tools_ != indexed) return fail(ErrorKind::ProtocolCorrupt, "mixed tool ordering is ambiguous");
    indexed_tools_ = indexed;
    const uint64_t base = 3 + limits_.max_parts;
    const auto order = indexed ? base + wire_index * limits_.max_parts + tools_.size() : base + tools_.size();
    ToolBinding binding{{next_part_++}, std::string(id.as_string()), std::string(name.as_string()), order};
    if (!emit(PartBegin{{0}, binding.local, PartKind::ToolCall, {binding.id, binding.name, ToolCallKind::ClientExecuted}, order})) return false;
    found = tools_.size(); tools_.push_back(std::move(binding));
    if (!streaming || index.valid()) indices_[wire_index] = *found;
  }
  auto& binding = tools_[*found];
  if (name.valid() && binding.name != name.as_string()) return fail(ErrorKind::ProtocolCorrupt, "contradictory function name");
  if (!streaming) return emit(PartSeal{binding.local, args.as_string()});
  return !args.valid() || emit(PartDelta{binding.local, {PartKind::ToolCall, args.as_string()}});
}
bool Codec::usage(json::Value value) {
  if (!value.is_object()) return fail(ErrorKind::ProtocolCorrupt, "usage must be object");
  unknown(value, {"prompt_tokens", "completion_tokens", "total_tokens", "prompt_tokens_details", "completion_tokens_details"});
  for (auto key : {"prompt_tokens", "completion_tokens", "total_tokens"})
    if (!value.get(key).valid()) return fail(ErrorKind::ProtocolCorrupt, "required usage counter missing");
  Usage result; result.stage = UsageStage::Partial;
  auto counter = [&](json::Value parent, std::string_view key, std::optional<Count>& destination) {
    auto v = parent.get(key);
    if (!v.valid()) return true;
    if (!unsigned_count(v)) return fail(ErrorKind::ProtocolCorrupt, "usage counter must be unsigned integer");
    destination = Count{uint_value(v), Evidence::Reported}; return true;
  };
  if (!counter(value, "prompt_tokens", result.input_total) || !counter(value, "completion_tokens", result.output_total) || !counter(value, "total_tokens", result.provider_reported_total)) return false;
  auto prompt = value.get("prompt_tokens_details"), completion = value.get("completion_tokens_details");
  for (auto detail : {prompt, completion}) if (detail.valid() && !detail.is_null() && !detail.is_object()) return fail(ErrorKind::ProtocolCorrupt, "usage details must be object or null");
  if (prompt.is_object()) {
    unknown(prompt, {"cached_tokens", "cache_write_tokens", "audio_tokens"});
    if (!counter(prompt, "cached_tokens", result.cache_read) || !counter(prompt, "cache_write_tokens", result.cache_write)) return false;
    std::optional<Count> audio;
    if (!counter(prompt, "audio_tokens", audio)) return false;
    if (audio) result.extra["input_audio"] = *audio;
  }
  if (completion.is_object()) {
    unknown(completion, {"reasoning_tokens", "audio_tokens", "accepted_prediction_tokens", "rejected_prediction_tokens"});
    if (!counter(completion, "reasoning_tokens", result.reasoning)) return false;
    for (auto key : {"audio_tokens", "accepted_prediction_tokens", "rejected_prediction_tokens"}) {
      std::optional<Count> c;
      if (!counter(completion, key, c)) return false;
      if (c) result.extra[key] = *c;
    }
  }
  auto conflict = [&](std::string counter_name, std::string detail) { result.quality = UsageQuality::Inconsistent; result.conflicts.push_back({std::move(counter_name), std::move(detail)}); };
  if (result.input_total && result.output_total) {
    if (result.output_total->value > std::numeric_limits<uint64_t>::max() - result.input_total->value) return fail(ErrorKind::ProtocolCorrupt, "usage sum overflow");
    result.total = Count{result.input_total->value + result.output_total->value, Evidence::Derived};
    if (result.provider_reported_total && result.total->value != result.provider_reported_total->value) conflict("total", "reported total differs from input plus output");
  }
  if (result.input_total) {
    const uint64_t read = result.cache_read ? result.cache_read->value : 0;
    const uint64_t written = result.cache_write ? result.cache_write->value : 0;
    if (written > std::numeric_limits<uint64_t>::max() - read) return fail(ErrorKind::ProtocolCorrupt, "cache sum overflow");
    const uint64_t known_cached = read + written;
    if (known_cached > result.input_total->value) conflict("input_uncached", "known cache subset exceeds input total");
    else if (result.cache_read && result.cache_write)
      result.input_uncached = Count{result.input_total->value - known_cached, Evidence::Derived};
  }
  if (result.output_total && result.reasoning && result.reasoning->value > result.output_total->value) conflict("reasoning", "reasoning subset exceeds output total");
  return emit(UsageUpdate{std::move(result)});
}
void Codec::finish(Close close) {
  if (closed_ || accumulator_.terminal()) { closed_ = true; return; }
  closed_ = true;
  if (!close.normal) { fail(close.error, "abnormal response close"); return; }
  if (!begun_ || !stopped_ || (mode_ == Mode::Sse && !done_)) { fail(ErrorKind::Truncated, "missing family terminal evidence"); return; }
  if (mode_ == Mode::Sse) {
    if (text_ && !emit(PartSeal{*text_, {}})) return;
    if (refusal_ && !emit(PartSeal{*refusal_, {}})) return;
    if (thinking_ && !emit(PartSeal{*thinking_, {}})) return;
    for (const auto& tool : tools_) if (!emit(PartSeal{tool.local, {}})) return;
  }
  if (!emit(MessageSeal{{0}})) return;
  emit(Commit{mode_ == Mode::Sse ? "chat.finish_reason+DONE+normal_close" : "chat.finish_reason+normal_close"});
}
} // namespace sp::chat
