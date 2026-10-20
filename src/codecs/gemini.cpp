#include "codecs/gemini.h"
#include "codecs/served_model.h"
#include "codecs/media_output.h"
#include "codecs/reason.h"
#include "core/native.h"
#include "crypto/crypto.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include <algorithm>
#include <limits>

namespace sp::gemini {
namespace {
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
// The google.generate stop_reasons policy table restates this mapping (STOP as its call-free
// EndTurn); the codec does not consult it. gemini_semantic_test checks that the two agree.
StopKind stop_kind(std::string_view reason, bool call) {
  if (reason == "STOP") return call ? StopKind::ToolUse : StopKind::EndTurn;
  if (reason == "MAX_TOKENS") return StopKind::MaxTokens;
  if (reason == "MALFORMED_FUNCTION_CALL" || reason == "UNEXPECTED_TOOL_CALL" || reason == "TOO_MANY_TOOL_CALLS") return StopKind::MalformedCall;
  if (reason == "SAFETY" || reason == "RECITATION" || reason == "BLOCKLIST" || reason == "PROHIBITED_CONTENT" || reason == "SPII" || reason == "IMAGE_SAFETY" || reason == "IMAGE_PROHIBITED_CONTENT" || reason == "IMAGE_RECITATION" || reason == "ESCALATION" || reason == "PUP_LIMITED_DISABLED") return StopKind::ContentFilter;
  return StopKind::Unknown;
}
}
namespace {
// Formats whose frames concatenate into one valid stream: raw PCM, G.711 and MP3. Containers with a header per
// frame (WAV, Ogg) and every image or video stay one part per frame.
bool concatenable_audio(std::string_view mime) noexcept {
  const auto essence = mime_essence(mime);
  for (const std::string_view known : {"audio/l16", "audio/pcm", "audio/mulaw", "audio/alaw", "audio/basic", "audio/mpeg", "audio/mp3"}) {
    if (essence.size() != known.size()) continue;
    bool same = true;
    for (std::size_t i = 0; i < known.size(); ++i) {
      char c = essence[i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
      if (c != known[i]) { same = false; break; }
    }
    if (same) return true;
  }
  return false;
}
// Whether `next` has the members of `head` and the same values for all of them but `data`, which both carry.
bool same_apart_from(json::Value head, json::Value next, std::string_view data) {
  if (!head.is_object() || !next.is_object() || head.size() != next.size() || !next.get(data).valid()) return false;
  for (auto member : head.members())
    if (member.key != data && !json::equal(member.value, next.get(member.key))) return false;
  return true;
}
// A wire part continues the one before it when the two differ only in their streamed data: the text, or the base64 of an
// inline blob. `inner` names that member inside the `outer` object, or is empty when `outer` holds the data itself.
bool continues_wire(json::Value head, json::Value next, std::string_view outer, std::string_view inner) {
  return same_apart_from(head, next, outer) && (inner.empty() || same_apart_from(head.get(outer), next.get(outer), inner));
}
}  // namespace
Codec::Codec(const descriptor::ValidatedDescriptor& d, Mode mode, Accumulator& a,
             std::shared_ptr<const NativeContext> context, SemanticLimits limits)
    : descriptor_(d), mode_(mode), accumulator_(a), context_(std::move(context)), limits_(limits) {
  served::observe(descriptor_, accumulator_, context_, {});
}
bool Codec::emit(const Event& e) { return accumulator_.accept(e, buffered_failure_ ? &*buffered_failure_ : nullptr); }
bool Codec::fail(ErrorKind kind, std::string label, std::string_view vendor_code) {
  if (buffered_failure_ && !closed_) return false;
  Error error{kind, std::move(label)};
  error.reason = reason::code(reason::Source::Gemini, error.safe_message);
  const bool machine = !vendor_code.empty() && vendor_code.size() <= 64 && vendor_code.front() >= 'A' && vendor_code.front() <= 'Z' &&
      std::all_of(vendor_code.begin(), vendor_code.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; });
  if (machine) error.vendor_code = std::string(vendor_code);
  emit(Fail{std::move(error)}); return false;
}
bool Codec::local_id(std::string& id, LocalId part) {
  if (ownership_token_.empty()) {
    unsigned char random[8];
    if (!crypto::random_bytes(random, sizeof random)) return fail(ErrorKind::ResourceLimit, "Gemini local ownership id exhausted");
    for (const auto byte : random) { ownership_token_ += "0123456789abcdef"[byte >> 4]; ownership_token_ += "0123456789abcdef"[byte & 15]; }
  }
  id = "gemini-local-" + ownership_token_ + '-' + std::to_string(part.value);
  return true;
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
  return document(data, event);
}
bool Codec::identity(json::Value root) {
  auto id = root.get("responseId"), model = root.get("modelVersion");
  if (id.valid()) {
    if (!nonempty(id) || (!generation_.empty() && generation_ != id.as_string())) return fail(ErrorKind::ProtocolCorrupt, "Gemini response identity changed");
    generation_ = id.as_string();
  }
  if (model.valid()) {
    if (!nonempty(model) || (!model_.empty() && model_ != model.as_string())) return fail(ErrorKind::ProtocolCorrupt, "Gemini model identity changed");
    // Google serves aliases and auto-routed models under other names (gemini-3.5-flash as
    // gemini-3.6-flash, lyria-3-pro-preview as lyria-3.5); the name is recorded, never refused.
    if (model_.empty()) served::observe(descriptor_, accumulator_, context_, model.as_string());
    model_ = model.as_string();
  }
  return true;
}
bool Codec::document(std::string_view bytes, std::string_view event) {
  const bool remote_error = event == "error";
  const bool unsupported_event = !event.empty() && event != "message" && !remote_error;
  auto reject = [&](ErrorKind kind, std::string label) {
    if (remote_error) return fail(ErrorKind::RemoteFailure, "remote Gemini error event");
    if (unsupported_event) return fail(ErrorKind::Unsupported, "unknown Gemini SSE event");
    return fail(kind, std::move(label));
  };
  if (!context_ || descriptor_.family() != "google.generate" || !context_->matches_descriptor(descriptor_)) return reject(ErrorKind::InvalidConfig, "Gemini request context required");
  if (bytes.size() > limits_.max_content_bytes - std::min(input_bytes_, limits_.max_content_bytes)) return reject(ErrorKind::ResourceLimit, "Gemini response byte limit");
  input_bytes_ += bytes.size();
  auto parsed = json::parse(bytes, {limits_.max_content_bytes, limits_.max_json_depth});
  std::shared_ptr<const json::Document> wire;
  if (auto e = std::get_if<json::ParseError>(&parsed)) {
    if (e->code == json::ParseCode::DuplicateKey && envelope_unique(e->context.root()))
      wire = std::make_shared<const json::Document>(std::move(e->context));
    else return reject(e->code == json::ParseCode::SizeExceeded || e->code == json::ParseCode::DepthExceeded ? ErrorKind::ResourceLimit : ErrorKind::ProtocolCorrupt, "invalid Gemini response JSON");
  } else wire = std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
  const auto root = wire->root();
  if (!root.is_object()) return reject(ErrorKind::ProtocolCorrupt, "Gemini response must be object");
  const Event raw = RawWire{event.empty() ? "generateContentResponse" : std::string(event), wire};
  if (remote_error) {
    const Error error{ErrorKind::RemoteFailure, "remote Gemini error event"};
    if (!accumulator_.accept(raw, &error)) return false;
    return fail(error.kind, error.safe_message);
  }
  if (!emit(raw)) return false;
  if (unsupported_event) return fail(ErrorKind::Unsupported, "unknown Gemini SSE event");
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
    if (blocked.valid() && (!blocked.is_string() || (blocked.as_string() != "BLOCK_REASON_UNSPECIFIED" && !blocked.as_string().empty()))) return fail(blocked.is_string() ? ErrorKind::RemoteFailure : ErrorKind::ProtocolCorrupt, "Gemini prompt blocked",
                                                                                                                                       blocked.is_string() ? blocked.as_string() : std::string_view{});
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
    if (run_.open && !close_run()) return false;
    if (!emit(Stop{*stop_})) return false;
    for (auto local : pending_seals_) if (!emit(PartSeal{local, {}, {}})) return false;
    pending_seals_.clear();
    if (stop_->kind == StopKind::ContentFilter || raw == "MISSING_THOUGHT_SIGNATURE" || raw == "MALFORMED_RESPONSE")
      return fail(ErrorKind::RemoteFailure, "Gemini generation blocked or rejected", raw);
  }
  return true;
}
// Seals the open run and fills the wire slot reserved for it: the first chunk as received when it was alone, else a part
// with the first chunk's members and all the data.
bool Codec::close_run() {
  Run run = std::move(run_);
  run_ = Run{};
  if (run.kind == PartKind::Media) {
    const auto tail = media_output::finish_stream(run.carry);
    if (!tail.empty()) {
      run.data += tail;
      if (!emit(PartDelta{run.part, {PartKind::Media, tail}})) return false;
    }
  }
  if (!emit(PartSeal{run.part, {}, {}})) return false;
  if (run.chunks == 1) { wire_parts_[run.slot] = std::move(run.head); return true; }
  const auto head = run.head->root();
  std::string bytes;
  json::BoundedWriter writer({limits_.max_content_bytes, limits_.max_json_depth}, &bytes);
  writer.raw("{");
  bool comma = false;
  for (auto member : head.members()) {
    if (comma) writer.raw(",");
    comma = true;
    writer.quoted(member.key).raw(":");
    if (member.key != run.outer) { writer.value(member.value, 1); continue; }
    if (run.inner.empty()) { writer.quoted(run.data); continue; }
    writer.raw("{");
    bool inner_comma = false;
    for (auto field : member.value.members()) {
      if (inner_comma) writer.raw(",");
      inner_comma = true;
      writer.quoted(field.key).raw(":");
      if (field.key == run.inner) writer.quoted(run.data); else writer.value(field.value, 2);
    }
    writer.raw("}");
  }
  writer.raw("}");
  if (!writer.ok()) return fail(ErrorKind::ResourceLimit, "Gemini native array limit");
  auto parsed = json::parse(bytes, {limits_.max_content_bytes, limits_.max_json_depth});
  auto* document = std::get_if<json::Document>(&parsed);
  if (!document) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini native array");
  wire_parts_[run.slot] = std::make_shared<const json::Document>(std::move(*document));
  return true;
}
bool Codec::part(json::Value p) {
  if (!p.is_object()) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini part");
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
  std::string_view fragment;
  std::optional<std::string_view> payload;
  std::string_view media_mime;
  const auto transcription = p.get("audioTranscription");
  if (data_key == "text") {
    if (!p.get("text").is_string()) return fail(ErrorKind::ProtocolCorrupt, "Gemini text must be string");
    kind = thought.is_bool() && thought.as_bool() ? PartKind::Thinking : PartKind::Text;
    fragment = p.get("text").as_string();
  } else if (data_key.empty() && transcription.valid()) {
    // A transcription model answers with a part whose only data is the transcript; it is the answer's text.
    // Next to inline audio the same member stays metadata of that part.
    if (!transcription.is_object() || !transcription.get("text").is_string()) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini audio transcription");
    unknown(transcription, {"text"});
    header.wire_type = "audioTranscription";
    kind = PartKind::Text;
    fragment = transcription.get("text").as_string();
  } else if (data_key == "functionCall") {
    auto call = p.get("functionCall");
    if (!call.is_object() || !nonempty(call.get("name"))) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini function identity");
    auto id = call.get("id");
    if (id.valid() && !nonempty(id)) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini function call id");
    if (!context_->client_tool_declared(call.get("name").as_string()) && !context_->service_declares_functions())
      return fail(ErrorKind::Unsupported, "undeclared Gemini client function");
    if (call.get("partialArgs").valid() || call.get("willContinue").valid()) return fail(ErrorKind::Unsupported, "streamed partial function arguments unsupported");
    header.name = call.get("name").as_string();
    if (id.valid()) header.wire_id = id.as_string();
    else if (!local_id(header.wire_id, local)) return false;
    if (!calls_.insert(header.wire_id).second) return fail(ErrorKind::ProtocolCorrupt, "duplicate Gemini function call id");
    auto args = call.get("args");
    content = args.valid() ? args.dump() : "{}";
    fragment = content;
    kind = PartKind::ToolCall;
    has_call_ = true;
    unknown(call, {"id", "name", "args"});
  } else if (data_key == "executableCode" || data_key == "toolCall") {
    // A call the service runs itself: code execution always, other built-in tools and MCP servers with
    // includeServerSideToolInvocations. The whole part stays the call's native metadata.
    const auto call = p.get(data_key);
    if (!call.is_object()) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini opaque part payload");
    const auto id = call.get("id"), tool_name = call.get("toolName"), tool_type = call.get("toolType"), args = call.get("args");
    if ((id.valid() && !nonempty(id)) || (tool_name.valid() && !nonempty(tool_name)) || (tool_type.valid() && !nonempty(tool_type)) ||
        (args.valid() && !args.is_object()))
      return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini server tool call");
    if (data_key == "executableCode") header.name = "code_execution";
    else if (tool_name.valid()) header.name = tool_name.as_string();
    else if (tool_type.valid()) {
      header.name = tool_type.as_string();
      for (auto& c : header.name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    } else return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini server tool call");
    if (id.valid()) header.wire_id = id.as_string();
    else if (!local_id(header.wire_id, local)) return false;
    if (!calls_.insert(header.wire_id).second) return fail(ErrorKind::ProtocolCorrupt, "duplicate Gemini function call id");
    open_server_calls_.emplace_back(data_key == "executableCode" ? "codeExecutionResult" : "toolResponse", header.wire_id);
    header.tool_kind = ToolCallKind::ServerExecuted;
    content = data_key == "executableCode" ? call.dump() : args.valid() ? args.dump() : "{}";
    fragment = content;
    kind = PartKind::ToolCall;
  } else if (data_key == "codeExecutionResult" || data_key == "toolResponse") {
    const auto result = p.get(data_key);
    if (!result.is_object()) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini opaque part payload");
    const auto id = result.get("id");
    if (id.valid() && !nonempty(id)) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini server tool result");
    // Without an id the result answers the oldest open call of its kind.
    const auto open = std::find_if(open_server_calls_.begin(), open_server_calls_.end(), [&](const auto& entry) {
      return entry.first == data_key && (!id.valid() || entry.second == id.as_string());
    });
    if (open != open_server_calls_.end()) { header.wire_id = open->second; open_server_calls_.erase(open); }
    else if (id.valid()) header.wire_id = id.as_string();
    else if (!local_id(header.wire_id, local)) return false;
    header.wire_metadata.reset();
    content = p.dump();
    payload = content;
    kind = PartKind::ServerToolResult;
  } else if ((data_key == "inlineData" || data_key == "fileData") && !(thought.is_bool() && thought.as_bool())) {
    // Generated media becomes a typed part. A thought image is an interim reasoning artifact and
    // stays opaque wire; the whole part is retained for the native array either way.
    const auto blob = p.get(data_key);
    const bool inline_data = data_key == "inlineData";
    const auto mime = blob.get("mimeType"), data = blob.get(inline_data ? "data" : "fileUri");
    if (!blob.is_object() || !data.is_string() || (mime.valid() && (!mime.is_string() || !mime_syntax(mime.as_string()))) ||
        (inline_data && !mime.is_string()) || (!inline_data && !reference_syntax(data.as_string())))
      return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini media part");
    if (inline_data) unknown(blob, {"mimeType", "data", "displayName"}); else unknown(blob, {"mimeType", "fileUri", "displayName"});
    header.media = media_output::describe(mime.is_string() ? mime.as_string() : std::string_view{},
                                          inline_data ? MediaSource::Inline : MediaSource::File,
                                          inline_data ? std::string_view{} : data.as_string());
    if (auto name = blob.get("displayName"); name.is_string()) header.media.name = std::string(name.as_string());
    header.wire_metadata.reset();
    kind = PartKind::Media;
    if (inline_data) payload = data.as_string();
    if (mime.is_string()) media_mime = mime.as_string();
  } else if (!data_key.empty() && !p.get(data_key).is_object()) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini opaque part payload");
  // Consecutive chunks of one answer, thought or transcript arrive as parts of the same shape, and so do the frames of one
  // audio stream. A chunk with a signature, a call, another member or another value of one is a part of its own.
  const bool text_run = mode_ == Mode::Sse && (kind == PartKind::Text || kind == PartKind::Thinking) && !signature.valid() &&
      (data_key == "text" || header.wire_type == "audioTranscription");
  const bool audio_run = kind == PartKind::Media && payload && !signature.valid() && concatenable_audio(media_mime);
  const std::string_view outer = audio_run ? "inlineData" : header.wire_type == "audioTranscription" ? "audioTranscription" : "text";
  const std::string_view inner = audio_run ? "data" : header.wire_type == "audioTranscription" ? "text" : "";
  if (run_.open) {
    if ((text_run || audio_run) && run_.kind == kind && run_.outer == outer && continues_wire(run_.head->root(), p, outer, inner)) {
      ++run_.chunks;
      if (audio_run) {
        if (!canonical_base64(*payload)) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini media part");
        const auto encoded = media_output::stream_chunk(run_.carry, *payload);
        run_.data += encoded;
        return encoded.empty() || emit(PartDelta{run_.part, {PartKind::Media, encoded}});
      }
      run_.data += fragment;
      return fragment.empty() || emit(PartDelta{run_.part, {kind, fragment}});
    }
    if (!close_run()) return false;
  }
  // A new wire part: the limit bounds the parts of the answer, not the chunks they arrive in.
  if (wire_parts_.size() >= limits_.max_parts) return fail(ErrorKind::ResourceLimit, "Gemini part limit");
  // An empty text part without a signature (Gemini ends most streams with one) says nothing: it stays in the
  // retained wire for native replay and is not a typed part.
  if (kind == PartKind::Text && fragment.empty() && !signature.valid()) {
    wire_parts_.push_back(std::move(retained));
    return true;
  }
  if (audio_run && !canonical_base64(*payload)) return fail(ErrorKind::ProtocolCorrupt, "invalid Gemini media part");
  const auto header_kind = header.tool_kind;
  if (!emit(PartBegin{{0}, local, kind, std::move(header), wire_parts_.size()})) return false;
  if (text_run || audio_run) {
    run_ = Run{};
    run_.open = true; run_.kind = kind; run_.part = local; run_.slot = wire_parts_.size(); run_.chunks = 1;
    run_.head = retained; run_.outer = std::string(outer); run_.inner = std::string(inner);
    wire_parts_.push_back(nullptr);
    if (audio_run) {
      const auto encoded = media_output::stream_chunk(run_.carry, *payload);
      run_.data = encoded;
      return encoded.empty() || emit(PartDelta{local, {PartKind::Media, encoded}});
    }
    run_.data = std::string(fragment);
    return emit(PartDelta{local, {kind, fragment}});
  }
  if (kind == PartKind::Text || kind == PartKind::Thinking || kind == PartKind::ToolCall)
    if (!emit(PartDelta{local, {kind, fragment}})) return false;
  if (kind == PartKind::Thinking && signature.valid()) if (!emit(PartDelta{local, {kind, signature.as_string(), DeltaChannel::Signature}})) return false;
  // A client call is sealed with the stop reason, which decides whether it is complete; the service's call is.
  if (kind == PartKind::ToolCall && header_kind == ToolCallKind::ClientExecuted) pending_seals_.push_back(local);
  else if (!emit(PartSeal{local, payload, {}})) return false;
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
  // When the request set the thinking budget to zero the vendor cannot have
  // generated thoughts, and it then omits thoughtsTokenCount: the candidates are
  // the whole output. Without that request evidence, or a total proving zero thoughts
  // (below), a missing count stays unknown.
  const bool no_thoughts = !next.reasoning && context_ && context_->thinking_disabled();
  // A reported total that already equals prompt + candidates leaves no room for thoughts,
  // so the omitted count is a derived zero. A nonzero tool-use prompt count makes that
  // equation ambiguous (it is not part of the derived total), so it stays unknown.
  if (!next.reasoning && candidates && next.input_total && next.provider_reported_total && (!tool || tool->value == 0) &&
      candidates->value <= std::numeric_limits<uint64_t>::max() - next.input_total->value &&
      next.input_total->value + candidates->value == next.provider_reported_total->value)
    next.reasoning = Count{0, Evidence::Derived};
  if (candidates && (next.reasoning || no_thoughts)) {
    const uint64_t thoughts = next.reasoning ? next.reasoning->value : 0;
    if (thoughts > std::numeric_limits<uint64_t>::max() - candidates->value)
      return fail(ErrorKind::ProtocolCorrupt, "Gemini usage sum overflow");
    const auto sum = candidates->value + thoughts;
    next.output_total = Count{sum, Evidence::Derived};
  }
  // The output budget can be spent while thinking: usageMetadata then carries the prompt, thoughts and
  // total counts but no candidatesTokenCount. The total still bounds the output, since everything
  // beyond the prompt was generated, so output_total (and the total below) derive from it. A nonzero
  // tool-use prompt count is part of the total but not of the output, which makes that arithmetic
  // ambiguous, and with a candidates count the rules above apply.
  if (!candidates && !next.output_total && next.input_total && next.provider_reported_total && (!tool || tool->value == 0)) {
    const auto reported = next.provider_reported_total->value, prompt = next.input_total->value;
    if (reported < prompt) conflict("total", "provider total is below the prompt count");
    else if (next.reasoning && next.reasoning->value > reported - prompt) conflict("reasoning", "thoughts exceed what the provider total leaves after the prompt");
    else next.output_total = Count{reported - prompt, Evidence::Derived};
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
  if (run_.open && !close_run()) return;
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
