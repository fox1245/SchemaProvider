#include <neograph/llm/schema_provider.h>
#include <neograph/completion_provider.h>
#include <neograph/async/run_sync.h>
#include "builtin_schemas.h"
#include "tests/support/posix_owner.h"
#include <cerrno>
#include <sys/mman.h>
#include <iostream>
#include <limits>
#include <string>

namespace {
using neograph::json;
json project(const neograph::ChatCompletion& c) {
  json out = json::object();
  out["outcome"] = "completion";
  json message;
  neograph::to_json(message, c.message);
  out["message"] = std::move(message);
  out["stop_reason"] = c.stop_reason;
  out["usage"] = json{{"input_total", c.usage.prompt_tokens}, {"output_total", c.usage.completion_tokens},
    {"total", c.usage.total_tokens}, {"cache_read", c.usage.cached_prompt_tokens}, {"reasoning", c.usage.reasoning_tokens}};
  out["artifact_count"] = static_cast<unsigned long long>(c.artifacts.size());
  return out;
}
}
int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::invalid_argument("usage");
    const std::string origin(argv[1]);
    const std::string prefix = "http://127.0.0.1:";
    if (!origin.starts_with(prefix) || origin.size() == prefix.size() ||
        origin.find_first_not_of("0123456789", prefix.size()) != std::string::npos)
      throw std::invalid_argument("loopback required");
    std::string input;
    std::getline(std::cin, input);
    if (input.size() > (16U << 20)) throw std::invalid_argument("input bound");
    const auto envelope = json::parse(input);
    const auto fixture = envelope.at("fixture");
    if (fixture.at("fixture_version").get<int>() != 2 || fixture.at("provenance").at("kind").get<std::string>() != "synthetic")
      throw std::invalid_argument("synthetic fixture required");
    const bool messages = fixture.at("origin").at("family").get<std::string>() == "anthropic.messages";
    // Translate only declared fixture configuration into the current dialect;
    // do not leave synthetic headers or omission defaults unconfigured.
    auto schema = json::parse(neograph::llm::builtin::schemas().at(messages ? "claude" : "openai"));
    const auto descriptor = envelope.at("descriptor");
    const auto connection = descriptor.at("connection");
    const auto paths = connection.at("paths");
    schema["connection"]["endpoint"] = paths.at("buffered");
    schema["connection"]["stream_endpoint"] = paths.at("streaming");
    if (connection.contains("headers")) schema["connection"]["extra_headers"] = connection.at("headers");
    const auto headers = fixture.at("request").at("semantic_headers");
    // These headers are mode-specific transport intent, not provider identity.
    if (headers.contains("accept")) schema["connection"]["extra_headers"]["Accept"] = headers.at("accept");
    if (descriptor.contains("bindings")) {
      const auto bindings = descriptor.at("bindings");
      for (const auto& mapping : {std::pair{"model", "model_field"}, {"messages", "messages_field"},
                                  {"stream", "stream_field"}, {"max_output_tokens", "max_tokens_path"}})
        if (bindings.contains(mapping.first)) schema["request"][mapping.second] = bindings.at(mapping.first);
      if (bindings.contains("usage") && bindings.at("usage").size() == 1)
        schema["response"]["usage_path"] = bindings.at("usage").at(0);
    }
    if (descriptor.contains("stop_reasons")) {
      const std::pair<const char*, const char*> normalized[] = {
        {"EndTurn", "end_turn"}, {"ToolUse", "tool_use"}, {"MaxTokens", "max_tokens"},
        {"StopSequence", "stop_sequence"}, {"ContentFilter", "content_filter"}, {"Refusal", "refusal"},
        {"PauseTurn", "pause_turn"}, {"ContextLimit", "context_limit"}, {"MalformedCall", "malformed_call"},
        {"Unknown", "unknown"}
      };
      schema["response"]["stop_reason_map"] = json::object();
      for (const auto& [raw, meaning] : descriptor.at("stop_reasons").items())
        for (const auto& mapping : normalized)
          if (meaning.get<std::string>() == mapping.first) {
            schema["response"]["stop_reason_map"][raw] = mapping.second;
            break;
          }
    }
    schema["request"]["extra_fields"] = json::object();
    schema["request"]["per_call_fields"] = json::array({"thinking"});
    runtime_test::Fd schema_file(::memfd_create("sp-legacy-fixture-schema", MFD_CLOEXEC));
    if (!schema_file) throw std::runtime_error("schema storage");
    std::string bytes = schema.dump();
    std::string_view rest(bytes);
    while (!rest.empty()) {
      const auto count = ::write(schema_file.get(), rest.data(), rest.size());
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) throw std::runtime_error("schema write");
      rest.remove_prefix(static_cast<std::size_t>(count));
    }
    neograph::llm::SchemaProvider::Config config;
    config.schema_path = "/proc/self/fd/" + std::to_string(schema_file.get());
    config.api_key = "synthetic-not-a-credential";
    config.base_url_override = origin;
    config.allow_insecure_loopback = true;
    config.timeout_seconds = 5;
    auto provider = neograph::llm::SchemaProvider::create(config);
    const auto in = fixture.at("input");
    neograph::CompletionParams params;
    params.model = in.at("model").get<std::string>();
    params.timeout_seconds = 5;
    params.temperature = in.contains("temperature") ? in.at("temperature").get<float>() : -1.0f;
    if (in.contains("system")) params.messages.push_back({"system", in.at("system").get<std::string>()});
    for (const auto& m : in.at("messages")) params.messages.push_back({m.at("role").get<std::string>(), m.at("content").get<std::string>()});
    if (in.contains("max_tokens")) params.max_tokens = in.at("max_tokens").get<int>();
    else if (messages) params.max_tokens = 1024; // Existing fixture-runner request default.
    if (in.contains("thinking_budget")) params.extra_fields = json{{"thinking", json{{"type", "enabled"}, {"budget_tokens", in.at("thinking_budget")}}}};
    json gaps = json::array();
    if (descriptor.contains("bindings") && descriptor.at("bindings").contains("usage") &&
        descriptor.at("bindings").at("usage").size() > 1) gaps.push_back("usage-path-precedence");
    if (in.contains("tools")) for (const auto& t : in.at("tools")) {
      neograph::ChatTool tool;
      tool.name = t.at("name").get<std::string>();
      tool.description = t.value("description", "");
      tool.parameters = t.contains("input_schema") ? t.at("input_schema") : json::object();
      params.tools.push_back(std::move(tool));
      if (t.contains("type") || t.contains("max_uses")) gaps.push_back("server-tool-definition");
    }
    if (fixture.contains("replay_from")) {
      gaps.push_back("unsealed-native-replay");
      if (envelope.contains("replay_message")) {
        neograph::ChatMessage previous;
        neograph::from_json(envelope.at("replay_message"), previous);
        params.messages.push_back(std::move(previous));
      } else gaps.push_back("missing-legacy-replay-source");
      if (in.contains("tool_results")) for (const auto& r : in.at("tool_results")) {
        neograph::ChatMessage result{"tool", r.at("content").get<std::string>()};
        result.tool_call_id = r.at("tool_use_id").get<std::string>();
        params.messages.push_back(std::move(result));
        if (r.value("is_error", false)) gaps.push_back("tool-result-error-flag");
      }
    }
    json result;
    try {
      const auto request = fixture.at("transport").at("mode").get<std::string>() == "sse"
        ? neograph::CompletionRequest::stream(params) : neograph::CompletionRequest::collect(params);
      result = project(neograph::async::run_sync(neograph::invoke_completion(*provider, request)));
    } catch (const neograph::RateLimitError&) {
      result = json{{"outcome", "failure"}, {"class", "RateLimited"}};
    } catch (const std::exception&) {
      // Legacy exceptions may contain raw vendor text. Never forward what().
      result = json{{"outcome", "failure"}, {"class", "legacy-untyped"}};
    }
    result["input_gaps"] = std::move(gaps);
    std::cout << result.dump() << '\n';
    return 0;
  } catch (const std::exception&) {
    std::cerr << "legacy fixture driver failed before dispatch\n";
    return 2;
  }
}
