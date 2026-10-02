#include "codecs/gemini.h"
#include "core/native.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include <algorithm>
#include <atomic>
#include <limits>

namespace sp::gemini {
namespace {
uint64_t next_ownership_generation() {
  static std::atomic<uint64_t> next{1};
  auto value = next.load(std::memory_order_relaxed);
  while (value != std::numeric_limits<uint64_t>::max()) {
    if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return value;
  }
  return 0;
}
bool count(json::Value v) { return v.is_uint() || (v.is_int() && v.as_int() >= 0); }
uint64_t number(json::Value v) { return v.is_uint() ? v.as_uint() : static_cast<uint64_t>(v.as_int()); }
bool nonempty(json::Value v) { return v.is_string() && !v.as_string().empty(); }
bool envelope_unique(json::Value v, bool function_call = false) {
  if (v.is_object()) {
    std::set<std::string_view> keys;
    for (auto m : v.members()) {
      if (!keys.insert(m.key).second) return false;
      if (function_call && m.key == "args") continue;
      if (!envelope_unique(m.value, m.key == "functionCall")) return false;
    }
  } else if (v.is_array()) for (auto p : v.elements()) if (!envelope_unique(p)) return false;
  return true;
}
StopKind stop_kind(std::string_view reason, bool call) {
  if (reason == "STOP") return call ? StopKind::ToolUse : StopKind::EndTurn;
  if (reason == "MAX_TOKENS") return StopKind::MaxTokens;
  if (reason == "MALFORMED_FUNCTION_CALL" || reason == "UNEXPECTED_TOOL_CALL" || reason == "TOO_MANY_TOOL_CALLS") return StopKind::MalformedCall;
  if (reason == "SAFETY" || reason == "RECITATION" || reason == "BLOCKLIST" || reason == "PROHIBITED_CONTENT" || reason == "SPII" || reason == "IMAGE_SAFETY" || reason == "IMAGE_PROHIBITED_CONTENT" || reason == "IMAGE_RECITATION" || reason == "ESCALATION" || reason == "PUP_LIMITED_DISABLED") return StopKind::ContentFilter;
  return StopKind::Unknown;
}
}
Codec::Codec(const descriptor::ValidatedDescriptor& d, Mode mode, Accumulator& a,
             std::shared_ptr<const NativeContext> context, SemanticLimits limits)
    : descriptor_(d), mode_(mode), accumulator_(a), context_(std::move(context)), limits_(limits) {}
bool Codec::emit(const Event& e) { return accumulator_.accept(e, buffered_failure_ ? &*buffered_failure_ : nullptr); }
bool Codec::fail(ErrorKind kind, std::string label) {
  if (buffered_failure_ && !closed_) return false;
  emit(Fail{{kind, std::move(label)}}); return false;
}
void Codec::unknown(json::Value v, std::initializer_list<std::string_view> keys) {
  for (auto m : v.members()) if (std::find(keys.begin(), keys.end(), m.key) == keys.end()) ++diagnostics_.unknown_properties;
}
std::shared_ptr<const json::Document> Codec::own(json::Value v) {
  auto parsed = json::parse(v.dump(), {limits_.max_content_bytes, limits_.max_json_depth});
  if (auto doc = std::get_if<json::Document>(&parsed)) return std::make_shared<const json::Document>(std::move(*doc));
  // Duplicate argument keys remain diagnostic, non-executable calls. Preserve
  // their exact native subtree rather than laundering it into a valid object.
  if (auto e = std::get_if<json::ParseError>(&parsed); e && e->code == json::ParseCode::DuplicateKey && envelope_unique(e->context.root()))
    return std::make_shared<const json::Document>(std::move(e->context));
  fail(ErrorKind::ProtocolCorrupt, "invalid retained Gemini JSON"); return {};
}
bool Codec::buffered(std::string_view body, Close close) {
  if (closed_ || accumulator_.terminal()) return false;
  if (mode_ != Mode::Buffered || body_seen_) return fail(ErrorKind::Misuse, "unexpected buffered Gemini response");
  body_seen_ = true;
  if (!close.normal) buffered_failure_ = Error{close.error, "abnormal Gemini response close"};
  const bool decoded = document(body);
  finish(close);
  return decoded && accumulator_.outcome() && std::holds_alternative<Completion>(*accumulator_.outcome());
}
bool Codec::frame(std::string_view event, std::string_view data) {
  if (closed_ || accumulator_.terminal()) return false;
  if (mode_ != Mode::Sse) return fail(ErrorKind::Misuse, "unexpected Gemini stream frame");
  if (event == "error") return fail(ErrorKind::RemoteFailure, "remote Gemini error event");
  if (!event.empty() && event != "message") return fail(ErrorKind::Unsupported, "unknown Gemini SSE event");
  return document(data);
}
bool Codec::identity(json::Value root) {
  auto id = root.get("responseId"), model = root.get("modelVersion");
  if (id.valid()) {
    if (!nonempty(id) || (!generation_.empty() && generation_ != id.as_string())) return fail(ErrorKind::ProtocolCorrupt, "Gemini response identity changed");
    generation_ = id.as_string();
  }
  if (model.valid()) {
    if (!nonempty(model) || (!model_.empty() && model_ != model.as_string())) return fail(ErrorKind::ProtocolCorrupt, "Gemini model identity changed");
    const auto expected = context_->model();
    auto value = model.as_string();
    bool match = value == expected;
    // An alias can resolve to a numeric version, but never to another model.
    if (!match && value.starts_with(expected) && value.size() > expected.size() + 1 && value[expected.size()] == '-') {
      match = true;
      for (auto c : value.substr(expected.size() + 1)) if ((c < '0' || c > '9') && c != '-') match = false;
    }
    if (!match) return fail(ErrorKind::ProtocolCorrupt, "Gemini response model mismatch");
    model_ = value;
  }
  return true;
}
bool Codec::document(std::string_view bytes) {
  if (!context_ || descriptor_.family() != "google.generate" || !context_->matches_descriptor(descriptor_)) return fail(ErrorKind::InvalidConfig, "Gemini request context required");
  if (bytes.size() > limits_.max_content_bytes - std::min(input_bytes_, limits_.max_content_bytes)) return fail(ErrorKind::ResourceLimit, "Gemini response byte limit");
  input_bytes_ += bytes.size();
  auto parsed = json::parse(bytes, {limits_.max_content_bytes, limits_.max_json_depth});
  json::Value root;
  if (auto e = std::get_if<json::ParseError>(&parsed)) {
    if (e->code == json::ParseCode::DuplicateKey && envelope_unique(e->context.root())) root = e->context.root();
    else return fail(e->code == json::ParseCode::SizeExceeded || e->code == json::ParseCode::DepthExceeded ? ErrorKind::ResourceLimit : ErrorKind::ProtocolCorrupt, "invalid Gemini response JSON");
  } else root = std::get<json::Document>(parsed).root();
  if (!root.is_object()) return fail(ErrorKind::ProtocolCorrupt, "Gemini response must be object");
  if (root.get("error").valid()) return fail(ErrorKind::RemoteFailure, "remote Gemini error response");
  if (!identity(root)) return false;
  unknown(root, {"candidates", "promptFeedback", "usageMetadata", "modelVersion", "responseId", "modelStatus"});
  if (!begun_) {
    if (!emit(Begin{generation_}) || !emit(MessageBegin{{0}, generation_.empty() ? std::nullopt : std::optional<std::string>{generation_}, Role::Assistant, context_})) return false;
    begun_ = true;
  }
  if (auto feedback = root.get("promptFeedback"); feedback.valid()) {
    if (!feedback.is_object()) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini prompt feedback");
    auto blocked = feedback.get("blockReason");
    if (blocked.valid() && (!blocked.is_string() || (blocked.as_string() != "BLOCK_REASON_UNSPECIFIED" && !blocked.as_string().empty()))) return fail(blocked.is_string() ? ErrorKind::RemoteFailure : ErrorKind::ProtocolCorrupt, "Gemini prompt blocked");
  }
  if (auto candidates = root.get("candidates"); candidates.valid()) {
    if (!candidates.is_array() || candidates.size() > 1) return fail(ErrorKind::ProtocolCorrupt, "Gemini requires a single candidate");
    for (auto c : candidates.elements()) if (!candidate(c)) return false;
  }
  if (auto u = root.get("usageMetadata"); u.valid() && !u.is_null()) if (!usage(u)) return false;
  return true;
}
bool Codec::candidate(json::Value c) {
  if (!c.is_object()) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini candidate");
  auto index = c.get("index");
  if (index.valid() && (!count(index) || number(index) != 0)) return fail(ErrorKind::ProtocolCorrupt, "Gemini candidate index mismatch");
  auto reason = c.get("finishReason");
  if (reason.valid() && !reason.is_string()) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini finish reason");
  const bool terminal = reason.is_string() && !reason.as_string().empty() && reason.as_string() != "FINISH_REASON_UNSPECIFIED";
  if (stopped_) return fail(ErrorKind::ProtocolCorrupt, "Gemini candidate after finish reason");
  const auto raw = terminal ? reason.as_string() : std::string_view{};
  if (auto content = c.get("content"); content.valid()) {
    if (!content.is_object()) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini candidate content");
    if (auto role = content.get("role"); role.valid() && (!role.is_string() || role.as_string() != "model")) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini output role");
    auto parts = content.get("parts");
    if ((!parts.valid() && !terminal) || (parts.valid() && !parts.is_array()))
      return fail(ErrorKind::ProtocolCorrupt, "Gemini parts array required outside an empty terminal");
    for (auto p : parts.elements()) if (!part(p)) return false;
  }
  unknown(c, {"index", "content", "finishReason", "finishMessage", "safetyRatings", "citationMetadata", "tokenCount", "groundingAttributions", "groundingMetadata", "avgLogprobs", "logprobsResult", "urlContextMetadata"});
  if (terminal) {
    auto details = own(c); if (!details) return false;
    stop_ = StopReason{stop_kind(raw, has_call_), std::string(raw), {}, std::move(details)};
    stopped_ = true;
    if (!emit(Stop{*stop_})) return false;
    for (auto local : pending_seals_) if (!emit(PartSeal{local, {}, {}})) return false;
    pending_seals_.clear();
    if (stop_->kind == StopKind::ContentFilter || raw == "MISSING_THOUGHT_SIGNATURE" || raw == "MALFORMED_RESPONSE") return fail(ErrorKind::RemoteFailure, "Gemini generation blocked or rejected");
  }
  return true;
}
bool Codec::part(json::Value p) {
  if (!p.is_object()) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini part");
  if (wire_parts_.size() >= limits_.max_parts) return fail(ErrorKind::ResourceLimit, "Gemini part limit");
  auto thought = p.get("thought"), signature = p.get("thoughtSignature");
  if ((thought.valid() && !thought.is_bool()) || (signature.valid() && !signature.is_string())) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini thought metadata");
  unsigned data_fields = 0;
  std::string_view data_key;
  for (auto key : {"text", "inlineData", "functionCall", "functionResponse", "fileData", "executableCode", "codeExecutionResult", "toolCall", "toolResponse"})
    if (p.get(key).valid()) { ++data_fields; data_key = key; }
  if (data_fields > 1) return fail(ErrorKind::ProtocolCorrupt, "Gemini part contains conflicting data fields");
  for (auto m : p.members()) {
    if (m.key != data_key && m.key != "thought" && m.key != "thoughtSignature" && m.key != "partMetadata" && m.key != "mediaResolution" && m.key != "mediaProcessing" && m.key != "audioTranscription" && m.key != "speechMetadata" && m.key != "videoMetadata") return fail(ErrorKind::Unsupported, "unknown Gemini part semantic");
  }
  auto retained = own(p); if (!retained) return false;
  const LocalId local{static_cast<uint32_t>(wire_parts_.size())};
  PartHeader header; header.wire_type = data_key.empty() ? "empty" : std::string(data_key); header.wire_metadata = retained;
  PartKind kind = PartKind::Opaque;
  std::string content;
  if (data_key == "text") {
    if (!p.get("text").is_string()) return fail(ErrorKind::ProtocolCorrupt, "Gemini text must be string");
    kind = thought.is_bool() && thought.as_bool() ? PartKind::Thinking : PartKind::Text;
    content = p.get("text").as_string();
  } else if (data_key == "functionCall") {
    auto call = p.get("functionCall");
    if (!call.is_object() || !nonempty(call.get("name"))) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini function identity");
    auto id = call.get("id");
    if (id.valid() && !nonempty(id)) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini function call id");
    if (!context_->client_tool_declared(call.get("name").as_string())) return fail(ErrorKind::Unsupported, "undeclared Gemini client function");
    if (call.get("partialArgs").valid() || call.get("willContinue").valid()) return fail(ErrorKind::Unsupported, "streamed partial function arguments unsupported");
    header.name = call.get("name").as_string();
    if (id.valid()) header.wire_id = id.as_string();
    else {
      if (!ownership_generation_) ownership_generation_ = next_ownership_generation();
      if (!ownership_generation_) return fail(ErrorKind::ResourceLimit, "Gemini local ownership id exhausted");
      header.wire_id = "gemini-local-" + std::to_string(ownership_generation_) + '-' + std::to_string(local.value);
    }
    if (!calls_.insert(header.wire_id).second) return fail(ErrorKind::ProtocolCorrupt, "duplicate Gemini function call id");
    auto args = call.get("args");
    content = args.valid() ? args.dump() : "{}";
    kind = PartKind::ToolCall;
    has_call_ = true;
    unknown(call, {"id", "name", "args"});
  } else if (!data_key.empty() && !p.get(data_key).is_object()) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini opaque part payload");
  if (!emit(PartBegin{{0}, local, kind, std::move(header), wire_parts_.size()})) return false;
  if (kind == PartKind::Text || kind == PartKind::Thinking || kind == PartKind::ToolCall)
    if (!emit(PartDelta{local, {kind, content}})) return false;
  if (kind == PartKind::Thinking && signature.valid()) if (!emit(PartDelta{local, {kind, signature.as_string(), DeltaChannel::Signature}})) return false;
  if (kind == PartKind::ToolCall) pending_seals_.push_back(local);
  else if (!emit(PartSeal{local, {}, {}})) return false;
  wire_parts_.push_back(std::move(retained));
  return true;
}
bool Codec::usage(json::Value v) {
  if (!v.is_object()) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini usage");
  Usage next; next.stage = UsageStage::Partial;
  auto field = [&](std::string_view key, std::optional<Count>& out) {
    auto n = v.get(key);
    if (!n.valid() || n.is_null()) return true;
    if (!count(n)) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini usage counter");
    out = Count{number(n), Evidence::Reported}; return true;
  };
  std::optional<Count> candidates, tool;
  if (!field("promptTokenCount", next.input_total) || !field("cachedContentTokenCount", next.cache_read) || !field("candidatesTokenCount", candidates) || !field("thoughtsTokenCount", next.reasoning) || !field("toolUsePromptTokenCount", tool) || !field("totalTokenCount", next.provider_reported_total)) return false;
  if (candidates) next.extra["candidatesTokenCount"] = *candidates;
  if (tool) next.extra["toolUsePromptTokenCount"] = *tool;
  auto conflict = [&](std::string key, std::string detail) { next.quality = UsageQuality::Inconsistent; next.conflicts.push_back({std::move(key), std::move(detail)}); };
  // Generated billable output is candidates + thoughts, not candidates with
  // thoughts treated as an inclusive subset. Missing thoughts remain unknown.
  if (candidates && next.reasoning) {
    if (next.reasoning->value > std::numeric_limits<uint64_t>::max() - candidates->value)
      return fail(ErrorKind::ProtocolCorrupt, "Gemini usage sum overflow");
    const auto sum = candidates->value + next.reasoning->value;
    next.output_total = Count{sum, Evidence::Derived};
  }
  if (next.input_total && next.cache_read) {
    if (next.cache_read->value > next.input_total->value) conflict("cache_read", "cached prompt exceeds prompt total");
    else next.input_uncached = Count{next.input_total->value - next.cache_read->value, Evidence::Derived};
  }
  if (next.input_total && next.output_total) {
    if (next.output_total->value > std::numeric_limits<uint64_t>::max() - next.input_total->value)
      return fail(ErrorKind::ProtocolCorrupt, "Gemini usage sum overflow");
    const auto sum = next.input_total->value + next.output_total->value;
    next.total = Count{sum, Evidence::Derived};
    if (next.provider_reported_total && next.provider_reported_total->value != sum) conflict("total", "provider total differs from prompt plus candidates plus thoughts");
  }
  for (auto key : {"promptTokensDetails", "cacheTokensDetails", "candidatesTokensDetails", "toolUsePromptTokensDetails"}) {
    auto details = v.get(key);
    if (!details.valid() || details.is_null()) continue;
    if (!details.is_array()) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini usage details");
    for (auto entry : details.elements()) {
      if (!entry.is_object() || !nonempty(entry.get("modality")) || !count(entry.get("tokenCount"))) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini modality usage");
      auto name = std::string(key) + "." + std::string(entry.get("modality").as_string());
      if (!next.extra.emplace(name, Count{number(entry.get("tokenCount")), Evidence::Reported}).second) return fail(ErrorKind::ProtocolCorrupt, "duplicate Gemini modality usage");
    }
  }
  unknown(v, {"promptTokenCount", "cachedContentTokenCount", "candidatesTokenCount", "thoughtsTokenCount", "toolUsePromptTokenCount", "totalTokenCount", "promptTokensDetails", "cacheTokensDetails", "candidatesTokensDetails", "toolUsePromptTokensDetails", "serviceTier"});
  usage_ = std::move(next);
  return emit(UsageUpdate{usage_});
}
void Codec::finish(Close close) {
  if (closed_ || accumulator_.terminal()) { closed_ = true; return; }
  closed_ = true;
  if (!close.normal) { fail(close.error, "abnormal Gemini response close"); return; }
  if (!begun_ || !stopped_) { fail(ErrorKind::Truncated, "missing Gemini finish reason"); return; }
  std::string array;
  auto build = [&](json::BoundedWriter& w) {
    w.raw("["); bool comma = false;
    for (const auto& p : wire_parts_) { if (comma) w.raw(","); comma = true; w.value(p->root(), 1); }
    w.raw("]");
  };
  json::BoundedWriter measure({limits_.max_content_bytes, limits_.max_json_depth}); build(measure);
  if (!measure.ok()) { fail(ErrorKind::ResourceLimit, "Gemini native array limit"); return; }
  array.reserve(measure.size());
  json::BoundedWriter writer({measure.size(), limits_.max_json_depth}, &array); build(writer);
  auto parsed = json::parse(array, {limits_.max_content_bytes, limits_.max_json_depth});
  std::shared_ptr<const json::Document> wire;
  if (auto d = std::get_if<json::Document>(&parsed)) wire = std::make_shared<const json::Document>(std::move(*d));
  else if (auto e = std::get_if<json::ParseError>(&parsed); e && e->code == json::ParseCode::DuplicateKey && envelope_unique(e->context.root())) wire = std::make_shared<const json::Document>(std::move(e->context));
  else { fail(ErrorKind::ProtocolCorrupt, "invalid Gemini native array"); return; }
  if (!emit(MessageSeal{{0}, std::move(wire)})) return;
  if (usage_.stage != UsageStage::Missing) { usage_.stage = UsageStage::Final; if (!emit(UsageUpdate{usage_})) return; }
  emit(Commit{mode_ == Mode::Sse ? "gemini.finishReason+normal_sse_eof" : "gemini.finishReason+normal_close"});
}
} // namespace sp::gemini
