// Usage: sp_vision_campaign_tests <node> <tests/support/vision_server.mjs>
// Real HTTP/SSE, synthetic local images, no model or paid API calls.
#include "runtime/client.h"
#include "codecs/gemini_request.h"
#include "codecs/interactions_request.h"
#include "core/native.h"
#include "support/runtime_peer.h"
#include "support/vision_fixture.h"
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <type_traits>

namespace {
using namespace std::chrono_literals;
using runtime_test::require;
using runtime_test::Peer;
using sp::runtime::Result;
enum class Family { Chat, Responses, Messages, Gemini, Interactions };
constexpr Family families[] = {Family::Chat, Family::Responses, Family::Messages, Family::Gemini, Family::Interactions};
std::shared_ptr<const sp::json::Document> document(std::string_view value) {
  return std::make_shared<const sp::json::Document>(runtime_test::parse(value));
}
std::shared_ptr<const sp::json::Document> tool_schema() {
  return document(R"({"type":"object","properties":{"red_circles":{"type":"integer"},"blue_squares":{"type":"integer"},"weighted":{"type":"integer"}},"required":["red_circles","blue_squares","weighted"],"additionalProperties":false})");
}
sp::descriptor::ValidatedDescriptor descriptor(Peer& peer, Family family, const std::string& model) {
  const std::string name = family == Family::Chat ? "openai.chat" : family == Family::Responses ? "openai.responses" : family == Family::Messages ? "anthropic.messages" : family == Family::Gemini ? "google.generate" : "google.interactions";
  const std::string path = family == Family::Chat ? "/v1/chat/completions" : family == Family::Responses ? "/v1/responses" : family == Family::Messages ? "/v1/messages" : family == Family::Gemini ? "/v1beta/models/" + model + ":generateContent" : "/v1beta/interactions";
  const std::string streaming = family == Family::Gemini ? "/v1beta/models/" + model + ":streamGenerateContent?alt=sse" : path;
  const auto messages = family == Family::Responses || family == Family::Interactions ? "input" : family == Family::Gemini ? "contents" : "messages";
  auto loaded = sp::descriptor::load("{\"descriptor_version\":1,\"revision\":1,\"id\":\"vision-runtime\",\"family\":" + sp::json::quote(name)
      + ",\"connection\":{\"base_url\":" + sp::json::quote("http://127.0.0.1:" + std::to_string(peer.port))
      + ",\"paths\":{\"buffered\":" + sp::json::quote(path) + ",\"streaming\":" + sp::json::quote(streaming)
      + "},\"headers\":{\"anthropic-version\":\"2023-06-01\"}},\"bindings\":{\"model\":\"model\",\"messages\":" + sp::json::quote(messages)
      + ",\"stream\":\"stream\",\"max_output_tokens\":\"max_tokens\",\"usage\":[\"usage\"]},\"stop_reasons\":{\"stop\":\"EndTurn\",\"end_turn\":\"EndTurn\",\"tool_use\":\"ToolUse\"}}");
  require(std::holds_alternative<sp::descriptor::ValidatedDescriptor>(loaded), "vision descriptor rejected");
  return std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded));
}
sp::runtime::Options options() {
  sp::runtime::Options value; value.api_key = vision_test::secret; value.default_timeout = 15s; value.retry_tokens_per_second = 0; return value;
}
sp::runtime::RunOptions run(bool streaming) { sp::runtime::RunOptions value; value.streaming = streaming; return value; }
sp::runtime::Request request(Family family, const std::string& model, const vision_test::Scene& scene, bool on = true, bool tools = false) {
  const sp::Message user{"", sp::Role::User, {sp::Text{std::string(vision_test::question)}, scene.image()}};
  if (family == Family::Chat) {
    sp::chat::Request value; value.model = model; value.max_output_tokens = 2048; value.reasoning_effort = on ? "low" : "none";
    value.messages.push_back({sp::Role::User, std::string(vision_test::question), {}, "", {scene.image()}});
    if (tools) value.tools.push_back({"observe_scene", "Report counted colored shapes", tool_schema()});
    return value;
  }
  if (family == Family::Responses) {
    sp::responses::Request value; value.model = model; value.account_scope = "vision-account"; value.max_output_tokens = 2048;
    value.reasoning = sp::responses::ReasoningOptions{on ? "low" : "none", "auto"}; value.messages.push_back(user);
    if (tools) value.tools.push_back({"observe_scene", "Report counted colored shapes", tool_schema(), true});
    return value;
  }
  if (family == Family::Messages) {
    sp::messages::Request value; value.model = model; value.account_scope = "vision-account"; value.max_tokens = 2048;
    if (on) value.thinking_budget = 1024;
    value.messages.push_back(user);
    if (tools) value.tools.push_back({"observe_scene", "Report counted colored shapes", tool_schema(), "", {}});
    return value;
  }
  if (family == Family::Gemini) {
    sp::gemini::Request value; value.model = model; value.account_scope = "vision-account"; value.max_output_tokens = 2048;
    value.thinking_budget = on ? 1024 : 0; value.include_thoughts = on; value.messages.push_back(user);
    if (tools) value.tools.push_back({"observe_scene", "Report counted colored shapes", tool_schema()});
    return value;
  }
  sp::interactions::Request value; value.model = model; value.account_scope = "vision-account"; value.max_output_tokens = 2048;
  if (on) value.thinking_level = "low";
  value.thinking_summaries = on; value.messages.push_back(user);
  if (tools) value.tools.push_back({"observe_scene", "Report counted colored shapes", tool_schema()});
  return value;
}
const sp::Completion& completion(const Result& value) {
  require(value && std::holds_alternative<sp::Completion>(*value), "expected vision completion"); return std::get<sp::Completion>(*value);
}
const sp::Failure& failure(const Result& value) {
  require(value && std::holds_alternative<sp::Failure>(*value), "expected vision failure"); const auto& failed = std::get<sp::Failure>(*value);
  for (const auto marker : {vision_test::secret, vision_test::private_signature}) {
    require(failed.error.safe_message.find(marker) == std::string::npos && failed.error.vendor_code.find(marker) == std::string::npos, "private marker leaked");
  }
  return failed;
}
std::string text(const std::vector<sp::Message>& messages) {
  std::string value; for (const auto& message : messages) for (const auto& part : message.parts) if (const auto* t = std::get_if<sp::Text>(&part)) value += t->value; return value;
}
void oracle(sp::json::Value value, const vision_test::Scene& scene) {
  require(value.size() == 3 && value.get("red_circles").as_uint() == scene.red && value.get("blue_squares").as_uint() == scene.blue && value.get("weighted").as_uint() == scene.weighted, "image-dependent scene oracle mismatch");
}
void answer(const Result& value, const vision_test::Scene& scene) {
  const auto& output = completion(value); require(output.stop.kind == sp::StopKind::EndTurn, "vision answer stop incorrect");
  const auto parsed = runtime_test::parse(text(output.messages)); oracle(parsed.root(), scene);
}
void usage(const Result& value, Family family, bool on, bool missing = false) {
  const auto& output = completion(value);
  if (missing) {
    if (family == Family::Messages) require(output.usage.stage == sp::UsageStage::Final && !output.usage.input_total && !output.usage.reasoning && output.usage.output_total && output.usage.output_total->value == 7, "unknown Messages cache/input/reasoning fabricated");
    else require(output.usage.stage == sp::UsageStage::Missing && !output.usage.reasoning && !output.usage.output_total, "missing usage fabricated");
    return;
  }
  require(output.usage.stage == sp::UsageStage::Final && output.usage.input_total && output.usage.input_total->value == 10, "reported image usage lost");
  if (family == Family::Messages) require(!output.usage.reasoning, "Messages invented unreported reasoning count");
  else require(output.usage.reasoning && output.usage.reasoning->evidence == sp::Evidence::Reported && output.usage.reasoning->value == (on ? 3 : 0), "reported reasoning count lost");
  require(output.usage.output_total && output.usage.output_total->value == (family == Family::Gemini || family == Family::Interactions ? (on ? 7 : 4) : 7), "reasoning output algebra incorrect");
}
void set_image(sp::runtime::Request& value, sp::Image image) {
  std::visit([&](auto& typed) {
    using T = std::decay_t<decltype(typed)>;
    if constexpr (std::is_same_v<T, sp::chat::Request>) typed.messages[0].images[0] = image;
    else std::get<sp::Image>(typed.messages[0].parts[1]) = image;
  }, value);
}
void changed_images_and_controls(Peer& peer) {
  for (const auto family : families) for (const bool streaming : {false, true}) {
    for (const auto* scene : {&vision_test::scene_a(), &vision_test::scene_b()}) {
      const auto model = peer.arm(scene == &vision_test::scene_a() ? "vision-a-on" : "vision-b-on");
      sp::runtime::Client client(descriptor(peer, family, model), options());
      auto output = client.complete(request(family, model, *scene), run(streaming)); answer(output, *scene); usage(output, family, true); peer.count(model, 1);
      require(peer.stats(model).root().get("images").as_uint() == 1, "actual PNG not verified by peer");
    }
    const auto off = peer.arm("vision-a-off"); sp::runtime::Client disabled(descriptor(peer, family, off), options());
    auto output = disabled.complete(request(family, off, vision_test::scene_a(), false), run(streaming)); answer(output, vision_test::scene_a()); usage(output, family, false); peer.count(off, 1);
    const auto unknown = peer.arm("missing-usage"); sp::runtime::Client unreported(descriptor(peer, family, unknown), options());
    output = unreported.complete(request(family, unknown, vision_test::scene_a()), run(streaming)); answer(output, vision_test::scene_a()); usage(output, family, true, true); peer.count(unknown, 1);
  }
}
sp::runtime::Request continuation(sp::runtime::Request value, const Result& first) {
  std::visit([&](auto& typed) {
    using T = std::decay_t<decltype(typed)>;
    if constexpr (std::is_same_v<T, sp::chat::Request>) {
      sp::chat::InputMessage assistant; assistant.role = sp::Role::Assistant;
      for (const auto& message : completion(first).messages) for (const auto& part : message.parts) if (const auto* call = std::get_if<sp::ToolCall>(&part)) assistant.tool_calls.push_back(*call);
      typed.messages.push_back(std::move(assistant)); typed.messages.push_back({sp::Role::Tool, vision_test::scene_a().answer(), {}, "scene_call"});
    } else {
      for (const auto& message : completion(first).messages) typed.messages.push_back(message);
      typed.messages.push_back({"", sp::Role::User, {sp::ToolResult{"scene_call", vision_test::scene_a().answer()}}});
    }
  }, value); return value;
}
void preflight_refused(const Result& value) {
  const auto& rejected = failure(value); require(rejected.error.kind == sp::ErrorKind::ReplayIneligible && !rejected.error.attempt.request_may_have_left && rejected.error.attempt.request_body_bytes == 0, "native tamper crossed HTTP boundary");
}
void native_tool_loop(Peer& peer) {
  for (const auto family : families) for (const bool streaming : {false, true}) {
    const auto model = peer.arm("vision-a-on"); sp::runtime::Client client(descriptor(peer, family, model), options());
    auto original = request(family, model, vision_test::scene_a(), true, true); const auto first = client.complete(original, run(streaming));
    const auto& initial = completion(first); require(initial.stop.kind == sp::StopKind::ToolUse, "image function call absent");
    size_t calls = 0; for (const auto& message : initial.messages) for (const auto& part : message.parts) if (const auto* call = std::get_if<sp::ToolCall>(&part)) {
      require(call->name == "observe_scene" && call->id == "scene_call" && call->input, "tool identity lost"); oracle(call->input->root(), vision_test::scene_a()); ++calls;
    }
    require(calls == 1, "function generation duplicated executable calls"); usage(first, family, true);
    auto next = continuation(original, first);
    if (family != Family::Chat) {
      auto bad = next; set_image(bad, vision_test::scene_b().image()); preflight_refused(client.complete(bad, run(!streaming))); peer.count(model, 1);
      bad = next;
      std::visit([](auto& typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (!std::is_same_v<T, sp::chat::Request>) {
          for (auto& part : typed.messages[1].parts) {
            if (auto* p = std::get_if<sp::Thinking>(&part)) p->signature = "edited";
            if (auto* p = std::get_if<sp::Reasoning>(&part)) p->encrypted_content = "edited";
            if (auto* p = std::get_if<sp::Thought>(&part)) p->signature = "edited";
          }
        }
      }, bad);
      preflight_refused(client.complete(bad, run(!streaming))); peer.count(model, 1);
      bad = next;
      std::visit([](auto& typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (!std::is_same_v<T, sp::chat::Request>) typed.messages[1].native.reset();
      }, bad);
      preflight_refused(client.complete(bad, run(!streaming))); peer.count(model, 1);
    }
    const auto second = client.complete(next, run(!streaming)); answer(second, vision_test::scene_a()); peer.count(model, 2);
    require(peer.stats(model).root().get("replayed").as_uint() == 1, "native continuation not checked on real HTTP");
  }
}
struct Capture {
  std::mutex mutex; std::condition_variable cv; Result result; size_t outcomes = 0, deltas = 0, terminals = 0;
  sp::runtime::Callbacks callbacks() {
    return {[this](const sp::Event& event) { std::lock_guard lock(mutex);
      if (std::holds_alternative<sp::PartDelta>(event)) ++deltas;
      if (std::holds_alternative<sp::Commit>(event) || std::holds_alternative<sp::Fail>(event)) ++terminals;
      cv.notify_all(); }, [this](Result value) { std::lock_guard lock(mutex); result = std::move(value); ++outcomes; cv.notify_all(); }};
  }
  void observed() { std::unique_lock lock(mutex); require(cv.wait_for(lock, 15s, [&] { return deltas != 0; }), "vision delta timed out"); }
  void pending() { std::lock_guard lock(mutex); require(!result && outcomes == 0 && terminals == 0, "vision committed without normal close"); }
  Result finish(sp::runtime::Operation& operation) {
    { std::unique_lock lock(mutex); require(cv.wait_for(lock, 20s, [&] { return !!result; }), "vision outcome timed out"); }
    auto joined = operation.join(); std::lock_guard lock(mutex); require(outcomes == 1 && terminals == 0 && joined == result, "vision outcome was not singular and owned"); return result;
  }
};
void ownership_close_and_errors(Peer& peer) {
  for (const auto family : families) {
    const auto model = peer.arm("close-gate"); sp::runtime::Client client(descriptor(peer, family, model), options()); Capture capture;
    auto input = request(family, model, vision_test::scene_a()); auto operation = client.start(input, run(true), capture.callbacks());
    // Reassign the caller's image after start. The accepted request owns its original bytes.
    set_image(input, vision_test::scene_b().image()); input = request(family, model, vision_test::scene_b());
    peer.wait(model, "held"); capture.observed(); capture.pending(); peer.release(model); answer(capture.finish(operation), vision_test::scene_a()); peer.count(model, 1);
    const auto partial = peer.arm("partial-error"); sp::runtime::Client cut(descriptor(peer, family, partial), options()); Capture interrupted;
    auto pending = cut.start(request(family, partial, vision_test::scene_a()), run(true), interrupted.callbacks());
    peer.wait(partial, "held"); interrupted.observed(); interrupted.pending(); peer.release(partial);
    const auto output = interrupted.finish(pending); const auto& failed = failure(output);
    const auto parsed = runtime_test::parse(text(failed.partial.messages)); oracle(parsed.root(), vision_test::scene_a());
    require(failed.partial.usage.stage != sp::UsageStage::Final, "failed image generation finalized usage");
    for (const auto& message : failed.partial.messages)
      require(!message.native || !message.native->complete(), "partial output gained native replay authority");
    peer.count(partial, 1, 1);
    const auto truncated = peer.arm("short-close"); sp::runtime::Client abnormal(descriptor(peer, family, truncated), options());
    failure(abnormal.complete(request(family, truncated, vision_test::scene_a()), run(true))); peer.count(truncated, 1, 1);
    const auto invalid = peer.arm("vision-a-on"); sp::runtime::Client guarded(descriptor(peer, family, invalid), options());
    auto bad = request(family, invalid, vision_test::scene_a()); set_image(bad, {"image/png", std::make_shared<const std::string>("not-base64")});
    const auto result = guarded.complete(bad, run(true)); require(!failure(result).error.attempt.request_may_have_left, "invalid image dispatched"); peer.count(invalid, 0);
  }
}
void safe_remote_errors(Peer& peer) {
  for (const auto family : {Family::Responses, Family::Messages, Family::Gemini, Family::Interactions}) {
    const auto model = peer.arm("replay-rejected"); sp::runtime::Client client(descriptor(peer, family, model), options());
    auto original = request(family, model, vision_test::scene_a(), true, true); auto first = client.complete(original, run(true));
    const auto output = client.complete(continuation(original, first), run(false)); const auto& failed = failure(output);
    require(failed.error.http_status == 400 && failed.error.attempt.request_may_have_left, "remote signature rejection not observable"); peer.count(model, 2, 1);
  }
}
} // namespace
int main(int argc, char** argv) {
#ifdef _WIN32
  runtime_test::Arguments arguments(argc, argv);
  argc = arguments.argc(); argv = arguments.argv();
#endif
  try {
    require(argc == 3, "usage: sp_vision_campaign_tests <node> <vision_server.mjs>"); runtime_test::LogCapture diagnostics;
    { Peer peer(argv[1], argv[2]); changed_images_and_controls(peer); native_tool_loop(peer); ownership_close_and_errors(peer); safe_remote_errors(peer); }
    const auto logs = diagnostics.finish(); for (const auto marker : {vision_test::secret, vision_test::private_signature}) require(logs.find(marker) == std::string::npos, "private marker leaked in diagnostics");
    std::cout << "Five-family synthetic vision HTTP/SSE oracle and native reasoning contracts passed (model-free only)\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
