#include "codecs/accumulator.h"
#include "core/native.h"
#include "json/json.h"
#include <algorithm>
#include <utility>

namespace sp {
namespace {
bool incomplete(std::string_view s) {
  size_t depth = 0; bool quoted = false, escaped = false;
  for (char c : s) {
    if (quoted) { if (escaped) escaped = false; else if (c == '\\') escaped = true; else if (c == '"') quoted = false; }
    else if (c == '"') quoted = true;
    else if (c == '{' || c == '[') ++depth;
    else if (c == '}' || c == ']') { if (!depth) return false; --depth; }
  }
  return quoted || depth != 0;
}
bool metadata_size(json::Value value, size_t depth, size_t maximum, size_t max_depth, size_t& size) {
  auto claim = [&](size_t bytes) {
    if (bytes > maximum - std::min(size, maximum)) return false;
    size += bytes; return true;
  };
  if (depth > max_depth || !value.valid() || !claim(8)) return false;
  if (value.is_string()) return claim(value.as_string().size());
  if (value.is_object()) for (auto member : value.members())
    if (!claim(member.key.size()) || !metadata_size(member.value, depth + 1, maximum, max_depth, size)) return false;
  if (value.is_array()) for (auto element : value.elements())
    if (!metadata_size(element, depth + 1, maximum, max_depth, size)) return false;
  return true;
}
bool reasoning_metadata(json::Value value, std::string_view id) {
  if (!value.is_object() || value.get("type").as_string() != "reasoning" ||
      !value.get("id").is_string() || value.get("id").as_string() != id) return false;
  auto strings = [](json::Value values, std::string_view type) {
    if (!values.is_array()) return false;
    for (auto item : values.elements())
      if (!item.is_object() || item.get("type").as_string() != type || !item.get("text").is_string()) return false;
    return true;
  };
  if (!strings(value.get("summary"), "summary_text")) return false;
  const auto content = value.get("content"), encrypted = value.get("encrypted_content"), status = value.get("status");
  return (!content.valid() || strings(content, "reasoning_text")) &&
      (!encrypted.valid() || encrypted.is_null() || encrypted.is_string()) &&
      (!status.valid() || status.is_null() || status.is_string());
}
}
Accumulator::Accumulator(SemanticLimits limits, Sink sink) : limits_(limits), sink_(std::move(sink)) {}
bool Accumulator::accept(const Event& event, const Error* failure_override) {
  if (terminal()) return false;
  failure_override_ = failure_override;
  const bool ok = std::visit([this](const auto& e) { return apply(e); }, event);
  failure_override_ = nullptr;
  if (ok && sink_) sink_(event);
  return ok;
}
bool Accumulator::reject(ErrorKind kind, std::string message) {
  Fail e{failure_override_ ? *failure_override_ : Error{kind, std::move(message)}};
  apply(e);
  if (sink_) sink_(Event{e});
  return false;
}
bool Accumulator::apply(const Begin&) {
  if (state_ != State::Created) return reject(ErrorKind::ProtocolCorrupt, "duplicate begin");
  state_ = State::Receiving; return true;
}
bool Accumulator::apply(const MessageBegin& e) {
  if (state_ != State::Receiving || messages_.contains(e.message.value)) return reject(ErrorKind::ProtocolCorrupt, "invalid message begin");
  if (messages_.size() >= limits_.max_parts) return reject(ErrorKind::ResourceLimit, "message limit");
  messages_.emplace(e.message.value, MessageCursor{Message{e.vendor_id.value_or(""), e.role, {}}, false, e.native_context, {}});
  return true;
}
bool Accumulator::apply(const PartBegin& e) {
  auto m = messages_.find(e.message.value);
  if (state_ != State::Receiving || m == messages_.end() || m->second.sealed || parts_.contains(e.part.value)) return reject(ErrorKind::ProtocolCorrupt, "invalid part begin");
  if (parts_.size() >= limits_.max_parts) return reject(ErrorKind::ResourceLimit, "part limit");
  if (m->second.parts_by_order.contains(e.order)) return reject(ErrorKind::ProtocolCorrupt, "duplicate part order");
  const size_t remaining = limits_.max_content_bytes - std::min(content_bytes_, limits_.max_content_bytes);
  size_t header_bytes = 0;
  auto count_header = [&](size_t bytes) {
    if (bytes > remaining - header_bytes) return false;
    header_bytes += bytes; return true;
  };
  if (!count_header(e.header.wire_id.size()) || !count_header(e.header.name.size()) || !count_header(e.header.wire_type.size()))
    return reject(ErrorKind::ResourceLimit, "part header limit");
  if (e.header.wire_metadata) {
    const auto root = e.header.wire_metadata->root();
    if (!root.is_object()) return reject(ErrorKind::ProtocolCorrupt, "tool metadata must be object");
    // Metadata arrives as one bounded immutable object, never as argument fragments.
    auto count_value = [&](auto&& self, json::Value value, size_t depth) -> bool {
      if (depth > limits_.max_json_depth || !count_header(8)) return false;
      if (value.is_string()) return count_header(value.as_string().size());
      if (value.is_object()) for (auto member : value.members())
        if (!count_header(member.key.size()) || !self(self, member.value, depth + 1)) return false;
      if (value.is_array()) for (auto element : value.elements()) if (!self(self, element, depth + 1)) return false;
      return true;
    };
    if (!count_value(count_value, root, 0)) return reject(ErrorKind::ResourceLimit, "part metadata limit");
  }
  content_bytes_ += header_bytes;
  parts_.emplace(e.part.value, Cursor{e.kind, e.header, {}, {}, false, {}});
  m->second.parts_by_order.emplace(e.order, e.part.value);
  return true;
}
bool Accumulator::apply(const PartDelta& e) {
  auto p = parts_.find(e.part.value);
  if (state_ != State::Receiving || p == parts_.end() || p->second.sealed || p->second.kind != e.payload.kind) return reject(ErrorKind::ProtocolCorrupt, "invalid part delta");
  auto& c = p->second;
  if (e.payload.channel != DeltaChannel::Content && e.payload.channel != DeltaChannel::Signature)
    return reject(ErrorKind::ProtocolCorrupt, "invalid delta channel");
  if (e.payload.channel == DeltaChannel::Signature && c.kind != PartKind::Thinking)
    return reject(ErrorKind::ProtocolCorrupt, "signature on non-thinking part");
  if (e.payload.bytes.size() > limits_.max_content_bytes - std::min(content_bytes_, limits_.max_content_bytes) || (c.kind == PartKind::ToolCall && e.payload.bytes.size() > limits_.max_tool_bytes - std::min(c.bytes.size(), limits_.max_tool_bytes))) return reject(ErrorKind::ResourceLimit, "content limit");
  if (e.payload.channel == DeltaChannel::Signature) {
    if (!c.signature) c.signature.emplace();
    c.signature->append(e.payload.bytes);
  } else c.bytes.append(e.payload.bytes);
  content_bytes_ += e.payload.bytes.size(); return true;
}
bool Accumulator::apply(const PartSeal& e) {
  auto p = parts_.find(e.part.value);
  if ((state_ != State::Receiving && state_ != State::Draining) || p == parts_.end()) return reject(ErrorKind::ProtocolCorrupt, "invalid part seal");
  auto& c = p->second;
  if (e.wire_metadata) {
    const auto metadata = e.wire_metadata->root();
    if (!metadata.is_object() ||
        (c.kind != PartKind::Reasoning && c.kind != PartKind::Opaque && c.kind != PartKind::ToolCall))
      return reject(ErrorKind::ProtocolCorrupt, "invalid native part metadata");
    if (c.sealed) {
      if (!c.header.wire_metadata || !json::equal(c.header.wire_metadata->root(), metadata))
        return reject(ErrorKind::ProtocolCorrupt, "contradictory native part snapshot");
    } else {
      size_t bytes = 0;
      if (!metadata_size(metadata, 0, limits_.max_content_bytes - std::min(content_bytes_, limits_.max_content_bytes),
                         limits_.max_json_depth, bytes))
        return reject(ErrorKind::ResourceLimit, "native part metadata limit");
      content_bytes_ += bytes;
      c.header.wire_metadata = e.wire_metadata;
    }
  }
  if (!c.sealed && c.kind == PartKind::Reasoning) {
    if (!c.header.wire_metadata || !reasoning_metadata(c.header.wire_metadata->root(), c.header.wire_id))
      return reject(ErrorKind::ProtocolCorrupt, "invalid completed reasoning item");
    std::string_view prefix(c.bytes);
    for (auto item : c.header.wire_metadata->root().get("summary").elements()) {
      const auto text = item.get("text").as_string();
      const auto size = std::min(prefix.size(), text.size());
      if (prefix.substr(0, size) != text.substr(0, size))
        return reject(ErrorKind::ProtocolCorrupt, "contradictory reasoning summary snapshot");
      prefix.remove_prefix(size);
    }
    if (!prefix.empty()) return reject(ErrorKind::ProtocolCorrupt, "incomplete reasoning summary snapshot");
  }
  if (!c.sealed && c.kind == PartKind::Opaque &&
      (!c.header.wire_metadata || c.header.wire_metadata->root().get("type").as_string() != c.header.wire_type))
    return reject(ErrorKind::ProtocolCorrupt, "invalid opaque item metadata");
  if (c.sealed) {
    if (!e.snapshot) return true;
    if (e.snapshot->size() > limits_.max_content_bytes ||
        (c.kind == PartKind::ToolCall && e.snapshot->size() > limits_.max_tool_bytes))
      return reject(ErrorKind::ResourceLimit, "snapshot limit");
    bool equal = false;
    if (const auto* text = std::get_if<Text>(&*c.value)) equal = text->value == *e.snapshot;
    else if (const auto* refusal = std::get_if<Refusal>(&*c.value)) equal = refusal->text == *e.snapshot;
    else if (const auto* thinking = std::get_if<Thinking>(&*c.value)) equal = thinking->text == *e.snapshot;
    else if (const auto* redacted = std::get_if<RedactedThinking>(&*c.value)) equal = redacted->data == *e.snapshot;
    else if (const auto* result = std::get_if<ServerToolResult>(&*c.value)) {
      auto snapshot = json::parse(*e.snapshot, {limits_.max_content_bytes, limits_.max_json_depth});
      if (const auto* document = std::get_if<json::Document>(&snapshot))
        equal = result->content && json::equal(result->content->root(), document->root());
    }
    else if (const auto* invalid = std::get_if<InvalidToolCall>(&*c.value)) equal = invalid->raw_fragment == *e.snapshot;
    else if (const auto* tool = std::get_if<ToolCall>(&*c.value)) {
      auto snapshot = json::parse(*e.snapshot, {limits_.max_tool_bytes, limits_.max_json_depth});
      if (const auto* document = std::get_if<json::Document>(&snapshot))
        equal = json::equal(tool->input->root(), document->root());
    }
    return equal || reject(ErrorKind::ProtocolCorrupt, "contradictory sealed snapshot");
  }
  if (e.snapshot) {
    if (!e.snapshot->starts_with(c.bytes)) return reject(ErrorKind::ProtocolCorrupt, "contradictory part snapshot");
    const auto extra = e.snapshot->size() - c.bytes.size();
    if (extra > limits_.max_content_bytes - std::min(content_bytes_, limits_.max_content_bytes) || (c.kind == PartKind::ToolCall && e.snapshot->size() > limits_.max_tool_bytes)) return reject(ErrorKind::ResourceLimit, "snapshot limit");
    c.bytes.assign(*e.snapshot); content_bytes_ += extra;
  }
  c.value = seal_value(c, false); c.sealed = true;
  if (const auto* result = std::get_if<ServerToolResult>(&*c.value); result && !result->content)
    return reject(ErrorKind::ProtocolCorrupt, "invalid server result block");
  return true;
}
Part Accumulator::seal_value(Cursor& c, bool partial) {
  if (c.kind == PartKind::Text) return Text{std::move(c.bytes)};
  if (c.kind == PartKind::Refusal) return Refusal{std::move(c.bytes), {}};
  if (c.kind == PartKind::Thinking) return Thinking{std::move(c.bytes), std::move(c.signature)};
  if (c.kind == PartKind::RedactedThinking) return RedactedThinking{std::move(c.bytes)};
  if (c.kind == PartKind::Reasoning) {
    Reasoning result;
    result.id = c.header.wire_id;
    if (!c.header.wire_metadata) {
      if (!c.bytes.empty()) result.summary.push_back(std::move(c.bytes));
      return result;
    }
    const auto item = c.header.wire_metadata->root();
    result.summary.reserve(item.get("summary").size());
    for (auto value : item.get("summary").elements()) result.summary.emplace_back(value.get("text").as_string());
    result.content.reserve(item.get("content").size());
    for (auto value : item.get("content").elements()) result.content.emplace_back(value.get("text").as_string());
    if (item.get("encrypted_content").is_string()) result.encrypted_content = item.get("encrypted_content").as_string();
    if (item.get("status").is_string()) result.status = item.get("status").as_string();
    std::string{}.swap(c.bytes);
    return result;
  }
  if (c.kind == PartKind::Opaque) return Opaque{c.header.wire_type, c.header.wire_metadata};
  if (c.kind == PartKind::ServerToolResult) {
    auto parsed = json::parse(c.bytes, {limits_.max_content_bytes, limits_.max_json_depth});
    std::shared_ptr<const json::Document> content;
    if (auto* doc = std::get_if<json::Document>(&parsed); doc && doc->root().is_object()) {
      const auto type = doc->root().get("type"), id = doc->root().get("tool_use_id");
      if (type.is_string() && id.is_string() && type.as_string() == c.header.wire_type && id.as_string() == c.header.wire_id)
        content = std::make_shared<const json::Document>(std::move(*doc));
    }
    if (content) std::string{}.swap(c.bytes);
    return ServerToolResult{c.header.wire_id, c.header.wire_type, std::move(content)};
  }
  auto invalid = [&](InvalidReason reason) -> Part { return InvalidToolCall{c.header.wire_id, c.header.name, c.header.tool_kind, std::move(c.bytes), reason, c.header.wire_type, c.header.wire_metadata}; };
  if (partial || (c.header.wire_type == "function_call" && c.header.wire_metadata &&
      c.header.wire_metadata->root().get("status").as_string() == "incomplete"))
    return invalid(InvalidReason::Truncated);
  if (c.bytes.empty()) return invalid(InvalidReason::Empty);
  auto parsed = json::parse(c.bytes, {limits_.max_tool_bytes, limits_.max_json_depth});
  if (auto* error = std::get_if<json::ParseError>(&parsed)) {
    InvalidReason reason = InvalidReason::NotJson;
    if (error->code == json::ParseCode::DuplicateKey) reason = InvalidReason::DuplicateKey;
    else if (error->code == json::ParseCode::DepthExceeded) reason = InvalidReason::DepthExceeded;
    else if (error->code == json::ParseCode::Syntax && stop_ && stop_->kind == StopKind::MaxTokens && incomplete(c.bytes)) reason = InvalidReason::Truncated;
    return invalid(reason);
  }
  auto doc = std::move(std::get<json::Document>(parsed));
  if (!doc.root().is_object()) return invalid(InvalidReason::NotJson);
  if (c.header.wire_id.empty() || c.header.name.empty()) return invalid(InvalidReason::Other);
  std::string{}.swap(c.bytes);
  return ToolCall{c.header.wire_id, c.header.name, c.header.tool_kind, std::make_shared<const json::Document>(std::move(doc)), c.header.wire_type, c.header.wire_metadata};
}
bool Accumulator::apply(const MessageSeal& e) {
  auto m = messages_.find(e.message.value);
  if ((state_ != State::Receiving && state_ != State::Draining) || m == messages_.end() || m->second.sealed) return reject(ErrorKind::ProtocolCorrupt, "invalid message seal");
  for (const auto& [order, id] : m->second.parts_by_order) {
    (void)order;
    if (!parts_.at(id).sealed) return reject(ErrorKind::ProtocolCorrupt, "unsealed message part");
  }
  if (e.wire_output) {
    if (!e.wire_output->root().is_array()) return reject(ErrorKind::ProtocolCorrupt, "native output must be ordered array");
    size_t bytes = 0;
    if (!metadata_size(e.wire_output->root(), 0,
                       limits_.max_content_bytes - std::min(content_bytes_, limits_.max_content_bytes),
                       limits_.max_json_depth, bytes))
      return reject(ErrorKind::ResourceLimit, "native output metadata limit");
    content_bytes_ += bytes;
    m->second.message.wire_output = e.wire_output;
  }
  m->second.sealed = true; return true;
}
bool Accumulator::apply(const UsageUpdate& e) {
  if (state_ != State::Receiving && state_ != State::Draining) return reject(ErrorKind::ProtocolCorrupt, "invalid usage transition");
  usage_ = e.snapshot; return true;
}
bool Accumulator::apply(const Stop& e) {
  if (state_ != State::Receiving || e.reason.raw.empty()) return reject(ErrorKind::ProtocolCorrupt, "invalid stop transition");
  if (e.reason.kind == StopKind::MaxTokens) {
    for (auto& [id, p] : parts_) {
      (void)id;
      if (p.value) if (auto* invalid = std::get_if<InvalidToolCall>(&*p.value);
          invalid && invalid->reason == InvalidReason::NotJson && incomplete(invalid->raw_fragment))
        invalid->reason = InvalidReason::Truncated;
    }
  }
  stop_ = e.reason; state_ = State::Draining; return true;
}
std::vector<Message> Accumulator::take_messages(bool partial) {
  std::vector<Message> result;
  result.reserve(messages_.size());
  for (auto& [id, m] : messages_) {
    (void)id;
    m.message.parts.reserve(m.parts_by_order.size());
    for (const auto& [order, part_id] : m.parts_by_order) {
      (void)order;
      auto& p = parts_.at(part_id);
      m.message.parts.push_back(p.value ? std::move(*p.value) : seal_value(p, partial));
    }
    if (m.native_context) m.message.native = std::shared_ptr<const NativeReplay>(
        new NativeReplay(std::move(m.native_context), m.message, stop_ ? &*stop_ : nullptr, !partial));
    result.push_back(std::move(m.message));
  }
  parts_.clear();
  messages_.clear();
  return result;
}
bool Accumulator::apply(const Commit& e) {
  if (state_ != State::Draining || !stop_ || e.evidence.empty()) return reject(ErrorKind::ProtocolCorrupt, "commit without evidence");
  for (const auto& [id, m] : messages_) { (void)id; if (!m.sealed) return reject(ErrorKind::ProtocolCorrupt, "commit with open message"); }
  auto messages = take_messages(false);
  if (stop_->kind == StopKind::EndTurn) {
    for (const auto& m : messages) for (const auto& part : m.parts) if (const auto* call = std::get_if<ToolCall>(&part); call && call->kind != ToolCallKind::ServerExecuted) stop_->kind = StopKind::ToolUse;
  }
  if (usage_.stage != UsageStage::Missing) usage_.stage = UsageStage::Final;
  outcome_ = Completion{std::move(messages), std::move(*stop_), std::move(usage_)};
  state_ = State::Terminal; return true;
}
bool Accumulator::apply(const Fail& e) {
  if (terminal()) return false;
  if (usage_.stage != UsageStage::Missing) usage_.stage = UsageStage::Partial;
  outcome_ = Failure{e.error, PartialCompletion{take_messages(true), std::move(usage_), std::move(stop_)}};
  state_ = State::Terminal; return true;
}
} // namespace sp
