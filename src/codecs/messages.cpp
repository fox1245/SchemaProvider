#include "codecs/messages.h"
#include "core/native.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace sp::messages {
namespace {
bool count_value(json::Value v) { return v.is_uint() || (v.is_int() && v.as_int() >= 0); }
uint64_t number(json::Value v) { return v.is_uint() ? v.as_uint() : static_cast<uint64_t>(v.as_int()); }
bool nonempty(json::Value v) { return v.is_string() && !v.as_string().empty(); }
bool nullable_string(json::Value v) { return !v.valid() || v.is_null() || v.is_string(); }
StopKind mapped(descriptor::StopKind k) {
  switch (k) {
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
std::string_view result_name(std::string_view type) {
  if (type == "web_search_tool_result") return "web_search";
  if (type == "web_fetch_tool_result") return "web_fetch";
  if (type == "code_execution_tool_result") return "code_execution";
  if (type == "bash_code_execution_tool_result") return "bash_code_execution";
  if (type == "text_editor_code_execution_tool_result") return "text_editor_code_execution";
  if (type == "tool_search_tool_result") return "tool_search";
  return {};
}
bool result_matches(std::string_view type, std::string_view name) {
  const auto expected = result_name(type);
  return expected == name || (expected == "tool_search" && (name == "tool_search_tool_regex" || name == "tool_search_tool_bm25"));
}
// Only tool argument objects may retain duplicate keys as InvalidToolCall.
// Validate every other subtree of a rejected-but-syntactically-valid document.
bool unique_envelope(json::Value v, bool root = false, bool block = false) {
  if (v.is_object()) {
    std::set<std::string_view> names;
    for (auto m : v.members()) {
      if (!names.insert(m.key).second) return false;
      if (block && m.key == "input" && v.get("type").is_string() &&
          (v.get("type").as_string() == "tool_use" || v.get("type").as_string() == "server_tool_use")) continue;
      if (root && m.key == "content" && m.value.is_array()) {
        for (auto item : m.value.elements()) if (!unique_envelope(item, false, true)) return false;
      } else if (!unique_envelope(m.value)) return false;
    }
  } else if (v.is_array()) for (auto item : v.elements()) if (!unique_envelope(item)) return false;
  return true;
}
}
Codec::Codec(const descriptor::ValidatedDescriptor& descriptor, Mode mode, Accumulator& accumulator,
             std::shared_ptr<const NativeContext> context, SemanticLimits limits)
    : descriptor_(descriptor), mode_(mode), accumulator_(accumulator), context_(std::move(context)),
      context_valid_(context_ && context_->matches_descriptor(descriptor_)), limits_(limits) {}
bool Codec::emit(const Event& e) { return accumulator_.accept(e, buffered_failure_ ? &*buffered_failure_ : nullptr); }
bool Codec::fail(ErrorKind kind, std::string text) {
  if (buffered_failure_ && !closed_) return false;
  emit(Fail{{kind, std::move(text)}}); return false;
}
void Codec::unknown(json::Value v, std::initializer_list<std::string_view> known) {
  for (auto m : v.members()) if (std::find(known.begin(), known.end(), m.key) == known.end()) ++diagnostics_.unknown_properties;
}
std::shared_ptr<const json::Document> Codec::own(json::Value v) {
  auto parsed = json::parse(v.dump(), {limits_.max_content_bytes, limits_.max_json_depth});
  if (auto* doc = std::get_if<json::Document>(&parsed)) return std::make_shared<const json::Document>(std::move(*doc));
  fail(ErrorKind::ProtocolCorrupt, "invalid retained JSON"); return {};
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
  // A named error is already authoritative, including after message_stop.
  if (event == "error") return fail(ErrorKind::RemoteFailure, "remote error event");
  return document(data, event, true);
}
bool Codec::document(std::string_view bytes, std::string_view event, bool streaming) {
  if (!context_valid_) return fail(ErrorKind::InvalidConfig, "Messages request context required");
  if (bytes.size() > limits_.max_content_bytes - std::min(input_bytes_, limits_.max_content_bytes)) return fail(ErrorKind::ResourceLimit, "response byte limit");
  input_bytes_ += bytes.size();
  auto parsed = json::parse(bytes, {limits_.max_content_bytes, limits_.max_json_depth});
  json::Value root;
  if (auto* e = std::get_if<json::ParseError>(&parsed)) {
    if (!streaming && e->code == json::ParseCode::DuplicateKey && e->context.root().is_object() && unique_envelope(e->context.root(), true))
      root = e->context.root();
    else return fail(e->code == json::ParseCode::SizeExceeded || e->code == json::ParseCode::DepthExceeded ? ErrorKind::ResourceLimit : ErrorKind::ProtocolCorrupt, "invalid response JSON");
  } else root = std::get<json::Document>(parsed).root();
  if (!root.is_object()) return fail(ErrorKind::ProtocolCorrupt, "response must be object");
  if (root.get("error").valid() || (root.get("type").is_string() && root.get("type").as_string() == "error")) return fail(ErrorKind::RemoteFailure, "remote error response");
  if (!root.get("type").is_string()) return fail(ErrorKind::ProtocolCorrupt, "response type required");
  const auto type = root.get("type").as_string();
  if (!streaming) return message(root, false);
  if (event != type) return fail(ErrorKind::ProtocolCorrupt, "SSE event and payload type disagree");
  if (type == "ping") { unknown(root, {"type"}); return true; }
  if (done_) return fail(ErrorKind::ProtocolCorrupt, "event after message_stop");
  if (type == "message_start") {
    unknown(root, {"type", "message"});
    if (begun_) return fail(ErrorKind::ProtocolCorrupt, "duplicate message_start");
    return message(root.get("message"), true);
  }
  if (type != "content_block_start" && type != "content_block_delta" && type != "content_block_stop" && type != "message_delta" && type != "message_stop") return fail(ErrorKind::Unsupported, "unknown Messages event");
  if (!begun_) return fail(ErrorKind::ProtocolCorrupt, "event before message_start");
  if (!identity(root, false)) return false;
  if (type == "message_delta") {
    unknown(root, {"type", "delta", "usage", "id", "model", descriptor_.usage_path().front()});
    if (!all_blocks_closed()) return false;
    message_delta_ = true;
    auto d = root.get("delta");
    if (!d.is_object()) return fail(ErrorKind::ProtocolCorrupt, "message delta required");
    unknown(d, {"stop_reason", "stop_sequence", "stop_details", "container", "id", "model", "role", "input_transformations", "context_management"});
    if (!identity(d, false) || !metadata(d) || !usage(usage_at(root), false)) return false;
    return terminal(d);
  }
  if (type == "message_stop") {
    unknown(root, {"type", "id", "model"});
    if (!all_blocks_closed()) return false;
    done_ = true; return true;
  }
  if (message_delta_ || stopped_) return fail(ErrorKind::ProtocolCorrupt, "content after message delta");
  auto index = root.get("index");
  if (!count_value(index)) return fail(ErrorKind::ProtocolCorrupt, "block index required");
  const auto position = number(index);
  if (position >= limits_.max_parts || position >= std::numeric_limits<uint32_t>::max()) return fail(ErrorKind::ResourceLimit, "block index limit");
  if (type == "content_block_start") { unknown(root, {"type", "index", "content_block", "id", "model"}); return block(root.get("content_block"), position, true); }
  if (type == "content_block_delta") { unknown(root, {"type", "index", "delta", "id", "model"}); return delta(root.get("delta"), position); }
  unknown(root, {"type", "index", "id", "model"}); return end_block(position);
}
bool Codec::identity(json::Value v, bool required) {
  auto id = v.get("id"), model = v.get("model"), role = v.get("role");
  if ((required && (!id.valid() || !model.valid() || !role.valid())) ||
      (id.valid() && !nonempty(id)) || (model.valid() && !nonempty(model)) ||
      (role.valid() && (!role.is_string() || role.as_string() != "assistant"))) return fail(ErrorKind::ProtocolCorrupt, "invalid message identity");
  if (model.valid() && model.as_string() != context_->model()) return fail(ErrorKind::ProtocolCorrupt, "response model differs from request");
  if (begun_ && ((id.valid() && id.as_string() != generation_) || (model.valid() && model.as_string() != model_))) return fail(ErrorKind::ProtocolCorrupt, "inconsistent message identity");
  return true;
}
bool Codec::metadata(json::Value v) {
  for (auto key : {"container", "context_management", "diagnostics", "input_transformations"}) {
    auto item = v.get(key);
    if (!item.valid() || item.is_null()) continue;
    const bool array = std::string_view(key) == "input_transformations";
    if (array ? !item.is_array() : !item.is_object()) return fail(ErrorKind::ProtocolCorrupt, "invalid known message metadata");
    if (array && item.size() == 0) continue;
    return fail(ErrorKind::Unsupported, "unsupported semantic message metadata");
  }
  return true;
}
bool Codec::message(json::Value v, bool streaming) {
  if (!v.is_object() || !v.get("type").is_string()) return fail(ErrorKind::ProtocolCorrupt, "message object required");
  if (v.get("type").as_string() != "message") return fail(ErrorKind::Unsupported, "unknown response object type");
  if (begun_) return fail(ErrorKind::ProtocolCorrupt, "duplicate message");
  unknown(v, {"id", "type", "role", "model", "content", "usage", "stop_reason", "stop_sequence", "stop_details", "container", "diagnostics", "context_management", "input_transformations", descriptor_.usage_path().front()});
  if (!identity(v, true) || !metadata(v)) return false;
  auto content = v.get("content");
  if (!content.is_array() || (streaming && content.size())) return fail(ErrorKind::ProtocolCorrupt, "invalid initial content");
  if (!usage_at(v).is_object()) return fail(ErrorKind::ProtocolCorrupt, "usage object required");
  generation_ = v.get("id").as_string(); model_ = v.get("model").as_string();
  if (!emit(Begin{generation_}) || !emit(MessageBegin{{0}, generation_, Role::Assistant, context_})) return false;
  begun_ = true;
  uint64_t index = 0;
  for (auto item : content.elements()) if (!block(item, index++, false)) return false;
  if (!usage(usage_at(v), true)) return false;
  return terminal(v, streaming);
}
bool Codec::all_blocks_closed() {
  uint64_t expected = 0;
  for (const auto& [index, b] : blocks_) if (index != expected++ || !b.closed) return fail(ErrorKind::ProtocolCorrupt, "incomplete block lifecycle or index gap");
  return true;
}
bool Codec::terminal(json::Value v, bool initial) {
  auto reason = v.get("stop_reason"), sequence = v.get("stop_sequence"), details = v.get("stop_details");
  if (!reason.valid() || (!reason.is_null() && !nonempty(reason)) || !nullable_string(sequence) || (details.valid() && !details.is_null() && !details.is_object())) return fail(ErrorKind::ProtocolCorrupt, "invalid stop fields");
  if (details.is_object()) {
    if (!nonempty(details.get("type")) || !nullable_string(details.get("category")) || !nullable_string(details.get("explanation"))) return fail(ErrorKind::ProtocolCorrupt, "invalid stop details");
    if (details.get("type").as_string() != "refusal") return fail(ErrorKind::Unsupported, "unknown stop details type");
  }
  if (initial && (!reason.is_null() || sequence.is_string() || details.is_object())) return fail(ErrorKind::ProtocolCorrupt, "terminal fields in message_start");
  if (reason.is_null()) {
    if (sequence.is_string() || details.is_object()) return fail(ErrorKind::ProtocolCorrupt, "stop metadata without reason");
    return true;
  }
  if (reason.as_string() == "error") return fail(ErrorKind::RemoteFailure, "remote failure stop");
  StopReason next{mapped(descriptor_.stop_kind(reason.as_string())), std::string(reason.as_string())};
  if (sequence.is_string()) next.sequence = std::string(sequence.as_string());
  if (details.is_object()) next.details = own(details);
  if ((next.kind == StopKind::StopSequence) != next.sequence.has_value() || (next.details && next.kind != StopKind::Refusal)) return fail(ErrorKind::ProtocolCorrupt, "contradictory stop metadata");
  if (stop_) {
    if (stop_->raw != next.raw || stop_->sequence != next.sequence || bool(stop_->details) != bool(next.details) || (next.details && !json::equal(stop_->details->root(), next.details->root()))) return fail(ErrorKind::ProtocolCorrupt, "contradictory stop reason");
    return true;
  }
  if (!emit(Stop{next})) return false;
  stop_ = std::move(next); stopped_ = true; return true;
}
bool Codec::caller(json::Value v) {
  if (!v.valid()) return true;
  if (!v.is_object() || !nonempty(v.get("type"))) return fail(ErrorKind::ProtocolCorrupt, "invalid tool caller");
  const auto type = v.get("type").as_string();
  if (type == "direct") {
    if (v.get("tool_id").valid()) return fail(ErrorKind::ProtocolCorrupt, "direct caller has tool id");
    return true;
  }
  if (type != "code_execution_20250825" && type != "code_execution_20260120" && type != "code_execution_20260521") return fail(ErrorKind::Unsupported, "unknown tool caller type");
  return nonempty(v.get("tool_id")) || fail(ErrorKind::ProtocolCorrupt, "server caller id required");
}
bool Codec::block(json::Value v, uint64_t index, bool streaming) {
  if (index >= limits_.max_parts || index >= std::numeric_limits<uint32_t>::max() || blocks_.size() >= limits_.max_parts) return fail(ErrorKind::ResourceLimit, "block count limit");
  if (blocks_.contains(index)) return fail(ErrorKind::ProtocolCorrupt, "duplicate block index");
  if (!v.is_object() || !nonempty(v.get("type"))) return fail(ErrorKind::ProtocolCorrupt, "content block type required");
  const auto type = v.get("type").as_string();
  PartKind kind;
  PartHeader header;
  std::string storage;
  std::string_view bytes;
  bool initial_input = false;
  if (type == "text") {
    kind = PartKind::Text;
    unknown(v, {"type", "text", "citations"});
    if (!v.get("text").is_string()) return fail(ErrorKind::ProtocolCorrupt, "text required");
    auto citations = v.get("citations");
    if (citations.valid() && !citations.is_null()) {
      if (!citations.is_array()) return fail(ErrorKind::ProtocolCorrupt, "invalid citations");
      if (citations.size()) return fail(ErrorKind::Unsupported, "citations not mapped");
    }
    bytes = v.get("text").as_string();
  } else if (type == "thinking") {
    kind = PartKind::Thinking; unknown(v, {"type", "thinking", "signature"});
    if (!v.get("thinking").is_string() || (v.get("signature").valid() && !v.get("signature").is_string())) return fail(ErrorKind::ProtocolCorrupt, "invalid thinking fields");
    bytes = v.get("thinking").as_string();
  } else if (type == "redacted_thinking") {
    kind = PartKind::RedactedThinking; unknown(v, {"type", "data"});
    if (!v.get("data").is_string()) return fail(ErrorKind::ProtocolCorrupt, "redacted data required");
    bytes = v.get("data").as_string();
  } else if (type == "tool_use" || type == "server_tool_use") {
    kind = PartKind::ToolCall;
    if (!nonempty(v.get("id")) || !nonempty(v.get("name")) || !v.get("input").is_object()) return fail(ErrorKind::ProtocolCorrupt, "invalid tool block");
    if (!caller(v.get("caller"))) return false;
    const auto toolset = v.get("toolset_name");
    if (type == "tool_use" && (!nullable_string(toolset) || (toolset.is_string() &&
        (toolset.as_string().empty() || toolset.as_string().size() > 64 ||
         !std::all_of(toolset.as_string().begin(), toolset.as_string().end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
         }))))) return fail(ErrorKind::ProtocolCorrupt, "invalid toolset name");
    header.wire_id = v.get("id").as_string(); header.name = v.get("name").as_string(); header.wire_type = type;
    header.tool_kind = type == "server_tool_use" ? ToolCallKind::ServerExecuted : ToolCallKind::ClientExecuted;
    if (calls_.contains(header.wire_id) || context_->server_tool_name(header.wire_id)) return fail(ErrorKind::ProtocolCorrupt, "duplicate tool id");
    std::string extras = "{";
    for (auto m : v.members()) if (m.key != "type" && m.key != "id" && m.key != "name" && m.key != "input") {
      if (extras.size() > 1) extras += ',';
      extras += json::quote(m.key); extras += ':'; extras += m.value.dump();
    }
    if (extras.size() > 1) {
      extras += '}'; auto doc = json::parse(extras, {limits_.max_content_bytes, limits_.max_json_depth});
      if (!std::holds_alternative<json::Document>(doc)) return fail(ErrorKind::ProtocolCorrupt, "invalid tool metadata");
      header.wire_metadata = std::make_shared<const json::Document>(std::move(std::get<json::Document>(doc)));
    }
    calls_.emplace(header.wire_id, Call{header.name, header.tool_kind, index});
    initial_input = v.get("input").size() != 0;
    if (!streaming || initial_input) { storage = v.get("input").dump(); bytes = storage; }
  } else if (!result_name(type).empty()) {
    kind = PartKind::ServerToolResult;
    if (!nonempty(v.get("tool_use_id"))) return fail(ErrorKind::ProtocolCorrupt, "invalid server result identity");
    if (!caller(v.get("caller"))) return false;
    header.wire_id = v.get("tool_use_id").as_string(); header.wire_type = type;
    auto call = calls_.find(header.wire_id);
    bool correlated = false;
    if (call != calls_.end()) correlated = call->second.kind == ToolCallKind::ServerExecuted && call->second.index < index && result_matches(type, call->second.name);
    else if (auto name = context_->server_tool_name(header.wire_id)) correlated = result_matches(type, *name);
    if (!correlated || !results_.insert(header.wire_id).second) return fail(ErrorKind::ProtocolCorrupt, "uncorrelated server result");
    if (!server_content(type, v.get("content"))) return false;
    storage = v.dump(); bytes = storage;
  } else return fail(ErrorKind::Unsupported, "unsupported content block type");
  Block binding{LocalId{static_cast<uint32_t>(index + 1)}, kind};
  binding.initial_input = initial_input;
  if (!emit(PartBegin{{0}, binding.local, kind, std::move(header), index})) return false;
  if ((kind != PartKind::ToolCall || !streaming || initial_input) && !emit(PartDelta{binding.local, {kind, bytes}})) return false;
  if (kind == PartKind::Thinking && v.get("signature").valid()) {
    if (!emit(PartDelta{binding.local, {kind, v.get("signature").as_string(), DeltaChannel::Signature}})) return false;
    binding.signature_started = !v.get("signature").as_string().empty();
  }
  blocks_.emplace(index, binding);
  return streaming || end_block(index);
}
bool Codec::delta(json::Value v, uint64_t index) {
  auto found = blocks_.find(index);
  if (found == blocks_.end() || found->second.closed) return fail(ErrorKind::ProtocolCorrupt, "delta outside open block");
  if (!v.is_object() || !nonempty(v.get("type"))) return fail(ErrorKind::ProtocolCorrupt, "delta type required");
  auto& b = found->second;
  auto type = v.get("type").as_string();
  std::string_view field;
  DeltaChannel channel = DeltaChannel::Content;
  if (type == "text_delta" && b.kind == PartKind::Text) field = "text";
  else if (type == "thinking_delta" && b.kind == PartKind::Thinking) {
    if (b.signature_started) return fail(ErrorKind::ProtocolCorrupt, "thinking after signature");
    field = "thinking";
  } else if (type == "signature_delta" && b.kind == PartKind::Thinking) { field = "signature"; channel = DeltaChannel::Signature; b.signature_started = true; }
  else if (type == "input_json_delta" && b.kind == PartKind::ToolCall) field = "partial_json";
  else if (type == "citations_delta" || type == "refusal_delta") return fail(ErrorKind::Unsupported, "unsupported semantic delta");
  else if (type == "text_delta" || type == "thinking_delta" || type == "signature_delta" || type == "input_json_delta") return fail(ErrorKind::ProtocolCorrupt, "delta kind differs from block");
  else return fail(ErrorKind::Unsupported, "unknown content delta");
  unknown(v, {"type", field});
  auto text = v.get(field);
  if (!text.is_string()) return fail(ErrorKind::ProtocolCorrupt, "delta string required");
  if (b.kind == PartKind::ToolCall && !text.as_string().empty()) {
    if (b.initial_input) return fail(ErrorKind::ProtocolCorrupt, "tool snapshot mixed with argument deltas");
    b.argument_bytes = true;
  }
  return emit(PartDelta{b.local, {b.kind, text.as_string(), channel}});
}
bool Codec::end_block(uint64_t index) {
  auto found = blocks_.find(index);
  if (found == blocks_.end() || found->second.closed) return fail(ErrorKind::ProtocolCorrupt, "stop outside open block");
  auto& b = found->second;
  // The empty initial object is not an argument prefix. Supply it only for a call
  // that never received actual JSON bytes (empty delta strings do not count).
  if (mode_ == Mode::Sse && b.kind == PartKind::ToolCall && !b.initial_input && !b.argument_bytes)
    if (!emit(PartDelta{b.local, {b.kind, "{}"}})) return false;
  if (!emit(PartSeal{b.local, {}})) return false;
  b.closed = true; return true;
}
bool Codec::server_content(std::string_view outer, json::Value v) {
  auto corrupt = [&] { return fail(ErrorKind::ProtocolCorrupt, "invalid server result content"); };
  auto strings = [](json::Value item, std::initializer_list<std::string_view> keys) {
    for (auto key : keys) if (!item.get(key).is_string()) return false;
    return true;
  };
  if (outer == "web_search_tool_result" && v.is_array()) {
    for (auto item : v.elements()) {
      if (!item.is_object() || !strings(item, {"type", "encrypted_content", "title", "url"}) ||
          item.get("type").as_string() != "web_search_result" || !nullable_string(item.get("page_age"))) return corrupt();
    }
    return true;
  }
  if (!v.is_object() || !nonempty(v.get("type"))) return corrupt();
  const auto type = v.get("type").as_string();
  if (type == std::string(outer) + "_error") {
    if (!nonempty(v.get("error_code")) || !nullable_string(v.get("error_message"))) return corrupt();
    return true;
  }
  if (outer == "web_fetch_tool_result" && type == "web_fetch_result") {
    auto doc = v.get("content"), source = doc.get("source"), citations = doc.get("citations");
    if (!strings(v, {"url"}) || !nullable_string(v.get("retrieved_at")) ||
        !doc.is_object() || !strings(doc, {"type"}) || doc.get("type").as_string() != "document" ||
        !nullable_string(doc.get("title")) || !source.is_object() || !strings(source, {"type", "data", "media_type"})) return corrupt();
    if (citations.valid() && !citations.is_null() && (!citations.is_object() || !citations.get("enabled").is_bool())) return corrupt();
    if (!((source.get("type").as_string() == "base64" && source.get("media_type").as_string() == "application/pdf") ||
          (source.get("type").as_string() == "text" && source.get("media_type").as_string() == "text/plain"))) return fail(ErrorKind::Unsupported, "unknown fetch document source");
    return true;
  }
  const bool code = outer == "code_execution_tool_result" && (type == "code_execution_result" || type == "encrypted_code_execution_result");
  const bool bash = outer == "bash_code_execution_tool_result" && type == "bash_code_execution_result";
  if (code || bash) {
    auto output = v.get("content"), rc = v.get("return_code");
    if (!output.is_array() || (!rc.is_int() && !rc.is_uint()) || !strings(v, {"stderr"}) ||
        !v.get(type == "encrypted_code_execution_result" ? "encrypted_stdout" : "stdout").is_string()) return corrupt();
    for (auto file : output.elements()) if (!file.is_object() || !strings(file, {"type", "file_id"}) ||
        file.get("type").as_string() != (bash ? "bash_code_execution_output" : "code_execution_output")) return corrupt();
    return true;
  }
  if (outer == "text_editor_code_execution_tool_result") {
    if (type == "text_editor_code_execution_create_result") return v.get("is_file_update").is_bool() || corrupt();
    if (type == "text_editor_code_execution_view_result") {
      if (!strings(v, {"content", "file_type"})) return corrupt();
      const auto file = v.get("file_type").as_string();
      if (file != "text" && file != "image" && file != "pdf") return fail(ErrorKind::Unsupported, "unknown editor file type");
      for (auto key : {"num_lines", "start_line", "total_lines"}) {
        auto n = v.get(key); if (n.valid() && !n.is_null() && !count_value(n)) return corrupt();
      }
      return true;
    }
    if (type == "text_editor_code_execution_str_replace_result") {
      auto lines = v.get("lines");
      if (lines.valid() && !lines.is_null()) {
        if (!lines.is_array()) return corrupt();
        for (auto line : lines.elements()) if (!line.is_string()) return corrupt();
      }
      for (auto key : {"new_lines", "new_start", "old_lines", "old_start"}) {
        auto n = v.get(key); if (n.valid() && !n.is_null() && !count_value(n)) return corrupt();
      }
      return true;
    }
  }
  if (outer == "tool_search_tool_result" && type == "tool_search_tool_search_result") {
    auto refs = v.get("tool_references");
    if (!refs.is_array()) return corrupt();
    for (auto ref : refs.elements()) if (!ref.is_object() || !nonempty(ref.get("tool_name")) ||
        !ref.get("type").is_string() || ref.get("type").as_string() != "tool_reference") return corrupt();
    return true;
  }
  return fail(ErrorKind::Unsupported, "unknown server result content type");
}
void Codec::conflict(std::string_view key, std::string_view detail) {
  usage_.quality = UsageQuality::Inconsistent;
  for (const auto& c : usage_.conflicts) if (c.counter == key && c.detail == detail) return;
  usage_.conflicts.push_back({std::string(key), std::string(detail)});
}
bool Codec::counter(json::Value v, std::optional<Count>& target, std::string_view key, bool nullable) {
  if (!v.valid() || (nullable && v.is_null())) return true;
  if (!count_value(v)) return fail(ErrorKind::ProtocolCorrupt, "usage counter must be unsigned integer");
  const auto n = number(v);
  if (target && n < target->value) conflict(key, "cumulative counter regressed");
  else target = Count{n, Evidence::Reported};
  return true;
}
bool Codec::usage_leaves(json::Value value, const std::string& prefix) {
  // Iteration subusage and provider-specific counters stay separately addressed;
  // they are never added to the authoritative aggregate counters.
  if (value.is_null()) return true;
  if (value.is_object()) {
    for (auto member : value.members()) if (!usage_leaves(member.value, prefix + "." + std::string(member.key))) return false;
    return true;
  }
  if (value.is_array()) {
    size_t index = 0;
    for (auto item : value.elements()) if (!usage_leaves(item, prefix + "." + std::to_string(index++))) return false;
    return true;
  }
  if (value.is_string() && (prefix.ends_with(".type") || prefix.ends_with(".service_tier") || prefix.ends_with(".inference_geo"))) return true;
  if (!count_value(value)) { ++diagnostics_.unknown_properties; return true; }
  std::optional<Count> count;
  auto found = usage_.extra.find(prefix);
  if (found != usage_.extra.end()) count = found->second;
  if (!counter(value, count, prefix)) return false;
  if (count) usage_.extra[prefix] = *count;
  return true;
}
json::Value Codec::usage_at(json::Value root) const {
  for (const auto& member : descriptor_.usage_path()) {
    if (!root.is_object()) return {};
    root = root.get(member);
  }
  return root;
}
bool Codec::usage(json::Value v, bool initial) {
  if (!v.is_object() || !v.get("output_tokens").valid() || (initial && !v.get("input_tokens").valid())) return fail(ErrorKind::ProtocolCorrupt, "required usage counters missing");
  usage_.stage = UsageStage::Partial;
  if (!counter(v.get("input_tokens"), usage_.input_uncached, "input_tokens", !initial) ||
      !counter(v.get("output_tokens"), usage_.output_total, "output_tokens") ||
      !counter(v.get("cache_read_input_tokens"), usage_.cache_read, "cache_read_input_tokens", true) ||
      !counter(v.get("cache_creation_input_tokens"), usage_.cache_write, "cache_creation_input_tokens", true)) return false;
  for (auto key : {"cache_creation", "server_tool_use", "output_tokens_details"}) {
    auto detail = v.get(key);
    if (!detail.valid() || detail.is_null()) continue;
    if (!detail.is_object()) return fail(ErrorKind::ProtocolCorrupt, "usage details must be object");
    for (auto member : detail.members()) {
      if (std::string_view(key) == "output_tokens_details" && member.key == "thinking_tokens") {
        if (!counter(member.value, usage_.reasoning, "thinking_tokens")) return false;
      } else {
        const bool known =
            (std::string_view(key) == "cache_creation" && (member.key == "ephemeral_5m_input_tokens" || member.key == "ephemeral_1h_input_tokens")) ||
            (std::string_view(key) == "server_tool_use" && (member.key == "web_search_requests" || member.key == "web_fetch_requests"));
        if (!known) {
          ++diagnostics_.unknown_properties;
          if (!count_value(member.value)) continue;
        }
        const std::string address = std::string(key) + "." + std::string(member.key);
        std::optional<Count> count;
        auto found = usage_.extra.find(address);
        if (found != usage_.extra.end()) count = found->second;
        if (!counter(member.value, count, address)) return false;
        if (count) usage_.extra[address] = *count;
      }
    }
  }
  auto iterations = v.get("iterations");
  if (iterations.valid() && !iterations.is_null()) {
    if (!iterations.is_array()) return fail(ErrorKind::ProtocolCorrupt, "usage iterations must be array");
    for (auto iteration : iterations.elements()) {
      if (!iteration.is_object()) return fail(ErrorKind::ProtocolCorrupt, "usage iteration must be object");
      for (auto key : {"input_tokens", "output_tokens", "cache_read_input_tokens", "cache_creation_input_tokens"})
        if (auto n = iteration.get(key); n.valid() && !n.is_null() && !count_value(n)) return fail(ErrorKind::ProtocolCorrupt, "invalid iteration counter");
    }
    if (!usage_leaves(iterations, "iterations")) return false;
  }
  for (auto key : {"service_tier", "inference_geo"}) if (!nullable_string(v.get(key))) return fail(ErrorKind::ProtocolCorrupt, "invalid usage metadata");
  unknown(v, {"input_tokens", "output_tokens", "cache_read_input_tokens", "cache_creation_input_tokens", "cache_creation", "server_tool_use", "output_tokens_details", "iterations", "service_tier", "inference_geo"});
  auto sum = [&](uint64_t a, uint64_t b, uint64_t& out) {
    if (b > std::numeric_limits<uint64_t>::max() - a) return fail(ErrorKind::ProtocolCorrupt, "usage sum overflow");
    out = a + b; return true;
  };
  if (usage_.input_uncached && usage_.cache_read && usage_.cache_write) {
    uint64_t total;
    if (!sum(usage_.input_uncached->value, usage_.cache_read->value, total) || !sum(total, usage_.cache_write->value, total)) return false;
    usage_.input_total = Count{total, Evidence::Derived};
  }
  if (usage_.input_total && usage_.output_total) {
    uint64_t total;
    if (!sum(usage_.input_total->value, usage_.output_total->value, total)) return false;
    usage_.total = Count{total, Evidence::Derived};
  }
  if (usage_.reasoning && usage_.output_total && usage_.reasoning->value > usage_.output_total->value) conflict("reasoning", "reasoning subset exceeds output total");
  const auto five = usage_.extra.find("cache_creation.ephemeral_5m_input_tokens");
  const auto hour = usage_.extra.find("cache_creation.ephemeral_1h_input_tokens");
  if (five != usage_.extra.end() && hour != usage_.extra.end() && usage_.cache_write) {
    uint64_t total;
    if (!sum(five->second.value, hour->second.value, total)) return false;
    if (total != usage_.cache_write->value) conflict("cache_write", "cache creation detail differs from aggregate");
  }
  return emit(UsageUpdate{usage_});
}
void Codec::finish(Close close) {
  if (closed_ || accumulator_.terminal()) { closed_ = true; return; }
  closed_ = true;
  if (!close.normal) { fail(close.error, "abnormal response close"); return; }
  if (!begun_ || !stopped_ || (mode_ == Mode::Sse && !done_)) { fail(ErrorKind::Truncated, "missing Messages terminal evidence"); return; }
  if (!all_blocks_closed() || !emit(MessageSeal{{0}})) return;
  emit(Commit{mode_ == Mode::Sse ? "messages.stop_reason+message_stop+normal_close" : "messages.stop_reason+normal_close"});
}
} // namespace sp::messages
