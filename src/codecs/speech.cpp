#include "codecs/speech.h"
#include "codecs/reason.h"
#include "crypto/crypto.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <algorithm>

namespace sp::speech {
namespace {
std::string_view header(const Close& close, std::string_view name) {
  for (const auto& [key, value] : close.headers) if (key == name) return value;
  return {};
}
// The MIME type without parameters, lower-cased.
std::string essence(std::string_view type) {
  type = type.substr(0, type.find(';'));
  while (!type.empty() && type.back() == ' ') type.remove_suffix(1);
  std::string out(type);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}
}  // namespace

Codec::Codec(const descriptor::ValidatedDescriptor& descriptor, Accumulator& accumulator, std::string requested, SemanticLimits limits)
    : descriptor_(descriptor), accumulator_(accumulator), requested_(std::move(requested)), limits_(limits) {}
bool Codec::emit(const Event& event) { return accumulator_.accept(event); }
bool Codec::fail(ErrorKind kind, const reason::Reason& text) {
  Error error = reason::error(kind, text);
  emit(Fail{std::move(error)});
  return false;
}
bool Codec::buffered(std::string_view body, Close close) {
  if (!close.normal) return fail(close.error, reason::transport::response_close);
  const auto type = essence(header(close, "content-type"));
  // A JSON or text body here is not audio: an SSE stream or an error the status did not report.
  if (type.empty() || (!type.starts_with("audio/") && type != "application/octet-stream")) return fail(ErrorKind::ProtocolCorrupt, reason::speech::response_not_audio);
  if (body.empty()) return fail(ErrorKind::ProtocolCorrupt, reason::speech::response_empty);
  const auto encoded = base64_encode(body);
  if (encoded.size() > limits_.max_content_bytes) return fail(ErrorKind::ResourceLimit, reason::speech::audio_limit);
  const auto digest = crypto::sha256(body);
  std::string hex;
  for (const auto byte : digest) { const char* digits = "0123456789abcdef"; hex += digits[byte >> 4]; hex += digits[byte & 15]; }
  auto record = json::parse("{\"content_type\":" + json::quote(type) + ",\"bytes\":" + std::to_string(body.size()) + ",\"sha256\":\"" + hex + "\"}");
  if (!emit(RawWire{"speech.response", std::make_shared<const json::Document>(std::move(std::get<json::Document>(record)))})) return false;
  accumulator_.served_model({requested_, {}, ServedRelation::NotReported});
  const auto id = std::string(header(close, "x-generation-id"));
  if (!emit(Begin{id}) || !emit(MessageBegin{{0}, id.empty() ? std::nullopt : std::optional<std::string>(id), Role::Assistant, nullptr})) return false;
  PartHeader part;
  part.media.kind = MediaKind::Audio;
  part.media.source = MediaSource::Inline;
  part.media.mime = type == "application/octet-stream" ? std::string() : type;
  const LocalId local{1};
  return emit(PartBegin{{0}, local, PartKind::Media, std::move(part), 0}) && emit(PartDelta{local, {PartKind::Media, encoded}}) &&
         emit(PartSeal{local, {}}) && emit(UsageUpdate{Usage{}}) && emit(Stop{StopReason{StopKind::EndTurn, "completed"}}) &&
         emit(MessageSeal{{0}}) && emit(Commit{"speech.audio+normal_close"});
}

Outcome typed(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  Result result;
  for (const auto& message : completion.messages) {
    if (message.id.size()) result.id = message.id;
    for (const auto& part : message.parts) if (const auto* media = std::get_if<Media>(&part)) result.audio = *media;
  }
  result.usage = completion.usage;
  result.model = completion.model;
  result.attempt = completion.attempt;
  result.raw_events = completion.raw_events;
  return result;
}
}  // namespace sp::speech
