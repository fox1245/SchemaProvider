#include "codecs/interactions.h"
#include "codecs/media_output.h"
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
bool media_type(std::string_view type) { return type == "image" || type == "audio" || type == "video" || type == "document"; }
MediaKind media_kind_of(std::string_view type) {
  return type == "image" ? MediaKind::Image : type == "audio" ? MediaKind::Audio : type == "video" ? MediaKind::Video : MediaKind::Document;
}
bool media_description_field(std::string_view name) {
  return name == "name" || name == "resolution" || name == "channels" || name == "sample_rate" || name == "processing";
}
bool thought_shape(json::Value content) {
  if (!content.is_object() || !content.get("type").is_string()) return false;
  if (content.get("type").as_string() == "text") return content.get("text").is_string();
  if (content.get("type").as_string() != "image") return false;
  const auto data = content.get("data"), uri = content.get("uri"), mime = content.get("mime_type");
  const bool inline_data = data.valid() && !data.is_null(), location = uri.valid() && !uri.is_null();
  return inline_data != location && (!mime.valid() || mime.is_null() || (mime.is_string() && mime_syntax(mime.as_string()))) &&
      (inline_data ? data.is_string() && canonical_base64(data.as_string()) : uri.is_string() && reference_syntax(uri.as_string()));
}
} // namespace
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
  if (event == "done" && data == "[DONE]") {
    if (!terminal_ || done_) return fail(ErrorKind::ProtocolCorrupt, "unexpected stream sentinel");
    done_ = true; return true;
  }
  return document(data, event, true);
}
bool Codec::identity(json::Value resource, bool initial) {
  auto id = resource.get("id");
  const auto status = resource.get("status");
  if (!resource.is_object() ||
      (!nonempty(status) && !(initial && mode_ == Mode::Sse && !status.valid())) ||
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
  const bool remote_error = streaming && event == "error";
  auto reject = [&](ErrorKind kind, std::string label) {
    return remote_error ? fail(ErrorKind::RemoteFailure, "remote interaction error") : fail(kind, std::move(label));
  };
  if (!context_ || descriptor_.family() != "google.interactions" || !context_->matches_descriptor(descriptor_))
    return reject(ErrorKind::InvalidConfig, "Interactions request context required");
  if (bytes.size() > limits_.max_content_bytes - std::min(input_bytes_, limits_.max_content_bytes))
    return reject(ErrorKind::ResourceLimit, "interaction response byte limit");
  input_bytes_ += bytes.size();
  auto parsed = json::parse(bytes, {limits_.max_content_bytes, limits_.max_json_depth});
  if (auto error = std::get_if<json::ParseError>(&parsed)) return reject(error->code == json::ParseCode::SizeExceeded ||
      error->code == json::ParseCode::DepthExceeded ? ErrorKind::ResourceLimit : ErrorKind::ProtocolCorrupt, "invalid interaction JSON");
  auto wire = std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
  const auto root = wire->root();
  if (!root.is_object()) return reject(ErrorKind::ProtocolCorrupt, "interaction object required");
  const auto type = root.get("event_type");
  std::string wire_type = streaming
      ? (nonempty(type) ? std::string(type.as_string()) : event.empty() ? "interaction" : std::string(event))
      : "interaction";
  const Event raw = RawWire{std::move(wire_type), wire};
  if (remote_error) {
    const Error error{ErrorKind::RemoteFailure, "remote interaction error"};
    if (!accumulator_.accept(raw, &error)) return false;
    return fail(error.kind, error.safe_message);
  }
  if (!emit(raw)) return false;
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
  if (!type.is_string() || (!event.empty() && event != type.as_string())) return fail(ErrorKind::ProtocolCorrupt, "interaction event discriminator mismatch");
  auto name = type.as_string();
  if (name == "error") return fail(ErrorKind::RemoteFailure, "remote interaction error");
  if (terminal_ || done_) return fail(ErrorKind::ProtocolCorrupt, "event after terminal interaction");
  if (name == "interaction.created") {
    unknown(root, {"event_type", "event_id", "interaction"});
    if (begun_ || !identity(root.get("interaction"), true)) return begun_ ? fail(ErrorKind::ProtocolCorrupt, "duplicate interaction creation") : false;
    auto resource = root.get("interaction");
    if (resource.get("status").valid() && resource.get("status").as_string() != "in_progress")
      return fail(ErrorKind::ProtocolCorrupt, "new interaction must be in progress");
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
  auto begin_part = [&](PartKind kind, PartHeader header, uint64_t offset) { return begin(step, index, kind, std::move(header), offset); };
  if (type == "thought") {
    unknown(value, {"type", "signature", "summary"});
    auto signature = value.get("signature"), summary = value.get("summary");
    if (signature.valid()) { if (!signature.is_string()) return fail(ErrorKind::ProtocolCorrupt, "invalid thought signature"); step.signature = signature.as_string(); }
    if (summary.valid() && !summary.is_array()) return fail(ErrorKind::ProtocolCorrupt, "invalid thought summary");
    if (!begin_part(PartKind::Thought, {}, 0)) return false;
    if (summary.valid()) for (auto content : summary.elements())
      if (!thought_content(step, content)) return false;
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
      const auto kind = item.get("type").as_string();
      if (media_type(kind)) { if (!media_item(step, index, item, false)) return false; continue; }
      if (kind != "text") return fail(ErrorKind::Unsupported, "unsupported model output content");
      if (!item.get("text").is_string()) return fail(ErrorKind::ProtocolCorrupt, "model output text required");
      unknown(item, {"type", "text", "annotations"});
      Content text; text.type = "text"; text.text = std::string(item.get("text").as_string()); step.content.push_back(std::move(text));
      if (!annotate(step.content.back(), item.get("annotations"))) return false;
      if (!begin_part(PartKind::Text, {}, step.content.size() - 1) || !emit(PartDelta{step.parts.back(), {PartKind::Text, step.content.back().text}})) return false;
    }
  }
  steps_.emplace(index, std::move(step)); return true;
}
bool Codec::begin(Step& step, uint64_t index, PartKind kind, PartHeader header, uint64_t offset) {
  if (next_part_ >= limits_.max_parts || next_part_ == std::numeric_limits<uint32_t>::max()) return fail(ErrorKind::ResourceLimit, "interaction part limit");
  LocalId id{next_part_++}; step.parts.push_back(id);
  return emit(PartBegin{{0}, id, kind, std::move(header), index * (static_cast<uint64_t>(limits_.max_parts) + 1) + offset});
}
// One image, audio, video or document content item of a model_output step. A complete item always
// begins a content item; a streamed audio delta whose MIME type agrees continues the previous audio run.
bool Codec::media_item(Step& step, uint64_t, json::Value item, bool streamed) {
  const auto type = item.get("type").as_string();
  unknown(item, {"type", "mime_type", "data", "uri", "resolution", "channels", "sample_rate", "rate", "processing", "name"});
  const auto present = [](json::Value v) { return v.valid() && !v.is_null(); };
  const auto mime = item.get("mime_type"), data = item.get("data"), uri = item.get("uri");
  if ((present(mime) && (!mime.is_string() || !mime_syntax(mime.as_string()))) || present(data) == present(uri) ||
      (present(data) && !data.is_string()) || (present(uri) && !uri.is_string()))
    return fail(ErrorKind::ProtocolCorrupt, "model output media needs a valid MIME type and exactly one of data or uri");
  auto description = [&](Content& content) {
    for (auto member : item.members()) {
      if (member.key == "type" || member.key == "mime_type" || member.key == "data" || member.key == "uri") continue;
      auto [field, inserted] = content.fields.emplace(member.key, member.value);
      if (!inserted && media_description_field(member.key) && !json::equal(field->second, member.value))
        return fail(ErrorKind::ProtocolCorrupt, "model output media description changed");
      if (!inserted) field->second = member.value;
    }
    return true;
  };
  if (streamed && present(data) && type == "audio" && !step.content.empty()) {
    auto& run = step.content.back();
    if (run.type == "audio" && run.uri.empty() && (!present(mime) || run.mime.empty() || mime.as_string() == run.mime)) {
      if (!description(run)) return false;
      if (present(mime) && run.mime.empty()) run.mime = mime.as_string();
      if (!canonical_base64(data.as_string())) return fail(ErrorKind::ProtocolCorrupt, "model output audio must be canonical base64");
      const auto size = base64_decoded_size(run.payload) + base64_decoded_size(data.as_string());
      if (size > limits_.max_content_bytes / 4 * 3) return fail(ErrorKind::ResourceLimit, "model output audio limit");
      if (!run.audio_run) run.audio_run = std::make_unique<std::string>(run.payload);
      media_output::append_audio(*run.audio_run, data.as_string());
      run.payload = *run.audio_run;
      return true;
    }
  }
  Content content; content.type = std::string(type);
  if (present(mime)) content.mime = std::string(mime.as_string());
  const bool inline_data = present(data);
  if (inline_data) {
    if (!canonical_base64(data.as_string())) return fail(ErrorKind::ProtocolCorrupt, "model output media must be canonical base64");
    content.payload = data.as_string();
  }
  else {
    content.uri = std::string(uri.as_string());
    if (!reference_syntax(content.uri)) return fail(ErrorKind::ProtocolCorrupt, "model output media location is invalid");
  }
  if (!description(content)) return false;
  step.content.push_back(std::move(content));
  // Delay the header until reconciliation: the final wire can supply a missing MIME type.
  if (next_part_ >= limits_.max_parts || next_part_ == std::numeric_limits<uint32_t>::max())
    return fail(ErrorKind::ResourceLimit, "interaction part limit");
  step.parts.push_back(LocalId{next_part_++});
  return true;
}
bool Codec::thought_content(Step& step, json::Value content) {
  if (!thought_shape(content)) return fail(ErrorKind::ProtocolCorrupt, "invalid thought summary content");
  auto doc = own(content); if (!doc) return false;
  step.summary.push_back(std::move(doc));
  return content.get("type").as_string() != "text" ||
      emit(PartDelta{step.parts[0], {PartKind::Thought, content.get("text").as_string()}});
}
bool Codec::annotations(json::Value value, std::string_view text) {
  if (!value.valid()) return true;
  if (!value.is_array()) return fail(ErrorKind::ProtocolCorrupt, "text annotations must be an array");
  for (auto annotation : value.elements()) {
    if (!annotation.is_object() || !annotation.get("type").is_string())
      return fail(ErrorKind::ProtocolCorrupt, "text annotation discriminator required");
    const auto type = annotation.get("type").as_string();
    if (type != "word_info" && type != "speech_metadata" && type != "url_citation" &&
        type != "file_citation" && type != "place_citation")
      return fail(ErrorKind::Unsupported, "unsupported critical text annotation");
    const auto start = annotation.get("start_index"), end = annotation.get("end_index");
    for (const auto position : {start, end})
      if (position.valid() && (!unsigned_count(position) ||
          number(position) > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) || number(position) > text.size()))
        return fail(ErrorKind::ProtocolCorrupt, "text annotation byte index outside content");
    if (start.valid() && end.valid() && number(start) > number(end))
      return fail(ErrorKind::ProtocolCorrupt, "text annotation byte range reversed");
    for (auto member : annotation.members()) {
      const auto name = member.key;
      const bool string_field =
          (type == "word_info" && (name == "text" || name == "speaker" || name == "start_offset" || name == "end_offset")) ||
          (type == "speech_metadata" && (name == "speaker" || name == "style")) ||
          (type == "url_citation" && (name == "url" || name == "title")) ||
          (type == "file_citation" && (name == "document_uri" || name == "file_name" || name == "media_id" || name == "source")) ||
          (type == "place_citation" && (name == "name" || name == "place_id" || name == "url"));
      if (string_field && !member.value.is_string())
        return fail(ErrorKind::ProtocolCorrupt, "invalid text annotation string metadata");
      if ((type == "file_citation" && name == "custom_metadata" && !member.value.is_object()) ||
          (type == "place_citation" && name == "review_snippets" && !member.value.is_array()) ||
          (type == "file_citation" && name == "page_number" && !member.value.is_int() && !member.value.is_uint()))
        return fail(ErrorKind::ProtocolCorrupt, "invalid text annotation metadata");
    }
    const auto word = annotation.get("text");
    if (type == "word_info" && word.is_string() && start.valid() && end.valid() &&
        text.substr(static_cast<size_t>(number(start)), static_cast<size_t>(number(end) - number(start))) != word.as_string())
      return fail(ErrorKind::ProtocolCorrupt, "transcribed word contradicts annotated text");
    if (type == "word_info" && word.is_string() && (!start.valid() || !end.valid()) &&
        text.find(word.as_string()) == std::string_view::npos)
      return fail(ErrorKind::ProtocolCorrupt, "transcribed word missing from annotated text");
  }
  return true;
}
bool Codec::annotate(Content& content, json::Value value) {
  if (!annotations(value, content.text)) return false;
  if (value.valid()) {
    content.annotations_present = true;
    for (auto annotation : value.elements()) content.annotations.push_back(annotation);
  }
  return true;
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
    return thought_content(step, content);
  }
  if (type == "text" && step.type == "model_output") {
    unknown(value, {"type", "text"}); auto text = value.get("text");
    if (!text.is_string()) return fail(ErrorKind::ProtocolCorrupt, "text delta required");
    if (step.content.empty() || step.content.back().type != "text") {
      Content item; item.type = "text"; step.content.push_back(std::move(item));
      if (!begin(step, index, PartKind::Text, {}, step.content.size() - 1)) return false;
    }
    step.content.back().text.append(text.as_string()); return emit(PartDelta{step.parts.back(), {PartKind::Text, text.as_string()}});
  }
  if (type == "text_annotation_delta") {
    unknown(value, {"type", "annotations"});
    // The schema supplies only a step index, not a content index. An annotation belongs to the
    // current text run; never reach backwards across intervening media or invent a text part.
    if (step.type != "model_output" || step.content.empty() || step.content.back().type != "text")
      return fail(ErrorKind::ProtocolCorrupt, "text annotation without current model output text");
    return annotate(step.content.back(), value.get("annotations"));
  }
  if (media_type(type) && step.type == "model_output") return media_item(step, index, value, true);
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
    if (original.get("content").valid() || !step.content.empty()) {
      key("content"); writer.raw("[");
      for (size_t i = 0; i < step.content.size(); ++i) {
        if (i) writer.raw(",");
        const auto& entry = step.content[i];
        if (entry.type != "text") {
          writer.raw("{\"type\":").quoted(entry.type);
          if (!entry.mime.empty()) writer.raw(",\"mime_type\":").quoted(entry.mime);
          if (entry.uri.empty()) writer.raw(",\"data\":").quoted(entry.payload); else writer.raw(",\"uri\":").quoted(entry.uri);
          for (const auto& [name, value] : entry.fields) writer.raw(",").quoted(name).raw(":").value(value, 3);
          writer.raw("}");
          continue;
        }
        writer.raw("{"); bool item_comma = false;
        auto item = original.get("content").at(i);
        for (auto member : item.members()) if (member.key != "text" && member.key != "annotations") {
          if (item_comma) writer.raw(",");
          item_comma = true; writer.quoted(member.key).raw(":").value(member.value, 3);
        }
        if (!item.valid()) { writer.raw("\"type\":\"text\""); item_comma = true; }
        if (item_comma) writer.raw(",");
        if (entry.annotations_present) {
          writer.raw("\"annotations\":[");
          for (size_t j = 0; j < entry.annotations.size(); ++j) {
            if (j) writer.raw(",");
            writer.value(entry.annotations[j], 4);
          }
          writer.raw("],");
        }
        writer.raw("\"text\":").quoted(entry.text).raw("}");
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
      if (!thought_shape(item) || (i < step.summary.size() && !json::equal(item, step.summary[i]->root())))
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
  if (!content.valid() && step.content.empty()) return true;
  if (!content.is_array() || content.size() != step.content.size()) return fail(ErrorKind::ProtocolCorrupt, "final output content mismatch");
  for (size_t i = 0; i < content.size(); ++i) {
    auto item = content.at(i);
    if (!item.is_object() || !item.get("type").is_string()) return fail(ErrorKind::ProtocolCorrupt, "invalid final output block");
    const auto type = item.get("type").as_string();
    const auto& entry = step.content[i];
    if (type != "text" && !media_type(type)) return fail(ErrorKind::Unsupported, "unsupported final output content");
    if (type != entry.type) return fail(ErrorKind::ProtocolCorrupt, "final output content type mismatch");
    if (type == "text") {
      if (!item.get("text").is_string() || !prefix(entry.text, item.get("text").as_string())) return fail(ErrorKind::ProtocolCorrupt, "final output prefix mismatch");
      const auto supplied = item.get("annotations");
      if (!annotations(supplied, item.get("text").as_string())) return false;
      if ((!supplied.valid() && entry.annotations_present) ||
          (supplied.valid() && supplied.size() < entry.annotations.size()))
        return fail(ErrorKind::ProtocolCorrupt, "final text annotations lost");
      size_t j = 0;
      for (const auto annotation : supplied.elements()) {
        if (j == entry.annotations.size()) break;
        if (!json::equal(entry.annotations[j++], annotation))
          return fail(ErrorKind::ProtocolCorrupt, "final text annotation prefix mismatch");
      }
      continue;
    }
    // A streamed description that omitted the MIME type has nothing to contradict.
    const auto mime = item.get("mime_type"), data = item.get("data"), uri = item.get("uri");
    const bool inline_data = data.valid() && !data.is_null(), location = uri.valid() && !uri.is_null();
    if ((mime.valid() && !mime.is_null() && (!mime.is_string() || !mime_syntax(mime.as_string()))) ||
        (!entry.mime.empty() && (!mime.is_string() || mime.as_string() != entry.mime)))
      return fail(ErrorKind::ProtocolCorrupt, "final output media type mismatch");
    if (inline_data == location || (entry.uri.empty() ? !data.is_string() || data.as_string() != entry.payload
                                                    : !uri.is_string() || uri.as_string() != entry.uri))
      return fail(ErrorKind::ProtocolCorrupt, "final output media mismatch");
    for (const auto& [name, value] : entry.fields)
      if (media_description_field(name) && !json::equal(value, item.get(name)))
        return fail(ErrorKind::ProtocolCorrupt, "final output media description mismatch");
  }
  return true;
}
bool Codec::seal(Step& step, json::Value final, uint64_t index) {
  if (step.type == "thought") { auto metadata = own(final); return metadata && emit(PartSeal{step.parts[0], {}, std::move(metadata)}); }
  if (step.type == "function_call") {
    auto arguments = final.get("arguments");
    if (!arguments.is_object()) return fail(ErrorKind::ProtocolCorrupt, "function arguments must be object");
    std::string bytes = step.argument_delta ? step.arguments : arguments.dump(); auto metadata = own(final);
    return metadata && emit(PartSeal{step.parts[0], bytes, std::move(metadata)});
  }
  for (size_t i = 0; i < step.parts.size(); ++i) {
    const auto item = final.get("content").at(i);
    std::optional<std::string_view> snapshot;
    if (step.content[i].type == "text") snapshot = item.get("text").as_string();
    else {
      const auto mime = item.get("mime_type"), data = item.get("data"), uri = item.get("uri");
      PartHeader header; header.wire_type = step.content[i].type;
      header.media = media_output::describe(mime.is_string() ? mime.as_string() : std::string_view{},
                                            data.is_string() ? MediaSource::Inline : MediaSource::File,
                                            uri.is_string() ? uri.as_string() : std::string_view{});
      header.media.kind = media_kind_of(step.content[i].type);
      if (auto name = item.get("name"); name.is_string()) header.media.name = std::string(name.as_string());
      if (!emit(PartBegin{{0}, step.parts[i], PartKind::Media, std::move(header),
                          index * (static_cast<uint64_t>(limits_.max_parts) + 1) + i})) return false;
      if (data.is_string()) snapshot = data.as_string();
    }
    if (!emit(PartSeal{step.parts[i], snapshot})) return false;
  }
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
    if (!reconcile(step, final) || !seal(step, final, index)) return false;
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
