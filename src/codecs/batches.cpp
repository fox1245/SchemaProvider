#include "codecs/batches.h"
#include "codecs/gemini.h"
#include "codecs/messages.h"
#include "codecs/reason.h"
#include "codecs/responses.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <algorithm>
#include <type_traits>

namespace sp::batches {
namespace {
std::string text_of(json::Value value) { return value.is_string() ? std::string(value.as_string()) : std::string(); }
// A count the provider gives as a number or, at Google, as a string of digits.
std::uint64_t count_of(json::Value value) {
  if (value.is_uint()) return value.as_uint();
  if (!value.is_string() || value.as_string().empty() || value.as_string().size() > 19) return 0;
  std::uint64_t out = 0;
  for (const char c : value.as_string()) { if (c < '0' || c > '9') return 0; out = out * 10 + static_cast<std::uint64_t>(c - '0'); }
  return out;
}
std::optional<std::uint64_t> time_of(json::Value value) {
  if (value.is_uint()) return value.as_uint();
  if (value.is_string()) return endpoints::unix_seconds(value.as_string());
  return std::nullopt;
}
std::optional<Count> counter(json::Value value) {
  if (!value.is_uint()) return std::nullopt;
  return Count{value.as_uint(), Evidence::Reported};
}
std::shared_ptr<const json::Document> copy(json::Value value) {
  auto parsed = json::parse(value.dump());
  return std::holds_alternative<json::Document>(parsed) ? std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed))) : nullptr;
}
// Google's inline results, where the finished batch carries them.
json::Value inline_results(json::Value root) {
  for (auto holder : {root.get("response"), root.get("metadata").get("output")}) {
    const auto list = holder.get("inlinedResponses").get("inlinedResponses");
    if (list.is_array()) return list;
  }
  return {};
}
std::string responses_file(json::Value root) {
  for (auto holder : {root.get("response"), root.get("metadata").get("output")})
    if (holder.get("responsesFile").is_string()) return std::string(holder.get("responsesFile").as_string());
  return {};
}
// The batch object of each shape: false when it names no id or status.
bool batch_of(json::Value root, std::string_view shape, std::string_view origin, Batch& batch) {
  if (!root.is_object()) return false;
  batch.origin = std::string(origin);
  batch.shape = std::string(shape);
  if (shape == "google") {
    const auto metadata = root.get("metadata");
    if (!root.get("name").is_string() || !root.get("name").as_string().starts_with("batches/") || !metadata.get("state").is_string()) return false;
    batch.id = std::string(root.get("name").as_string());
    batch.raw_status = std::string(metadata.get("state").as_string());
    const auto& s = batch.raw_status;
    batch.status = s == "BATCH_STATE_PENDING" ? jobs::Status::Pending : s == "BATCH_STATE_RUNNING" ? jobs::Status::Running
                 : s == "BATCH_STATE_SUCCEEDED" ? jobs::Status::Completed : s == "BATCH_STATE_FAILED" ? jobs::Status::Failed
                 : s == "BATCH_STATE_CANCELLED" ? jobs::Status::Cancelled : s == "BATCH_STATE_EXPIRED" ? jobs::Status::Expired : jobs::Status::Unknown;
    const auto stats = metadata.get("batchStats");
    batch.counts.total = count_of(stats.get("requestCount"));
    batch.counts.pending = count_of(stats.get("pendingRequestCount"));
    batch.counts.succeeded = count_of(stats.get("successfulRequestCount"));
    batch.counts.failed = count_of(stats.get("failedRequestCount"));
    batch.created_at = time_of(metadata.get("createTime"));
    batch.ended_at = time_of(metadata.get("endTime"));
    batch.output_file = responses_file(root);
    batch.results_ready = inline_results(root).is_array();
    return true;
  }
  const auto status = root.get(shape == "anthropic" ? "processing_status" : "status");
  if (!root.get("id").is_string() || root.get("id").as_string().empty() || !status.is_string()) return false;
  batch.id = std::string(root.get("id").as_string());
  batch.raw_status = std::string(status.as_string());
  const auto& s = batch.raw_status;
  const auto counts = root.get("request_counts");
  batch.created_at = time_of(root.get("created_at"));
  batch.expires_at = time_of(root.get("expires_at"));
  if (shape == "anthropic") {
    batch.status = s == "in_progress" || s == "canceling" ? jobs::Status::Running : s == "ended" ? jobs::Status::Completed : jobs::Status::Unknown;
    batch.counts.pending = count_of(counts.get("processing"));
    batch.counts.succeeded = count_of(counts.get("succeeded"));
    batch.counts.failed = count_of(counts.get("errored"));
    batch.counts.cancelled = count_of(counts.get("canceled"));
    batch.counts.expired = count_of(counts.get("expired"));
    batch.counts.total = batch.counts.pending + batch.counts.succeeded + batch.counts.failed + batch.counts.cancelled + batch.counts.expired;
    batch.ended_at = time_of(root.get("ended_at"));
    batch.results_ready = root.get("results_url").is_string();
    return true;
  }
  batch.status = s == "validating" ? jobs::Status::Pending : s == "in_progress" || s == "finalizing" || s == "cancelling" ? jobs::Status::Running
               : s == "completed" ? jobs::Status::Completed : s == "failed" ? jobs::Status::Failed : s == "expired" ? jobs::Status::Expired
               : s == "cancelled" ? jobs::Status::Cancelled : jobs::Status::Unknown;
  batch.counts.total = count_of(counts.get("total"));
  batch.counts.succeeded = count_of(counts.get("completed"));
  batch.counts.failed = count_of(counts.get("failed"));
  batch.counts.pending = batch.counts.total - std::min(batch.counts.total, batch.counts.succeeded + batch.counts.failed);
  batch.output_file = text_of(root.get("output_file_id"));
  batch.error_file = text_of(root.get("error_file_id"));
  for (const char* key : {"completed_at", "failed_at", "expired_at", "cancelled_at"})
    if (auto at = time_of(root.get(key))) batch.ended_at = at;
  if (const auto usage = root.get("usage"); usage.is_object()) {
    batch.usage.input_total = counter(usage.get("input_tokens"));
    batch.usage.output_total = counter(usage.get("output_tokens"));
    batch.usage.total = counter(usage.get("total_tokens"));
    batch.usage.cache_read = counter(usage.get("input_tokens_details").get("cached_tokens"));
    batch.usage.reasoning = counter(usage.get("output_tokens_details").get("reasoning_tokens"));
    batch.usage.stage = UsageStage::Final;
  }
  return true;
}
struct Context { std::string shape, origin, subject; json::Value response; };
std::optional<Context> context_of(const Completion& completion) {
  Context context;
  bool seen = false;
  for (const auto& raw : completion.raw_events) {
    if (!raw.payload) continue;
    const auto root = raw.payload->root();
    if (raw.type == "batches.context") {
      context.shape = text_of(root.get("shape"));
      context.origin = text_of(root.get("origin"));
      context.subject = text_of(root.get("subject"));
    } else if (raw.type == "batches.response") { context.response = root; seen = true; }
  }
  if (!seen) return std::nullopt;
  return context;
}
// The outcome of a line the provider failed: its document stays in the raw events.
sp::Outcome failed(std::shared_ptr<const json::Document> line, Error error) {
  Accumulator accumulator;
  error.reason = std::string(reason::code(reason::Source::Batches, error.safe_message));
  accumulator.accept(RawWire{"batches.line", std::move(line)});
  accumulator.accept(Fail{std::move(error)});
  return *accumulator.outcome();
}
}  // namespace

Codec::Codec(const descriptor::ValidatedDescriptor& descriptor, Accumulator& accumulator, Kind kind, std::string subject, SemanticLimits limits)
    : descriptor_(descriptor), accumulator_(accumulator), kind_(kind), subject_(std::move(subject)), limits_(limits) {}
bool Codec::emit(const Event& event) { return accumulator_.accept(event); }
bool Codec::fail(ErrorKind kind, std::string text) {
  Error error{kind, std::move(text)};
  error.reason = reason::code(reason::Source::Batches, error.safe_message);
  emit(Fail{std::move(error)});
  return false;
}
bool Codec::buffered(std::string_view body, Close close) {
  if (!close.normal) return fail(close.error, "abnormal response close");
  const auto* fact = descriptor_.policy()->endpoint("batches", descriptor_.base_url());
  if (!fact) return fail(ErrorKind::Unsupported, "batch endpoints are not admitted at this origin");
  auto parsed = json::parse(body.empty() ? std::string_view("{}") : body, {limits_.max_content_bytes, limits_.max_json_depth});
  if (auto* error = std::get_if<json::ParseError>(&parsed))
    return fail(error->code == json::ParseCode::SizeExceeded || error->code == json::ParseCode::DepthExceeded
                    ? ErrorKind::ResourceLimit : ErrorKind::ProtocolCorrupt, "invalid response JSON");
  auto wire = std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
  const auto root = wire->root();
  if (!root.is_object()) return fail(ErrorKind::ProtocolCorrupt, "response must be object");
  auto context = json::parse("{\"shape\":" + json::quote(fact->shape) + ",\"origin\":" + json::quote(descriptor_.base_url()) +
                             ",\"subject\":" + json::quote(subject_) + "}");
  if (!emit(RawWire{"batches.context", std::make_shared<const json::Document>(std::move(std::get<json::Document>(context)))}) ||
      !emit(RawWire{"batches.response", wire}))
    return false;
  Batch probe;
  if (kind_ == Kind::Batch && !batch_of(root, fact->shape, descriptor_.base_url(), probe)) return fail(ErrorKind::ProtocolCorrupt, "a batch object names its id and status");
  // Anthropic answers a deletion with a `message_batch_deleted` object, Google with an empty one.
  if (kind_ == Kind::Deleted && fact->shape == "anthropic" && text_of(root.get("type")) != "message_batch_deleted")
    return fail(ErrorKind::ProtocolCorrupt, "the response does not confirm the deletion");
  if (!emit(Begin{}) || !emit(MessageBegin{{0}, std::nullopt, Role::Assistant, nullptr})) return false;
  return emit(UsageUpdate{probe.usage}) && emit(Stop{StopReason{StopKind::EndTurn, "completed"}}) && emit(MessageSeal{{0}}) &&
         emit(Commit{"batches.response+normal_close"});
}

Outcome typed(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  Batch batch;
  if (const auto context = context_of(completion)) {
    batch_of(context->response, context->shape, context->origin, batch);
    batch.raw = copy(context->response);
  }
  batch.attempt = completion.attempt;
  batch.raw_events = completion.raw_events;
  return batch;
}
DeleteOutcome typed_deleted(const sp::Outcome& internal) {
  if (const auto* failure = std::get_if<Failure>(&internal)) return *failure;
  const auto& completion = std::get<Completion>(internal);
  Deleted deleted;
  if (const auto context = context_of(completion)) deleted.id = context->subject;
  deleted.attempt = completion.attempt;
  deleted.raw_events = completion.raw_events;
  return deleted;
}

std::vector<std::string> inline_lines(const Batch& batch) {
  std::vector<std::string> lines;
  if (!batch.raw) return lines;
  const auto list = inline_results(batch.raw->root());
  if (list.is_array()) for (auto item : list.elements()) lines.push_back(item.dump());
  return lines;
}
std::optional<std::string> custom_id(std::string_view line) {
  auto parsed = json::parse(line);
  if (!std::holds_alternative<json::Document>(parsed)) return std::nullopt;
  const auto root = std::get<json::Document>(parsed).root();
  for (auto value : {root.get("custom_id"), root.get("key"), root.get("metadata").get("key")})
    if (value.is_string()) return std::string(value.as_string());
  return std::nullopt;
}

std::variant<Line, Error> decode(const descriptor::ValidatedDescriptor& family, const Request& original, std::string_view line, SemanticLimits limits) {
  auto parsed = json::parse(line, {limits.max_content_bytes, limits.max_json_depth});
  if (!std::holds_alternative<json::Document>(parsed)) return Error{ErrorKind::InvalidRequest, "a result line is not a batch result"};
  auto document = std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
  const auto root = document->root();
  Line out;
  json::Value body;
  // OpenAI: the HTTP answer each line got. Anthropic: a typed result. Google: a response or an error.
  if (root.get("custom_id").is_string() && root.get("response").valid()) {
    out.custom_id = std::string(root.get("custom_id").as_string());
    const auto response = root.get("response");
    if (root.get("error").valid() && !root.get("error").is_null()) {
      out.outcome = failed(document, Error{ErrorKind::RemoteFailure, "the provider failed the batch request"});
      return out;
    }
    const auto status = response.get("status_code");
    if (!status.is_uint() || status.as_uint() < 200 || status.as_uint() >= 300) {
      Error error{ErrorKind::RemoteFailure, "the provider failed the batch request"};
      if (status.is_uint()) error.http_status = static_cast<int>(status.as_uint());
      out.outcome = failed(document, std::move(error));
      return out;
    }
    body = response.get("body");
  } else if (root.get("custom_id").is_string() && root.get("result").is_object()) {
    out.custom_id = std::string(root.get("custom_id").as_string());
    const auto result = root.get("result");
    const auto type = text_of(result.get("type"));
    if (type == "canceled") { out.outcome = failed(document, Error{ErrorKind::Cancelled, "the batch request was cancelled"}); return out; }
    if (type == "expired") { out.outcome = failed(document, Error{ErrorKind::DeadlineExceeded, "the batch request expired"}); return out; }
    if (type != "succeeded") { out.outcome = failed(document, Error{ErrorKind::RemoteFailure, "the provider failed the batch request"}); return out; }
    body = result.get("message");
  } else if (auto key = root.get("key").is_string() ? root.get("key") : root.get("metadata").get("key"); key.is_string()) {
    out.custom_id = std::string(key.as_string());
    if (root.get("error").is_object() || !root.get("response").is_object()) {
      out.outcome = failed(document, Error{ErrorKind::RemoteFailure, "the provider failed the batch request"});
      return out;
    }
    body = root.get("response");
  } else {
    return Error{ErrorKind::InvalidRequest, "a result line is not a batch result"};
  }
  if (!body.is_object()) return Error{ErrorKind::InvalidRequest, "a result line is not a batch result"};
  // The family codec decodes the answer against the request it answers, as it would a buffered call.
  const auto text = body.dump();
  return std::visit([&](const auto& typed) -> std::variant<Line, Error> {
    using T = std::decay_t<decltype(typed)>;
    Accumulator accumulator;
    if constexpr (std::is_same_v<T, chat::Request>) {
      auto encoded = chat::encode(family, typed, false);
      if (auto* error = std::get_if<Error>(&encoded)) return *error;
      chat::Codec codec(family, chat::Mode::Buffered, accumulator, limits, std::get<0>(encoded).context);
      codec.buffered(text, {});
    } else if constexpr (std::is_same_v<T, messages::Request>) {
      auto encoded = messages::encode(family, typed, false);
      if (auto* error = std::get_if<Error>(&encoded)) return *error;
      messages::Codec codec(family, messages::Mode::Buffered, accumulator, std::get<0>(encoded).context, limits);
      codec.buffered(text, {});
    } else if constexpr (std::is_same_v<T, responses::Request>) {
      auto encoded = responses::encode(family, typed, false);
      if (auto* error = std::get_if<Error>(&encoded)) return *error;
      responses::Codec codec(family, responses::Mode::Buffered, accumulator, std::get<0>(encoded).context, limits);
      codec.buffered(text, {});
    } else {
      auto encoded = gemini::encode(family, typed, false);
      if (auto* error = std::get_if<Error>(&encoded)) return *error;
      gemini::Codec codec(family, gemini::Mode::Buffered, accumulator, std::get<0>(encoded).context, limits);
      codec.buffered(text, {});
    }
    if (!accumulator.outcome()) return Error{ErrorKind::ProtocolCorrupt, "a result line did not decode to an outcome"};
    out.outcome = *accumulator.outcome();
    return std::move(out);
  }, original);
}
}  // namespace sp::batches
