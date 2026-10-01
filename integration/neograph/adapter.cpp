#include "adapter.h"
#include "json/json.h"
#include <neograph/graph/cancel.h>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>

namespace sp::neograph_integration {
namespace {
[[noreturn]] void unsupported(const char* gap) { throw BoundaryError({gap}); }
Role role(const std::string& value) {
  if (value == "user") return Role::User;
  if (value == "assistant") return Role::Assistant;
  if (value == "system") return Role::System;
  if (value == "developer") return Role::Developer;
  if (value == "tool") return Role::Tool;
  unsupported("input-role");
}
std::shared_ptr<const json::Document> document(std::string_view value) {
  auto parsed = json::parse(value);
  if (!std::holds_alternative<json::Document>(parsed)) unsupported("input-json");
  auto result = std::make_shared<const json::Document>(std::get<json::Document>(std::move(parsed)));
  if (!result->root().is_object()) unsupported("input-json-object");
  return result;
}
runtime::Request translate(const neograph::CompletionParams& p, Family family) {
  if (!p.prompt.empty()) unsupported("prompt-envelope");
  if (!p.extra_fields.is_null() && !p.extra_fields.empty()) unsupported("extra-fields");
  if (p.max_tokens != -1 && p.max_tokens <= 0) unsupported("output-cap");
  if (p.timeout_seconds != -1 && p.timeout_seconds <= 0) unsupported("timeout");
  chat::Request chat;
  messages::Request messages;
  chat.model = messages.model = p.model;
  if (!std::isfinite(p.temperature)) unsupported("sampling-temperature");
  if (p.temperature >= 0) chat.temperature = messages.temperature = p.temperature;
  if (p.max_tokens > 0) {
    chat.max_output_tokens = static_cast<std::uint64_t>(p.max_tokens);
    messages.max_tokens = static_cast<std::uint64_t>(p.max_tokens);
  }
  for (const auto& m : p.messages) {
    if (!m.image_urls.empty()) unsupported("image-input");
    if (!m.reasoning.empty() || !m.reasoning_details.empty()) unsupported("unsealed-native-input");
    if (!m.tool_name.empty()) unsupported("tool-name-input");
    if (!m.tool_status.empty() || m.tool_retryable || m.tool_effect_uncertain) unsupported("tool-execution-status");
    const auto r = role(m.role);
    if (r != Role::Assistant && !m.tool_calls.empty()) unsupported("tool-call-role");
    if (r != Role::Tool && !m.tool_call_id.empty()) unsupported("tool-result-role");
    if (family == Family::Chat) {
      chat::InputMessage input{r, m.content};
      input.tool_call_id = m.tool_call_id;
      for (const auto& t : m.tool_calls)
        input.tool_calls.push_back({t.id, t.name, ToolCallKind::ClientExecuted, document(t.arguments)});
      chat.messages.push_back(std::move(input));
    } else {
      if (r == Role::Developer) unsupported("messages-developer-role");
      if (!m.tool_calls.empty()) unsupported("unsealed-tool-history");
      if (r == Role::System) {
        if (!messages.messages.empty() || !messages.system.empty()) unsupported("system-message-order");
        messages.system = m.content;
        continue;
      }
      Message input;
      input.role = r == Role::Tool ? Role::User : r;
      if (r == Role::Tool) input.parts.emplace_back(ToolResult{m.tool_call_id, m.content});
      else {
        if (!m.content.empty()) input.parts.emplace_back(Text{m.content});
      }
      messages.messages.push_back(std::move(input));
    }
  }
  for (const auto& t : p.tools) {
    auto schema = document(t.parameters.dump());
    if (family == Family::Chat) chat.tools.push_back({t.name, t.description, std::move(schema)});
    else messages.tools.push_back({t.name, t.description, std::move(schema), {}, {}});
  }
  if (family == Family::Chat) return chat;
  return messages;
}
struct Mailbox {
  std::mutex mutex;
  std::vector<std::string> text;
  std::size_t bytes = 0;
  bool overflow = false;
  runtime::Result result;
};
const char* stop_name(StopKind kind) {
  switch (kind) {
    case StopKind::EndTurn: return "end_turn";
    case StopKind::ToolUse: return "tool_use";
    case StopKind::MaxTokens: return "max_tokens";
    case StopKind::StopSequence: return "stop_sequence";
    case StopKind::ContentFilter: return "content_filter";
    case StopKind::Refusal: return "refusal";
    default: return nullptr;
  }
}
}
BoundaryError::BoundaryError(std::vector<std::string> gaps, runtime::Result evidence)
    : std::runtime_error("SchemaProvider: legacy boundary is not lossless"),
      gaps_(std::move(gaps)), evidence_(std::move(evidence)) {}
RuntimeProvider::RuntimeProvider(std::shared_ptr<runtime::Client> client, Family family)
    : client_(std::move(client)), family_(family) {
  if (!client_) throw std::invalid_argument("SchemaProvider: missing runtime client");
}
asio::awaitable<runtime::Result> RuntimeProvider::invoke_owned(neograph::CompletionRequest request) {
  // Copy the shared owner before suspension. Worker callbacks never retain this,
  // the caller executor, borrowed event data, or the caller's callback.
  auto client = client_;
  auto input = translate(request.params(), family_);
  auto mailbox = std::make_shared<Mailbox>();
  std::stop_source stop;
  runtime::RunOptions options;
  options.streaming = request.streaming();
  options.retry.enabled = false;
  options.stop_token = stop.get_token();
  if (request.params().timeout_seconds > 0)
    options.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(request.params().timeout_seconds);
  const auto token = request.params().cancel_token;
  if (token && token->is_cancelled()) stop.request_stop();
  runtime::Callbacks callbacks;
  if (request.on_chunk()) callbacks.on_event = [mailbox, stop](const Event& event) mutable {
    const auto* delta = std::get_if<PartDelta>(&event);
    if (!delta || delta->payload.kind != PartKind::Text) return;
    bool overflow = false;
    {
      std::lock_guard lock(mailbox->mutex);
      if (mailbox->overflow) return;
      if (mailbox->text.size() == 1024 || delta->payload.bytes.size() > (256U << 10) - mailbox->bytes) {
        mailbox->overflow = overflow = true;
      } else {
        try {
          mailbox->text.emplace_back(delta->payload.bytes);
          mailbox->bytes += delta->payload.bytes.size();
        } catch (...) {
          mailbox->overflow = overflow = true;
        }
      }
    }
    if (overflow) stop.request_stop();
  };
  callbacks.on_outcome = [mailbox](runtime::Result result) {
    std::lock_guard lock(mailbox->mutex);
    mailbox->result = std::move(result);
  };
  runtime::Operation operation;
  try { operation = client->start(std::move(input), options, std::move(callbacks)); }
  catch (const runtime::AdmissionError& error) { co_return error.outcome(); }
  asio::steady_timer timer(co_await asio::this_coro::executor);
  std::vector<std::string> pending;
  for (;;) {
    if (token && token->is_cancelled()) stop.request_stop();
    runtime::Result result;
    bool overflow;
    {
      std::lock_guard lock(mailbox->mutex);
      pending.swap(mailbox->text);
      mailbox->bytes = 0;
      result = mailbox->result;
      overflow = mailbox->overflow;
    }
    // Throwing observers unwind Operation, cancelling without blocking the
    // runtime worker. Only owned mailbox memory remains reachable there.
    for (const auto& text : pending) request.on_chunk()(text);
    pending.clear();
    if (result) {
      if (overflow) throw BoundaryError({"observer-backpressure"}, std::move(result));
      co_return result;
    }
    // Polling deliberately avoids posting to an executor after its io_context
    // has been destroyed. It uses no request worker; cancellation/abandonment
    // unwinds the RAII Operation. Poll interval is 1ms while the caller runs.
    timer.expires_after(std::chrono::milliseconds(1));
    co_await timer.async_wait(asio::use_awaitable);
  }
}
neograph::ChatCompletion RuntimeProvider::project(runtime::Result evidence) {
  if (!evidence) throw BoundaryError({"missing-outcome"});
  if (std::holds_alternative<Failure>(*evidence)) throw BoundaryError({"owned-runtime-failure"}, evidence);
  const auto& c = std::get<Completion>(*evidence);
  std::vector<std::string> gaps{"usage-provenance"};
  if (c.messages.size() != 1) gaps.emplace_back("message-cardinality");
  for (const auto& message : c.messages) {
    if (message.role != Role::Assistant) gaps.emplace_back("output-role");
    if (message.native) gaps.emplace_back("sealed-native-provenance");
    if (!message.id.empty()) gaps.emplace_back("message-identity");
    bool tool_seen = false;
    unsigned text_parts = 0;
    for (const auto& part : message.parts) {
      if (std::holds_alternative<Text>(part)) {
        if (tool_seen || ++text_parts > 1) gaps.emplace_back("ordered-content-parts");
      } else if (const auto* tool = std::get_if<ToolCall>(&part)) {
        tool_seen = true;
        if (tool->kind != ToolCallKind::ClientExecuted || tool->wire_metadata ||
            (!tool->wire_type.empty() && tool->wire_type != "function" && tool->wire_type != "tool_use"))
          gaps.emplace_back("non-client-tool-authority");
        if (!tool->input) gaps.emplace_back("missing-tool-input");
      } else if (std::holds_alternative<InvalidToolCall>(part)) gaps.emplace_back("invalid-tool-call");
      else gaps.emplace_back("non-text-native-part");
    }
  }
  if (!stop_name(c.stop.kind)) gaps.emplace_back("richer-stop-kind");
  if (!c.stop.raw.empty() || c.stop.sequence || c.stop.details) gaps.emplace_back("stop-evidence");
  for (const auto* value : {&c.usage.input_total, &c.usage.output_total, &c.usage.total,
                            &c.usage.cache_read, &c.usage.reasoning}) {
    if (!*value) gaps.emplace_back("nullable-usage");
    else if ((*value)->value > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
      gaps.emplace_back("usage-overflow");
  }
  // There is no lossless legacy success, even with all five integer counters:
  // stage, quality and reported/derived provenance are absent from that ABI.
  // Fail with owned evidence rather than manufacture a projection.
  std::sort(gaps.begin(), gaps.end());
  gaps.erase(std::unique(gaps.begin(), gaps.end()), gaps.end());
  throw BoundaryError(std::move(gaps), std::move(evidence));
}
asio::awaitable<neograph::ChatCompletion> RuntimeProvider::do_invoke(neograph::CompletionRequest request) {
  co_return project(co_await invoke_owned(std::move(request)));
}
} // namespace sp::neograph_integration
