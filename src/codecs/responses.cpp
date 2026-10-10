#include "codecs/responses.h"
#include "codecs/media_output.h"
#include "codecs/provider_cost.h"
#include "codecs/responses_model.h"
#include "core/native.h"
#include "descriptor/descriptor.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace sp::responses {
namespace {
bool count_value(json::Value v) { return v.is_uint() || (v.is_int() && v.as_int() >= 0); }
uint64_t number(json::Value v) { return v.is_uint() ? v.as_uint() : static_cast<uint64_t>(v.as_int()); }
bool nonempty(json::Value v) { return v.is_string() && !v.as_string().empty(); }
bool nullable_string(json::Value v) { return !v.valid() || v.is_null() || v.is_string(); }
bool item_status(json::Value v, bool optional, bool final) {
  if (!v.valid() || v.is_null()) return optional;
  if (!v.is_string()) return false;
  return v.as_string() == "completed" || v.as_string() == "incomplete" || (!final && v.as_string() == "in_progress");
}
bool opaque(std::string_view type) {
  return type != "message" && type != "function_call" && type != "reasoning";
}
// Opaque server execution is observable metadata, never a host tool invocation.
bool server_event(std::string_view type) {
  return type == "response.web_search_call.in_progress" || type == "response.web_search_call.searching" || type == "response.web_search_call.completed" ||
         type == "response.file_search_call.in_progress" || type == "response.file_search_call.searching" || type == "response.file_search_call.completed" ||
         type == "response.code_interpreter_call.in_progress" || type == "response.code_interpreter_call.interpreting" || type == "response.code_interpreter_call.completed" ||
         type == "response.mcp_call.in_progress" || type == "response.mcp_call.completed" || type == "response.mcp_call.failed" ||
         type == "response.mcp_list_tools.in_progress" || type == "response.mcp_list_tools.completed" || type == "response.mcp_list_tools.failed" ||
         type == "response.image_generation_call.in_progress" || type == "response.image_generation_call.generating" ||
         type == "response.image_generation_call.partial_image" || type == "response.image_generation_call.completed";
}
uint64_t order(uint64_t output, uint64_t content = 0) { return (output << 32) | content; }
bool valid_annotation(json::Value value) {
  if (!value.is_object() || !nonempty(value.get("type"))) return false;
  const auto type = value.get("type").as_string();
  if (type == "file_citation" || type == "file_path")
    return value.get("file_id").is_string() && count_value(value.get("index")) &&
        (type == "file_path" || value.get("filename").is_string());
  if (type == "url_citation" || type == "container_file_citation") {
    if (!count_value(value.get("start_index")) || !count_value(value.get("end_index")) ||
        number(value.get("start_index")) > number(value.get("end_index"))) return false;
    return type == "url_citation" ? value.get("url").is_string() && value.get("title").is_string() :
        value.get("container_id").is_string() && value.get("file_id").is_string() && value.get("filename").is_string();
  }
  return false;
}
bool valid_annotations(json::Value value) {
  if (!value.is_array()) return false;
  for (auto element : value.elements()) if (!valid_annotation(element)) return false;
  return true;
}
bool valid_logprob(json::Value value) {
  if (!value.is_object() || !value.get("token").is_string() || !value.get("bytes").is_array() ||
      !value.get("logprob").is_number() || !std::isfinite(value.get("logprob").as_double())) return false;
  for (auto byte : value.get("bytes").elements()) if (!count_value(byte) || number(byte) > 255) return false;
  return true;
}
bool valid_logprobs(json::Value value) {
  if (!value.valid() || value.is_null()) return true;
  if (!value.is_array()) return false;
  for (auto element : value.elements()) {
    if (!valid_logprob(element) || !element.get("top_logprobs").is_array()) return false;
    for (auto top : element.get("top_logprobs").elements()) if (!valid_logprob(top)) return false;
  }
  return true;
}
bool equal_completed_item(json::Value done, json::Value terminal) {
  if (done.get("type").as_string() != "reasoning") return json::equal(done, terminal);
  if (!terminal.is_object() || !nullable_string(terminal.get("encrypted_content"))) return false;
  // Completed item.done is the documented streaming replay authority. The
  // terminal envelope may carry another opaque encrypted representation.
  for (auto member : done.members())
    if (member.key != "encrypted_content" && !json::equal(member.value, terminal.get(member.key))) return false;
  for (auto member : terminal.members())
    if (member.key != "encrypted_content" && !done.get(member.key).valid()) return false;
  return true;
}
}
Codec::Codec(const descriptor::ValidatedDescriptor& d, Mode mode, Accumulator& a,
             std::shared_ptr<const NativeContext> context, SemanticLimits limits)
    : descriptor_(d), mode_(mode), accumulator_(a), context_(std::move(context)),
      context_valid_(context_ && context_->matches_descriptor(d)), limits_(limits) {}
bool Codec::emit(const Event& event) { return accumulator_.accept(event, buffered_failure_ ? &*buffered_failure_ : nullptr); }
bool Codec::fail(ErrorKind kind, std::string message) {
  if (buffered_failure_ && !closed_) return false;
  emit(Fail{{kind, std::move(message)}}); return false;
}
void Codec::unknown(json::Value v, std::initializer_list<std::string_view> known) {
  for (auto m : v.members()) if (std::find(known.begin(), known.end(), m.key) == known.end()) ++diagnostics_.unknown_properties;
}
std::shared_ptr<const json::Document> Codec::own(json::Value v) {
  const json::Limits bounds{limits_.max_content_bytes, limits_.max_json_depth};
  json::BoundedWriter measure(bounds); measure.value(v);
  if (!measure.ok() || measure.size() > limits_.max_content_bytes - std::min(retained_bytes_, limits_.max_content_bytes)) {
    fail(ErrorKind::ResourceLimit, "retained response metadata limit"); return {};
  }
  std::string bytes; bytes.reserve(measure.size());
  json::BoundedWriter writer(bounds, &bytes); writer.value(v);
  auto parsed = json::parse(bytes, bounds);
  if (!writer.ok() || !std::holds_alternative<json::Document>(parsed)) { fail(ErrorKind::ProtocolCorrupt, "invalid retained response metadata"); return {}; }
  retained_bytes_ += measure.size();
  return std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
}
std::shared_ptr<const json::Document> Codec::own_completed_output() {
  const json::Limits bounds{limits_.max_content_bytes, limits_.max_json_depth};
  auto build = [&](json::BoundedWriter& writer) {
    writer.raw("[");
    bool comma = false;
    for (const auto& [position, item] : items_) {
      (void)position;
      if (comma) writer.raw(",");
      comma = true;
      writer.value(item.done->root(), 1);
    }
    writer.raw("]");
  };
  json::BoundedWriter measure(bounds);
  build(measure);
  if (!measure.ok() || measure.size() > limits_.max_content_bytes - std::min(retained_bytes_, limits_.max_content_bytes)) {
    fail(ErrorKind::ResourceLimit, "retained response metadata limit"); return {};
  }
  std::string bytes; bytes.reserve(measure.size());
  json::BoundedWriter writer(bounds, &bytes);
  build(writer);
  auto parsed = json::parse(bytes, bounds);
  if (!writer.ok() || !std::holds_alternative<json::Document>(parsed)) {
    fail(ErrorKind::ProtocolCorrupt, "invalid retained response metadata"); return {};
  }
  retained_bytes_ += measure.size();
  return std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
}
bool Codec::buffered(std::string_view body, Close close) {
  if (closed_ || accumulator_.terminal()) return false;
  if (mode_ != Mode::Buffered || body_seen_) return fail(ErrorKind::Misuse, "unexpected buffered response");
  body_seen_ = true;
  if (!close.normal) buffered_failure_ = Error{close.error, "abnormal response close"};
  const bool decoded = document(body, {}, false);
  finish(close);
  return decoded && accumulator_.outcome() && std::holds_alternative<Completion>(*accumulator_.outcome());
}
bool Codec::frame(std::string_view event, std::string_view data) {
  if (closed_ || accumulator_.terminal()) return false;
  if (mode_ != Mode::Sse) return fail(ErrorKind::Misuse, "unexpected stream frame");
  if (data == "[DONE]" && event != "error")
    return terminal_ ? true : fail(ErrorKind::Truncated, "Responses sentinel lacks terminal envelope");
  return document(data, event, true);
}
bool Codec::index(json::Value root, std::string_view key, uint64_t& target) {
  auto v = root.get(key);
  if (!count_value(v)) return fail(ErrorKind::ProtocolCorrupt, "event index required");
  target = number(v);
  if (target >= limits_.max_parts || target >= std::numeric_limits<uint32_t>::max()) return fail(ErrorKind::ResourceLimit, "response index limit");
  return true;
}
Codec::Item* Codec::event_item(json::Value root, uint64_t& position) {
  if (!index(root, "output_index", position)) return nullptr;
  auto found = items_.find(position);
  if (found == items_.end() || found->second.closed || !nonempty(root.get("item_id")) || root.get("item_id").as_string() != found->second.id) {
    fail(ErrorKind::ProtocolCorrupt, "event item identity or lifecycle mismatch"); return nullptr;
  }
  return &found->second;
}
bool Codec::document(std::string_view bytes, std::string_view event, bool streaming) {
  const bool named_error = streaming && event == "error";
  if (!named_error && !context_valid_) return fail(ErrorKind::InvalidConfig, "Responses request context required");
  if (bytes.size() > limits_.max_content_bytes - std::min(input_bytes_, limits_.max_content_bytes))
    return fail(named_error ? ErrorKind::RemoteFailure : ErrorKind::ResourceLimit,
                named_error ? "remote Responses error payload" : "response byte limit");
  input_bytes_ += bytes.size();
  auto parsed = json::parse(bytes, {limits_.max_content_bytes, limits_.max_json_depth});
  if (auto* e = std::get_if<json::ParseError>(&parsed))
    return fail(named_error ? ErrorKind::RemoteFailure :
                e->code == json::ParseCode::SizeExceeded || e->code == json::ParseCode::DepthExceeded ? ErrorKind::ResourceLimit : ErrorKind::ProtocolCorrupt,
                named_error ? "remote Responses error payload" : "invalid response JSON");
  auto wire = std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
  const auto root = wire->root();
  if (!root.is_object())
    return fail(named_error ? ErrorKind::RemoteFailure : ErrorKind::ProtocolCorrupt,
                named_error ? "remote Responses error payload" : "response object required");
  const auto type = root.get("type");
  std::string wire_type = named_error ? std::string(event) : streaming
      ? (nonempty(type) ? std::string(type.as_string()) : std::string(event))
      : "response";
  // The SSE error name is authoritative even when its payload type is absent
  // or contradictory, and remains so if raw retention rejects the document.
  if (named_error) {
    const Error remote{ErrorKind::RemoteFailure, "remote Responses error payload"};
    if (!accumulator_.accept(RawWire{std::move(wire_type), wire}, &remote)) return false;
    return fail(remote.kind, remote.safe_message);
  }
  if (!emit(RawWire{std::move(wire_type), wire})) return false;
  if (!streaming) {
    if (!emit(ResponseEnvelope{wire})) return false;
    return response(root, {}, false);
  }
  if (!nonempty(type)) return fail(ErrorKind::ProtocolCorrupt, "event type required");
  // Errors remain authoritative until the actual HTTP stream closes.
  if (type.as_string() == "error") return fail(ErrorKind::RemoteFailure, "remote Responses error payload");
  if (!event.empty() && event != "message" && type.as_string() != event)
    return fail(ErrorKind::ProtocolCorrupt, "SSE event and payload type disagree");
  event = type.as_string();
  const auto envelope = root.get("response");
  const bool response_event = event == "response.created" || event == "response.in_progress" ||
      event == "response.completed" || event == "response.incomplete" || event == "response.done" ||
      event == "response.failed" || event == "response.cancelled";
  if (response_event && envelope.is_object()) {
    auto owned = own(envelope);
    if (!owned || !emit(ResponseEnvelope{std::move(owned)})) return false;
  }
  auto seq = root.get("sequence_number");
  if (seq.valid()) {
    if (!count_value(seq) || (sequence_ && number(seq) <= *sequence_)) return fail(ErrorKind::ProtocolCorrupt, "event sequence regression or duplicate");
    sequence_ = number(seq);
  }
  if (event == "response.failed" || event == "response.cancelled") {
    auto r = root.get("response");
    if (r.is_object() && !response(r, event, true)) return false;
    return fail(event == "response.cancelled" ? ErrorKind::Cancelled : ErrorKind::RemoteFailure, "remote response failure");
  }
  const bool supported = server_event(event) || event == "response.function_call_arguments.delta" || event == "response.function_call_arguments.done" ||
      event == "response.content_part.added" || event == "response.content_part.done" || event == "response.reasoning_summary_part.added" || event == "response.reasoning_summary_part.done" ||
      event == "response.output_text.delta" || event == "response.output_text.done" || event == "response.refusal.delta" || event == "response.refusal.done" ||
      event == "response.reasoning_summary_text.delta" || event == "response.reasoning_summary_text.done" || event == "response.reasoning_text.delta" || event == "response.reasoning_text.done" ||
      event == "response.output_text.annotation.added";
  const bool lifecycle = event == "response.created" || event == "response.in_progress" ||
      event == "response.completed" || event == "response.incomplete" || event == "response.done";
  const bool output_item = event == "response.output_item.added" || event == "response.output_item.done";
  // Raw ownership does not establish that an undeclared event is semantically harmless.
  if (!supported && !lifecycle && !output_item)
    return fail(ErrorKind::Unsupported, "unsupported semantic Responses event");
  if (terminal_) return fail(ErrorKind::ProtocolCorrupt, "output after response terminal");
  if (lifecycle) return response(root.get("response"), event, true);
  if (!begun_) return fail(ErrorKind::ProtocolCorrupt, "event before response.created");
  uint64_t position;
  if (output_item) {
    if (!index(root, "output_index", position)) return false;
    return event == "response.output_item.added" ? add_item(root.get("item"), position, true) : done_item(root.get("item"), position, true);
  }
  Item* item = event_item(root, position);
  if (!item) return false;
  if (event == "response.function_call_arguments.delta" || event == "response.function_call_arguments.done") {
    if (item->type != "function_call" || item->arguments_done) return fail(ErrorKind::ProtocolCorrupt, "arguments event lifecycle mismatch");
    const bool done = event.ends_with(".done");
    auto text = root.get(done ? "arguments" : "delta");
    if (!text.is_string()) return fail(ErrorKind::ProtocolCorrupt, "argument string required");
    if (done) {
      if (!text.as_string().starts_with(item->arguments)) return fail(ErrorKind::ProtocolCorrupt, "argument snapshot contradicts prefix");
      if (text.as_string().size() > limits_.max_tool_bytes) return fail(ErrorKind::ResourceLimit, "tool argument limit");
      if (!emit(PartDelta{item->local, {PartKind::ToolCall, text.as_string().substr(item->arguments.size())}})) return false;
      if (item->arguments.size() != text.as_string().size()) item->arguments.assign(text.as_string());
      item->arguments_done = true;
    } else {
      if (text.as_string().size() > limits_.max_tool_bytes - std::min(item->arguments.size(), limits_.max_tool_bytes)) return fail(ErrorKind::ResourceLimit, "tool argument limit");
      item->arguments.append(text.as_string());
      if (!emit(PartDelta{item->local, {PartKind::ToolCall, text.as_string()}})) return false;
    }
    return true;
  }
  if (event == "response.content_part.added" || event == "response.content_part.done" || event == "response.reasoning_summary_part.added" || event == "response.reasoning_summary_part.done") {
    const bool summary = event.starts_with("response.reasoning_summary");
    uint64_t content;
    if (!index(root, summary ? "summary_index" : "content_index", content)) return false;
    return part(*item, content, root.get("part"), summary, event.ends_with(".done"));
  }
  if (event == "response.output_text.delta" || event == "response.output_text.done" || event == "response.refusal.delta" || event == "response.refusal.done" ||
      event == "response.reasoning_summary_text.delta" || event == "response.reasoning_summary_text.done" || event == "response.reasoning_text.delta" || event == "response.reasoning_text.done") return text_event(*item, root, event);
  if (event == "response.output_text.annotation.added") {
    uint64_t content, annotation;
    if (item->type != "message") return fail(ErrorKind::ProtocolCorrupt, "annotation event on non-message item");
    if (!index(root, "content_index", content) || !index(root, "annotation_index", annotation)) return false;
    auto found = item->content.find(content);
    if (found == item->content.end() || found->second.part_done || found->second.type != "output_text" ||
        !valid_annotation(root.get("annotation")) || found->second.annotations.contains(annotation))
      return fail(ErrorKind::ProtocolCorrupt, "invalid annotation lifecycle or metadata");
    auto metadata = own(root.get("annotation")); if (!metadata) return false;
    found->second.annotations.emplace(annotation, std::move(metadata)); return true;
  }
  if (server_event(event)) {
    const auto prefix = std::string("response.") + item->type + ".";
    if (!opaque(item->type) || !event.starts_with(prefix)) return fail(ErrorKind::ProtocolCorrupt, "server event item type mismatch");
    // A preview frame is an observation retained as raw wire; the finished image arrives with the
    // completed item, so its payload is only checked for shape here.
    if (event == "response.image_generation_call.partial_image" &&
        (!root.get("partial_image_b64").is_string() || !count_value(root.get("partial_image_index"))))
      return fail(ErrorKind::ProtocolCorrupt, "partial image requires payload and index");
    return true;
  }
  return fail(ErrorKind::Unsupported, "unsupported semantic Responses event");
}
bool Codec::identity(json::Value v) {
  auto id = v.get("id"), object = v.get("object"), model = v.get("model"), created = v.get("created_at");
  if (!nonempty(id) || !object.is_string() || object.as_string() != "response" || !nonempty(model) || !created.is_number() || !std::isfinite(created.as_double()) || created.as_double() < 0)
    return fail(ErrorKind::ProtocolCorrupt, "invalid response identity metadata");
  if (!requested_model_matches(context_->model(), model.as_string(), descriptor_.base_url()))
    return fail(ErrorKind::ProtocolCorrupt, "response model differs from request");
  if (begun_ && (id.as_string() != generation_ || model.as_string() != model_ || created.as_double() != created_at_)) return fail(ErrorKind::ProtocolCorrupt, "response identity changed");
  return true;
}
bool Codec::response(json::Value v, std::string_view event, bool streaming) {
  if (!v.is_object()) return fail(ErrorKind::ProtocolCorrupt, "response payload required");
  auto error = v.get("error");
  if (error.valid() && !error.is_null() && !error.is_object()) return fail(ErrorKind::ProtocolCorrupt, "invalid response error metadata");
  // A standalone HTTP-200 error envelope need not contain response identity.
  if (error.is_object() && !v.get("id").valid()) return fail(ErrorKind::RemoteFailure, "remote error response");
  if (!identity(v)) return false;
  auto status = v.get("status"), output = v.get("output"), details = v.get("incomplete_details");
  if (!nonempty(status) || !output.is_array() || (details.valid() && !details.is_null() && !details.is_object())) return fail(ErrorKind::ProtocolCorrupt, "invalid response status or output");
  const auto state = status.as_string();
  const bool final = state == "completed" || state == "incomplete";
  if (state == "failed" || state == "cancelled" || error.is_object()) {
    if (!begun_) {
      generation_ = v.get("id").as_string(); model_ = v.get("model").as_string(); created_at_ = v.get("created_at").as_double();
      if (!emit(Begin{generation_}) || !emit(MessageBegin{{0}, generation_, Role::Assistant, context_})) return false;
      begun_ = true;
      uint64_t n = 0;
      for (auto item : output.elements()) {
        if (!add_item(item, n, true)) return false;
        auto state = item.get("status");
        if ((!state.valid() || state.is_null() || (state.is_string() && state.as_string() != "in_progress")) && !done_item(item, n, false)) return false;
        ++n;
      }
    }
    if (!usage(usage_at(v))) return false;
    return fail(state == "cancelled" ? ErrorKind::Cancelled : ErrorKind::RemoteFailure, "remote response failure status");
  }
  if (!final && state != "in_progress" && state != "queued") return fail(ErrorKind::Unsupported, "unsupported response status");
  if (!streaming && !final) return fail(ErrorKind::Truncated, "buffered response is not terminal");
  if (streaming && event == "response.done" && !final)
    return fail(ErrorKind::ProtocolCorrupt, "response.done requires a terminal response envelope");
  if (streaming && ((event == "response.completed" && state != "completed") || (event == "response.incomplete" && state != "incomplete") ||
      (event == "response.in_progress" && state != "in_progress") ||
      ((event == "response.created" || event == "response.in_progress") && final))) return fail(ErrorKind::ProtocolCorrupt, "event and response status disagree");
  if (!final && details.is_object()) return fail(ErrorKind::ProtocolCorrupt, "incomplete details before terminal");
  if (state == "completed" && details.is_object()) return fail(ErrorKind::ProtocolCorrupt, "completed response has incomplete details");
  StopReason stop;
  if (state == "incomplete") {
    if (!details.is_object() || !nonempty(details.get("reason"))) return fail(ErrorKind::ProtocolCorrupt, "incomplete reason required");
    const auto reason = details.get("reason").as_string();
    if (reason != "max_output_tokens" && reason != "content_filter") return fail(ErrorKind::Unsupported, "unsupported incomplete reason");
    stop = {reason == "max_output_tokens" ? StopKind::MaxTokens : StopKind::ContentFilter, std::string(reason)};
    stop.details = own(details); if (!stop.details) return false;
  }
  if (!begun_) {
    if (streaming && event != "response.created") return fail(ErrorKind::ProtocolCorrupt, "response.created required");
    generation_ = v.get("id").as_string(); model_ = v.get("model").as_string(); created_at_ = v.get("created_at").as_double();
    if (!emit(Begin{generation_}) || !emit(MessageBegin{{0}, generation_, Role::Assistant, context_})) return false;
    begun_ = true;
  } else if (event == "response.created") return fail(ErrorKind::ProtocolCorrupt, "duplicate response.created");
  if (status_ == "in_progress" && state == "queued") return fail(ErrorKind::ProtocolCorrupt, "response status regressed");
  status_ = state;
  const auto reported_usage = usage_at(v);
  if (final && (!reported_usage.valid() || reported_usage.is_null())) {
    usage_ = {};
    if (!emit(UsageUpdate{usage_})) return false;
  } else if (!usage(reported_usage)) return false;
  unknown(v, {"id", "object", "model", "created_at", "status", "error", "incomplete_details", "output", "usage", "store", "instructions", "tools", "tool_choice", "parallel_tool_calls", "reasoning", "max_output_tokens", "temperature", "top_p", "text", "metadata", "previous_response_id", "service_tier", "background", "truncation", "user", "safety_identifier", "prompt_cache_key"});
  if (!final) {
    if (output.size()) return fail(ErrorKind::ProtocolCorrupt, "initial response output must be empty");
    return true;
  }
  if (streaming) {
    if (output.size() != items_.size()) return fail(ErrorKind::ProtocolCorrupt, "aggregate output item count differs");
    uint64_t position = 0;
    for (auto item : output.elements()) {
      auto found = items_.find(position++);
      if (found == items_.end() || !found->second.closed || !found->second.done ||
          !equal_completed_item(found->second.done->root(), item))
        return fail(ErrorKind::ProtocolCorrupt, "aggregate output contradicts done item");
    }
  } else {
    uint64_t position = 0;
    for (auto item : output.elements()) if (!add_item(item, position++, false)) return false;
  }
  if (state == "completed") {
    for (const auto& [position, item] : items_) {
      (void)position;
      auto item_state = item.done->root().get("status");
      if (!opaque(item.type) && item_state.is_string() && item_state.as_string() != "completed") return fail(ErrorKind::ProtocolCorrupt, "completed response contains incomplete item");
    }
    stop = {function_seen_ ? StopKind::ToolUse : refusal_seen_ ? StopKind::Refusal : StopKind::EndTurn, "completed"};
  }
  auto captured = streaming ? own_completed_output() : own(output); if (!captured) return false;
  if (!emit(Stop{std::move(stop)}) || !emit(MessageSeal{{0}, std::move(captured)})) return false;
  terminal_ = true; return true;
}
bool Codec::begin_part(LocalId& id, PartKind kind, PartHeader header, uint64_t position) {
  if (next_part_ > limits_.max_parts || next_part_ == std::numeric_limits<uint32_t>::max()) return fail(ErrorKind::ResourceLimit, "response part limit");
  id = {next_part_++};
  return emit(PartBegin{{0}, id, kind, std::move(header), position});
}
bool Codec::item_fields(json::Value v, bool final) {
  if (!v.is_object() || !nonempty(v.get("type")) || !nonempty(v.get("id"))) return fail(ErrorKind::ProtocolCorrupt, "output item identity required");
  const auto type = v.get("type").as_string();
  if (type == "message") {
    auto phase = v.get("phase");
    if (!v.get("role").is_string() || v.get("role").as_string() != "assistant" || !v.get("content").is_array() || !item_status(v.get("status"), false, final) ||
        !nullable_string(phase) || (phase.is_string() && phase.as_string() != "commentary" && phase.as_string() != "final_answer")) return fail(ErrorKind::ProtocolCorrupt, "invalid message item fields");
    unknown(v, {"id", "type", "role", "status", "content", "phase"});
  } else if (type == "function_call") {
    if (!nonempty(v.get("call_id")) || !nonempty(v.get("name")) || !v.get("arguments").is_string() || !item_status(v.get("status"), true, final)) return fail(ErrorKind::ProtocolCorrupt, "invalid function item fields");
    if (v.get("arguments").as_string().size() > limits_.max_tool_bytes) return fail(ErrorKind::ResourceLimit, "tool argument limit");
    unknown(v, {"id", "type", "call_id", "name", "arguments", "status"});
  } else if (type == "reasoning") {
    if (!v.get("summary").is_array() || !nullable_string(v.get("encrypted_content")) || !item_status(v.get("status"), true, final) ||
        (v.get("content").valid() && !v.get("content").is_null() && !v.get("content").is_array())) return fail(ErrorKind::ProtocolCorrupt, "invalid reasoning item fields");
    unknown(v, {"id", "type", "summary", "content", "encrypted_content", "status"});
  } else if (opaque(type)) {
    auto status = v.get("status");
    if (status.valid() && !nonempty(status)) return fail(ErrorKind::ProtocolCorrupt, "invalid server item status");
    if (status.is_string()) {
      const auto state = status.as_string();
      if (state.empty()) return fail(ErrorKind::ProtocolCorrupt, "empty server item status");
      if (final && (state == "in_progress" || state == "searching" || state == "interpreting"))
        return fail(ErrorKind::ProtocolCorrupt, "nonterminal server item at done");
    }
    // The entire server item remains immutable in native output and Opaque.
  } else return fail(ErrorKind::Unsupported, "unsupported Responses output item type");
  return true;
}
bool Codec::initial_metadata(json::Value initial, json::Value final, std::initializer_list<std::string_view> changing) {
  for (auto m : initial.members()) {
    if (std::find(changing.begin(), changing.end(), m.key) != changing.end()) continue;
    if (m.key == "phase" && m.value.is_null()) continue;
    if (!json::equal(m.value, final.get(m.key))) return fail(ErrorKind::ProtocolCorrupt, "item metadata changed or disappeared");
  }
  return true;
}
bool Codec::add_item(json::Value v, uint64_t position, bool streaming) {
  if (position >= limits_.max_parts || items_.size() >= limits_.max_parts) return fail(ErrorKind::ResourceLimit, "output item limit");
  if (items_.contains(position) || !item_fields(v, !streaming)) return items_.contains(position) ? fail(ErrorKind::ProtocolCorrupt, "duplicate output index") : false;
  Item item; item.id = v.get("id").as_string(); item.type = v.get("type").as_string();
  item.index = position;
  if (!ids_.insert(item.id).second) return fail(ErrorKind::ProtocolCorrupt, "duplicate output item id");
  if (streaming) { item.added = own(v); if (!item.added) return false; }
  PartHeader header; header.wire_type = item.type; header.wire_id = item.id;
  if (item.type == "function_call") {
    item.call_id = v.get("call_id").as_string(); item.name = v.get("name").as_string();
    if (!call_ids_.insert(item.call_id).second) return fail(ErrorKind::ProtocolCorrupt, "duplicate function call id");
    header.wire_id = item.call_id; header.name = item.name; function_seen_ = true;
    if (!begin_part(item.local, PartKind::ToolCall, std::move(header), order(position))) return false;
    item.arguments = v.get("arguments").as_string();
    if (!emit(PartDelta{item.local, {PartKind::ToolCall, item.arguments}})) return false;
  } else if (item.type == "reasoning" || (opaque(item.type) && item.type != "image_generation_call")) {
    if (!begin_part(item.local, item.type == "reasoning" ? PartKind::Reasoning : PartKind::Opaque, std::move(header), order(position))) return false;
  }
  auto [where, inserted] = items_.emplace(position, std::move(item)); (void)inserted;
  auto& stored = where->second;
  if (stored.type == "message" || stored.type == "reasoning") {
    uint64_t n = 0;
    auto content = v.get(stored.type == "message" ? "content" : "summary");
    for (auto p : content.elements()) if (!part(stored, n++, p, stored.type == "reasoning", false)) return false;
    if (stored.type == "reasoning" && v.get("content").is_array()) {
      n = 0; for (auto p : v.get("content").elements()) if (!part(stored, n++, p, false, false)) return false;
    }
  }
  return streaming || done_item(v, position, false);
}
bool Codec::reconcile(TextState& text, std::string_view snapshot) {
  if (text.text_done ? snapshot != text.bytes : !snapshot.starts_with(text.bytes)) return fail(ErrorKind::ProtocolCorrupt, "text snapshot contradicts streamed prefix");
  if (text.bytes.size() != snapshot.size()) text.bytes.assign(snapshot);
  return true;
}
bool Codec::part(Item& item, uint64_t position, json::Value v, bool summary, bool done) {
  if (position >= limits_.max_parts) return fail(ErrorKind::ResourceLimit, "content index limit");
  if (!v.is_object() || !nonempty(v.get("type"))) return fail(ErrorKind::ProtocolCorrupt, "content part type required");
  const auto type = v.get("type").as_string();
  if ((summary && (item.type != "reasoning" || type != "summary_text")) || (!summary && item.type == "reasoning" && type != "reasoning_text") ||
      (!summary && item.type != "reasoning" && (item.type != "message" || (type != "output_text" && type != "refusal")))) return fail(ErrorKind::Unsupported, "unsupported content part type");
  auto bytes = v.get(type == "refusal" ? "refusal" : "text");
  if (!bytes.is_string()) return fail(ErrorKind::ProtocolCorrupt, "content text required");
  if (type == "output_text") {
    auto annotations = v.get("annotations"), logprobs = v.get("logprobs");
    if (!valid_annotations(annotations) || !valid_logprobs(logprobs))
      return fail(ErrorKind::ProtocolCorrupt, "invalid output text metadata");
  }
  auto& parts = summary ? item.summary : item.content;
  auto found = parts.find(position);
  if (!done) {
    if (found != parts.end()) return fail(ErrorKind::ProtocolCorrupt, "duplicate content part index");
    TextState text; text.type = type; text.kind = type == "refusal" ? PartKind::Refusal : PartKind::Text;
    text.bytes = bytes.as_string(); text.added = own(v); if (!text.added) return false;
    if (item.type == "message") {
      PartHeader header; header.wire_id = item.id; header.wire_type = type;
      if (!begin_part(text.local, text.kind, std::move(header), order(item.index, position)) || !emit(PartDelta{text.local, {text.kind, text.bytes}})) return false;
      refusal_seen_ = refusal_seen_ || type == "refusal";
    }
    parts.emplace(position, std::move(text));
    return !summary || emit_summary(item);
  }
  if (found == parts.end() || found->second.part_done || found->second.type != type) return fail(ErrorKind::ProtocolCorrupt, "content part done lifecycle mismatch");
  auto& text = found->second;
  if (!reconcile(text, bytes.as_string()) || !initial_metadata(text.added->root(), v, {"text", "refusal", "annotations", "logprobs"})) return false;
  auto annotations = v.get("annotations");
  for (const auto& [n, metadata] : text.annotations) if (!annotations.is_array() || n >= annotations.size() || !json::equal(metadata->root(), annotations.at(n))) return fail(ErrorKind::ProtocolCorrupt, "annotation snapshot contradiction");
  auto original_annotations = text.added->root().get("annotations");
  if (original_annotations.is_array()) for (size_t n = 0; n < original_annotations.size(); ++n) if (!annotations.is_array() || n >= annotations.size() || !json::equal(original_annotations.at(n), annotations.at(n))) return fail(ErrorKind::ProtocolCorrupt, "initial annotation contradiction");
  if (!logprobs(text, v.get("logprobs"), true)) return false;
  text.done = own(v); if (!text.done) return false;
  text.part_done = true; text.text_done = true;
  if (item.type == "message") {
    if (!emit(PartSeal{text.local, std::string_view(text.bytes)})) return false;
  }
  return !summary || emit_summary(item);
}
bool Codec::text_event(Item& item, json::Value root, std::string_view event) {
  const bool summary = event.starts_with("response.reasoning_summary_text");
  const bool reasoning = summary || event.starts_with("response.reasoning_text");
  const bool refusal = event.starts_with("response.refusal");
  if ((reasoning && item.type != "reasoning") || (!reasoning && item.type != "message")) return fail(ErrorKind::ProtocolCorrupt, "text event item type mismatch");
  uint64_t position;
  if (!index(root, summary ? "summary_index" : "content_index", position)) return false;
  auto& parts = summary ? item.summary : item.content;
  auto found = parts.find(position);
  if (found == parts.end()) return fail(ErrorKind::ProtocolCorrupt, "text event before part added");
  auto& text = found->second;
  const auto expected = reasoning ? summary ? "summary_text" : "reasoning_text" : refusal ? "refusal" : "output_text";
  if (text.type != expected || text.text_done || text.part_done) return fail(ErrorKind::ProtocolCorrupt, "text event lifecycle mismatch");
  const bool done = event.ends_with(".done");
  auto bytes = root.get(done ? refusal ? "refusal" : "text" : "delta");
  if (!bytes.is_string()) return fail(ErrorKind::ProtocolCorrupt, "text event string required");
  if (!reasoning && !refusal) {
    auto probabilities = root.get("logprobs");
    if (probabilities.valid() && !probabilities.is_null()) {
      if (!valid_logprobs(probabilities)) return fail(ErrorKind::ProtocolCorrupt, "invalid text event logprobs");
      if (done) {
        if (!logprobs(text, probabilities, false)) return false;
        text.logprobs_done = own(probabilities); if (!text.logprobs_done) return false;
      } else if (probabilities.size()) {
        auto metadata = own(probabilities); if (!metadata) return false;
        text.logprob_deltas.push_back(std::move(metadata));
      }
    }
  }
  if (done) {
    if (!reasoning && bytes.as_string().starts_with(text.bytes) && !emit(PartDelta{text.local, {text.kind, bytes.as_string().substr(text.bytes.size())}})) return false;
    if (!reconcile(text, bytes.as_string())) return false;
    text.text_done = true;
  } else {
    if (bytes.as_string().size() > limits_.max_content_bytes - std::min(text.bytes.size(), limits_.max_content_bytes)) return fail(ErrorKind::ResourceLimit, "text byte limit");
    text.bytes.append(bytes.as_string());
    if (!reasoning && !emit(PartDelta{text.local, {text.kind, bytes.as_string()}})) return false;
  }
  return !summary || emit_summary(item);
}
bool Codec::emit_summary(Item& item) {
  uint64_t expected = 0;
  size_t offset = 0;
  for (const auto& [position, text] : item.summary) {
    if (position != expected++) break;
    if (offset > limits_.max_content_bytes || text.bytes.size() > limits_.max_content_bytes - offset) return fail(ErrorKind::ResourceLimit, "reasoning summary limit");
    const auto end = offset + text.bytes.size();
    if (end > item.summary_emitted) {
      const auto start = item.summary_emitted > offset ? item.summary_emitted - offset : 0;
      if (!emit(PartDelta{item.local, {PartKind::Reasoning, std::string_view(text.bytes).substr(start)}})) return false;
      item.summary_emitted = end;
    }
    offset = end;
    if (!text.text_done) break;
  }
  return true;
}
bool Codec::logprobs(TextState& text, json::Value final, bool complete) {
  auto initial = text.added->root().get("logprobs");
  size_t position = 0;
  auto prefix = [&](json::Value probabilities) {
    if (!probabilities.is_array()) return true;
    for (auto value : probabilities.elements()) {
      if (!final.is_array() || position >= final.size() || !json::equal(value, final.at(position++))) return false;
    }
    return true;
  };
  if (!prefix(initial)) return fail(ErrorKind::ProtocolCorrupt, "initial logprobs contradict snapshot");
  for (const auto& probabilities : text.logprob_deltas) if (!prefix(probabilities->root())) return fail(ErrorKind::ProtocolCorrupt, "delta logprobs contradict snapshot");
  if (complete && text.logprobs_done && !json::equal(text.logprobs_done->root(), final)) return fail(ErrorKind::ProtocolCorrupt, "done logprobs contradict part snapshot");
  return true;
}
bool Codec::reconcile_parts(Item& item, json::Value array, bool summary, bool streaming) {
  auto& parts = summary ? item.summary : item.content;
  if (!array.valid() || array.is_null()) return parts.empty() || fail(ErrorKind::ProtocolCorrupt, "content disappeared at item done");
  if (!array.is_array()) return fail(ErrorKind::ProtocolCorrupt, "item content array required");
  (void)streaming;
  if (array.size() < parts.size()) return fail(ErrorKind::ProtocolCorrupt, "done item content count differs");
  uint64_t position = 0;
  for (auto v : array.elements()) {
    auto found = parts.find(position);
    if (found == parts.end()) {
      if (!part(item, position, v, summary, false)) return false;
      found = parts.find(position);
    }
    auto& text = found->second;
    if (text.part_done) {
      if (!json::equal(text.done->root(), v)) return fail(ErrorKind::ProtocolCorrupt, "item done contradicts content done");
    } else {
      if (!part(item, position, v, summary, true)) return false;
    }
    ++position;
  }
  if (parts.size() != array.size()) return fail(ErrorKind::ProtocolCorrupt, "content index gap");
  return true;
}
bool Codec::done_item(json::Value v, uint64_t position, bool streaming) {
  auto found = items_.find(position);
  if (found == items_.end() || found->second.closed) return fail(ErrorKind::ProtocolCorrupt, "output item done lifecycle mismatch");
  auto& item = found->second;
  if (!item_fields(v, true)) return false;
  if (v.get("id").as_string() != item.id || v.get("type").as_string() != item.type) return fail(ErrorKind::ProtocolCorrupt, "output item done identity mismatch");
  // Hosted/unknown items have provider-defined evolving payload fields. Their
  // identity and terminal status are checked; retain each full wire observation
  // and the final opaque snapshot without inventing field-level semantics.
  if (item.added && !opaque(item.type) &&
      !initial_metadata(item.added->root(), v, {"status", "content", "summary", "arguments", "encrypted_content", "output", "error", "code", "results", "action", "tools"})) return false;
  if (item.added) {
    auto previous = item.added->root().get("status"), next = v.get("status");
    if (previous.is_string() && (previous.as_string() == "completed" || previous.as_string() == "incomplete" || previous.as_string() == "failed") &&
        (!next.is_string() || previous.as_string() != next.as_string())) return fail(ErrorKind::ProtocolCorrupt, "item terminal status changed");
    if (item.type == "image_generation_call" && previous.is_string() &&
        (previous.as_string() == "completed" || previous.as_string() == "incomplete" || previous.as_string() == "failed") &&
        !json::equal(item.added->root(), v))
      return fail(ErrorKind::ProtocolCorrupt, "terminal image item snapshot changed");
  }
  if (item.type == "function_call") {
    auto args = v.get("arguments").as_string();
    if (v.get("call_id").as_string() != item.call_id || v.get("name").as_string() != item.name || (item.arguments_done ? args != item.arguments : !args.starts_with(item.arguments))) return fail(ErrorKind::ProtocolCorrupt, "function done snapshot contradiction");
    auto metadata = own(v); if (!metadata) return false;
    if (!emit(PartSeal{item.local, args, metadata})) return false;
    item.done = std::move(metadata);
  } else if (item.type == "reasoning") {
    if (!reconcile_parts(item, v.get("summary"), true, streaming) || !reconcile_parts(item, v.get("content"), false, streaming)) return false;
    auto metadata = own(v); if (!metadata) return false;
    if (!emit(PartSeal{item.local, std::nullopt, metadata})) return false;
    item.done = std::move(metadata);
  } else if (item.type == "message") {
    if (!reconcile_parts(item, v.get("content"), false, streaming)) return false;
    item.done = own(v); if (!item.done) return false;
  } else if (item.type == "image_generation_call") {
    // A finished image becomes typed media; an unfinished or failed call stays opaque.
    auto metadata = own(v); if (!metadata) return false;
    const auto result = v.get("result"), format = v.get("output_format"), status = v.get("status");
    PartHeader header; header.wire_type = item.type; header.wire_id = item.id;
    if (result.is_string() && !result.as_string().empty() && status.as_string() == "completed") {
      std::string_view mime;
      if (format.is_string()) {
        const auto name = format.as_string();
        mime = name == "png" ? "image/png" : name == "jpeg" ? "image/jpeg" : name == "webp" ? "image/webp" : std::string_view{};
      }
      header.media = media_output::describe(mime, MediaSource::Inline);
      header.media.kind = MediaKind::Image;
      header.media.id = std::string(item.id);
      if (!begin_part(item.local, PartKind::Media, std::move(header), order(position)) ||
          !emit(PartSeal{item.local, result.as_string(), nullptr})) return false;
    } else {
      if (!begin_part(item.local, PartKind::Opaque, std::move(header), order(position)) ||
          !emit(PartSeal{item.local, std::nullopt, metadata})) return false;
    }
    item.done = std::move(metadata);
  } else {
    auto metadata = own(v); if (!metadata) return false;
    if (!emit(PartSeal{item.local, std::nullopt, metadata})) return false;
    item.done = std::move(metadata);
  }
  item.closed = true; return true;
}
void Codec::conflict(std::string_view key, std::string_view detail) {
  usage_.quality = UsageQuality::Inconsistent;
  for (const auto& c : usage_.conflicts) if (c.counter == key && c.detail == detail) return;
  usage_.conflicts.push_back({std::string(key), std::string(detail)});
}
bool Codec::counter(json::Value v, std::optional<Count>& target, std::string_view key) {
  if (!v.valid() || v.is_null()) { target.reset(); return true; }
  if (!count_value(v)) return fail(ErrorKind::ProtocolCorrupt, "usage counter must be nonnegative integer or null");
  const auto n = number(v);
  if (target && n < target->value) conflict(key, "cumulative counter regressed");
  target = Count{n, Evidence::Reported}; return true;
}
bool Codec::usage_leaves(json::Value v, const std::string& prefix) {
  if (v.is_object()) {
    for (auto m : v.members()) {
      if (prefix.empty() && codecs::monetary_member(m.key)) continue;
      if (!usage_leaves(m.value, prefix.empty() ? std::string(m.key) : prefix + "." + std::string(m.key))) return false;
    }
  } else if (v.is_array()) {
    size_t n = 0; for (auto child : v.elements()) if (!usage_leaves(child, prefix + "." + std::to_string(n++))) return false;
  } else if (v.is_number()) {
    if (!count_value(v)) return fail(ErrorKind::ProtocolCorrupt, "numeric usage leaf must be nonnegative integer");
    if (prefix != "input_tokens" && prefix != "output_tokens" && prefix != "total_tokens" && prefix != "input_tokens_details.cached_tokens" && prefix != "input_tokens_details.cache_write_tokens" && prefix != "output_tokens_details.reasoning_tokens") usage_.extra[prefix] = {number(v), Evidence::Reported};
  }
  return true;
}
json::Value Codec::usage_at(json::Value root) const {
  for (const auto& member : descriptor_.usage_path()) {
    if (!root.is_object()) return {};
    root = root.get(member);
  }
  return root;
}
bool Codec::usage(json::Value v) {
  if (!v.valid() || v.is_null()) return true;
  if (!v.is_object()) return fail(ErrorKind::ProtocolCorrupt, "usage must be object or null");
  auto input = v.get("input_tokens_details"), output = v.get("output_tokens_details");
  if ((input.valid() && !input.is_null() && !input.is_object()) || (output.valid() && !output.is_null() && !output.is_object())) return fail(ErrorKind::ProtocolCorrupt, "usage details must be object or null");
  usage_.stage = UsageStage::Partial;
  usage_.extra.clear();
  usage_.provider_cost = codecs::provider_cost(v, descriptor_);
  if (!counter(v.get("input_tokens"), usage_.input_total, "input_tokens") || !counter(v.get("output_tokens"), usage_.output_total, "output_tokens") ||
      !counter(v.get("total_tokens"), usage_.provider_reported_total, "total_tokens") || !counter(input.get("cached_tokens"), usage_.cache_read, "cached_tokens") ||
      !counter(input.get("cache_write_tokens"), usage_.cache_write, "cache_write_tokens") || !counter(output.get("reasoning_tokens"), usage_.reasoning, "reasoning_tokens") || !usage_leaves(v, "")) return false;
  usage_.input_uncached.reset(); usage_.total.reset();
  if (usage_.input_total && usage_.cache_read) {
    if (usage_.cache_read->value > usage_.input_total->value) conflict("cache_read", "cache read subset exceeds input total");
    else usage_.input_uncached = Count{usage_.input_total->value - usage_.cache_read->value, Evidence::Derived};
  }
  if (usage_.input_total && usage_.output_total) {
    if (usage_.output_total->value > std::numeric_limits<uint64_t>::max() - usage_.input_total->value) return fail(ErrorKind::ProtocolCorrupt, "usage total overflow");
    usage_.total = Count{usage_.input_total->value + usage_.output_total->value, Evidence::Derived};
    if (usage_.provider_reported_total && usage_.total->value != usage_.provider_reported_total->value) conflict("total_tokens", "provider total differs from input plus output");
  } else usage_.total = usage_.provider_reported_total;
  if (usage_.reasoning && usage_.output_total && usage_.reasoning->value > usage_.output_total->value) conflict("reasoning_tokens", "reasoning subset exceeds output total");
  return emit(UsageUpdate{usage_});
}
void Codec::finish(Close close) {
  if (closed_ || accumulator_.terminal()) { closed_ = true; return; }
  closed_ = true;
  if (!close.normal) { fail(close.error, "abnormal response close"); return; }
  if (!begun_ || !terminal_) { fail(ErrorKind::Truncated, "missing Responses terminal evidence"); return; }
  emit(Commit{mode_ == Mode::Sse ? "responses.terminal+normal_close" : "responses.status+normal_close"});
}
} // namespace sp::responses
