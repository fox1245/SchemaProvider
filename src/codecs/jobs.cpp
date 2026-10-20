#include "codecs/jobs.h"
#include "codecs/provider_cost.h"
#include "codecs/reason.h"
#include "crypto/crypto.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <algorithm>

namespace sp::jobs {
namespace {
Status status_of(std::string_view raw) {
  if (raw == "pending" || raw == "queued") return Status::Pending;
  if (raw == "in_progress" || raw == "running" || raw == "processing") return Status::Running;
  if (raw == "completed" || raw == "succeeded") return Status::Completed;
  if (raw == "failed") return Status::Failed;
  if (raw == "cancelled" || raw == "canceled") return Status::Cancelled;
  if (raw == "expired") return Status::Expired;
  return Status::Unknown;
}
std::string_view header(const Close& close, std::string_view name) {
  for (const auto& [key, value] : close.headers) if (key == name) return value;
  return {};
}
std::string essence(std::string_view type) {
  type = type.substr(0, type.find(';'));
  while (!type.empty() && type.back() == ' ') type.remove_suffix(1);
  std::string out(type);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}
}  // namespace

StateCodec::StateCodec(const descriptor::ValidatedDescriptor& descriptor, Accumulator& accumulator, std::string requested, std::string endpoint, SemanticLimits limits)
    : descriptor_(descriptor), accumulator_(accumulator), requested_(std::move(requested)), endpoint_(std::move(endpoint)), limits_(limits) {}
bool StateCodec::emit(const Event& event) { return accumulator_.accept(event); }
bool StateCodec::fail(ErrorKind kind, std::string text) {
  Error error{kind, std::move(text)};
  error.reason = reason::code(reason::Source::Jobs, error.safe_message);
  emit(Fail{std::move(error)});
  return false;
}
bool StateCodec::buffered(std::string_view body, Close close) {
  if (!close.normal) return fail(close.error, "abnormal response close");
  auto parsed = json::parse(body, {limits_.max_content_bytes, limits_.max_json_depth});
  if (auto* error = std::get_if<json::ParseError>(&parsed))
    return fail(error->code == json::ParseCode::SizeExceeded || error->code == json::ParseCode::DepthExceeded
                    ? ErrorKind::ResourceLimit : ErrorKind::ProtocolCorrupt, "invalid response JSON");
  auto wire = std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
  const auto root = wire->root();
  if (!root.is_object()) return fail(ErrorKind::ProtocolCorrupt, "response must be object");
  if (!emit(RawWire{"job.response", wire})) return false;
  const auto id = root.get("id"), status = root.get("status");
  if (!id.is_string() || id.as_string().empty() || !status.is_string()) return fail(ErrorKind::ProtocolCorrupt, "a job names its id and status");
  for (const char* key : {"urls", "unsigned_urls"})
    if (root.get(key).valid() && !root.get(key).is_null() && !root.get(key).is_array()) return fail(ErrorKind::ProtocolCorrupt, "job outputs are a list");
  // Which job this is, for the typed state: the endpoint and origin the call reached.
  auto handle = json::parse("{\"endpoint\":" + json::quote(endpoint_) + ",\"origin\":" + json::quote(descriptor_.base_url()) +
                            ",\"model\":" + json::quote(requested_) + "}");
  if (!emit(RawWire{"job.handle", std::make_shared<const json::Document>(std::move(std::get<json::Document>(handle)))})) return false;
  accumulator_.served_model({requested_, {}, ServedRelation::NotReported});
  if (!emit(Begin{std::string(id.as_string())}) || !emit(MessageBegin{{0}, std::string(id.as_string()), Role::Assistant, nullptr})) return false;
  Usage usage;
  if (const auto reported = root.get("usage"); reported.is_object()) {
    usage.provider_cost = codecs::provider_cost(reported, descriptor_);
    usage.stage = UsageStage::Final;
  }
  // A job still running is Pending, as a background interaction is; a finished one ends the turn.
  const auto kind = status_of(status.as_string());
  const bool done = kind == Status::Completed || kind == Status::Failed || kind == Status::Cancelled || kind == Status::Expired;
  return emit(UsageUpdate{std::move(usage)}) && emit(Stop{StopReason{done ? StopKind::EndTurn : StopKind::Pending, std::string(status.as_string())}}) &&
         emit(MessageSeal{{0}}) && emit(Commit{"job.state+normal_close"});
}

ContentCodec::ContentCodec(Accumulator& accumulator, SemanticLimits limits, bool documents)
    : accumulator_(accumulator), limits_(limits), documents_(documents) {}
bool ContentCodec::emit(const Event& event) { return accumulator_.accept(event); }
bool ContentCodec::fail(ErrorKind kind, std::string text) {
  Error error{kind, std::move(text)};
  error.reason = reason::code(reason::Source::Jobs, error.safe_message);
  emit(Fail{std::move(error)});
  return false;
}
bool ContentCodec::buffered(std::string_view body, Close close) {
  if (!close.normal) return fail(close.error, "abnormal response close");
  // Only a 200 carries the content: 202 says it is not ready, 206 gives part of it.
  if (close.status && close.status != 200) return fail(ErrorKind::ProtocolCorrupt, "a download is the body of a 200 response");
  // A job's output is media. A document in its place is the provider's answer about the job, not the output; a
  // file's content or a batch's results may be any type.
  const auto type = essence(header(close, "content-type"));
  if (!documents_ && (type.starts_with("application/json") || type.starts_with("text/"))) return fail(ErrorKind::ProtocolCorrupt, "a download is not a document");
  std::string record = "{\"content_type\":" + json::quote(type);
  std::string encoded;
  if (close.sink) {
    if (!close.sink->bytes) return fail(ErrorKind::ProtocolCorrupt, "a download is empty");
    record += ",\"path\":" + json::quote(close.sink->path) + ",\"bytes\":" + std::to_string(close.sink->bytes) + ",\"sha256\":\"" + close.sink->sha256 + "\"";
  } else {
    if (body.empty()) return fail(ErrorKind::ProtocolCorrupt, "a download is empty");
    encoded = base64_encode(body);
    if (encoded.size() > limits_.max_content_bytes) return fail(ErrorKind::ResourceLimit, "a download exceeds the content limit");
    const auto digest = crypto::sha256(body);
    std::string hex;
    for (const auto byte : digest) { hex += "0123456789abcdef"[byte >> 4]; hex += "0123456789abcdef"[byte & 15]; }
    record += ",\"bytes\":" + std::to_string(body.size()) + ",\"sha256\":\"" + hex + "\"";
  }
  auto document = json::parse(record + "}");
  if (!emit(RawWire{"download.response", std::make_shared<const json::Document>(std::move(std::get<json::Document>(document)))})) return false;
  if (!emit(Begin{}) || !emit(MessageBegin{{0}, std::nullopt, Role::Assistant, nullptr})) return false;
  if (!close.sink) {
    PartHeader part;
    part.media.kind = type.starts_with("video/") ? MediaKind::Video : type.starts_with("audio/") ? MediaKind::Audio
                    : type.starts_with("image/") ? MediaKind::Image : MediaKind::Document;
    part.media.source = MediaSource::Inline;
    part.media.mime = type;
    const LocalId local{1};
    if (!emit(PartBegin{{0}, local, PartKind::Media, std::move(part), 0}) || !emit(PartDelta{local, {PartKind::Media, encoded}}) ||
        !emit(PartSeal{local, {}})) return false;
  }
  return emit(UsageUpdate{Usage{}}) && emit(Stop{StopReason{StopKind::EndTurn, "completed"}}) && emit(MessageSeal{{0}}) &&
         emit(Commit{"download.body+normal_close"});
}

Outcome typed(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  State state;
  for (const auto& raw : completion.raw_events) {
    if (!raw.payload) continue;
    const auto root = raw.payload->root();
    if (raw.type == "job.handle") {
      state.handle.endpoint = std::string(root.get("endpoint").as_string());
      state.handle.origin = std::string(root.get("origin").as_string());
      state.handle.model = std::string(root.get("model").as_string());
    } else if (raw.type == "job.response") {
      state.handle.id = std::string(root.get("id").as_string());
      state.raw_status = std::string(root.get("status").as_string());
      state.status = status_of(state.raw_status);
      state.outputs = root.get("unsigned_urls").is_array() ? root.get("unsigned_urls").size() : root.get("urls").is_array() ? root.get("urls").size() : 0;
      if (root.get("expires_at").is_uint()) state.expires_at = root.get("expires_at").as_uint();
      if (root.get("generation_id").is_string()) state.generation = std::string(root.get("generation_id").as_string());
    }
  }
  state.usage = completion.usage;
  state.model = completion.model;
  state.attempt = completion.attempt;
  state.raw_events = completion.raw_events;
  return state;
}
DownloadOutcome typed_content(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  Content content;
  for (const auto& message : completion.messages)
    for (const auto& part : message.parts) if (const auto* media = std::get_if<Media>(&part)) content.media = *media;
  for (const auto& raw : completion.raw_events) {
    if (raw.type != "download.response" || !raw.payload) continue;
    const auto root = raw.payload->root();
    content.content_type = std::string(root.get("content_type").as_string());
    if (root.get("path").is_string()) content.path = std::string(root.get("path").as_string());
    content.bytes = root.get("bytes").as_uint();
    content.sha256 = std::string(root.get("sha256").as_string());
  }
  content.attempt = completion.attempt;
  content.raw_events = completion.raw_events;
  return content;
}
}  // namespace sp::jobs
