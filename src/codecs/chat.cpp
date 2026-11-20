#include "codecs/chat.h"
#include "codecs/served_model.h"
#include "codecs/media_output.h"
#include "codecs/provider_cost.h"
#include "codecs/reason.h"
#include "codecs/stop_table.h"
#include "core/native.h"
#include "descriptor/descriptor.h"
#include "descriptor/stop_facts.h"
#include "json/json.h"
#include <algorithm>
#include <iterator>
#include <limits>

namespace sp::chat {
namespace {
// A streamed choice without a finish reason whose delta holds nothing but the assistant role and empty text.
bool empty_choice(json::Value choice) {
  const auto reason = choice.get("finish_reason");
  if (reason.valid() && !reason.is_null()) return false;
  const auto delta = choice.get("delta");
  if (!delta.is_object()) return false;
  for (auto member : delta.members()) {
    if (member.key == "role" && member.value.is_string() && member.value.as_string() == "assistant") continue;
    if (member.key == "content" && (member.value.is_null() || (member.value.is_string() && member.value.as_string().empty()))) continue;
    return false;
  }
  return true;
}
bool unsigned_count(json::Value v) { return v.is_uint() || (v.is_int() && v.as_int() >= 0); }
uint64_t uint_value(json::Value v) { return v.is_uint() ? v.as_uint() : static_cast<uint64_t>(v.as_int()); }
// Chat does not state the audio format of a reply; it is the one the request asked for.
std::string_view audio_mime(std::string_view format) {
  if (format == "wav") return "audio/wav";
  if (format == "mp3") return "audio/mpeg";
  if (format == "flac") return "audio/flac";
  if (format == "opus") return "audio/opus";
  if (format == "aac") return "audio/aac";
  if (format == "pcm16") return "audio/l16";
  return {};
}
bool metadata_only(json::Value root) {
  static constexpr std::string_view fields[]{"id", "model", "object", "created", "choices", "usage",
                                             "service_tier", "system_fingerprint", "obfuscation"};
  for (auto member : root.members())
    if (std::find(std::begin(fields), std::end(fields), member.key) == std::end(fields)) return false;
  return true;
}
}
Codec::Codec(const descriptor::ValidatedDescriptor& descriptor, Mode mode, Accumulator& accumulator,
             SemanticLimits limits, std::shared_ptr<const NativeContext> context)
    : descriptor_(descriptor), mode_(mode), accumulator_(accumulator), context_(std::move(context)), limits_(limits) {
  if (descriptor_.family() != "openai.chat") fail(ErrorKind::InvalidConfig, reason::chat::descriptor_family_mismatch);
  if (context_ && !context_->matches_descriptor(descriptor_))
    fail(ErrorKind::InvalidConfig, reason::chat::context_descriptor_mismatch);
  served::observe(descriptor_, accumulator_, context_, {}, false);
}
bool Codec::emit(const Event& e) {
  return accumulator_.accept(e, buffered_failure_ ? &*buffered_failure_ : nullptr);
}
bool Codec::fail(ErrorKind kind, const reason::Reason& message) {
  if (buffered_failure_ && !closed_) return false;
  Error error = reason::error(kind, message);
  emit(Fail{std::move(error)}); return false;
}
void Codec::unknown(json::Value v, std::initializer_list<std::string_view> known) {
  for (auto member : v.members()) if (std::find(known.begin(), known.end(), member.key) == known.end()) ++diagnostics_.unknown_properties;
}
bool Codec::buffered(std::string_view body, Close close) {
  if (closed_ || accumulator_.terminal()) return false;
  if (mode_ != Mode::Buffered || body_seen_) return fail(ErrorKind::Misuse, reason::chat::buffered_unexpected);
  body_seen_ = true;
  if (!close.normal) buffered_failure_ = Error{close.error, "abnormal response close"};
  const bool decoded = document(body, {}, false);
  finish(close);
  return decoded && accumulator_.outcome() && std::holds_alternative<Completion>(*accumulator_.outcome());
}
bool Codec::frame(std::string_view event, std::string_view data) {
  if (closed_ || accumulator_.terminal()) return false;
  if (mode_ != Mode::Sse) return fail(ErrorKind::Misuse, reason::chat::stream_frame_unexpected);
  if (data == "[DONE]" && event != "error") {
    if (!event.empty() && event != "message") return fail(ErrorKind::Unsupported, reason::chat::stream_event_unknown);
    if (done_) return fail(ErrorKind::ProtocolCorrupt, reason::chat::done_duplicate);
    if (!stopped_) {
      // Audio streams may end with final usage and an expiry update instead of a finish reason.
      // Neither a heartbeat, usage alone nor an unfinished audio prefix supplies that evidence.
      if (!audio_usage_tail_ || !audio_.expires_at || !tools_.empty())
        return fail(ErrorKind::ProtocolCorrupt, reason::chat::done_without_terminal);
      if (!finish_reasoning_details() || !finish_audio()) return false;
      if (!emit(Stop{{StopKind::EndTurn, "derived:audio_usage+audio.expires_at+DONE"}})) return false;
      stopped_ = true;
    }
    done_ = true; return true;
  }
  return document(data, event, true);
}
bool Codec::document(std::string_view bytes, std::string_view event, bool streaming) {
  const bool named_error = streaming && event == "error";
  const bool unknown_event = streaming && !event.empty() && event != "message" && !named_error;
  if (bytes.size() > limits_.max_content_bytes - std::min(input_bytes_, limits_.max_content_bytes)) {
    if (unknown_event) return fail(ErrorKind::Unsupported, reason::chat::stream_event_unknown);
    return fail(named_error ? ErrorKind::RemoteFailure : ErrorKind::ResourceLimit, named_error ? reason::chat::remote_error_event : reason::chat::response_byte_limit);
  }
  input_bytes_ += bytes.size();
  auto parsed = json::parse(bytes, {limits_.max_content_bytes, limits_.max_json_depth});
  if (auto* error = std::get_if<json::ParseError>(&parsed)) {
    if (unknown_event) return fail(ErrorKind::Unsupported, reason::chat::stream_event_unknown);
    return fail(named_error ? ErrorKind::RemoteFailure : error->code == json::ParseCode::SizeExceeded || error->code == json::ParseCode::DepthExceeded ? ErrorKind::ResourceLimit : ErrorKind::ProtocolCorrupt,
                named_error ? reason::chat::remote_error_event : reason::chat::response_json_invalid);
  }
  auto wire = std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
  const auto root = wire->root();
  if (!root.is_object()) {
    if (unknown_event) return fail(ErrorKind::Unsupported, reason::chat::stream_event_unknown);
    return fail(named_error ? ErrorKind::RemoteFailure : ErrorKind::ProtocolCorrupt, named_error ? reason::chat::remote_error_event : reason::chat::response_object_required);
  }
  const auto object = root.get("object");
  std::string wire_type = named_error ? std::string(event)
      : object.is_string() && !object.as_string().empty() ? std::string(object.as_string())
      : !event.empty() ? std::string(event)
      : streaming ? "chat.completion.chunk" : "chat.completion";
  if (named_error || unknown_event) {
    const auto& refusal = named_error ? reason::chat::remote_error_event : reason::chat::stream_event_unknown;
    // The classification takes precedence over a failure to retain the document, and carries no code there.
    const Error classified{named_error ? ErrorKind::RemoteFailure : ErrorKind::Unsupported, std::string(refusal.message)};
    if (!accumulator_.accept(RawWire{std::move(wire_type), wire}, &classified)) return false;
    return fail(classified.kind, refusal);
  }
  if (!emit(RawWire{std::move(wire_type), wire})) return false;
  if (root.get("error").valid()) return fail(ErrorKind::RemoteFailure, reason::chat::remote_error_property);
  if (done_) return fail(ErrorKind::ProtocolCorrupt, reason::chat::done_frame_after);
  for (auto name : {"id", "model", "object"}) if (!root.get(name).is_string()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::metadata_string_required);
  if (root.get("object").as_string() != (streaming ? "chat.completion.chunk" : "chat.completion")) return fail(ErrorKind::Unsupported, reason::chat::object_tag_unknown);
  auto created = root.get("created");
  if (!unsigned_count(created)) return fail(ErrorKind::ProtocolCorrupt, reason::chat::created_required);
  // Stream identity is id + model. Some OpenAI-compatible servers stamp each chunk with
  // the current time, so created may repeat or advance between chunks but never go back.
  if (begun_ && (generation_ != root.get("id").as_string() || model_ != root.get("model").as_string()))
    return fail(ErrorKind::ProtocolCorrupt, reason::chat::identity_changed);
  if (begun_ && uint_value(created) < created_) return fail(ErrorKind::ProtocolCorrupt, reason::chat::created_decreased);
  created_ = uint_value(created);
  // A gateway names the serving upstream on every chunk; the first usable name is kept and another one is a different stream.
  switch (served::observe_upstream(descriptor_, root, upstream_, accumulator_)) {
    case served::UpstreamSeen::Conflict: return fail(ErrorKind::ProtocolCorrupt, reason::chat::identity_changed);
    case served::UpstreamSeen::Unusable: ++diagnostics_.unknown_properties; break;
    case served::UpstreamSeen::Fine: break;
  }
  unknown(root, {"id", "model", "object", "created", "choices", "usage", "error", served::upstream_key(descriptor_)});
  auto choices = root.get("choices");
  // A bare envelope without choices is still malformed; omitted-choice heartbeats carry
  // an explicit usage slot or obfuscation metadata rather than silently losing a choice.
  const bool omitted_choices = streaming && !choices.valid() &&
      (root.get("usage").valid() || root.get("obfuscation").is_string());
  if (!choices.is_array() && !omitted_choices)
    return fail(ErrorKind::ProtocolCorrupt, reason::chat::choices_array_required);
  const auto choice_count = choices.is_array() ? choices.size() : 0;
  if (choice_count > 1) return fail(ErrorKind::Unsupported, reason::chat::choices_multiple_unsupported);
  if (!begun_) {
    generation_ = root.get("id").as_string();
    model_ = root.get("model").as_string();
    // Chat has never bound replay to the served name (compatible servers report their own), so the
    // name is only recorded.
    served::observe(descriptor_, accumulator_, context_, model_, false);
    if (!emit(Begin{generation_}) || !emit(MessageBegin{{0}, generation_, Role::Assistant, context_})) return false;
    begun_ = true;
  }
  auto usage_value = root;
  for (const auto& key : descriptor_.usage_path()) {
    if (!usage_value.valid()) break;
    if (!usage_value.is_object()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::usage_path_invalid);
    usage_value = usage_value.get(key);
  }
  if (choice_count == 0) {
    // Omitted or empty choices are genuine metadata heartbeats, not missing semantic deltas.
    if (!streaming || ((!choices.valid() || !usage_value.valid() || usage_value.is_null()) && !metadata_only(root)))
      return fail(ErrorKind::ProtocolCorrupt, reason::chat::choices_empty_unexplained);
  } else {
    const bool terminal_tail = stopped_ || audio_usage_tail_;
    auto choice = choices.at(0);
    if (!choice.is_object()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::choice_object_required);
    unknown(choice, {"index", "message", "delta", "finish_reason", "logprobs"});
    if (auto opposite = choice.get(streaming ? "message" : "delta"); opposite.valid())
      return fail(opposite.is_object() ? ErrorKind::Unsupported : ErrorKind::ProtocolCorrupt, reason::chat::choice_shape_invalid);
    auto index = choice.get("index");
    if (!unsigned_count(index)) return fail(ErrorKind::ProtocolCorrupt, reason::chat::choice_index_required);
    if (uint_value(index) != 0) return fail(ErrorKind::Unsupported, reason::chat::choice_index_unsupported);
    if (auto lp = choice.get("logprobs"); lp.valid() && !lp.is_null()) {
      if (!lp.is_object()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::logprobs_invalid);
      return fail(ErrorKind::Unsupported, reason::chat::logprobs_placement_unsupported);
    }
    auto reason = choice.get("finish_reason");
    const auto delta = choice.get(streaming ? "delta" : "message");
    const auto expiry_audio = delta.get("audio");
    // The usage-terminated audio expiry tail may omit finish_reason entirely. That is
    // not permission for ordinary choices, incomplete audio or semantic tails to omit it.
    const bool omitted_expiry_reason = !reason.valid() && streaming && audio_usage_tail_ &&
        audio_.seen && !audio_.payload.empty() && audio_.pending.empty() && tools_.empty() &&
        delta.is_object() && delta.size() == 1 && expiry_audio.is_object() && expiry_audio.size() == 1 &&
        unsigned_count(expiry_audio.get("expires_at"));
    // Google's OpenAI-compatible endpoint leaves finish_reason out of every chunk before
    // the last. On an open stream an absent reason reads as null; a buffered choice and a
    // terminal tail still need the member (apart from the expiry case above), and the
    // stream still needs a string reason before [DONE].
    const bool omitted_open_reason = !reason.valid() && streaming && !terminal_tail;
    if ((!reason.valid() && !omitted_expiry_reason && !omitted_open_reason) ||
        (reason.valid() && !reason.is_null() && !reason.is_string()))
      return fail(ErrorKind::ProtocolCorrupt, reason::chat::finish_reason_invalid);
    // A failure-class word (no stop table may list one) fails the call: no stop is emitted and nothing of the choice is
    // sealed, so no tool call of it is executable.
    const auto failure_word = reason.is_string() ? descriptor::failure_stop_class(reason.as_string()) : std::nullopt;
    const auto failure_reason = [&] {
      if (*failure_word == descriptor::FailureStop::Cancelled) return fail(ErrorKind::Cancelled, reason::chat::finish_reason_cancelled);
      if (*failure_word == descriptor::FailureStop::Incomplete) return fail(ErrorKind::Unsupported, reason::chat::finish_reason_unsupported);
      return fail(ErrorKind::RemoteFailure, reason::chat::remote_failure_terminal);
    };
    if (terminal_tail) {
      if (failure_word) return failure_reason();
      const bool expiry_tail = delta.get("audio").is_object();
      const bool same_reason = reason.is_string() && !finish_reason_.empty() && reason.as_string() == finish_reason_;
      if (!streaming || !delta.is_object() || (!usage_value.is_object() && !expiry_tail) ||
          (!same_reason && !((reason.is_null() || omitted_expiry_reason) && expiry_tail)))
        return fail(ErrorKind::ProtocolCorrupt, reason::chat::terminal_tail_contradiction);
      // Do not call item(): empty content could create a new semantic part.
      for (auto member : delta.members()) {
        if (member.key == "role") {
          if (!member.value.is_string() || member.value.as_string() != "assistant")
            return fail(ErrorKind::ProtocolCorrupt, reason::chat::finish_reason_role_changed);
          continue;
        }
        if (member.key == "audio") {
          // Expiry remains metadata after final usage with a null or omitted finish reason.
          if (!member.value.is_object()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::finish_reason_data_after);
          for (auto field : member.value.members())
            if (field.key != "expires_at") return fail(ErrorKind::ProtocolCorrupt, reason::chat::finish_reason_data_after);
          const auto expires = member.value.get("expires_at");
          if (!audio_.seen || !unsigned_count(expires))
            return fail(ErrorKind::ProtocolCorrupt, reason::chat::audio_expiry_invalid);
          audio_.expires_at = uint_value(expires);
          continue;
        }
        const bool text_channel = member.key == "content" || member.key == "refusal" ||
            member.key == "reasoning" || member.key == "reasoning_content";
        if (!text_channel || (!member.value.is_null() && (!member.value.is_string() || !member.value.as_string().empty())))
          return fail(ErrorKind::ProtocolCorrupt, reason::chat::finish_reason_data_after);
      }
    } else if (!streaming && failure_word) {
      // A buffered choice seals its content as it reads it, so the word is checked before it. The body's usage is still
      // read, so that the failure reports what was billed. A stream seals nothing before its end, and keeps the deltas
      // it delivered.
      if (usage_value.valid() && !usage_value.is_null() && !usage(usage_value)) return false;
      return failure_reason();
    } else if (!item(delta, streaming)) return false;
    if (reason.is_string() && !terminal_tail) {
      if (reason.as_string().empty()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::finish_reason_empty);
      if (failure_word) return failure_reason();
      const auto kind = table_stop(descriptor_, reason.as_string());
      if (streaming && (!finish_reasoning_details() || !finish_audio())) return false;
      if (!emit(Stop{{kind, std::string(reason.as_string())}})) return false;
      stopped_ = true;
      finish_reason_ = reason.as_string();
    }
  }
  if (usage_value.valid() && !usage_value.is_null() && !usage(usage_value)) return false;
  // An audio usage-only frame closes the semantic channel; only its expiry metadata may follow.
  // The audio is not sealed or given replay authority until the remaining terminal evidence arrives.
  if (streaming && choice_count == 0 && usage_value.is_object() && audio_.seen && !stopped_)
    audio_usage_tail_ = true;
  // A declared OpenRouter origin sends the expiry first and then the final usage on a choice whose delta is empty and
  // whose finish_reason is null, with no usage-only frame. That chunk ends the semantic channel the same way.
  else if (streaming && choice_count == 1 && usage_value.is_object() && audio_.seen && audio_.expires_at && !stopped_ &&
           tools_.empty() && descriptor::routed_gateway(descriptor_) && empty_choice(choices.at(0)))
    audio_usage_tail_ = true;
  return true;
}
bool Codec::item(json::Value value, bool streaming) {
  if (!value.is_object()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::delta_object_required);
  unknown(value, {"role", "content", "refusal", "tool_calls", "function_call", "annotations", "audio", "images", "reasoning", "reasoning_content", "reasoning_details"});
  auto role = value.get("role");
  if ((!streaming && !role.valid()) || (role.valid() && (!role.is_string() || role.as_string() != "assistant"))) return fail(ErrorKind::ProtocolCorrupt, reason::chat::role_assistant_required);
  if (auto legacy = value.get("function_call"); legacy.valid() && !legacy.is_null())
    return fail(legacy.is_object() ? ErrorKind::Unsupported : ErrorKind::ProtocolCorrupt, reason::chat::message_feature_unsupported);
  if (!annotations(value.get("annotations"))) return false;
  auto reasoning = value.get("reasoning"), reasoning_content = value.get("reasoning_content");
  for (auto fragment : {reasoning, reasoning_content})
    if (fragment.valid() && !fragment.is_null() && !fragment.is_string())
      return fail(ErrorKind::ProtocolCorrupt, reason::chat::reasoning_type_invalid);
  if (reasoning.is_string() && reasoning_content.is_string() && !json::equal(reasoning, reasoning_content))
    return fail(ErrorKind::Unsupported, reason::chat::reasoning_alias_contradiction);
  if (!text(reasoning_content.is_string() ? reasoning_content : reasoning, PartKind::Thinking, streaming) ||
      !reasoning_details(value.get("reasoning_details"), streaming)) return false;
  auto content = value.get("content");
  if (!streaming && !content.valid()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::message_content_required);
  if (!this->content(content, streaming) || !text(value.get("refusal"), PartKind::Refusal, streaming)) return false;
  if (auto calls = value.get("tool_calls"); calls.valid()) {
    if (!calls.is_array()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::tool_calls_array_required);
    uint64_t pos = 0;
    for (auto call : calls.elements()) if (!tool(call, streaming, pos++)) return false;
  }
  if (!audio(value.get("audio"), streaming) || !images(value.get("images"))) return false;
  return true;
}
bool Codec::annotations(json::Value value) {
  if (!value.valid()) return true;
  if (!value.is_array()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::annotations_array_required);
  if (value.size() > limits_.max_parts) return fail(ErrorKind::ResourceLimit, reason::chat::annotations_count_limit);
  // These remain observations in the original owned RawWire document. They are
  // not semantic parts, imported file authority, or synthesized citation offsets.
  for (auto annotation : value.elements()) {
    if (!annotation.is_object() || !annotation.get("type").is_string())
      return fail(ErrorKind::ProtocolCorrupt, reason::chat::annotation_type_required);
    const auto type = annotation.get("type").as_string();
    if (type == "file") {
      if (!descriptor::contains(descriptor_.family_policy().openrouter_origins, descriptor_.base_url()))
        return fail(ErrorKind::Unsupported, reason::chat::annotation_file_origin_required);
      const auto file = annotation.get("file");
      if (!file.is_object() || !file.get("hash").is_string() ||
          (file.get("name").valid() && !file.get("name").is_string()) || !file.get("content").is_array())
        return fail(ErrorKind::ProtocolCorrupt, reason::chat::annotation_file_invalid);
      if (file.get("content").size() > limits_.max_parts)
        return fail(ErrorKind::ResourceLimit, reason::chat::annotation_file_content_limit);
      for (auto part : file.get("content").elements()) {
        if (!part.is_object() || !part.get("type").is_string())
          return fail(ErrorKind::ProtocolCorrupt, reason::chat::annotation_file_part_invalid);
        const auto kind = part.get("type").as_string();
        if (kind == "text") {
          if (!part.get("text").is_string()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::annotation_file_text_invalid);
        } else if (kind == "image_url") {
          if (!part.get("image_url").is_object() || !part.get("image_url").get("url").is_string())
            return fail(ErrorKind::ProtocolCorrupt, reason::chat::annotation_file_image_invalid);
        } else return fail(ErrorKind::Unsupported, reason::chat::annotation_file_part_unsupported);
      }
    } else if (type == "url_citation") {
      const auto citation = annotation.get("url_citation");
      if (!citation.is_object() || !citation.get("url").is_string() || !citation.get("title").is_string() ||
          !unsigned_count(citation.get("start_index")) || !unsigned_count(citation.get("end_index")) ||
          uint_value(citation.get("start_index")) > uint_value(citation.get("end_index")) ||
          (citation.get("content").valid() && !citation.get("content").is_string()))
        return fail(ErrorKind::ProtocolCorrupt, reason::chat::annotation_url_citation_invalid);
      // A stream may repeat an annotation; each distinct one becomes a citation of the text.
      const auto bytes = annotation.dump();
      if (citation_bytes_ + bytes.size() > limits_.max_content_bytes || citations_.size() >= limits_.max_parts)
        return fail(ErrorKind::ResourceLimit, reason::chat::annotations_count_limit);
      if (std::none_of(citations_.begin(), citations_.end(), [&](const Citation& seen) { return json::equal(seen.raw->root(), annotation); })) {
        auto parsed = json::parse(bytes);
        if (!std::holds_alternative<json::Document>(parsed)) return fail(ErrorKind::ProtocolCorrupt, reason::chat::annotation_url_citation_invalid);
        Citation cited;
        cited.kind = "url_citation";
        cited.url = citation.get("url").as_string();
        cited.title = citation.get("title").as_string();
        cited.start = uint_value(citation.get("start_index"));
        cited.end = uint_value(citation.get("end_index"));
        cited.raw = std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
        citation_bytes_ += bytes.size();
        citations_.push_back(std::move(cited));
      }
    } else return fail(ErrorKind::Unsupported, reason::chat::annotation_type_unsupported);
  }
  return true;
}
// Text of a content array. A streamed run of text elements is one Text part that grows by each element; an image or
// audio reply ends the run, so the parts stay in the order they arrived.
bool Codec::structured_text(std::string_view value, bool streaming) {
  if (streaming && structured_run_) return emit(PartDelta{*structured_run_, {PartKind::Text, value}});
  const LocalId id{next_part_++};
  if (!emit(PartBegin{{0}, id, PartKind::Text, {}, media_order()})) return false;
  if (!streaming) return emit(PartSeal{id, value});
  structured_run_ = id;
  return emit(PartDelta{id, {PartKind::Text, value}});
}
bool Codec::end_structured_text() {
  if (!structured_run_) return true;
  const auto id = *structured_run_;
  structured_run_.reset();
  return emit(PartSeal{id, {}});
}
bool Codec::content(json::Value value, bool streaming) {
  if (!value.is_array()) {
    if (!structured_content_ || !value.is_string()) return text(value, PartKind::Text, streaming);
    return structured_text(value.as_string(), streaming);
  }
  if (!descriptor::contains(descriptor_.family_policy().openrouter_origins, descriptor_.base_url()))
    return fail(ErrorKind::Unsupported, reason::chat::content_array_origin_required);
  structured_content_ = true;
  for (auto part : value.elements()) {
    if (!part.is_object() || !part.get("type").is_string())
      return fail(ErrorKind::ProtocolCorrupt, reason::chat::content_part_type_required);
    const auto type = part.get("type").as_string();
    if (type == "text") {
      if (!part.get("text").is_string()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::content_text_string_required);
      unknown(part, {"type", "text", "cache_control", "prompt_cache_breakpoint"});
      if (!structured_text(part.get("text").as_string(), streaming)) return false;
    } else if (type == "image_url") {
      if (!image(part, false)) return false;
    } else return fail(ErrorKind::Unsupported, reason::chat::content_part_type_unsupported);
  }
  return true;
}
bool Codec::text(json::Value value, PartKind kind, bool streaming) {
  if (!value.valid() || value.is_null()) return true;
  if (!value.is_string()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::text_type_invalid);
  auto& id = kind == PartKind::Text ? text_ : kind == PartKind::Thinking ? thinking_ : refusal_;
  if (!id) {
    id = LocalId{next_part_++};
    const uint64_t order = kind == PartKind::Thinking ? 0U :
        kind == PartKind::Text ? 1 + limits_.max_parts : 2 + limits_.max_parts;
    if (!emit(PartBegin{{0}, *id, kind, {}, order})) return false;
  }
  if (streaming) return emit(PartDelta{*id, {kind, value.as_string()}});
  return emit(PartSeal{*id, value.as_string(), {}, kind == PartKind::Text ? citations_ : std::vector<Citation>{}});
}
bool Codec::reasoning_details(json::Value value, bool streaming) {
  if (!value.valid() || value.is_null()) return true;
  if (!value.is_array()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::reasoning_details_array_required);
  if (!descriptor::contains(descriptor_.family_policy().openrouter_origins, descriptor_.base_url()))
    return fail(ErrorKind::Unsupported, reason::chat::reasoning_details_origin_required);
  for (auto detail : value.elements())
    if (!detail.is_object()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::reasoning_detail_object_required);
  if (!streaming) return emit_reasoning_details(value);
  // A streamed frame holds fragments of entries, not parts of the answer. They merge into the entries a buffered
  // response carries, and the one part holding them is emitted with the finish reason. The detail values alias the
  // frame's raw document, which the accumulator keeps.
  reasoning_streamed_ = true;
  // Indexed text/summary deltas can interleave; keep each binding at its first
  // arrival position. Unindexed deltas join only a compatible consecutive item.
  // Encrypted objects are discrete blobs and are never concatenated.
  for (auto detail : value.elements()) {
    const auto detail_type = detail.get("type");
    if (!detail_type.is_string() || detail_type.as_string().empty())
      return fail(ErrorKind::ProtocolCorrupt, reason::chat::reasoning_detail_type_required);
    const auto name = detail_type.as_string();
    const auto identity = detail.get("id"), index = detail.get("index"), format = detail.get("format");
    if ((identity.valid() && !identity.is_null() && !identity.is_string()) ||
        (index.valid() && !unsigned_count(index)) ||
        (format.valid() && !format.is_null() && !format.is_string()))
      return fail(ErrorKind::ProtocolCorrupt, reason::chat::reasoning_detail_identity_invalid);
    const std::string_view key = name == "reasoning.text" ? "text" : name == "reasoning.summary" ? "summary" : "";
    if (key.empty()) {
      if (name == "reasoning.encrypted" && !detail.get("data").is_string())
        return fail(ErrorKind::ProtocolCorrupt, reason::chat::reasoning_detail_encrypted_incomplete);
      if (reasoning_bindings_.size() >= limits_.max_parts)
        return fail(ErrorKind::ResourceLimit, reason::chat::reasoning_detail_count_limit);
      reasoning_bindings_.push_back({detail, {}, {}, {}});
      continue;
    }
    const auto fragment = detail.get(key), signature = detail.get("signature");
    if ((fragment.valid() && !fragment.is_null() && !fragment.is_string()) ||
        (signature.valid() && !signature.is_null() && !signature.is_string()))
      return fail(ErrorKind::ProtocolCorrupt, reason::chat::reasoning_detail_fragment_invalid);
    std::optional<size_t> binding_index;
    if (index.valid()) {
      const auto found = reasoning_indices_.find(uint_value(index));
      if (found != reasoning_indices_.end()) binding_index = found->second;
    }
    if (!binding_index && !reasoning_bindings_.empty())
      binding_index = reasoning_bindings_.size() - 1;
    if (binding_index) {
      const auto& previous_binding = reasoning_bindings_[*binding_index];
      bool compatible = previous_binding.payload_key == key;
      for (const auto identity_key : {"id", "index"}) {
        const auto previous = previous_binding.fields.find(identity_key);
        const auto next = detail.get(identity_key);
        if (previous != previous_binding.fields.end() && !previous->second.is_null() &&
            next.valid() && !next.is_null() && !json::equal(previous->second, next)) compatible = false;
      }
      if (!compatible) binding_index.reset();
    }
    if (!binding_index) {
      if (reasoning_bindings_.size() >= limits_.max_parts)
        return fail(ErrorKind::ResourceLimit, reason::chat::reasoning_detail_count_limit);
      binding_index = reasoning_bindings_.size();
      reasoning_bindings_.push_back({detail, {}, key, {}});
    }
    if (index.valid()) reasoning_indices_[uint_value(index)] = *binding_index;
    auto& binding = reasoning_bindings_[*binding_index];
    for (auto member : detail.members()) {
      if (member.key == key) continue;
      auto [found, inserted] = binding.fields.emplace(member.key, member.value);
      if (inserted || member.value.is_null()) continue;
      if (found->second.is_null() ||
          (found->second.is_string() && found->second.as_string().empty())) {
        found->second = member.value;
      } else if (!json::equal(found->second, member.value) &&
                 !(member.value.is_string() && member.value.as_string().empty())) {
        return fail(ErrorKind::ProtocolCorrupt, reason::chat::reasoning_detail_metadata_contradiction);
      }
    }
    if (fragment.is_string()) {
      const auto bytes = fragment.as_string();
      if (bytes.size() > limits_.max_content_bytes - std::min(binding.payload.size(), limits_.max_content_bytes))
        return fail(ErrorKind::ResourceLimit, reason::chat::reasoning_detail_payload_limit);
      binding.payload.append(bytes);
    }
  }
  return true;
}
// The part that holds a reply's reasoning_details. It is the same part, at the same position, whether the entries arrived
// whole or as streamed fragments.
bool Codec::emit_reasoning_part(std::shared_ptr<const json::Document> metadata) {
  const LocalId id{next_part_++};
  return emit(PartBegin{{0}, id, PartKind::Opaque, {{}, {}, ToolCallKind::ClientExecuted, "reasoning_details", std::move(metadata)}, 1}) &&
         emit(PartSeal{id, {}});
}
bool Codec::emit_reasoning_details(json::Value details) {
  const json::Limits limits{limits_.max_content_bytes, limits_.max_json_depth};
  auto write = [&](json::BoundedWriter& writer) {
    writer.raw("{\"type\":\"reasoning_details\",\"details\":").value(details, 1).raw("}");
  };
  json::BoundedWriter measure(limits);
  write(measure);
  if (!measure.ok()) return fail(ErrorKind::ResourceLimit, reason::chat::reasoning_details_metadata_limit);
  std::string bytes;
  bytes.reserve(measure.size());
  json::BoundedWriter output(limits, &bytes);
  write(output);
  auto parsed = json::parse(bytes, limits);
  auto* document = std::get_if<json::Document>(&parsed);
  if (!document) return fail(ErrorKind::ResourceLimit, reason::chat::reasoning_details_metadata_unretainable);
  return emit_reasoning_part(std::make_shared<const json::Document>(std::move(*document)));
}
bool Codec::finish_reasoning_details() {
  if (!reasoning_streamed_) return true;
  reasoning_streamed_ = false;
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
    writer.raw("]}");
  };
  json::BoundedWriter measure(limits);
  write(measure);
  if (!measure.ok()) return fail(ErrorKind::ResourceLimit, reason::chat::reasoning_detail_completed_metadata_limit);
  std::string bytes;
  bytes.reserve(measure.size());
  json::BoundedWriter output(limits, &bytes);
  write(output);
  auto parsed = json::parse(bytes, limits);
  auto* document = std::get_if<json::Document>(&parsed);
  if (!document) return fail(ErrorKind::ResourceLimit, reason::chat::reasoning_detail_completed_unretainable);
  reasoning_bindings_.clear();
  reasoning_indices_.clear();
  return emit_reasoning_part(std::make_shared<const json::Document>(std::move(*document)));
}
bool Codec::tool(json::Value value, bool streaming, uint64_t position) {
  if (!value.is_object()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::tool_call_object_required);
  unknown(value, {"index", "id", "type", "function"});
  auto index = value.get("index"), id = value.get("id"), type = value.get("type"), function = value.get("function");
  if (index.valid() && !unsigned_count(index)) return fail(ErrorKind::ProtocolCorrupt, reason::chat::tool_call_index_invalid);
  if (id.valid() && (!id.is_string() || id.as_string().empty())) return fail(ErrorKind::ProtocolCorrupt, reason::chat::tool_call_id_invalid);
  if ((!streaming && !type.valid()) || (type.valid() && !type.is_string())) return fail(ErrorKind::ProtocolCorrupt, reason::chat::tool_call_type_invalid);
  if (type.valid() && type.as_string() != "function") return fail(ErrorKind::Unsupported, reason::chat::tool_call_type_unsupported);
  if ((!streaming && !function.valid()) || (function.valid() && !function.is_object())) return fail(ErrorKind::ProtocolCorrupt, reason::chat::tool_call_function_invalid);
  if (function.valid()) unknown(function, {"name", "arguments"});
  auto name = function.get("name"), args = function.get("arguments");
  if (name.valid() && (!name.is_string() || name.as_string().empty())) return fail(ErrorKind::ProtocolCorrupt, reason::chat::tool_call_name_invalid);
  if ((!streaming && !args.valid()) || (args.valid() && !args.is_string())) return fail(ErrorKind::ProtocolCorrupt, reason::chat::tool_call_arguments_invalid);
  std::optional<size_t> found;
  const bool indexed = !streaming || index.valid();
  const uint64_t wire_index = index.valid() ? uint_value(index) : position;
  if (indexed && wire_index >= limits_.max_parts) return fail(ErrorKind::ResourceLimit, reason::chat::tool_call_index_limit);
  if (indexed && indexed_tools_ == false) return fail(ErrorKind::ProtocolCorrupt, reason::chat::tool_call_ordering_ambiguous);
  if (!streaming || index.valid()) {
    auto it = indices_.find(wire_index);
    if (it != indices_.end()) found = it->second;
    if (found && id.valid() && tools_[*found].id != id.as_string()) found.reset();
  } else if (id.valid()) {
    for (size_t i = 0; i < tools_.size(); ++i) if (tools_[i].id == id.as_string()) found = i;
  } else {
    if (tools_.size() != 1) return fail(ErrorKind::ProtocolCorrupt, reason::chat::tool_call_fragment_ambiguous);
    found = 0;
  }
  if (!found) {
    if (!id.valid() || !name.valid()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::tool_call_identity_required);
    for (const auto& call : tools_) if (call.id == id.as_string()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::tool_call_id_rebound);
    if (tools_.size() >= limits_.max_parts) return fail(ErrorKind::ResourceLimit, reason::chat::tool_call_count_limit);
    if (indexed_tools_ && *indexed_tools_ != indexed) return fail(ErrorKind::ProtocolCorrupt, reason::chat::tool_call_ordering_ambiguous);
    indexed_tools_ = indexed;
    const uint64_t base = 3 + limits_.max_parts;
    const auto order = indexed ? base + wire_index * limits_.max_parts + tools_.size() : base + tools_.size();
    ToolBinding binding{{next_part_++}, std::string(id.as_string()), std::string(name.as_string()), order};
    if (!emit(PartBegin{{0}, binding.local, PartKind::ToolCall, {binding.id, binding.name, ToolCallKind::ClientExecuted}, order})) return false;
    found = tools_.size(); tools_.push_back(std::move(binding));
    if (!streaming || index.valid()) indices_[wire_index] = *found;
  }
  auto& binding = tools_[*found];
  if (name.valid() && binding.name != name.as_string()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::tool_call_name_contradiction);
  if (!streaming) return emit(PartSeal{binding.local, args.as_string()});
  return !args.valid() || emit(PartDelta{binding.local, {PartKind::ToolCall, args.as_string()}});
}
bool Codec::usage(json::Value value) {
  if (!value.is_object()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::usage_object_required);
  unknown(value, {"prompt_tokens", "completion_tokens", "total_tokens", "prompt_tokens_details", "completion_tokens_details", "cost", "cost_details", "is_byok"});
  for (auto key : {"prompt_tokens", "completion_tokens", "total_tokens"})
    if (!value.get(key).valid()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::usage_counter_missing);
  Usage result; result.stage = UsageStage::Partial;
  result.provider_cost = codecs::provider_cost(value, descriptor_);
  auto counter = [&](json::Value parent, std::string_view key, std::optional<Count>& destination) {
    auto v = parent.get(key);
    if (!v.valid()) return true;
    if (!unsigned_count(v)) return fail(ErrorKind::ProtocolCorrupt, reason::chat::usage_counter_invalid);
    destination = Count{uint_value(v), Evidence::Reported}; return true;
  };
  if (!counter(value, "prompt_tokens", result.input_total) || !counter(value, "completion_tokens", result.output_total) || !counter(value, "total_tokens", result.provider_reported_total)) return false;
  auto prompt = value.get("prompt_tokens_details"), completion = value.get("completion_tokens_details");
  for (auto detail : {prompt, completion}) if (detail.valid() && !detail.is_null() && !detail.is_object()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::usage_details_invalid);
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
  // Like Responses, every other nonnegative integer leaf (for example a gateway's
  // prompt_tokens_details.video_tokens) stays addressable by its dotted path. Typed or
  // already-named counters are not repeated, and monetary members are never token leaves.
  static constexpr std::string_view mapped[] = {"prompt_tokens", "completion_tokens", "total_tokens",
      "prompt_tokens_details.cached_tokens", "prompt_tokens_details.cache_write_tokens", "prompt_tokens_details.audio_tokens",
      "completion_tokens_details.reasoning_tokens", "completion_tokens_details.audio_tokens",
      "completion_tokens_details.accepted_prediction_tokens", "completion_tokens_details.rejected_prediction_tokens"};
  const auto leaves = [&](const auto& self, json::Value node, const std::string& path) -> void {
    if (node.is_object()) {
      for (auto member : node.members()) {
        if (path.empty() && codecs::monetary_member(member.key)) continue;
        self(self, member.value, path.empty() ? std::string(member.key) : path + "." + std::string(member.key));
      }
    } else if (node.is_array()) {
      size_t n = 0;
      for (auto item : node.elements()) self(self, item, path + "." + std::to_string(n++));
    } else if (unsigned_count(node) && std::find(std::begin(mapped), std::end(mapped), path) == std::end(mapped)) {
      result.extra.emplace(path, Count{uint_value(node), Evidence::Reported});
    }
  };
  leaves(leaves, value, {});
  auto conflict = [&](std::string counter_name, std::string detail) { result.quality = UsageQuality::Inconsistent; result.conflicts.push_back({std::move(counter_name), std::move(detail)}); };
  if (result.input_total && result.output_total) {
    if (result.output_total->value > std::numeric_limits<uint64_t>::max() - result.input_total->value) return fail(ErrorKind::ProtocolCorrupt, reason::chat::usage_sum_overflow);
    result.total = Count{result.input_total->value + result.output_total->value, Evidence::Derived};
    if (result.provider_reported_total && result.total->value != result.provider_reported_total->value) conflict("total", "reported total differs from input plus output");
  }
  if (result.input_total) {
    const uint64_t read = result.cache_read ? result.cache_read->value : 0;
    const uint64_t written = result.cache_write ? result.cache_write->value : 0;
    if (written > std::numeric_limits<uint64_t>::max() - read) return fail(ErrorKind::ProtocolCorrupt, reason::chat::usage_cache_overflow);
    const uint64_t known_cached = read + written;
    // A declared OpenRouter origin always reports prompt_tokens_details.cache_write_tokens, so
    // its absence there leaves the cache-write share unknown. OpenAI-shaped Chat origins have no
    // cache-write counter at all; an absent one is zero and a reported one is still subtracted.
    const bool write_known = result.cache_write || !descriptor::routed_gateway(descriptor_);
    if (known_cached > result.input_total->value) conflict("input_uncached", "known cache subset exceeds input total");
    else if (result.cache_read && write_known)
      result.input_uncached = Count{result.input_total->value - known_cached, Evidence::Derived};
  }
  if (result.output_total && result.reasoning && result.reasoning->value > result.output_total->value) conflict("reasoning", "reasoning subset exceeds output total");
  return emit(UsageUpdate{std::move(result)});
}
void Codec::finish(Close close) {
  if (closed_ || accumulator_.terminal()) { closed_ = true; return; }
  closed_ = true;
  if (!close.normal) { fail(close.error, reason::transport::response_close); return; }
  if (!begun_ || !stopped_ || (mode_ == Mode::Sse && !done_)) { fail(ErrorKind::Truncated, reason::chat::terminal_missing); return; }
  if (mode_ == Mode::Sse) {
    if (!finish_audio()) return;
    if (!end_structured_text()) return;
    if (text_ && !emit(PartSeal{*text_, {}, {}, citations_})) return;
    if (refusal_ && !emit(PartSeal{*refusal_, {}})) return;
    if (thinking_ && !emit(PartSeal{*thinking_, {}})) return;
    for (const auto& tool : tools_) if (!emit(PartSeal{tool.local, {}})) return;
  }
  if (!emit(MessageSeal{{0}})) return;
  emit(Commit{mode_ == Mode::Sse
      ? audio_usage_tail_ ? "chat.audio_usage+audio.expires_at+DONE+normal_close" : "chat.finish_reason+DONE+normal_close"
      : "chat.finish_reason+normal_close"});
}
uint64_t Codec::media_order() { return 4 + limits_.max_parts + limits_.max_parts * limits_.max_parts + media_parts_++; }
bool Codec::audio(json::Value value, bool streaming) {
  if (!value.valid() || value.is_null()) return true;
  if (!value.is_object()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::audio_object_required);
  unknown(value, {"id", "data", "transcript", "expires_at"});
  const auto id = value.get("id"), data = value.get("data"), transcript = value.get("transcript"), expires = value.get("expires_at");
  for (const auto field : {id, data, transcript})
    if (field.valid() && !field.is_null() && !field.is_string()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::audio_fields_invalid);
  if (expires.valid() && !expires.is_null() && !unsigned_count(expires)) return fail(ErrorKind::ProtocolCorrupt, reason::chat::audio_expiry_type_invalid);
  if (unsigned_count(expires)) audio_.expires_at = uint_value(expires);
  if (id.is_string() && !id.as_string().empty()) {
    if (!audio_.id.empty() && audio_.id != id.as_string()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::audio_identity_changed);
    audio_.id = id.as_string();
  }
  if (!audio_.seen && !end_structured_text()) return false;
  if (!audio_.seen) audio_.order = media_order();
  audio_.seen = true;
  if (transcript.is_string()) {
    if (transcript.as_string().size() > limits_.max_content_bytes - std::min(audio_.transcript.size(), limits_.max_content_bytes))
      return fail(ErrorKind::ResourceLimit, reason::chat::audio_transcript_limit);
    audio_.transcript.append(transcript.as_string());
  }
  if (data.is_string() && !data.as_string().empty() && !audio_chunk(data.as_string(), streaming)) return false;
  return streaming || finish_audio();
}
bool Codec::audio_chunk(std::string_view data, bool streaming) {
  auto append = [&](std::string_view run) {
    if (!canonical_base64(run)) return fail(ErrorKind::ProtocolCorrupt, reason::chat::audio_data_invalid);
    const auto current = audio_.payload.empty() ? 0 : base64_decoded_size(audio_.payload);
    const auto added = base64_decoded_size(run);
    if (added > limits_.max_content_bytes - std::min(current, limits_.max_content_bytes) ||
        current + added > limits_.max_content_bytes / 4 * 3)
      return fail(ErrorKind::ResourceLimit, reason::chat::audio_byte_limit);
    media_output::append_audio(audio_.payload, run);
    return true;
  };
  if (!streaming) return append(data);
  // Providers may split encoded quartets or independently pad byte runs. Only the incomplete
  // quartet is copied; complete wire spans feed the canonical joined payload directly.
  while (!data.empty()) {
    if (!audio_.pending.empty()) {
      const auto take = std::min(data.size(), 4 - audio_.pending.size());
      audio_.pending.append(data.substr(0, take)); data.remove_prefix(take);
      if (audio_.pending.size() < 4) break;
      if (!append(audio_.pending)) return false;
      audio_.pending.clear();
      continue;
    }
    const auto padding = data.find('=');
    const auto complete = data.size() / 4 * 4;
    const auto count = padding == std::string_view::npos ? complete : std::min(complete, (padding / 4 + 1) * 4);
    if (count) {
      if (!append(data.substr(0, count))) return false;
      data.remove_prefix(count);
    } else {
      audio_.pending.assign(data); data = {};
    }
  }
  for (std::size_t i = 0; i < audio_.pending.size(); ++i) {
    const unsigned char c = audio_.pending[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '+' || c == '/' || (c == '=' && i >= 2)))
      return fail(ErrorKind::ProtocolCorrupt, reason::chat::audio_fragment_invalid);
  }
  return true;
}
bool Codec::finish_audio() {
  if (!audio_.seen || audio_.sealed) return true;
  audio_.sealed = true;
  if (!audio_.pending.empty()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::audio_quartet_incomplete);
  if (audio_.payload.empty()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::audio_data_missing);
  Media media = media_output::describe(audio_mime(context_ ? context_->audio_format() : std::string_view{}), MediaSource::Inline);
  media.kind = MediaKind::Audio;
  media.id = std::move(audio_.id);
  media.transcript = std::move(audio_.transcript);
  media.expires_at = audio_.expires_at;
  const bool ok = media_output::emit_part([this](const Event& event) { return emit(event); }, LocalId{0}, LocalId{next_part_++},
                                        audio_.order, std::move(media), audio_.payload);
  std::string{}.swap(audio_.payload);
  return ok;
}
bool Codec::images(json::Value value) {
  if (!value.valid() || value.is_null()) return true;
  if (!value.is_array()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::images_array_required);
  for (auto entry : value.elements()) if (!image(entry, true)) return false;
  return true;
}
bool Codec::image(json::Value entry, bool indexed) {
    if (!entry.is_object()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::image_object_required);
    unknown(entry, {"type", "image_url", "index"});
    const auto type = entry.get("type"), location = entry.get("image_url"), index = entry.get("index");
    if (type.valid() && (!type.is_string() || type.as_string() != "image_url")) return fail(ErrorKind::Unsupported, reason::chat::image_type_unsupported);
    if (!location.is_object() || !location.get("url").is_string() || location.get("url").as_string().empty())
      return fail(ErrorKind::ProtocolCorrupt, reason::chat::image_url_required);
    const auto url = location.get("url").as_string();
    if (indexed && index.valid() && !index.is_null()) {
      if (!unsigned_count(index)) return fail(ErrorKind::ProtocolCorrupt, reason::chat::image_index_invalid);
      const auto [found, inserted] = image_indices_.emplace(uint_value(index), url);
      if (!inserted) {
        if (found->second != url) return fail(ErrorKind::ProtocolCorrupt, reason::chat::image_indexed_changed);
        return true;
      }
    }
    std::string_view mime, payload;
    Media media;
    if (media_output::parse_data_uri(url, mime, payload)) media = media_output::describe(mime, MediaSource::Inline);
    else if (url_syntax(url) ||
             (url.starts_with("http://") && url.size() > 7 && url.size() <= 4096 &&
              std::all_of(url.begin(), url.end(), [](unsigned char c) { return c > 0x20 && c < 0x7f && c != '"' && c != '\\'; })))
      media = media_output::describe({}, MediaSource::Url, url), payload = {};
    else return fail(ErrorKind::ProtocolCorrupt, reason::chat::image_location_unsupported);
    media.kind = MediaKind::Image;
    if (!indexed) {
      const auto detail = location.get("detail");
      if (detail.valid()) {
        if (!detail.is_string()) return fail(ErrorKind::ProtocolCorrupt, reason::chat::image_detail_invalid);
        if (detail.as_string() == "auto") media.detail = ImageDetail::Auto;
        else if (detail.as_string() == "low") media.detail = ImageDetail::Low;
        else if (detail.as_string() == "high") media.detail = ImageDetail::High;
        else if (detail.as_string() == "original") media.detail = ImageDetail::Original;
        else return fail(ErrorKind::Unsupported, reason::chat::image_detail_unsupported);
      }
    }
    if (!end_structured_text() || !media_output::emit_part([this](const Event& event) { return emit(event); }, LocalId{0}, LocalId{next_part_++},
                                 media_order(), std::move(media), payload)) return false;
  return true;
}
} // namespace sp::chat
