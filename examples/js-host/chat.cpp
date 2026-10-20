// A chat call from a JavaScript host (Node, a browser, a Worker): the host seam wired up in a few lines.
//
// The module has no thread to wait on. `sp_example_chat` starts the call and returns to JavaScript's event loop; the
// executor and the fetch transport of sp_js_host carry it from there, and the outcome comes back through
// `Module.onOutcome(json)`. See run.mjs.
#include "descriptor/presets.h"
#include "json/json.h"
#include "runtime/client.h"
#include "runtime/js_host.h"

#include <emscripten.h>

#include <chrono>
#include <optional>
#include <string>

EM_JS(void, sp_example_report, (const char* json), { Module.onOutcome(UTF8ToString(json)); });
EM_JS_DEPS(sp_example_deps, "$UTF8ToString");

namespace {
using namespace std::chrono_literals;
// One client for the life of the module. The executor and the transport are the host seam: the Client runs its work
// on the first and sends every HTTP attempt through the second.
std::shared_ptr<sp::runtime::Executor> executor = sp::runtime::js::make_executor();
std::shared_ptr<sp::runtime::HostTransport> transport = sp::runtime::js::make_fetch_transport(executor);
std::optional<sp::runtime::Client> client;
sp::runtime::Operation operation;
std::size_t events = 0;

std::string outcome_json(const sp::runtime::Result& result) {
  namespace json = sp::json;
  if (const auto* error = sp::as_error(result))
    return "{\"ok\":false,\"kind\":" + std::to_string(static_cast<int>(error->kind)) + ",\"http_status\":" +
           std::to_string(error->http_status) + ",\"reason\":" + json::quote(error->reason) + "}";
  const auto* completion = sp::as_completion(result);
  std::string out = "{\"ok\":true,\"text\":" + json::quote(completion->joined_text()) + ",\"events\":" + std::to_string(events);
  if (const auto tokens = completion->usage.output_total_value()) out += ",\"output_tokens\":" + std::to_string(*tokens);
  if (const auto nano_usd = completion->usage.cost_nano_usd_value()) out += ",\"cost_nano_usd\":" + std::to_string(*nano_usd);
  return out + "}";
}
}  // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void sp_example_chat(const char* base_url, const char* api_key, const char* model,
                                                     const char* prompt, int streaming) {
  sp::descriptor::presets::PresetOptions preset;
  preset.base_url = base_url;  // empty: the preset's own origin
  client.emplace(sp::descriptor::presets::openrouter_chat(std::move(preset)), sp::runtime::Options::with_api_key(api_key), executor, transport);
  sp::runtime::RunOptions run;
  run.streaming = streaming != 0;
  run.retry = sp::runtime::RetryPolicy{false, false, 1, 0ms, 0ms};
  run.deadline = executor->now() + 60s;
  events = 0;
  sp::runtime::Callbacks callbacks;
  callbacks.on_event = [](const sp::Event&) { ++events; };
  callbacks.on_outcome = [](sp::runtime::Result result) { sp_example_report(outcome_json(result).c_str()); };
  operation = client->start(sp::chat::Request::simple(model, prompt, 64), run, std::move(callbacks));
}

int main() { return 0; }
