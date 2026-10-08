#include "runtime/client.h"
#include "codecs/interactions_request.h"
#include "core/native.h"
#include "json/json.h"
#include "support/runtime_peer.h"
#include "support/vision_fixture.h"
#include <condition_variable>
#include <iostream>
#include <mutex>

namespace {
using namespace std::chrono_literals;
using runtime_test::Peer;
using runtime_test::require;
using sp::runtime::Client;
using sp::runtime::Result;
constexpr std::string_view key = "INTERACTIONS_SYNTHETIC_KEY";
constexpr std::string_view signature = "INTERACTIONS_FINAL_PRIVATE_SIGNATURE";
std::shared_ptr<const sp::json::Document> document(std::string_view text) { return std::make_shared<const sp::json::Document>(runtime_test::parse(text)); }
sp::descriptor::ValidatedDescriptor descriptor(Peer& peer, bool foreign = false) {
  auto loaded = sp::descriptor::load("{\"descriptor_version\":1,\"revision\":1,\"id\":\"interactions-runtime\",\"family\":\"google.interactions\",\"connection\":{\"base_url\":"
    + sp::json::quote(std::string(foreign ? "http://localhost:" : "http://127.0.0.1:") + std::to_string(peer.port))
    + R"(,"paths":{"buffered":"/v1beta/interactions","streaming":"/v1beta/interactions"}},"bindings":{"model":"model","messages":"input","stream":"stream","max_output_tokens":"max_output_tokens","usage":["usage"]}})");
  require(std::holds_alternative<sp::descriptor::ValidatedDescriptor>(loaded), "Interactions descriptor rejected");
  return std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded));
}
sp::runtime::Options options() { sp::runtime::Options o; o.api_key = key; o.default_timeout = 15s; o.retry_tokens_per_second = 0; return o; }
sp::runtime::RunOptions run(bool streaming) { sp::runtime::RunOptions o; o.streaming = streaming; return o; }
sp::interactions::Request request(const std::string& model, const vision_test::Scene& scene = vision_test::scene_a()) {
  sp::interactions::Request r; r.model = model; r.account_scope = "synthetic-interactions-account";
  r.system = "Count the actual image"; r.max_output_tokens = 128; r.thinking_level = "low";
  r.tools.push_back({"observe_scene", "Return observed geometry", document(R"({"type":"object","properties":{"red_circles":{"type":"integer"},"blue_squares":{"type":"integer"},"weighted":{"type":"integer"}},"required":["red_circles","blue_squares","weighted"]})")});
  r.messages.push_back(sp::Message{"", sp::Role::User, {sp::Text{"Count red circles and blue squares"}, scene.image(), sp::Text{"Use the host function"}}}); return r;
}
const sp::Completion& completion(const Result& r) { require(r && std::holds_alternative<sp::Completion>(*r), "expected completion"); return std::get<sp::Completion>(*r); }
const sp::Failure& failure(const Result& r, std::optional<sp::ErrorKind> kind = {}) {
  require(r && std::holds_alternative<sp::Failure>(*r), "expected failure"); const auto& f = std::get<sp::Failure>(*r);
  if (kind) require(f.error.kind == *kind, "wrong failure kind");
  for (auto marker : {key, signature}) require(f.error.safe_message.find(marker) == std::string::npos && f.error.vendor_code.find(marker) == std::string::npos, "private marker leaked");
  return f;
}
std::string text(const std::vector<sp::Message>& messages) {
  std::string out; for (const auto& m : messages) for (const auto& part : m.parts) if (const auto* t = std::get_if<sp::Text>(&part)) out += t->value; return out;
}
void no_authority(const std::vector<sp::Message>& messages) { for (const auto& m : messages) require(!m.native || !m.native->complete(), "failure granted complete replay authority"); }
void ineligible(const Result& result) {
  const auto& f = failure(result, sp::ErrorKind::ReplayIneligible);
  require(!f.error.attempt.request_may_have_left && f.error.attempt.request_body_bytes == 0, "ineligible capture dispatched HTTP bytes");
}
void check_group(const Result& result, const vision_test::Scene& scene) {
  const auto& c = completion(result); require(c.stop.kind == sp::StopKind::ToolUse && c.messages.size() == 1, "atomic tool generation lost");
  const auto& m = c.messages[0]; require(m.native && m.native->complete() && m.wire_output, "native original group missing");
  const auto& thought = std::get<sp::Thought>(m.parts[0]);
  require(thought.signature == signature && thought.summary == std::vector<std::string>{"Count colored components"}, "final thought summary/signature conflated");
  require(m.wire_output->root().at(0).get("future").get("retained").as_bool(), "additive original property dropped");
  const auto& call = std::get<sp::ToolCall>(m.parts[1]); require(call.id == "scene_call" && call.name == "observe_scene" && call.kind == sp::ToolCallKind::ClientExecuted, "host function identity wrong");
  require(sp::json::equal(call.input->root(), runtime_test::parse(scene.answer()).root()), "actual image answer mismatch");
  require(text(c.messages) == "need host result", "interleaved output lost");
  require(c.usage.output_total->value == 47 && c.usage.reasoning->value == 22 && c.usage.provider_reported_total->value == 147 && c.usage.total->value == 147, "output/thought accounting double counted");
}
sp::interactions::Request continuation(const std::string& model, const vision_test::Scene& scene, const Result& first) {
  auto r = request(model, scene); r.messages.push_back(completion(first).messages[0]);
  r.messages.push_back(sp::Message{"", sp::Role::Tool, {sp::ToolResult{"scene_call", scene.answer()}}}); return r;
}
void two_turn_vision_and_mutations(Peer& peer) {
  for (bool streaming : {false, true}) for (const auto* scene : {&vision_test::scene_a(), &vision_test::scene_b()}) {
    const auto model = peer.arm("grouped"); Client client(descriptor(peer), options());
    const auto first = client.complete(request(model, *scene), run(streaming)); check_group(first, *scene); peer.count(model, 1);
    auto next = continuation(model, *scene, first);
    auto reject = [&](sp::interactions::Request bad) { ineligible(client.complete(std::move(bad), run(!streaming))); peer.count(model, 1); };
    auto bad = next; std::get<sp::Thought>(bad.messages[1].parts[0]).signature = "edited"; reject(bad);
    bad = next; std::get<sp::Thought>(bad.messages[1].parts[0]).summary[0] = "edited"; reject(bad);
    bad = next; std::get<sp::ToolCall>(bad.messages[1].parts[1]).id = "foreign"; reject(bad);
    bad = next; std::swap(bad.messages[1].parts[0], bad.messages[1].parts[2]); reject(bad);
    bad = next; bad.messages[1].parts.erase(bad.messages[1].parts.begin()); reject(bad);
    bad = next; bad.messages[1].native.reset(); reject(bad);
    bad = next; bad.messages[1].wire_output.reset(); reject(bad);
    bad = next; auto original = bad.messages[1].wire_output->root(); bad.messages[1].wire_output = document("[" + original.at(2).dump() + "," + original.at(1).dump() + "," + original.at(0).dump() + "]"); reject(bad);
    bad = next; std::get<sp::Image>(bad.messages[0].parts[1]) = scene == &vision_test::scene_a() ? vision_test::scene_b().image() : vision_test::scene_a().image(); reject(bad);
    bad = next; std::get<sp::Image>(bad.messages[0].parts[1]).detail = sp::ImageDetail::High; reject(bad);
    bad = next; std::swap(bad.messages[0].parts[0], bad.messages[0].parts[2]); reject(bad);
    bad = next; bad.account_scope = "foreign"; reject(bad);
    bad = next; bad.thinking_level = "high"; reject(bad);
    bad = next; bad.thinking_summaries = false; reject(bad);
    bad = next; bad.system += " edited"; reject(bad);
    bad = next; bad.tools[0].description += " edited"; reject(bad);
    Client foreign(descriptor(peer, true), options()); ineligible(foreign.complete(next, run(streaming))); peer.count(model, 1);
    bad = next; std::get<sp::ToolResult>(bad.messages.back().parts[0]).tool_use_id = "foreign";
    failure(client.complete(bad, run(streaming)), sp::ErrorKind::InvalidRequest); peer.count(model, 1);
    bad = next; bad.messages.push_back(bad.messages.back()); failure(client.complete(bad, run(streaming)), sp::ErrorKind::InvalidRequest); peer.count(model, 1);
    bad = next; bad.messages.pop_back(); failure(client.complete(bad, run(streaming)), sp::ErrorKind::InvalidRequest); peer.count(model, 1);
    auto second = client.complete(next, run(!streaming)); require(sp::json::equal(runtime_test::parse(text(completion(second).messages)).root(), runtime_test::parse(scene->answer()).root()), "tool-result continuation answer mismatch");
    peer.count(model, 2); auto stats = peer.stats(model); require(stats.root().get("replayed").as_uint() == 1 && stats.root().get("image_checked").as_uint() == 2, "independent HTTP image/replay oracle not exercised");
    check_group(first, *scene);
  }
}
struct Capture {
  std::mutex mutex; std::condition_variable cv; Result result; size_t deltas = 0, outcomes = 0;
  sp::runtime::Callbacks callbacks() {
    return {[this](const sp::Event& event) { std::lock_guard lock(mutex); if (std::holds_alternative<sp::PartDelta>(event)) ++deltas; cv.notify_all(); },
      [this](Result value) { std::lock_guard lock(mutex); result = std::move(value); ++outcomes; cv.notify_all(); }};
  }
  void observed() { std::unique_lock lock(mutex); require(cv.wait_for(lock, 15s, [&] { return deltas != 0; }), "semantic callback timed out"); }
  void pending() { std::lock_guard lock(mutex); require(!result && outcomes == 0, "terminal event committed before normal close"); }
  Result await() { std::unique_lock lock(mutex); require(cv.wait_for(lock, 20s, [&] { return !!result; }), "outcome timed out"); require(outcomes == 1, "duplicate outcome"); return result; }
};
Result finish(sp::runtime::Operation& op, Capture& capture) { auto result = capture.await(); require(op.join() == result, "join lost owned outcome"); return result; }
void terminals_nullable_and_errors(Peer& peer) {
  Client client(descriptor(peer), options());
  for (bool streaming : {false, true}) {
    for (auto [scenario, kind] : std::vector<std::pair<std::string, sp::ErrorKind>>{{"failed", sp::ErrorKind::RemoteFailure}, {"cancelled", sp::ErrorKind::Cancelled}, {"unsupported", sp::ErrorKind::Unsupported}}) {
      auto model = peer.arm(scenario); no_authority(failure(client.complete(request(model), run(streaming)), kind).partial.messages); peer.count(model, 1);
    }
    auto model = peer.arm("nullable"); auto result = client.complete(request(model), run(streaming));
    require(!completion(result).usage.output_total && !completion(result).usage.reasoning && completion(result).usage.provider_reported_total->value == 125, "nullable thought became zero or inferred"); peer.count(model, 1);
    model = peer.arm("incomplete"); result = client.complete(request(model), run(streaming)); require(completion(result).stop.kind == sp::StopKind::MaxTokens, "incomplete became success EndTurn");
    no_authority(completion(result).messages); auto replay = request(model); replay.messages.push_back(completion(result).messages[0]); ineligible(client.complete(replay, run(!streaming))); peer.count(model, 1);
    model = peer.arm("short-close"); failure(client.complete(request(model), run(streaming))); peer.count(model, 1, 1);
    model = peer.arm("forced"); auto forced = request(model); forced.required_tool = "observe_scene"; auto first = client.complete(forced, run(streaming)); check_group(first, vision_test::scene_a());
    auto next = continuation(model, vision_test::scene_a(), first); auto second = client.complete(next, run(!streaming)); require(text(completion(second).messages) == vision_test::scene_a().answer(), "forced-tool choice unnecessarily bound continuation"); peer.count(model, 2);
  }
  for (auto [scenario, kind] : std::vector<std::pair<std::string, sp::ErrorKind>>{{"terminal-error", sp::ErrorKind::RemoteFailure}, {"partial-error", sp::ErrorKind::RemoteFailure}, {"status-only", sp::ErrorKind::Truncated}}) {
    const auto model = peer.arm(scenario); const auto result = client.complete(request(model), run(true));
    const auto& f = failure(result, kind); no_authority(f.partial.messages); require(f.partial.usage.stage != sp::UsageStage::Final, "failure usage stayed final"); peer.count(model, 1, 1);
  }
  const auto reset = peer.arm("reset"); failure(client.complete(request(reset), run(true))); peer.count(reset, 1, 1);
}
void close_cancel_deadline_and_retention(Peer& peer) {
  Result retained, partial;
  {
    Client client(descriptor(peer), options());
    for (const auto scenario : {"close-gate", "terminal-reset"}) {
      const auto model = peer.arm(scenario); Capture capture; auto op = client.start(request(model), run(true), capture.callbacks());
      peer.wait(model, "held"); capture.observed(); capture.pending(); peer.release(model); auto result = finish(op, capture);
      if (std::string_view(scenario) == "close-gate") { retained = result; require(text(completion(result).messages) == vision_test::scene_a().answer(), "close gate discarded complete result"); peer.count(model, 1); }
      else { const auto& f = failure(result); no_authority(f.partial.messages); require(f.partial.usage.stage != sp::UsageStage::Final, "reset finalized usage"); peer.count(model, 1, 1); }
    }
    const auto model = peer.arm("partial"); Capture cancel; auto op = client.start(request(model), run(true), cancel.callbacks());
    peer.wait(model, "held"); cancel.observed(); op.cancel(); partial = finish(op, cancel);
    require(text(failure(partial, sp::ErrorKind::Cancelled).partial.messages) == "partial owned", "cancel lost observed partial text"); no_authority(std::get<sp::Failure>(*partial).partial.messages); peer.count(model, 1);
    const auto timed_model = peer.arm("hold"); Capture deadline; auto timed = run(false); timed.deadline = runtime_test::Clock::now() + 3s;
    auto timeout = client.start(request(timed_model), timed, deadline.callbacks()); peer.wait(timed_model, "held"); failure(finish(timeout, deadline), sp::ErrorKind::DeadlineExceeded); peer.count(timed_model, 1);
  }
  require(std::get<sp::Thought>(completion(retained).messages[0].parts[0]).signature == signature, "client destruction invalidated owned signature");
  require(text(failure(partial).partial.messages) == "partial owned", "client destruction invalidated owned partial");
}
}
int main(int argc, char** argv) {
#ifdef _WIN32
  runtime_test::Arguments arguments(argc, argv);
  argc = arguments.argc(); argv = arguments.argv();
#endif
  try {
    require(argc == 3, "usage: sp_interactions_runtime_tests <node> <interactions_server.mjs>"); runtime_test::LogCapture logs;
    { Peer peer(argv[1], argv[2]); two_turn_vision_and_mutations(peer); terminals_nullable_and_errors(peer); close_cancel_deadline_and_retention(peer); }
    const auto captured = logs.finish(); require(captured.find(key) == std::string::npos && captured.find(signature) == std::string::npos, "private material leaked in logs");
    std::cout << "Interactions model-free HTTP/SSE image and replay contracts passed (not vendor acceptance)\n"; return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
