// Example C ABI over SchemaProvider (see sp_capi.h). Chat Completions only, to stay readable:
// another family needs its own request builder in `build_request`.
#define SP_CAPI_BUILD
#include "sp_capi.h"

#include <cppdotenv/dotenv.hpp>
#include <runtime/client.h>
#include <json/json.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

struct sp_capi_client {
  std::shared_ptr<sp::runtime::Client> client;
};

struct sp_capi_conversation {
  std::shared_ptr<sp::runtime::Client> owner;
  std::string model;
  std::uint32_t max_output_tokens = 0;
  std::vector<sp::chat::ToolDefinition> tools;
  // Typed history, including the SDK's sealed assistant messages. It never leaves this library.
  std::vector<sp::Message> history;
  std::mutex mutex;
};

namespace {
char* duplicate(std::string_view text) noexcept {
  auto* copy = static_cast<char*>(std::malloc(text.size() + 1));
  if (copy) {
    std::memcpy(copy, text.data(), text.size());
    copy[text.size()] = '\0';
  }
  return copy;
}
void report(char** error, std::string_view message) noexcept {
  if (error) *error = duplicate(message);
}
std::string failure_json(std::string_view kind, std::string_view message) {
  return "{\"ok\":false,\"error\":{\"kind\":" + sp::json::quote(kind) + ",\"message\":" + sp::json::quote(message) + "}}";
}
char* failure_result(std::string_view kind, std::string_view message) noexcept {
  try { return duplicate(failure_json(kind, message)); }
  catch (...) { return nullptr; }
}
struct HistoryTurn {
  std::vector<sp::Message>& history;
  const std::size_t original_size;
  bool committed = false;
  ~HistoryTurn() { if (!committed) history.resize(original_size); }
};
const char* kind_name(sp::ErrorKind kind) {
  switch (kind) {
    case sp::ErrorKind::InvalidConfig: return "InvalidConfig";
    case sp::ErrorKind::InvalidRequest: return "InvalidRequest";
    case sp::ErrorKind::Unsupported: return "Unsupported";
    case sp::ErrorKind::Transport: return "Transport";
    case sp::ErrorKind::ProtocolCorrupt: return "ProtocolCorrupt";
    case sp::ErrorKind::Truncated: return "Truncated";
    case sp::ErrorKind::RemoteFailure: return "RemoteFailure";
    case sp::ErrorKind::Cancelled: return "Cancelled";
    case sp::ErrorKind::DeadlineExceeded: return "DeadlineExceeded";
    case sp::ErrorKind::ResourceLimit: return "ResourceLimit";
    case sp::ErrorKind::Misuse: return "Misuse";
    case sp::ErrorKind::ReplayIneligible: return "ReplayIneligible";
    case sp::ErrorKind::Authentication: return "Authentication";
    case sp::ErrorKind::Permission: return "Permission";
    case sp::ErrorKind::NotFound: return "NotFound";
    case sp::ErrorKind::RateLimited: return "RateLimited";
    case sp::ErrorKind::QuotaExhausted: return "QuotaExhausted";
    case sp::ErrorKind::LimitUnknown: return "LimitUnknown";
    case sp::ErrorKind::Overloaded: return "Overloaded";
  }
  return "Unknown";
}
const char* stop_name(sp::StopKind kind) {
  switch (kind) {
    case sp::StopKind::EndTurn: return "EndTurn";
    case sp::StopKind::ToolUse: return "ToolUse";
    case sp::StopKind::MaxTokens: return "MaxTokens";
    case sp::StopKind::StopSequence: return "StopSequence";
    case sp::StopKind::ContentFilter: return "ContentFilter";
    case sp::StopKind::Refusal: return "Refusal";
    case sp::StopKind::PauseTurn: return "PauseTurn";
    case sp::StopKind::ContextLimit: return "ContextLimit";
    case sp::StopKind::MalformedCall: return "MalformedCall";
    case sp::StopKind::Unknown: return "Unknown";
  }
  return "Unknown";
}

sp::json::Document parse_or_throw(std::string_view text) {
  auto parsed = sp::json::parse(text);
  if (auto* document = std::get_if<sp::json::Document>(&parsed)) return std::move(*document);
  throw std::invalid_argument("invalid JSON");
}

// The turn the host supplies: new user text, or results for the tool calls of the last answer.
std::vector<sp::Message> parse_turn(std::string_view turn_json) {
  const auto document = parse_or_throw(turn_json);
  const auto root = document.root();
  std::vector<sp::Message> turn;
  if (const auto user = root.get("user"); user.is_string()) {
    turn.push_back({"", sp::Role::User, {sp::Text{std::string(user.as_string())}}});
  } else if (const auto results = root.get("tool_results"); results.is_array()) {
    for (auto entry : results.elements()) {
      const auto id = entry.get("call_id"), content = entry.get("content");
      if (!id.is_string() || !content.is_string()) throw std::invalid_argument("tool result needs call_id and content text");
      turn.push_back({"", sp::Role::Tool, {sp::ToolResult{std::string(id.as_string()), std::string(content.as_string())}}});
    }
  }
  if (turn.empty()) throw std::invalid_argument("turn needs \"user\" text or \"tool_results\"");
  return turn;
}

std::string outcome_json(const sp::Completion& completion) {
  std::string text, calls;
  for (const auto& message : completion.messages) {
    for (const auto& part : message.parts) {
      if (const auto* value = std::get_if<sp::Text>(&part)) text += value->value;
      if (const auto* refusal = std::get_if<sp::Refusal>(&part)) text += refusal->text;
      if (const auto* call = std::get_if<sp::ToolCall>(&part)) {
        if (!calls.empty()) calls += ",";
        calls += "{\"id\":" + sp::json::quote(call->id) + ",\"name\":" + sp::json::quote(call->name) +
                 ",\"arguments\":" + call->input->root().dump() + "}";
      }
    }
  }
  auto count = [](const std::optional<sp::Count>& value) { return value ? std::to_string(value->value) : std::string("null"); };
  return "{\"ok\":true,\"text\":" + sp::json::quote(text) + ",\"tool_calls\":[" + calls + "],\"stop\":" +
         sp::json::quote(stop_name(completion.stop.kind)) + ",\"usage\":{\"input\":" + count(completion.usage.input_total) +
         ",\"output\":" + count(completion.usage.output_total) + "}}";
}
}  // namespace

extern "C" {

SP_CAPI_API uint32_t sp_capi_interface_revision(void) { return static_cast<uint32_t>(sp::EXPECTED_INTERFACE_REVISION); }

SP_CAPI_API void sp_capi_string_free(char* text) { std::free(text); }

SP_CAPI_API sp_capi_client* sp_capi_client_create(const char* descriptor_json, const char* api_key,
                                                  const char* dotenv_path, const char* api_key_name, char** error) {
  if (error) *error = nullptr;
  try {
    sp::runtime::require_interface_contract(sp::EXPECTED_INTERFACE_REVISION, sp::capability::RequiredProvider);
    if (!descriptor_json) throw std::invalid_argument("descriptor_json is required");
    auto loaded = sp::descriptor::load(descriptor_json);
    if (const auto* config = std::get_if<sp::descriptor::ConfigError>(&loaded))
      throw std::invalid_argument("descriptor rejected at " + config->pointer + ": " + config->expected);
    auto descriptor = std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded));
    if (descriptor.family() != "openai.chat") throw std::invalid_argument("this example supports family openai.chat only");
    sp::runtime::Options options;
    options.retry_tokens = 0;
    options.retry_tokens_per_second = 0;
    if (api_key) options.api_key = api_key;
    if (dotenv_path && api_key_name) {
      // The credential is read here and never printed or returned.
      auto values = cppdotenv::dotenv_values(dotenv_path, false);
      const auto found = values.find(api_key_name);
      if (found == values.end() || found->second.empty()) throw std::invalid_argument(std::string("key not found in .env: ") + api_key_name);
      options.api_key = std::move(found->second);
    }
    auto handle = std::make_unique<sp_capi_client>();
    handle->client = std::make_shared<sp::runtime::Client>(std::move(descriptor), std::move(options));
    return handle.release();
  } catch (const sp::descriptor::ConfigError&) {
    report(error, "invalid client options");
  } catch (const std::invalid_argument& failure) {
    report(error, failure.what());
  } catch (const std::exception&) {
    report(error, "client creation failed");
  } catch (...) {
    report(error, "unknown failure creating client");
  }
  return nullptr;
}

SP_CAPI_API void sp_capi_client_destroy(sp_capi_client* client) { delete client; }

SP_CAPI_API sp_capi_conversation* sp_capi_conversation_create(sp_capi_client* client, const char* model, const char* system,
                                                              uint32_t max_output_tokens, const char* tools_json, char** error) {
  if (error) *error = nullptr;
  try {
    if (!client || !model || !*model) throw std::invalid_argument("client and model are required");
    auto conversation = std::make_unique<sp_capi_conversation>();
    conversation->owner = client->client;
    conversation->model = model;
    conversation->max_output_tokens = max_output_tokens;
    if (system && *system) conversation->history.push_back({"", sp::Role::System, {sp::Text{system}}});
    if (tools_json && *tools_json) {
      const auto document = parse_or_throw(tools_json);
      if (!document.root().is_array()) throw std::invalid_argument("tools_json must be an array");
      for (auto tool : document.root().elements()) {
        const auto name = tool.get("name"), description = tool.get("description"), parameters = tool.get("parameters");
        if (!name.is_string() || !parameters.is_object()) throw std::invalid_argument("each tool needs a name and an object of parameters");
        conversation->tools.push_back({std::string(name.as_string()),
                                       description.is_string() ? std::string(description.as_string()) : std::string(),
                                       std::make_shared<const sp::json::Document>(parse_or_throw(parameters.dump()))});
      }
    }
    return conversation.release();
  } catch (const std::invalid_argument& failure) {
    report(error, failure.what());
  } catch (...) {
    report(error, "conversation creation failed");
  }
  return nullptr;
}

SP_CAPI_API void sp_capi_conversation_destroy(sp_capi_conversation* conversation) { delete conversation; }

SP_CAPI_API char* sp_capi_conversation_send(sp_capi_conversation* conversation, const char* turn_json, int streaming,
                                            uint32_t timeout_ms) {
  try {
    if (!conversation || !turn_json) return failure_result("InvalidRequest", "conversation and turn are required");
    // One send at a time per conversation keeps the typed history consistent.
    std::lock_guard lock(conversation->mutex);
    HistoryTurn turn{conversation->history, conversation->history.size()};
    auto& messages = conversation->history;
    for (auto& message : parse_turn(turn_json)) messages.push_back(std::move(message));

    sp::chat::Request request;
    request.model = conversation->model;
    if (conversation->max_output_tokens) request.max_output_tokens = conversation->max_output_tokens;
    request.tools = conversation->tools;
    request.canonical_messages = messages;
    sp::runtime::RunOptions run;
    run.streaming = streaming != 0;
    run.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms ? timeout_ms : 30000);
    sp::runtime::RetryPolicy retry;
    retry.enabled = false;
    retry.max_attempts = 1;
    run.retry = retry;

    const auto result = conversation->owner->complete(std::move(request), run);
    if (const auto* failure = std::get_if<sp::Failure>(result.get())) {
      const auto& error = failure->error;
      // The attempt evidence is what a host needs to decide whether a retry could duplicate work.
      return duplicate("{\"ok\":false,\"error\":{\"kind\":" + sp::json::quote(kind_name(error.kind)) +
                       ",\"message\":" + sp::json::quote(error.safe_message) +
                       ",\"http_status\":" + std::to_string(error.http_status) +
                       ",\"request_may_have_left\":" + (error.attempt.request_may_have_left ? "true" : "false") +
                       ",\"response_head_seen\":" + (error.attempt.response_head_seen ? "true" : "false") + "}}");
    }
    const auto& completion = std::get<sp::Completion>(*result);
    for (const auto& message : completion.messages) for (const auto& part : message.parts) {
      const auto* call = std::get_if<sp::ToolCall>(&part);
      if (std::holds_alternative<sp::InvalidToolCall>(part) || (call && !call->input))
        return failure_result("ProtocolCorrupt", "model returned invalid tool arguments");
    }
    // Keep the assistant messages exactly as the SDK sealed them, so a tool loop can continue.
    for (const auto& message : completion.messages) messages.push_back(message);
    auto* output = duplicate(outcome_json(completion));
    if (output) turn.committed = true;
    return output;
  } catch (const std::invalid_argument& failure) {
    return failure_result("InvalidRequest", failure.what());
  } catch (...) {
    return failure_result("Unknown", "send failed before returning an outcome");
  }
}

}  // extern "C"
