#include <cppdotenv/dotenv.hpp>
#include <json/json.h>
#include <runtime/client.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
// Only fixed, locally authored diagnostics are allowed through the exception boundary.
class ExampleError final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

sp::json::Document parse_json(std::string_view text) {
  auto parsed = sp::json::parse(text);
  if (auto* document = std::get_if<sp::json::Document>(&parsed)) return std::move(*document);
  throw ExampleError("example JSON could not be parsed");
}

sp::descriptor::ValidatedDescriptor chat_descriptor(std::string_view base_url) {
  const auto quoted_url = sp::json::quote(base_url);
  if (quoted_url.empty()) throw ExampleError("base URL is not valid UTF-8");
  auto loaded = sp::descriptor::load(
      "{\"descriptor_version\":1,\"revision\":1,\"id\":\"standalone-chat\","
      "\"family\":\"openai.chat\",\"connection\":{\"base_url\":" + quoted_url +
      ",\"paths\":{\"buffered\":\"/v1/chat/completions\","
      "\"streaming\":\"/v1/chat/completions\"}}}");
  if (std::holds_alternative<sp::descriptor::ConfigError>(loaded))
    throw ExampleError("chat descriptor rejected; check the base URL");
  return std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded));
}

std::int64_t integer_argument(sp::json::Value value) {
  if (value.is_int()) return value.as_int();
  if (value.is_uint() && value.as_uint() <=
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return static_cast<std::int64_t>(value.as_uint());
  throw ExampleError("add requires two signed 64-bit integers");
}

std::string execute_add(const sp::ToolCall& call) {
  if (call.name != "add") throw ExampleError("model requested an unknown tool");
  if (call.id.empty() || !call.input) throw ExampleError("tool call is incomplete");
  const auto arguments = call.input->root();
  if (!arguments.is_object() || arguments.size() != 2)
    throw ExampleError("add requires exactly the arguments a and b");
  const auto a = integer_argument(arguments.get("a"));
  const auto b = integer_argument(arguments.get("b"));
  if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
      (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b))
    throw ExampleError("add result exceeds the signed 64-bit integer range");
  return std::to_string(a + b);
}

int chat(std::string_view base_url, const char* model, const char* dotenv_path,
         const char* key_name, const char* question) {
  sp::runtime::require_interface_contract(sp::EXPECTED_INTERFACE_REVISION,
                                         sp::capability::RequiredProvider);
  sp::runtime::Options options;
  {
    // Read only this file, without interpolation or mutation of the process environment.
    // The temporary map dies here; the credential moves into the SDK-owned Client.
    auto values = cppdotenv::dotenv_values(dotenv_path, false);
    const auto found = values.find(key_name);
    if (found == values.end() || found->second.empty())
      throw ExampleError("named key is missing or empty in the dotenv file (or file is unreadable)");
    options.api_key = std::move(found->second);
  }
  sp::runtime::Client client(chat_descriptor(base_url), std::move(options));
  const std::vector<sp::chat::ToolDefinition> tools{{
      "add", "Add two signed 64-bit integers.",
      std::make_shared<const sp::json::Document>(parse_json(
          R"({"type":"object","properties":{"a":{"type":"integer"},"b":{"type":"integer"}},"required":["a","b"],"additionalProperties":false})"))}};
  std::vector<sp::Message> history{{"", sp::Role::User, {sp::Text{question}}}};
  constexpr unsigned max_tool_turns = 4;

  for (unsigned tool_turns = 0; tool_turns <= max_tool_turns; ++tool_turns) {
    sp::chat::Request request;
    request.model = model;
    request.max_output_tokens = 64;
    request.tools = tools;
    request.canonical_messages = history;
    sp::runtime::RunOptions run;
    run.streaming = false;
    run.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    sp::runtime::RetryPolicy retry;
    retry.enabled = false;
    retry.max_attempts = 1;
    run.retry = retry;
    const auto outcome = client.complete(std::move(request), run);
    if (const auto* failure = std::get_if<sp::Failure>(outcome.get())) {
      // Do not print provider bodies, URLs, credentials, or arbitrary exception messages.
      std::cerr << "request failed: SDK error kind=" << static_cast<int>(failure->error.kind)
                << ", HTTP status=" << failure->error.http_status << " (no retry)\n";
      return 1;
    }
    const auto& completion = std::get<sp::Completion>(*outcome);
    std::vector<sp::Message> results;
    std::string answer;
    for (const auto& message : completion.messages) {
      for (const auto& part : message.parts) {
        if (const auto* text = std::get_if<sp::Text>(&part)) answer += text->value;
        if (const auto* refusal = std::get_if<sp::Refusal>(&part)) answer += refusal->text;
        if (const auto* call = std::get_if<sp::ToolCall>(&part)) {
          if (tool_turns == max_tool_turns) throw ExampleError("tool turn limit reached");
          results.push_back({"", sp::Role::Tool, {sp::ToolResult{call->id, execute_add(*call)}}});
        }
      }
      // Copy the sealed SDK message intact, including authenticated native replay state.
      // Reconstructing assistant messages from text/tool JSON would lose that state.
      history.push_back(message);
    }
    if (results.empty()) {
      if ((completion.stop.kind != sp::StopKind::EndTurn && completion.stop.kind != sp::StopKind::Refusal) || answer.empty())
        throw ExampleError("model did not produce a complete final answer");
      std::cout << answer << '\n';
      return 0;
    }
    if (completion.stop.kind != sp::StopKind::ToolUse)
      throw ExampleError("model tool calls did not finish with a tool-use stop");
    for (auto& result : results) history.push_back(std::move(result));
  }
  throw ExampleError("tool turn limit reached");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 5 && argc != 6) {
    std::cerr << "Usage: sp_chat_standalone <base-url> <model> <dotenv-path> <key-name> [question]\n";
    return 2;
  }
  for (int index = 1; index < argc; ++index) {
    if (!*argv[index]) {
      std::cerr << "error: positional arguments must not be empty\n";
      return 2;
    }
  }
  try {
    return chat(argv[1], argv[2], argv[3], argv[4],
                argc == 6 ? argv[5] : "What is 2 + 3? Use the add tool.");
  } catch (const ExampleError& error) {
    std::cerr << "error: " << error.what() << '\n';
  } catch (const sp::descriptor::ConfigError&) {
    std::cerr << "error: SDK client configuration rejected\n";
  } catch (const sp::runtime::InterfaceContractError&) {
    std::cerr << "error: incompatible SDK interface\n";
  } catch (const std::exception&) {
    std::cerr << "error: unexpected client or tool-loop failure; diagnostics withheld\n";
  } catch (...) {
    std::cerr << "error: unknown client or tool-loop failure; diagnostics withheld\n";
  }
  return 1;
}
