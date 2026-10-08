// Usage: sp_gemini_runtime_tests <node> <gemini_server.mjs>
#include "runtime/client.h"
#include "codecs/gemini_request.h"
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
using sp::runtime::Result;
constexpr std::string_view key = "GEMINI_SYNTHETIC_KEY";
std::shared_ptr<const sp::json::Document> owned(std::string_view text) {
  return std::make_shared<const sp::json::Document>(runtime_test::parse(text));
}
sp::descriptor::ValidatedDescriptor descriptor(const Peer& peer, const std::string& model, bool foreign = false) {
  const auto path = "/v1beta/models/" + model;
  auto d = sp::descriptor::load("{\"descriptor_version\":1,\"revision\":1,\"id\":\"gemini-runtime\",\"family\":\"google.generate\",\"connection\":{\"base_url\":"
    + sp::json::quote(std::string(foreign ? "http://localhost:" : "http://127.0.0.1:") + std::to_string(peer.port))
    + ",\"paths\":{\"buffered\":" + sp::json::quote(path + ":generateContent") + ",\"streaming\":" + sp::json::quote(path + ":streamGenerateContent?alt=sse")
    + R"(}},"bindings":{"model":"model","messages":"contents","stream":"stream","max_output_tokens":"maxOutputTokens","usage":["usageMetadata"]}})");
  require(std::holds_alternative<sp::descriptor::ValidatedDescriptor>(d), "Gemini descriptor rejected");
  return std::get<sp::descriptor::ValidatedDescriptor>(std::move(d));
}
sp::runtime::Options options() { sp::runtime::Options o; o.api_key = key; o.default_timeout = 15s; o.retry_tokens_per_second = 0; return o; }
sp::runtime::RunOptions run(bool sse) { sp::runtime::RunOptions o; o.streaming = sse; return o; }
sp::gemini::Request request(const std::string& model, const vision_test::Scene& scene = vision_test::scene_a()) {
  sp::gemini::Request r; r.model = model; r.account_scope = "synthetic-account"; r.system = "Answer briefly";
  r.max_output_tokens = 2048; r.thinking_budget = 1024;
  r.tools.push_back({"lookup", "Find value", owned(R"({"type":"object","properties":{"x":{"type":"integer"}}})")});
  r.messages.push_back(sp::Message{"", sp::Role::User, {sp::Text{std::string(vision_test::question)}, scene.image()}}); return r;
}
const sp::Completion& complete(const Result& r) { require(r && std::holds_alternative<sp::Completion>(*r), "expected Gemini completion"); return std::get<sp::Completion>(*r); }
const sp::Failure& failure(const Result& r, std::optional<sp::ErrorKind> kind = {}) {
  require(r && std::holds_alternative<sp::Failure>(*r), "expected Gemini failure"); const auto& f = std::get<sp::Failure>(*r);
  if (kind) require(f.error.kind == *kind, "unexpected Gemini failure kind");
  for (auto marker : {key, std::string_view{"THOUGHT_SIG"}, std::string_view{"CALL_SIG"}})
    require(f.error.safe_message.find(marker) == std::string::npos && f.error.vendor_code.find(marker) == std::string::npos, "native data leaked in error");
  return f;
}
std::string text(const std::vector<sp::Message>& messages) {
  std::string value; for (const auto& m : messages) for (const auto& p : m.parts) if (auto t = std::get_if<sp::Text>(&p)) value += t->value; return value;
}
void incomplete(const sp::Failure& f) {
  require(f.partial.usage.stage != sp::UsageStage::Final, "failed usage reported final");
  for (const auto& m : f.partial.messages) require(!m.native || !m.native->complete(), "failed generation gained replay authority");
}
void check_group(const Result& result, unsigned weighted = 8) {
  const auto& c = complete(result); require(c.stop.kind == sp::StopKind::ToolUse && c.messages.size() == 1, "Gemini tool group not atomic");
  const auto& m = c.messages[0]; require(m.native && m.native->complete() && m.wire_output && m.wire_output->root().size() == 4, "native group missing");
  require(m.parts.size() == 4 && std::get<sp::Thinking>(m.parts[0]).signature == "THOUGHT_SIG" && std::get<sp::Text>(m.parts[1]).value == "need lookup", "typed parts or thinking changed");
  const auto& call = std::get<sp::ToolCall>(m.parts[2]);
  require(call.id == "call_owned" && call.name == "lookup" && call.input->root().get("x").as_uint() == weighted, "image-dependent function args changed");
  require(call.wire_metadata->root().get("thoughtSignature").as_string() == "CALL_SIG", "call signature relocated");
  require(m.wire_output->root().at(1).get("thoughtSignature").as_string() == "TEXT_SIG" && m.wire_output->root().at(3).get("thoughtSignature").as_string() == "EMPTY_SIG", "text/empty signature lost");
  require(c.usage.stage == sp::UsageStage::Final && c.usage.input_total->value == 10 && c.usage.output_total->value == 10 && c.usage.reasoning->value == 3 && c.usage.total->value == 20 && c.usage.input_uncached->value == 6, "candidate/thought usage double billed");
}
sp::gemini::Request continuation(const std::string& model, const Result& first, const vision_test::Scene& scene) {
  auto r = request(model, scene); r.messages.push_back(complete(first).messages[0]);
  r.messages.push_back(sp::Message{"", sp::Role::Tool, {sp::ToolResult{"call_owned", "{\"result\":" + std::to_string(scene.weighted) + "}"}}}); return r;
}
struct Capture {
  std::mutex mutex; std::condition_variable cv; Result result; size_t outcomes = 0, deltas = 0, terminals = 0;
  sp::runtime::Callbacks callbacks() {
    return {[this](const sp::Event& e) { std::lock_guard lock(mutex);
      if (std::holds_alternative<sp::PartDelta>(e)) ++deltas;
      if (std::holds_alternative<sp::Commit>(e) || std::holds_alternative<sp::Fail>(e)) ++terminals;
      cv.notify_all(); }, [this](Result r) { std::lock_guard lock(mutex); result = std::move(r); ++outcomes; cv.notify_all(); }};
  }
  void observed() { std::unique_lock lock(mutex); require(cv.wait_for(lock, 15s, [&] { return deltas > 0; }), "semantic delta timeout"); }
  void pending() { std::lock_guard lock(mutex); require(!result && outcomes == 0 && terminals == 0, "committed before normal EOF"); }
  Result await() { std::unique_lock lock(mutex); require(cv.wait_for(lock, 20s, [&] { return !!result; }), "outcome timeout"); require(outcomes == 1 && terminals == 0, "terminal callbacks duplicated"); return result; }
};
Result join(sp::runtime::Operation& op, Capture& capture) { auto r = capture.await(); require(op.join() == r, "join lost ownership"); return r; }
void images_and_cross_mode(Peer& peer) {
  for (bool sse : {false, true}) for (const auto* scene : {&vision_test::scene_a(), &vision_test::scene_b()}) {
    auto model = peer.arm(scene->weighted == 11 ? "scene-b" : "grouped"); sp::runtime::Client client(descriptor(peer, model), options());
    auto first = client.complete(request(model, *scene), run(sse)); check_group(first, scene->weighted); peer.count(model, 1);
    auto r = continuation(model, first, *scene);
    auto refuse = [&](sp::gemini::Request bad) {
      const auto result = client.complete(std::move(bad), run(!sse));
      const auto& f = failure(result, sp::ErrorKind::ReplayIneligible);
      require(!f.error.attempt.request_may_have_left && f.error.attempt.request_body_bytes == 0, "replay refusal dispatched bytes"); peer.count(model, 1);
    };
    auto bad = r; std::get<sp::Text>(bad.messages[0].parts[0]).value = "foreign prefix"; refuse(bad);
    bad = r; std::get<sp::Image>(bad.messages[0].parts[1]) = (scene->weighted == 11 ? vision_test::scene_a() : vision_test::scene_b()).image(); refuse(bad);
    bad = r; std::get<sp::Thinking>(bad.messages[1].parts[0]).signature = "changed"; refuse(bad);
    bad = r; std::get<sp::ToolCall>(bad.messages[1].parts[2]).id = "foreign"; refuse(bad);
    bad = r; bad.messages[1].native.reset(); refuse(bad);
    bad = r; bad.messages[1].wire_output = owned("[]"); refuse(bad);
    bad = r; std::swap(bad.messages[1].parts[0], bad.messages[1].parts[3]); refuse(bad);
    bad = r; bad.messages[1].parts.pop_back(); refuse(bad);
    bad = r; bad.messages.insert(bad.messages.begin(), sp::Message{"", sp::Role::User, {sp::Text{"inserted"}}}); refuse(bad);
    bad = r; bad.account_scope = "foreign"; refuse(bad);
    bad = r; bad.system += "foreign"; refuse(bad);
    bad = r; bad.thinking_budget = 0; refuse(bad);
    bad = r; bad.tools[0].description += "foreign"; refuse(bad);
    sp::runtime::Client foreign(descriptor(peer, model, true), options()); failure(foreign.complete(r, run(!sse)), sp::ErrorKind::ReplayIneligible); peer.count(model, 1);
    bad = r; bad.messages.pop_back(); failure(client.complete(bad, run(!sse)), sp::ErrorKind::InvalidRequest); peer.count(model, 1);
    bad = r; std::get<sp::ToolResult>(bad.messages[2].parts[0]).tool_use_id = "foreign"; failure(client.complete(bad, run(!sse)), sp::ErrorKind::InvalidRequest); peer.count(model, 1);
    bad = r; std::get<sp::ToolResult>(bad.messages[2].parts[0]).content = "not JSON"; failure(client.complete(bad, run(!sse)), sp::ErrorKind::InvalidRequest); peer.count(model, 1);
    r.required_tool.reset();
    r.history_mode = sp::gemini::HistoryMode::PortableForeign;
    const auto second = client.complete(r, run(!sse)); require(text(complete(second).messages) == "{\"result\":" + std::to_string(scene->weighted) + "}", "image loop final answer wrong");
    peer.count(model, 2); const auto stats = peer.stats(model);
    require(stats.root().get("replayed").as_uint() == 1 && stats.root().get("images").as_uint() == 2, "actual peer did not verify images and native replay");
    check_group(first, scene->weighted);
  }
}
void terminals_and_ownership(Peer& peer) {
  Result retained, retained_failure;
  for (const auto scenario : {"close-gate", "completed-reset"}) {
    auto model = peer.arm(scenario); sp::runtime::Client client(descriptor(peer, model), options());
    Capture capture; auto op = client.start(request(model), run(true), capture.callbacks());
    peer.wait(model, "held"); capture.observed(); capture.pending(); peer.release(model); auto r = join(op, capture);
    if (std::string_view(scenario) == "close-gate") { retained = r; require(text(complete(r).messages) == "hello", "normal EOF lost output"); peer.count(model, 1); }
    else { incomplete(failure(r)); peer.count(model, 1, 1); }
  }
  for (const auto scenario : {"partial", "partial-error", "completed-error"}) {
    auto model = peer.arm(scenario); sp::runtime::Client client(descriptor(peer, model), options());
    if (std::string_view(scenario) == "partial") {
      Capture capture; auto op = client.start(request(model), run(true), capture.callbacks());
      peer.wait(model, "held"); capture.observed(); op.cancel(); retained_failure = join(op, capture);
      const auto& f = failure(retained_failure, sp::ErrorKind::Cancelled); incomplete(f); require(text(f.partial.messages) == "partial owned", "cancel lost partial output"); peer.count(model, 1);
    } else {
      const auto r = client.complete(request(model), run(true)); const auto& f = failure(r, sp::ErrorKind::RemoteFailure); incomplete(f);
      require(text(f.partial.messages) == (std::string_view(scenario) == "partial-error" ? "partial owned" : "hello"), "late remote error lost output"); peer.count(model, 1, 1);
    }
  }
  require(text(complete(retained).messages) == "hello" && std::get<sp::Thinking>(complete(retained).messages[0].parts[0]).signature == "THOUGHT_SIG", "client destruction invalidated retained native output");
  require(text(failure(retained_failure).partial.messages) == "partial owned", "client destruction invalidated partial output");
  {
    auto model = peer.arm("hold"); sp::runtime::Client client(descriptor(peer, model), options()); Capture capture;
    auto timed = run(false); timed.deadline = runtime_test::Clock::now() + 5s;
    auto op = client.start(request(model), timed, capture.callbacks()); peer.wait(model, "held"); incomplete(failure(join(op, capture), sp::ErrorKind::DeadlineExceeded)); peer.count(model, 1);
  }
  for (bool sse : {false, true}) {
    auto model = peer.arm("short-close"); sp::runtime::Client client(descriptor(peer, model), options()); incomplete(failure(client.complete(request(model), run(sse)))); peer.count(model, 1, 1);
  }
  {
    auto model = peer.arm("reset"); sp::runtime::Client client(descriptor(peer, model), options()); failure(client.complete(request(model), run(true))); peer.count(model, 1, 1);
  }
}
void invalid_and_nullable(Peer& peer) {
  for (bool sse : {false, true}) {
    auto model = peer.arm("invalid-args"); sp::runtime::Client client(descriptor(peer, model), options());
    auto r = client.complete(request(model), run(sse)); const auto& c = complete(r);
    require(std::holds_alternative<sp::InvalidToolCall>(c.messages[0].parts[0]) && (!c.messages[0].native || !c.messages[0].native->complete()), "invalid model args executable or replayable"); peer.count(model, 1);
    model = peer.arm("nullable-usage"); sp::runtime::Client nullable(descriptor(peer, model), options());
    r = nullable.complete(request(model), run(sse)); const auto& usage = complete(r).usage;
    require(usage.input_total->value == 10 && !usage.output_total && !usage.reasoning && !usage.total && usage.provider_reported_total->value == 20, "unknown thought usage guessed"); peer.count(model, 1);
  }
}
void portable_controls_on_wire(Peer& peer) {
  for (bool sse : {false, true}) for (const auto scenario : {"portable-text", "portable"}) {
    const auto model = peer.arm(scenario); sp::runtime::Client client(descriptor(peer, model), options());
    auto r = request(model); r.thinking_budget.reset(); r.thinking_level = sp::gemini::ThinkingLevel::High;
    r.include_thoughts = false; r.temperature = 0.25; r.history_mode = sp::gemini::HistoryMode::PortableForeign;
    r.safety_settings = {{sp::gemini::SafetyCategory::Harassment, sp::gemini::SafetyThreshold::BlockNone},
        {sp::gemini::SafetyCategory::HateSpeech, sp::gemini::SafetyThreshold::BlockOnlyHigh},
        {sp::gemini::SafetyCategory::SexuallyExplicit, sp::gemini::SafetyThreshold::BlockMediumAndAbove},
        {sp::gemini::SafetyCategory::DangerousContent, sp::gemini::SafetyThreshold::BlockLowAndAbove},
        {sp::gemini::SafetyCategory::CivicIntegrity, sp::gemini::SafetyThreshold::Off}};
    r.tool_choice = sp::gemini::ToolChoice{sp::gemini::ToolChoiceMode::Validated, {"lookup"}};
    r.messages.push_back(sp::Message{"", sp::Role::Assistant, {sp::Text{"foreign answer"}}});
    if (std::string_view(scenario) == "portable") {
      r.messages.push_back(sp::Message{"", sp::Role::Assistant, {sp::Text{"checking"},
          sp::ToolCall{"foreign-a", "lookup", sp::ToolCallKind::ClientExecuted, owned(R"({"x":1})")},
          sp::ToolCall{"foreign-b", "lookup", sp::ToolCallKind::ClientExecuted, owned(R"({"x":2})")}}});
      r.messages.push_back(sp::Message{"", sp::Role::Tool, {
          sp::ToolResult{"foreign-b", R"({"result":2})"}, sp::ToolResult{"foreign-a", R"({"result":1})"}}});
    }
    auto refuse = [&](sp::gemini::Request bad, sp::ErrorKind kind, size_t count) {
      const auto result = client.complete(std::move(bad), run(sse));
      const auto& f = failure(result, kind);
      require(!f.error.attempt.request_may_have_left && f.error.attempt.request_body_bytes == 0, "invalid portable controls dispatched");
      peer.count(model, count);
    };
    auto bad = r; bad.history_mode = sp::gemini::HistoryMode::NativeOnly; refuse(bad, sp::ErrorKind::ReplayIneligible, 0);
    bad = r; bad.history_mode = static_cast<sp::gemini::HistoryMode>(99); refuse(bad, sp::ErrorKind::InvalidRequest, 0);
    bad = r; bad.thinking_level = static_cast<sp::gemini::ThinkingLevel>(99); refuse(bad, sp::ErrorKind::InvalidRequest, 0);
    bad = r; bad.thinking_budget = 1024; refuse(bad, sp::ErrorKind::InvalidRequest, 0);
    bad = r; bad.temperature = 3; refuse(bad, sp::ErrorKind::InvalidRequest, 0);
    bad = r; bad.safety_settings[0].category = static_cast<sp::gemini::SafetyCategory>(99); refuse(bad, sp::ErrorKind::InvalidRequest, 0);
    bad = r; bad.safety_settings[0].threshold = static_cast<sp::gemini::SafetyThreshold>(99); refuse(bad, sp::ErrorKind::InvalidRequest, 0);
    bad = r; bad.safety_settings.push_back(bad.safety_settings[0]); refuse(bad, sp::ErrorKind::InvalidRequest, 0);
    bad = r; bad.tool_choice->mode = static_cast<sp::gemini::ToolChoiceMode>(99); refuse(bad, sp::ErrorKind::InvalidRequest, 0);
    bad = r; bad.tool_choice->allowed_function_names = {"lookup", "lookup"}; refuse(bad, sp::ErrorKind::InvalidRequest, 0);
    bad = r; bad.tool_choice->allowed_function_names = {"missing"}; refuse(bad, sp::ErrorKind::InvalidRequest, 0);
    bad = r; bad.tool_choice->mode = sp::gemini::ToolChoiceMode::Auto; refuse(bad, sp::ErrorKind::InvalidRequest, 0);
    bad = r; bad.required_tool = "lookup"; refuse(bad, sp::ErrorKind::InvalidRequest, 0);
    bad = r; bad.messages[1].parts.push_back(sp::Thinking{"foreign reasoning", "captured"}); refuse(bad, sp::ErrorKind::ReplayIneligible, 0);
    bad = r; bad.messages[1].wire_output = owned("[]"); refuse(bad, sp::ErrorKind::ReplayIneligible, 0);
    if (std::string_view(scenario) == "portable") {
      bad = r; std::get<sp::ToolCall>(bad.messages[2].parts[1]).wire_metadata = owned(R"({"thoughtSignature":"captured"})");
      refuse(bad, sp::ErrorKind::ReplayIneligible, 0);
      bad = r; std::get<sp::ToolResult>(bad.messages[3].parts[1]).tool_use_id = "foreign-b";
      refuse(bad, sp::ErrorKind::InvalidRequest, 0);
    }
    const auto first = client.complete(r, run(sse));
    require(text(complete(first).messages) == "portable accepted", "foreign history not accepted on wire");
    peer.count(model, 1);
    for (const auto& message : r.messages) require(!message.native && !message.wire_output, "import gained native authority");
    r.messages.push_back(complete(first).messages[0]);
    require(r.messages.back().native && r.messages.back().native->complete(), "new output lacks authentic seal");
    bad = r; std::get<sp::Text>(bad.messages[1].parts[0]).value = "changed imported prefix";
    refuse(bad, sp::ErrorKind::ReplayIneligible, 1);
    bad = r; bad.messages.back().wire_output = owned(R"([{"text":"changed native","thoughtSignature":"NEW_NATIVE_SIG"}])");
    refuse(bad, sp::ErrorKind::ReplayIneligible, 1);
    bad = r; bad.temperature = 0.5; refuse(bad, sp::ErrorKind::ReplayIneligible, 1);
    bad = r; bad.thinking_level = sp::gemini::ThinkingLevel::Low; refuse(bad, sp::ErrorKind::ReplayIneligible, 1);
    bad = r; bad.safety_settings[0].threshold = sp::gemini::SafetyThreshold::Off; refuse(bad, sp::ErrorKind::ReplayIneligible, 1);
    sp::runtime::Client foreign(descriptor(peer, model, true), options());
    failure(foreign.complete(r, run(!sse)), sp::ErrorKind::ReplayIneligible); peer.count(model, 1);
    require(text(complete(client.complete(r, run(!sse))).messages) == "portable accepted", "authentic continuation after import changed");
    peer.count(model, 2);
    require(peer.stats(model).root().get("replayed").as_uint() == 1, "peer did not verify authentic replay following import");
  }
}
}
int main(int argc, char** argv) {
#ifdef _WIN32
  runtime_test::Arguments arguments(argc, argv);
  argc = arguments.argc(); argv = arguments.argv();
#endif
  if (argc != 3) return 2;
  try { Peer peer(argv[1], argv[2]); portable_controls_on_wire(peer); images_and_cross_mode(peer); terminals_and_ownership(peer); invalid_and_nullable(peer); std::cout << "Gemini native HTTP/SSE scenarios passed\n"; }
  catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
