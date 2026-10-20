// Provider jobs (D6): an OpenRouter video submit, a poll of its handle and a download of its output, the request
// each takes and the typed state or content of a response, and the runtime's file sink for a download.
#include "codecs/interaction_jobs.h"
#include "codecs/jobs.h"
#include "codecs/reason.h"
#include "descriptor/presets.h"
#include "codecs/videos.h"
#include "crypto/crypto.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include "runtime/testing.h"
#include "support/endpoint_fakes.h"
#include "sp/config_defaults.h"
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
using namespace sp;
using namespace sp::test;
using namespace std::chrono_literals;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)
constexpr std::string_view local = "http://127.0.0.1:18080";
// The builtin policy, with the video endpoint fact at `origin` too.
descriptor::PolicySnapshot policy_at(std::string_view origin) {
  auto parsed = json::parse(config_defaults::descriptor_policy_json);
  CHECK(std::holds_alternative<json::Document>(parsed));
  std::string text = "{";
  for (auto member : std::get<json::Document>(parsed).root().members()) {
    if (text.size() > 1) text += ',';
    if (member.key != "endpoints") { text += json::quote(member.key) + ":" + member.value.dump(); continue; }
    text += "\"endpoints\":[";
    for (auto fact : member.value.elements()) text += fact.dump() + ",";
    text += R"({"endpoint":"videos","shape":"openrouter","origins":[)" + json::quote(origin) +
            R"(],"paths":{"submit":"/api/v1/videos","poll":"/api/v1/videos/{id}","download":"/api/v1/videos/{id}/content"},"charge_on_accept":true}])";
  }
  auto policy = descriptor::load_policy(text + "}", config_defaults::codec_defaults_json);
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(policy));
  return std::get<descriptor::PolicySnapshot>(std::move(policy));
}
descriptor::ValidatedDescriptor at(std::string_view origin) {
  const std::string path = origin == "https://openrouter.ai" ? "/api/v1/chat/completions" : "/v1/chat/completions";
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"jobs-fixture","family":"openai.chat","connection":{"base_url":)" +
                                 json::quote(origin) + R"(,"paths":{"buffered":)" + json::quote(path) + R"(,"streaming":)" + json::quote(path) + "}}}",
                                 policy_at(local));
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
std::shared_ptr<const json::Document> document(std::string_view text) {
  auto parsed = json::parse(text);
  CHECK(std::holds_alternative<json::Document>(parsed));
  return std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
}
template <class Request, class Encode> endpoints::Encoded encoded(Encode encode, const descriptor::ValidatedDescriptor& d, const Request& r) {
  auto result = encode(d, r);
  if (const auto* error = std::get_if<Error>(&result)) throw std::runtime_error("encode refused: " + error->safe_message);
  return std::get<endpoints::Encoded>(std::move(result));
}
template <class Request, class Encode> ErrorKind refused(Encode encode, const descriptor::ValidatedDescriptor& d, const Request& r) {
  auto result = encode(d, r);
  CHECK(std::holds_alternative<Error>(result));
  return std::get<Error>(result).kind;
}
std::string hex(const crypto::Sha256Digest& digest) {
  std::string out;
  for (const auto byte : digest) { out += "0123456789abcdef"[byte >> 4]; out += "0123456789abcdef"[byte & 15]; }
  return out;
}
const char png[] = "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAEklEQVR4nGO4ckUEK2IYlRIBAPzqLqpk9vWsAAAAAElFTkSuQmCC";

void video_submit() {
  const auto router = at("https://openrouter.ai");
  videos::Request request{"x-ai/grok-imagine-video-1.5-lite", "A red fox runs through snow."};
  request.duration = 1; request.resolution = "480p"; request.aspect_ratio = "16:9"; request.audio = false;
  request.references = {Media::image("image/png", std::make_shared<const std::string>(png))};
  request.extensions = document(R"({"seed":7,"provider":{"order":["xai"]}})");
  const auto wire = encoded(videos::encode, router, request);
  CHECK(wire.path == "/api/v1/videos" && wire.method == "POST" && wire.charge_on_accept && wire.sink.empty());
  {
    const auto body = document(wire.body);
    const auto root = body->root();
    CHECK(root.get("model").as_string() == "x-ai/grok-imagine-video-1.5-lite" && root.get("duration").as_uint() == 1 &&
          root.get("resolution").as_string() == "480p" && root.get("aspect_ratio").as_string() == "16:9" && !root.get("generate_audio").as_bool() &&
          root.get("input_references").at(0).get("image_url").get("url").as_string().starts_with("data:image/png;base64,") &&
          root.get("seed").as_uint() == 7 && root.get("provider").get("order").at(0).as_string() == "xai" && !root.get("size").valid());
  }
  // A data URL cannot carry the whitespace a MIME type may have around its parameters.
  for (const char* mime : {"image/png; q=1", "image/png ;q=1", "image/png;\tq=1", "image/png \t;\t q=1"}) {
    auto parameterized = request; parameterized.references = {Media::image(mime, std::make_shared<const std::string>(png))};
    const auto body = document(encoded(videos::encode, router, parameterized).body);
    CHECK(body->root().get("input_references").at(0).get("image_url").get("url").as_string() == std::string("data:image/png;q=1;base64,") + png);
  }
  auto marked = request; marked.references = {Media::image("image/png;q=#", std::make_shared<const std::string>(png))};
  CHECK(refused(videos::encode, router, marked) == ErrorKind::InvalidRequest);
  auto none = request; none.prompt.clear();
  CHECK(refused(videos::encode, router, none) == ErrorKind::InvalidRequest);
  auto spaced = request; spaced.resolution = "480 p";
  CHECK(refused(videos::encode, router, spaced) == ErrorKind::InvalidRequest);
  auto zero = request; zero.duration = 0;
  CHECK(refused(videos::encode, router, zero) == ErrorKind::InvalidRequest);
  auto uploaded = request; uploaded.references = {Media::file(MediaKind::Image, "file-1", "image/png")};
  CHECK(refused(videos::encode, router, uploaded) == ErrorKind::Unsupported);
  auto owned = request; owned.extensions = document(R"({"duration":5})");
  CHECK(refused(videos::encode, router, owned) == ErrorKind::InvalidRequest);
  CHECK(refused(videos::encode, at("https://api.openai.com"), request) == ErrorKind::Unsupported);
}

void job_requests() {
  const auto router = at("https://openrouter.ai");
  const jobs::Handle handle{"videos", "https://openrouter.ai", "job-7f3a", "x-ai/grok-imagine-video-1.5-lite"};
  const auto poll = encoded(static_cast<std::variant<jobs::Encoded, Error> (*)(const descriptor::ValidatedDescriptor&, const jobs::Poll&)>(jobs::encode),
                            router, jobs::Poll{handle});
  CHECK(poll.path == "/api/v1/videos/job-7f3a" && poll.method == "GET" && poll.body.empty() && poll.content_type.empty() && !poll.charge_on_accept);
  using DownloadEncode = std::variant<jobs::Encoded, Error> (*)(const descriptor::ValidatedDescriptor&, const jobs::Download&);
  jobs::Download download{handle, 2, "/tmp/out.mp4", 1u << 20};
  const auto fetch = encoded(static_cast<DownloadEncode>(jobs::encode), router, download);
  CHECK(fetch.path == "/api/v1/videos/job-7f3a/content?index=2" && fetch.method == "GET" && fetch.accept == "*/*" &&
        fetch.sink == "/tmp/out.mp4" && fetch.sink_limit == (1u << 20) && !fetch.charge_on_accept);
  // A handle reaches only the origin that started its job, by an id that is a path token.
  auto elsewhere = download; elsewhere.handle.origin = "https://api.openai.com";
  CHECK(refused(static_cast<DownloadEncode>(jobs::encode), router, elsewhere) == ErrorKind::InvalidRequest);
  auto traversal = download; traversal.handle.id = "../keys";
  CHECK(refused(static_cast<DownloadEncode>(jobs::encode), router, traversal) == ErrorKind::InvalidRequest);
  // `.` and `..` are made of token characters, but a request for them leaves the job's path.
  for (const char* id : {".", ".."}) {
    auto dotted = download; dotted.handle.id = id;
    CHECK(refused(static_cast<DownloadEncode>(jobs::encode), router, dotted) == ErrorKind::InvalidRequest);
    using PollEncode = std::variant<jobs::Encoded, Error> (*)(const descriptor::ValidatedDescriptor&, const jobs::Poll&);
    CHECK(refused(static_cast<PollEncode>(jobs::encode), router, jobs::Poll{dotted.handle}) == ErrorKind::InvalidRequest);
  }
  auto dotted_name = download; dotted_name.handle.id = "job..7";
  CHECK(encoded(static_cast<DownloadEncode>(jobs::encode), router, dotted_name).path == "/api/v1/videos/job..7/content?index=2");
  auto unknown = download; unknown.handle.endpoint = "images";
  CHECK(refused(static_cast<DownloadEncode>(jobs::encode), router, unknown) == ErrorKind::Unsupported);
  auto unbounded = download; unbounded.max_bytes = 0;
  CHECK(refused(static_cast<DownloadEncode>(jobs::encode), router, unbounded) == ErrorKind::InvalidRequest);
}

// A background interaction read as a job: its handle, its poll request, and its state.
void interaction_jobs() {
  const auto d = descriptor::presets::google_interactions();
  Completion pending;
  Message message;
  message.id = "v1_ChdkMzRiNmY";
  pending.messages.push_back(message);
  pending.stop = StopReason{StopKind::Pending, "in_progress"};
  pending.model = {"gemini-3.5-flash-lite", {}, ServedRelation::NotReported};
  const auto handle = jobs::from_interaction(d, pending);
  CHECK(handle && handle->endpoint == "interactions" && handle->origin == "https://generativelanguage.googleapis.com" &&
        handle->id == "v1_ChdkMzRiNmY" && handle->model == "gemini-3.5-flash-lite");
  CHECK(!jobs::from_interaction(descriptor::presets::google_generate(), pending));
  const auto poll = jobs::interaction_poll(*handle);
  CHECK(poll.lifecycle == interactions::Lifecycle::Get && poll.interaction_id == handle->id && poll.model == handle->model && poll.messages.empty());
  const auto state = [&](sp::Outcome outcome) { return jobs::interaction_state(d, outcome); };
  CHECK(std::get<jobs::State>(state(pending)).status == jobs::Status::Running && std::get<jobs::State>(state(pending)).handle.id == handle->id);
  auto queued = pending; queued.stop.raw = "queued";
  CHECK(std::get<jobs::State>(state(queued)).status == jobs::Status::Pending);
  auto done = pending; done.stop = StopReason{StopKind::EndTurn, "completed"};
  CHECK(std::get<jobs::State>(state(done)).status == jobs::Status::Completed);
  auto cancelled = pending; cancelled.stop = StopReason{StopKind::Unknown, "cancelled"};
  CHECK(std::get<jobs::State>(state(cancelled)).status == jobs::Status::Cancelled);
  Error failed{ErrorKind::RemoteFailure, "interaction failed"};
  failed.reason = std::string(reason::code(reason::Source::Interactions, "interaction failed"));
  CHECK(std::get<jobs::State>(state(Failure{failed, {pending.messages}})).status == jobs::Status::Failed);
  Error lost{ErrorKind::Transport, "transport failure"};
  CHECK(std::holds_alternative<Failure>(state(Failure{lost, {}})));
}

jobs::Outcome state_of(const descriptor::ValidatedDescriptor& d, std::string_view body) {
  Accumulator accumulator;
  jobs::StateCodec codec(d, accumulator, "x-ai/grok-imagine-video-1.5-lite", "videos");
  codec.buffered(body, {});
  CHECK(accumulator.outcome());
  return jobs::typed(*accumulator.outcome());
}
void job_states() {
  const auto router = at("https://openrouter.ai");
  const auto pending = state_of(router, R"({"id":"job-7f3a","polling_url":"https://openrouter.ai/api/v1/videos/job-7f3a","status":"pending"})");
  const auto& started = std::get<jobs::State>(pending);
  CHECK(started.status == jobs::Status::Pending && started.handle.id == "job-7f3a" && started.handle.endpoint == "videos" &&
        started.handle.origin == "https://openrouter.ai" && started.handle.model == "x-ai/grok-imagine-video-1.5-lite" && started.outputs == 0);
  const auto done = state_of(router, R"({"id":"job-7f3a","generation_id":"gen-1","status":"completed","urls":["https://cdn.example/a"],"unsigned_urls":["https://openrouter.ai/api/v1/videos/job-7f3a/content?index=0"],"expires_at":1790000000,"usage":{"cost":0.02,"is_byok":false},"error":null})");
  const auto& finished = std::get<jobs::State>(done);
  CHECK(finished.status == jobs::Status::Completed && finished.outputs == 1 && finished.generation == "gen-1" && finished.expires_at == 1790000000u &&
        finished.usage.provider_cost.total && finished.usage.provider_cost.total->nano_usd == 20'000'000u && finished.raw_status == "completed");
  CHECK(std::get<jobs::State>(state_of(router, R"({"id":"j","status":"failed","error":"moderated"})")).status == jobs::Status::Failed);
  CHECK(std::get<jobs::State>(state_of(router, R"({"id":"j","status":"in_progress"})")).status == jobs::Status::Running);
  CHECK(std::get<jobs::State>(state_of(router, R"({"id":"j","status":"rendering"})")).status == jobs::Status::Unknown);
  for (const auto* body : {R"({"status":"pending"})", R"({"id":"j"})", R"({"id":"j","status":"completed","urls":"x"})", R"([1])"}) {
    const auto failed = state_of(router, body);
    CHECK(std::holds_alternative<Failure>(failed) && std::get<Failure>(failed).error.kind == ErrorKind::ProtocolCorrupt &&
          std::get<Failure>(failed).error.reason.starts_with("jobs."));
  }
}

jobs::DownloadOutcome content_of(std::string_view body, std::string type, std::optional<endpoints::Sink> sink = {}) {
  Accumulator accumulator;
  jobs::ContentCodec codec(accumulator);
  codec.buffered(body, {true, ErrorKind::Truncated, {{"content-type", std::move(type)}}, std::move(sink)});
  CHECK(accumulator.outcome());
  return jobs::typed_content(*accumulator.outcome());
}
void job_content() {
  const std::string mp4("\x00\x00\x00\x18" "ftypmp42synthetic", 21);
  const auto held = content_of(mp4, "video/mp4");
  const auto& memory = std::get<jobs::Content>(held);
  CHECK(memory.media && memory.media->kind == MediaKind::Video && memory.media->mime == "video/mp4" && memory.media->data &&
        *memory.media->data == base64_encode(mp4) && memory.bytes == mp4.size() && memory.sha256 == hex(crypto::sha256(mp4)) && memory.path.empty());
  const auto written = content_of({}, "video/mp4; codecs=avc1", endpoints::Sink{"/tmp/v.mp4", 4096, std::string(64, 'a')});
  const auto& file = std::get<jobs::Content>(written);
  CHECK(!file.media && file.path == "/tmp/v.mp4" && file.bytes == 4096 && file.sha256 == std::string(64, 'a') && file.content_type == "video/mp4");
  // A document where a download belongs is an error the status did not report.
  for (const auto& [body, type] : std::initializer_list<std::pair<std::string_view, std::string>>{{R"({"error":"x"})", "application/json"}, {"", "video/mp4"}}) {
    const auto failed = content_of(body, type);
    CHECK(std::holds_alternative<Failure>(failed) && std::get<Failure>(failed).error.kind == ErrorKind::ProtocolCorrupt);
  }
  CHECK(std::holds_alternative<Failure>(content_of({}, "video/mp4", endpoints::Sink{"/tmp/v.mp4", 0, {}})));
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void runtime_job() {
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"jobs-runtime","family":"openai.chat","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/chat/completions","streaming":"/v1/chat/completions"}}})",
                                 policy_at(local));
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  auto executor = std::make_shared<QueueExecutor>();
  auto wire = std::make_shared<ScriptTransport>();
  runtime::Options options;
  options.api_key = "synthetic-key";
  // Far below the video: a download bound for a file is held to its own limit.
  options.limits.max_response_bytes = 1u << 20;
  auto client = runtime::detail::ClientAccess::make(std::get<descriptor::ValidatedDescriptor>(std::move(loaded)), options, executor, wire);
  runtime::RunOptions run;
  // No duplicate billing risk accepted: only reads and calls that never reached the provider are retried.
  run.retry = runtime::RetryPolicy{true, false, 3, 0ms, 0ms};
  const auto run_for = [&] { auto value = run; value.deadline = executor->now() + 10s; return value; };
  const auto complete = [&](auto prepared) { auto call = client.start(std::move(prepared)); executor->drain(); return call.join(); };

  wire->script = [](Exchange& e) { e.reply(202, "application/json", R"({"id":"job-1","polling_url":"http://127.0.0.1:18080/api/v1/videos/job-1","status":"pending"})"); };
  const auto submitted = complete(client.prepare(videos::Request{"x-ai/grok-imagine-video-1.5-lite", "A fox."}, run_for()));
  const auto handle = std::get<jobs::State>(submitted).handle;
  const auto& submit = wire->exchanges.back()->request;
  CHECK(submit.method == "POST" && submit.url == "http://127.0.0.1:18080/api/v1/videos" && header(submit, "Content-Type") == "application/json" &&
        header(submit, "Authorization") == "Bearer synthetic-key" && document(submit.body)->root().get("prompt").as_string() == "A fox.");
  CHECK(handle.id == "job-1" && handle.origin == "http://127.0.0.1:18080" && handle.endpoint == "videos" && std::get<jobs::State>(submitted).status == jobs::Status::Pending);

  // Released, the same call runs as an untyped one, and the endpoint's typed() reads its Result.
  wire->script = [](Exchange& e) { e.reply(200, "application/json", R"({"id":"job-1","status":"in_progress"})"); };
  auto released = client.prepare(jobs::Poll{handle}, run_for());
  auto untyped = std::move(released).release();
  CHECK(!released.valid() && untyped.valid() && !untyped.error() && untyped.endpoint() == "videos");
  CHECK(untyped.method() == "GET" && untyped.url() == "http://127.0.0.1:18080/api/v1/videos/job-1" && untyped.encoded_body().empty());
  auto operation = client.start(std::move(untyped));
  executor->drain();
  const auto raw = operation.join();
  CHECK(raw && std::holds_alternative<Completion>(*raw));
  const auto typed = jobs::typed(*raw);
  CHECK(std::holds_alternative<jobs::State>(typed) && std::get<jobs::State>(typed).status == jobs::Status::Running &&
        std::get<jobs::State>(typed).handle.id == "job-1" && std::get<jobs::State>(typed).handle.endpoint == "videos");

  wire->script = [](Exchange& e) { e.reply(200, "application/json", R"({"id":"job-1","status":"completed","unsigned_urls":["u"],"usage":{"cost":0.02}})"); };
  const auto polled = complete(client.prepare(jobs::Poll{handle}, run_for()));
  const auto& poll = wire->exchanges.back()->request;
  CHECK(poll.method == "GET" && poll.url == "http://127.0.0.1:18080/api/v1/videos/job-1" && header(poll, "Content-Type").empty() && poll.body.empty());
  CHECK(std::get<jobs::State>(polled).status == jobs::Status::Completed && std::get<jobs::State>(polled).outputs == 1);

  const auto directory = std::filesystem::temp_directory_path() / ("sp-jobs-test-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(directory);
  const auto target = directory / "out.mp4";
  std::string video("\x00\x00\x00\x18" "ftypmp42", 12);
  while (video.size() < (3u << 20)) video += static_cast<char>('a' + video.size() % 23);
  wire->script = [&](Exchange& e) { e.reply(200, "video/mp4", video); };
  const auto fetched = complete(client.prepare(jobs::Download{handle, 0, target.string(), 8u << 20}, run_for()));
  const auto& download = wire->exchanges.back()->request;
  CHECK(download.url == "http://127.0.0.1:18080/api/v1/videos/job-1/content?index=0" && header(download, "Accept") == "*/*");
  const auto& content = std::get<jobs::Content>(fetched);
  CHECK(!content.media && content.path == target.string() && content.bytes == video.size() && content.sha256 == hex(crypto::sha256(video)) &&
        content.content_type == "video/mp4");
  CHECK(read_file(target) == video && !std::filesystem::exists(target.string() + ".part"));

  // Past the download's limit, or a document in its place: no file is left behind.
  std::filesystem::remove(target);
  const auto limited = complete(client.prepare(jobs::Download{handle, 0, target.string(), 1u << 20}, run_for()));
  CHECK(std::holds_alternative<Failure>(limited) && std::get<Failure>(limited).error.kind == ErrorKind::ResourceLimit && std::get<Failure>(limited).error.reason == "runtime.sink.limit");
  CHECK(!std::filesystem::exists(target) && !std::filesystem::exists(target.string() + ".part"));
  wire->script = [](Exchange& e) { e.reply(200, "application/json", R"({"error":"not ready"})"); };
  const auto mislabeled = complete(client.prepare(jobs::Download{handle, 0, target.string()}, run_for()));
  CHECK(std::holds_alternative<Failure>(mislabeled) && !std::filesystem::exists(target) && !std::filesystem::exists(target.string() + ".part"));
  // A failed status is no file either: the error body is read as an error.
  wire->script = [](Exchange& e) { e.reply(404, "application/json", R"({"error":{"code":404,"message":"not found"}})"); };
  const auto missing = complete(client.prepare(jobs::Download{handle, 0, target.string()}, run_for()));
  CHECK(std::holds_alternative<Failure>(missing) && std::get<Failure>(missing).error.http_status == 404 && !std::filesystem::exists(target));

  // A connection lost mid-body is retried, and the file holds the retry's body alone.
  std::size_t attempts = 0;
  wire->script = [&](Exchange& e) {
    if (++attempts == 1) {
      e.head(200, {{"content-type", "video/mp4"}});
      e.callbacks.on_body(std::string_view(video).substr(0, 4096));
      e.finish(transport::Status::Failed, transport::FailureKind::Receive);
    } else e.reply(200, "video/mp4", video.substr(0, 100000));
  };
  const auto retried = complete(client.prepare(jobs::Download{handle, 0, target.string()}, run_for()));
  CHECK(std::holds_alternative<jobs::Content>(retried) && attempts == 2 && read_file(target) == video.substr(0, 100000) &&
        std::get<jobs::Content>(retried).attempt.attempts == 2);

  // Without a path the output is held in memory, under the response limit.
  wire->script = [&](Exchange& e) { e.reply(200, "video/mp4", video.substr(0, 5000)); };
  const auto held = complete(client.prepare(jobs::Download{handle, 0}, run_for()));
  CHECK(std::get<jobs::Content>(held).media && *std::get<jobs::Content>(held).media->data == base64_encode(video.substr(0, 5000)));
  wire->script = [&](Exchange& e) { e.reply(200, "video/mp4", video); };
  CHECK(std::holds_alternative<Failure>(complete(client.prepare(jobs::Download{handle, 0}, run_for()))));
  std::filesystem::remove_all(directory);
}
}  // namespace

int main() {
  try { video_submit(); job_requests(); job_states(); job_content(); interaction_jobs(); runtime_job(); std::cout << "job properties passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << "job test failed: " << error.what() << '\n'; return 1; }
}
