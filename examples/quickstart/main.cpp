#include <cppdotenv/dotenv.hpp>
#include <runtime/client.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
class QuickstartError final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

std::string load_api_key(const char* dotenv_path, const char* key_name) {
  auto values = cppdotenv::dotenv_values(dotenv_path, false);
  const auto found = values.find(key_name);
  if (found == values.end() || found->second.empty())
    throw QuickstartError("named key is missing or empty in the dotenv file");
  return std::move(found->second);
}

sp::runtime::RunOptions default_run_options() {
  sp::runtime::RunOptions run;
  run.streaming = false;
  run.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  sp::runtime::RetryPolicy retry;
  retry.enabled = false;
  retry.max_attempts = 1;
  run.retry = retry;
  return run;
}

int print_outcome(const sp::runtime::Result& result) {
  if (const auto* error = sp::as_error(result)) {
    std::cerr << "request failed: SDK error kind=" << static_cast<int>(error->kind)
              << ", HTTP status=" << error->http_status << '\n';
    return 1;
  }
  const auto* completion = sp::as_completion(result);
  if (!completion) throw QuickstartError("missing completion outcome");
  std::cout << completion->joined_text() << '\n';
  // This program declares no tools, so a tool call is only reported, never executed. The
  // cpp-standalone example shows the full tool loop.
  if (const auto calls = completion->executable_tool_calls(); !calls.empty())
    std::cout << "tool calls requested: " << calls.size() << '\n';
  if (const auto out_tokens = completion->usage.output_total_value())
    std::cout << "output tokens: " << *out_tokens << '\n';
  else
    std::cout << "output tokens: unknown\n";
  if (const auto nano_usd = completion->usage.cost_nano_usd_value())
    std::cout << "reported cost (nanoUSD): " << *nano_usd << '\n';
  return 0;
}

int run_quickstart(std::string_view preset, const char* model, const char* dotenv_path,
                   const char* key_name, const char* prompt) {
  sp::runtime::require_interface_contract(sp::EXPECTED_INTERFACE_REVISION,
                                          sp::capability::RequiredProvider);
  std::string api_key = load_api_key(dotenv_path, key_name);
  const auto run = default_run_options();
  constexpr std::uint64_t max_tokens = 128;

  using namespace sp::descriptor;
  if (preset == "openai-chat") {
    sp::runtime::Client client(presets::openai_chat(), std::move(api_key));
    return print_outcome(client.complete(sp::chat::Request::simple(model, prompt, max_tokens), run));
  }
  if (preset == "openai-responses") {
    sp::runtime::Client client(presets::openai_responses(), std::move(api_key));
    return print_outcome(client.complete(sp::responses::Request::simple(model, prompt, max_tokens), run));
  }
  if (preset == "anthropic") {
    sp::runtime::Client client(presets::anthropic_messages(), std::move(api_key));
    return print_outcome(client.complete(sp::messages::Request::simple(model, prompt, max_tokens), run));
  }
  if (preset == "gemini") {
    sp::runtime::Client client(presets::google_generate(model), std::move(api_key));
    return print_outcome(client.complete(sp::gemini::Request::simple(model, prompt, max_tokens), run));
  }
  if (preset == "interactions") {
    sp::runtime::Client client(presets::google_interactions(), std::move(api_key));
    return print_outcome(client.complete(sp::interactions::Request::simple(model, prompt, max_tokens), run));
  }
  if (preset == "openrouter-chat") {
    sp::runtime::Client client(presets::openrouter_chat(), std::move(api_key));
    return print_outcome(client.complete(sp::chat::Request::simple(model, prompt, max_tokens), run));
  }
  if (preset == "openrouter-responses") {
    sp::runtime::Client client(presets::openrouter_responses(), std::move(api_key));
    return print_outcome(client.complete(sp::responses::Request::simple(model, prompt, max_tokens), run));
  }
  if (preset == "openrouter-messages") {
    sp::runtime::Client client(presets::openrouter_messages(), std::move(api_key));
    return print_outcome(client.complete(sp::messages::Request::simple(model, prompt, max_tokens), run));
  }
  if (preset.starts_with("http://") || preset.starts_with("https://")) {
    presets::PresetOptions opts;
    opts.id = "quickstart-custom-chat";
    opts.base_url = std::string(preset);
    sp::runtime::Client client(presets::openai_chat(std::move(opts)), std::move(api_key));
    return print_outcome(client.complete(sp::chat::Request::simple(model, prompt, max_tokens), run));
  }
  throw QuickstartError("unknown preset; use openai-chat, openai-responses, anthropic, gemini, interactions, openrouter-chat, openrouter-responses, openrouter-messages, or a base URL");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 5 && argc != 6) {
    std::cerr << "Usage: sp_quickstart <preset-or-base-url> <model> <dotenv-path> <key-name> [prompt]\n";
    return 2;
  }
  for (int index = 1; index < argc; ++index) {
    if (!*argv[index]) {
      std::cerr << "error: positional arguments must not be empty\n";
      return 2;
    }
  }
  try {
    return run_quickstart(argv[1], argv[2], argv[3], argv[4],
                          argc == 6 ? argv[5] : "Reply with one short greeting.");
  } catch (const QuickstartError& error) {
    std::cerr << "error: " << error.what() << '\n';
  } catch (const sp::descriptor::ConfigError&) {
    std::cerr << "error: SDK client configuration rejected\n";
  } catch (const sp::runtime::InterfaceContractError&) {
    std::cerr << "error: incompatible SDK interface\n";
  } catch (const std::invalid_argument&) {
    std::cerr << "error: preset descriptor rejected\n";
  } catch (const std::exception&) {
    std::cerr << "error: unexpected client failure; diagnostics withheld\n";
  } catch (...) {
    std::cerr << "error: unknown client failure; diagnostics withheld\n";
  }
  return 1;
}
