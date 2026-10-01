#include "codecs/accumulator.h"
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
  messages_.emplace(e.message.value, MessageCursor{Message{e.vendor_id.value_or(""), e.role, {}}, false});
  return true;
}
bool Accumulator::apply(const PartBegin& e) {
  auto m = messages_.find(e.message.value);
  if (state_ != State::Receiving || m == messages_.end() || m->second.sealed || parts_.contains(e.part.value)) return reject(ErrorKind::ProtocolCorrupt, "invalid part begin");
  if (parts_.size() >= limits_.max_parts) return reject(ErrorKind::ResourceLimit, "part limit");
  for (const auto& [id, p] : parts_) {
    (void)id;
    if (p.message == e.message && p.order == e.order) return reject(ErrorKind::ProtocolCorrupt, "duplicate part order");
  }
  if (e.header.wire_id.size() > limits_.max_content_bytes - std::min(content_bytes_, limits_.max_content_bytes) || e.header.name.size() > limits_.max_content_bytes - std::min(content_bytes_ + e.header.wire_id.size(), limits_.max_content_bytes)) return reject(ErrorKind::ResourceLimit, "part header limit");
  content_bytes_ += e.header.wire_id.size() + e.header.name.size();
  parts_.emplace(e.part.value, Cursor{e.message, e.kind, e.header, e.order, {}, false, {}});
  return true;
}
bool Accumulator::apply(const PartDelta& e) {
  auto p = parts_.find(e.part.value);
  if (state_ != State::Receiving || p == parts_.end() || p->second.sealed || p->second.kind != e.payload.kind) return reject(ErrorKind::ProtocolCorrupt, "invalid part delta");
  auto& c = p->second;
  if (e.payload.bytes.size() > limits_.max_content_bytes - std::min(content_bytes_, limits_.max_content_bytes) || (c.kind == PartKind::ToolCall && e.payload.bytes.size() > limits_.max_tool_bytes - std::min(c.bytes.size(), limits_.max_tool_bytes))) return reject(ErrorKind::ResourceLimit, "content limit");
  c.bytes.append(e.payload.bytes); content_bytes_ += e.payload.bytes.size(); return true;
}
bool Accumulator::apply(const PartSeal& e) {
  auto p = parts_.find(e.part.value);
  if ((state_ != State::Receiving && state_ != State::Draining) || p == parts_.end()) return reject(ErrorKind::ProtocolCorrupt, "invalid part seal");
  auto& c = p->second;
  if (c.sealed) {
    if (!e.snapshot) return true;
    if (e.snapshot->size() > limits_.max_content_bytes ||
        (c.kind == PartKind::ToolCall && e.snapshot->size() > limits_.max_tool_bytes))
      return reject(ErrorKind::ResourceLimit, "snapshot limit");
    bool equal = false;
    if (const auto* text = std::get_if<Text>(&*c.value)) equal = text->value == *e.snapshot;
    else if (const auto* refusal = std::get_if<Refusal>(&*c.value)) equal = refusal->text == *e.snapshot;
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
  c.value = seal_value(c, false); c.sealed = true; return true;
}
Part Accumulator::seal_value(Cursor& c, bool partial) {
  if (c.kind == PartKind::Text) return Text{std::move(c.bytes)};
  if (c.kind == PartKind::Refusal) return Refusal{std::move(c.bytes), {}};
  auto invalid = [&](InvalidReason reason) -> Part { return InvalidToolCall{c.header.wire_id, c.header.name, c.header.tool_kind, std::move(c.bytes), reason}; };
  if (partial) return invalid(InvalidReason::Truncated);
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
  return ToolCall{c.header.wire_id, c.header.name, c.header.tool_kind, std::make_shared<const json::Document>(std::move(doc))};
}
bool Accumulator::apply(const MessageSeal& e) {
  auto m = messages_.find(e.message.value);
  if ((state_ != State::Receiving && state_ != State::Draining) || m == messages_.end() || m->second.sealed) return reject(ErrorKind::ProtocolCorrupt, "invalid message seal");
  for (const auto& [id, p] : parts_) { (void)id; if (p.message == e.message && !p.sealed) return reject(ErrorKind::ProtocolCorrupt, "unsealed message part"); }
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
  std::vector<Cursor*> ordered;
  ordered.reserve(parts_.size());
  for (auto& [id, p] : parts_) { (void)id; ordered.push_back(&p); }
  std::sort(ordered.begin(), ordered.end(), [](const Cursor* a, const Cursor* b) { if (a->message.value != b->message.value) return a->message.value < b->message.value; return a->order < b->order; });
  for (auto* p : ordered) messages_.at(p->message.value).message.parts.push_back(p->value ? std::move(*p->value) : seal_value(*p, partial));
  for (auto& [id, m] : messages_) { (void)id; result.push_back(std::move(m.message)); }
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
