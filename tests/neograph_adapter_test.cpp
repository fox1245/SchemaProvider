#include "integration/neograph/adapter.h"
#include "core/native.h"
#include "json/json.h"
#include "support/runtime_peer.h"
#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {
using namespace std::chrono_literals;
using runtime_test::require;
using runtime_test::Peer;
using sp::neograph_integration::RuntimeProvider;
using sp::neograph_integration::BoundaryError;
using sp::neograph_integration::Family;
using neograph::CompletionRequest;
sp::descriptor::ValidatedDescriptor descriptor(const Peer& peer, bool messages) {
  auto source = "{\"descriptor_version\":1,\"revision\":1,\"id\":\"adapter-loopback\",\"family\":"
    + sp::json::quote(messages ? "anthropic.messages" : "openai.chat")
    + ",\"evidence\":{\"urls\":[\"https://example.com/spec\"],\"verified_at\":\"2026-10-01\"},"
      "\"connection\":{\"base_url\":" + sp::json::quote("http://127.0.0.1:" + std::to_string(peer.port))
    + ",\"paths\":{\"buffered\":" + sp::json::quote(messages ? "/v1/messages" : "/v1/chat/completions")
    + ",\"streaming\":" + sp::json::quote(messages ? "/v1/messages" : "/v1/chat/completions")
    + "},\"headers\":{\"anthropic-version\":\"2023-06-01\"}},"
      "\"bindings\":{\"model\":\"model\",\"messages\":\"messages\",\"stream\":\"stream\",\"max_output_tokens\":\"max_tokens\",\"usage\":[\"usage\"]},"
      "\"stop_reasons\":{\"stop\":\"EndTurn\",\"end_turn\":\"EndTurn\",\"tool_use\":\"ToolUse\"}}";
  auto loaded = sp::descriptor::load(source);
  require(std::holds_alternative<sp::descriptor::ValidatedDescriptor>(loaded), "descriptor failed");
  return std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded));
}
std::shared_ptr<sp::runtime::Client> client(const Peer& peer, bool messages) {
  sp::runtime::Options options;
  options.api_key = "RUNTIME_SECRET_MARKER_62c490";
  options.default_timeout = 10s;
  return std::make_shared<sp::runtime::Client>(descriptor(peer, messages), options);
}
neograph::CompletionParams params(std::string model) {
  neograph::CompletionParams p;
  p.model = std::move(model);
  p.messages.push_back({"user", "synthetic"});
  return p;
}
sp::runtime::Result run(RuntimeProvider& provider, CompletionRequest request) {
  return neograph::async::run_sync(provider.invoke_owned(std::move(request)));
}
const sp::Completion& completed(const sp::runtime::Result& result) {
  require(result && std::holds_alternative<sp::Completion>(*result), "expected owned completion");
  return std::get<sp::Completion>(*result);
}
const sp::Failure& failed(const sp::runtime::Result& result, sp::ErrorKind kind) {
  require(result && std::holds_alternative<sp::Failure>(*result), "expected owned failure");
  const auto& f = std::get<sp::Failure>(*result);
  require(f.error.kind == kind, "wrong failure kind");
  return f;
}
void has_gap(const BoundaryError& error, const std::string& gap) {
  require(std::find(error.gaps().begin(), error.gaps().end(), gap) != error.gaps().end(), "missing boundary class");
  require(std::string(error.what()).find("RUNTIME_SECRET") == std::string::npos, "unsafe exception text");
}
void modes_and_ownership(Peer& peer) {
  sp::runtime::Result retained;
  for (bool messages : {false, true}) {
    RuntimeProvider provider(client(peer, messages), messages ? Family::Messages : Family::Chat);
    for (bool streaming : {false, true}) {
      const auto model = peer.arm("normal");
      auto p = params(model);
      p.temperature = -1;
      retained = run(provider, streaming ? CompletionRequest::stream(p) : CompletionRequest::collect(p));
      const auto& c = completed(retained);
      require(std::get<sp::Text>(c.messages.at(0).parts.at(0)).value == "hello", "text lost");
      if (messages) {
        require(c.usage.input_uncached && c.usage.input_uncached->value == 2 && !c.usage.input_total,
                "Messages unknown cache counters falsely became an input total");
      } else require(c.usage.input_total && c.usage.input_total->value == 2, "Chat usage changed");
      require(!c.usage.cache_read, "unknown counter fabricated");
      peer.count(model, 1);
      const auto observation = peer.stats(model);
      require(observation.root().get("stream_seen").as_bool() == streaming, "explicit transport mode lost without observer");
      require(!observation.root().get("temperature_present").as_bool(), "omission sentinel became a wire value");
    }
    const auto model = peer.arm("normal");
    auto p = params(model);
    const auto caller = std::this_thread::get_id();
    std::string text;
    auto result = run(provider, CompletionRequest::stream(p, [&](const std::string& chunk) {
      require(caller == std::this_thread::get_id(), "observer not on caller executor");
      text += chunk;
    }));
    require(text == "hello", "observer lost text");
    completed(result);
    peer.count(model, 1);
    // Exercise the actual virtual Provider entry; it must not silently return
    // zero-valued counters or unsealed native JSON as a successful cutover.
    p.model = peer.arm("normal");
    neograph::Provider& legacy = provider;
    bool rejected = false;
    try { (void)legacy.complete(p); }
    catch (const BoundaryError& error) {
      rejected = true; has_gap(error, "nullable-usage"); has_gap(error, "usage-provenance");
      completed(error.evidence());
    }
    require(rejected, "legacy boundary falsely claimed lossless projection");
    peer.count(p.model, 1);
    p.model = peer.arm("normal");
    rejected = false;
    try {
      (void)neograph::async::run_sync(neograph::invoke_completion(legacy, CompletionRequest::stream(p)));
    } catch (const BoundaryError& error) {
      rejected = true; completed(error.evidence()); has_gap(error, "usage-provenance");
    }
    require(rejected, "async legacy result silently erased rich evidence");
    peer.count(p.model, 1);
    const auto stream_observation = peer.stats(p.model);
    require(stream_observation.root().get("stream_seen").as_bool(), "legacy STREAM without observer became collect");
  }
  require(std::get<sp::Text>(completed(retained).messages.at(0).parts.at(0)).value == "hello", "result outlived owner incorrectly");
}
void tool_native_and_failure(Peer& peer) {
  RuntimeProvider provider(client(peer, true), Family::Messages);
  auto p = params(peer.arm("tool"));
  auto result = run(provider, CompletionRequest::stream(p));
  const auto& c = completed(result);
  const auto& tool = std::get<sp::ToolCall>(c.messages.at(0).parts.at(0));
  require(tool.id == "tool-1" && tool.name == "synthetic" && tool.input->root().get("value").as_string().size() == 2048,
          "owned tool input changed");
  require(c.stop.kind == sp::StopKind::ToolUse, "tool stop changed");
  // Legacy Messages cannot carry the seal required for captured tool history.
  p.model = peer.arm("normal");
  neograph::ChatMessage assistant{"assistant", ""};
  assistant.tool_calls.push_back({tool.id, tool.name, tool.input->root().dump()});
  p.messages.push_back(std::move(assistant));
  neograph::ChatMessage reply{"tool", "inert synthetic result"};
  reply.tool_call_id = tool.id;
  p.messages.push_back(std::move(reply));
  bool history_rejected = false;
  try { (void)run(provider, CompletionRequest::collect(p)); }
  catch (const BoundaryError& error) { history_rejected = true; has_gap(error, "unsealed-tool-history"); }
  require(history_rejected, "unsealed Messages tool history was admitted");
  peer.count(p.model, 0);
  // Chat accepts ordinary client tool history. Verify correlation and arguments
  // on the actual HTTP wire, then finish the second turn through the runtime.
  RuntimeProvider chat_provider(client(peer, false), Family::Chat);
  completed(run(chat_provider, CompletionRequest::collect(p)));
  peer.count(p.model, 1);
  const auto observation = peer.stats(p.model);
  require(observation.root().get("tool_roundtrip_valid").as_bool(), "tool history/result wire correlation lost");
  p = params(peer.arm("reasoning"));
  result = run(provider, CompletionRequest::collect(p));
  require(completed(result).messages.at(0).native != nullptr, "native authority lost from rich outcome");
  bool rejected = false;
  try { (void)RuntimeProvider::project(result); }
  catch (const BoundaryError& error) {
    rejected = true; has_gap(error, "sealed-native-provenance");
    require(error.evidence().get() == result.get(), "exception copied or dropped owned result");
  }
  require(rejected, "native authority silently projected");
  p.messages.push_back({"assistant", ""});
  p.messages.back().reasoning_details = neograph::json::array({neograph::json{{"type", "thinking"}, {"signature", "forged"}}});
  rejected = false;
  try { (void)run(provider, CompletionRequest::collect(p)); }
  catch (const BoundaryError& error) { rejected = true; has_gap(error, "unsealed-native-input"); }
  require(rejected, "mutable native JSON was promoted to authority");
  peer.count(p.model, 1); // Rejected before dispatch; only the earlier capture ran.
  p = params(peer.arm("always-error"));
  failed(run(provider, CompletionRequest::collect(p)), sp::ErrorKind::Overloaded);
  peer.count(p.model, 1, 1); // Retries disabled.
}
void cancellation_and_exceptions(Peer& peer) {
  RuntimeProvider provider(client(peer, false), Family::Chat);
  auto p = params(peer.arm("normal"));
  p.cancel_token = std::make_shared<neograph::graph::CancelToken>();
  p.cancel_token->cancel();
  failed(run(provider, CompletionRequest::stream(p)), sp::ErrorKind::Cancelled);
  peer.count(p.model, 0);
  p = params(peer.arm("partial"));
  p.cancel_token = std::make_shared<neograph::graph::CancelToken>();
  auto token = p.cancel_token;
  auto result = run(provider, CompletionRequest::stream(p, [token](const std::string&) { token->cancel(); }));
  const auto& failure = failed(result, sp::ErrorKind::Cancelled);
  require(std::get<sp::Text>(failure.partial.messages.at(0).parts.at(0)).value == "partial", "cancel dropped owned partial text");
  peer.wait(p.model, "closed");
  p = params(peer.arm("partial"));
  struct ObserverFailure {};
  bool thrown = false;
  try { (void)run(provider, CompletionRequest::stream(p, [](const std::string&) { throw ObserverFailure{}; })); }
  catch (const ObserverFailure&) { thrown = true; }
  require(thrown, "observer exception swallowed");
  peer.wait(p.model, "closed");
  p = params(peer.arm("hold"));
  p.timeout_seconds = 1;
  failed(run(provider, CompletionRequest::collect(p)), sp::ErrorKind::DeadlineExceeded);
  peer.wait(p.model, "closed");
  p = params(peer.arm("partial"));
  asio::io_context io;
  asio::cancellation_signal signal;
  std::exception_ptr error;
  bool terminal = false;
  asio::co_spawn(io, provider.invoke_owned(CompletionRequest::stream(p, [&](const std::string&) {
    signal.emit(asio::cancellation_type::all);
  })), asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr e, sp::runtime::Result) {
    error = e; terminal = true;
  }));
  io.run();
  require(terminal && error, "caller coroutine cancellation did not propagate");
  peer.wait(p.model, "closed");
}
void abandoned_executor(Peer& peer) {
  RuntimeProvider provider(client(peer, false), Family::Chat);
  std::vector<std::string> models;
  auto thread_count = [] { return std::distance(std::filesystem::directory_iterator("/proc/self/task"), std::filesystem::directory_iterator{}); };
  const auto before = thread_count();
  {
    asio::io_context io;
    for (int i = 0; i < 12; ++i) {
      models.push_back(peer.arm("hold"));
      asio::co_spawn(io, provider.invoke_owned(CompletionRequest::stream(params(models.back()))), asio::detached);
    }
    io.poll();
    for (const auto& model : models) peer.wait(model, "held");
    require(thread_count() == before, "per-request bridge spawned threads");
    // io_context destruction abandons coroutines while producer callbacks can
    // still be executing. Those callbacks own only mailboxes, never io handles.
  }
  for (const auto& model : models) peer.wait(model, "closed");
  completed(run(provider, CompletionRequest::collect(params(peer.arm("normal")))));
  // A started owned operation is independent of the Provider object's lifetime.
  auto temporary = std::make_unique<RuntimeProvider>(client(peer, false), Family::Chat);
  auto p = params(peer.arm("hold"));
  asio::io_context io;
  sp::runtime::Result retained;
  std::exception_ptr error;
  asio::co_spawn(io, temporary->invoke_owned(CompletionRequest::collect(p)),
                 [&](std::exception_ptr e, sp::runtime::Result r) { error = e; retained = std::move(r); });
  io.poll();
  peer.wait(p.model, "held");
  temporary.reset();
  peer.release(p.model);
  io.run();
  if (error) std::rethrow_exception(error);
  completed(retained);
}
void numeric_and_part_boundaries() {
  sp::Completion c;
  c.messages.push_back({{}, sp::Role::Assistant, {sp::InvalidToolCall{"bad", "bad", sp::ToolCallKind::ClientExecuted, "{", sp::InvalidReason::NotJson}}});
  c.usage.input_total = sp::Count{1ULL << 40};
  c.stop.kind = sp::StopKind::PauseTurn;
  auto result = std::make_shared<const sp::Outcome>(std::move(c));
  try { (void)RuntimeProvider::project(result); require(false, "invalid tool projected"); }
  catch (const BoundaryError& error) {
    has_gap(error, "invalid-tool-call"); has_gap(error, "usage-overflow"); has_gap(error, "richer-stop-kind");
    require(error.evidence() == result, "rich invalid-tool evidence lost");
  }
}
}
int main(int argc, char** argv) {
  const char* stage = "startup";
  try {
    require(argc == 3, "usage: sp_neograph_adapter_tests <node> <neograph_peer.mjs>");
    Peer peer(argv[1], argv[2]);
    stage = "modes-and-ownership"; modes_and_ownership(peer);
    stage = "tool-native-failure"; tool_native_and_failure(peer);
    stage = "cancel-exception-deadline"; cancellation_and_exceptions(peer);
    stage = "abandoned-executor"; abandoned_executor(peer);
    stage = "numeric-part-boundaries"; numeric_and_part_boundaries();
    std::cout << "neograph adapter: owned runtime paths passed; legacy lossless cutover NO-GO\n";
  } catch (const std::exception&) { std::cerr << "neograph adapter probe failed: " << stage << '\n'; return 1; }
}
