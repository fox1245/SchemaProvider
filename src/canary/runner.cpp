#include "canary/canary.h"
#include "canary/io.h"
#include "core/native.h"
#include "json/json.h"
#include "runtime/testing.h"
#include <algorithm>
#include <memory>

namespace sp::canary {
namespace {
struct WireState {
  Case* result{};
  Totals total;
  bool total_known{};
  const Message* expected{};
  bool negative{}, retained{}, mutated{}, rejected{};
  std::string positive_wire;
};
std::shared_ptr<const json::Document> document(std::string_view text) {
  auto parsed = json::parse(text);
  if (!std::holds_alternative<json::Document>(parsed)) detail::fail();
  return std::make_shared<const json::Document>(std::get<json::Document>(std::move(parsed)));
}
bool retained(std::string_view body, const Message& expected) {
  auto parsed = json::parse(body, {16U << 20, 64});
  auto* doc = std::get_if<json::Document>(&parsed);
  if (!doc) return false;
  auto message = doc->root().get("messages").at(1);
  auto content = message.get("content");
  if (message.get("role").as_string() != "assistant" || !content.is_array() || content.size() != expected.parts.size()) return false;
  for (std::size_t i = 0; i < expected.parts.size(); ++i) {
    auto leaf = content.at(i);
    if (const auto* thinking = std::get_if<Thinking>(&expected.parts[i])) {
      if (leaf.get("type").as_string() != "thinking" || leaf.get("thinking").as_string() != thinking->text) return false;
      auto signature = leaf.get("signature");
      if (thinking->signature ? (!signature.is_string() || signature.as_string() != *thinking->signature) : signature.valid()) return false;
    } else if (const auto* redacted = std::get_if<RedactedThinking>(&expected.parts[i])) {
      if (leaf.get("type").as_string() != "redacted_thinking" || leaf.get("data").as_string() != redacted->data) return false;
    }
  }
  return true;
}
bool mutate(std::string& body, const Message& expected) {
  for (const auto& part : expected.parts) {
    const auto* thinking = std::get_if<Thinking>(&part);
    if (!thinking || !thinking->signature || thinking->signature->empty()) continue;
    const auto& signature = *thinking->signature;
    // Only literal ASCII token bytes: never alter quoting, escapes or UTF-8.
    auto byte = signature.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=_-");
    if (byte == std::string::npos) continue;
    const auto quoted = json::quote(signature);
    if (quoted.size() != signature.size() + 2) continue;
    const std::string needle = "\"signature\":" + quoted;
    auto pos = body.find(needle);
    if (pos == std::string::npos || body.find(needle, pos + 1) != std::string::npos) continue;
    pos += std::string_view("\"signature\":\"").size() + byte;
    body[pos] = body[pos] == 'A' ? 'B' : 'A';
    return true;
  }
  return false;
}
bool signature_rejection(int status, std::string_view body) {
  if (status != 400) return false;
  auto parsed = json::parse(body, {65536, 16});
  auto* doc = std::get_if<json::Document>(&parsed);
  if (!doc) return false;
  auto error = doc->root().get("error");
  if (error.get("type").as_string() != "invalid_request_error") return false;
  auto value = error.get("message");
  if (!value.is_string()) return false;
  std::string message(value.as_string());
  for (auto& c : message) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  // Only reviewed signature-specific diagnostics are evidence. Merely
  // mentioning verification (or a different invalid field) is inconclusive.
  if (message.starts_with("messages.")) {
    const auto colon = message.find(':');
    if (colon == std::string::npos) return false;
    std::string_view path(message.data(), colon);
    path.remove_prefix(9);
    auto index = [&] {
      const auto size = path.find_first_not_of("0123456789");
      if (size == 0 || path.empty()) return false;
      path.remove_prefix(size == std::string_view::npos ? path.size() : size);
      return true;
    };
    if (!index() || !path.starts_with(".content.")) return false;
    path.remove_prefix(9);
    if (!index() || (!path.empty() && path != ".signature" && path != ".thinking")) return false;
    message.erase(0, colon + 1);
  }
  message.erase(std::remove_if(message.begin(), message.end(), [](char c) { return c == '`' || c == '\''; }), message.end());
  const auto first = message.find_first_not_of(" \t");
  if (first == std::string::npos) return false;
  message.erase(0, first);
  while (!message.empty() && (message.back() == '.' || message.back() == ' ')) message.pop_back();
  return message == "invalid signature in thinking block" ||
      message == "signature verification failed" || message == "signature does not match thinking block";
}
class RealAttempt final : public runtime::detail::Attempt {
 public:
  explicit RealAttempt(transport::Operation operation) : operation_(std::move(operation)) {}
  void cancel() noexcept override { operation_.cancel(); }
  void resume() noexcept override { operation_.resume(); }
 private:
  transport::Operation operation_;
};
class GuardedTransport final : public runtime::detail::AttemptTransport {
 public:
  GuardedTransport(const Profile& profile, std::string ledger, std::shared_ptr<WireState> state)
      : profile_(profile), ledger_(std::move(ledger)), state_(std::move(state)), backend_(std::make_unique<transport::Transport>()) {}
  std::unique_ptr<runtime::detail::Attempt> start(transport::HttpRequest request, transport::Callbacks callbacks) override {
    auto& state = *state_;
    auto& result = *state.result;
    if (profile_.provider() == Provider::OpenAI) {
      // An omitted tier inherits a project's premium setting. This campaign
      // uses only standard pricing, independently of that account default.
      if (request.body.empty() || request.body.back() != '}') detail::fail();
      request.body.pop_back();
      request.body.append(",\"service_tier\":\"default\"}");
    }
    if (profile_.provider() == Provider::Gemini) {
      // Standard is Google's documented omitted tier. Explicitly disable
      // thinking on this 2.5 text-only smoke; never enable tools or caching.
      if (request.body.empty() || request.body.back() != '}') detail::fail();
      request.body.pop_back();
      request.body.append(",\"reasoning_effort\":\"none\"}");
    }
    if (state.expected) {
      state.retained = retained(request.body, *state.expected);
      if (!state.retained) { result.reason = Reason::RetentionMismatch; detail::fail(); }
      if (state.negative) {
        if (state.positive_wire != request.body || !mutate(request.body, *state.expected)) {
          result.reason = Reason::MutationUnavailable; detail::fail();
        }
        state.mutated = true;
      } else state.positive_wire = request.body;
    }
    Debit debit;
    try { debit = reserve(profile_, ledger_); }
    catch (...) { state.total_known = false; result.reason = Reason::LedgerFailure; throw; }
    state.total = debit.total;
    state.total_known = true;
    if (debit.result != Reservation::Allowed) {
      switch (debit.result) {
        case Reservation::Calls: result.reason = Reason::CallBudget; break;
        case Reservation::Tokens: result.reason = Reason::TokenBudget; break;
        case Reservation::Cost: result.reason = Reason::CostBudget; break;
        default: break;
      }
      detail::fail();
    }
    // The durable debit precedes even a possibly-failing backend start. No refunds.
    ++result.attempts;
    if (state.negative) {
      struct Response { int status{}; std::string body; bool overflow{}; };
      auto response = std::make_shared<Response>();
      auto head = std::move(callbacks.on_head);
      auto done = std::move(callbacks.on_done);
      auto body = std::move(callbacks.on_body);
      callbacks.on_head = [response, head = std::move(head)](const transport::ResponseHead& h) {
        response->status = h.status; if (head) head(h);
      };
      callbacks.on_body = [response, body = std::move(body)](std::string_view bytes) {
        if (body && !body(bytes)) return false;
        if (response->status == 400 && !response->overflow) {
          if (bytes.size() > 65536 - response->body.size()) { response->overflow = true; response->body.clear(); }
          else response->body.append(bytes);
        }
        return true;
      };
      callbacks.on_done = [response, done = std::move(done), state = state_](const transport::Result& r) {
        try {
          if (r.status == transport::Status::Completed && !response->overflow)
            state->rejected = signature_rejection(response->status, response->body);
        } catch (...) { state->rejected = false; }
        response->body.clear();
        if (done) done(r);
      };
    }
    auto operation = backend_->start(std::move(request), std::move(callbacks));
    result.dispatched = true;
    return std::make_unique<RealAttempt>(std::move(operation));
  }
  void shutdown() noexcept override { backend_.reset(); }
 private:
  Profile profile_;
  std::string ledger_;
  std::shared_ptr<WireState> state_;
  std::unique_ptr<transport::Transport> backend_;
};
descriptor::ValidatedDescriptor descriptor_for(const Profile& profile) {
  const bool chat = profile.provider() != Provider::Anthropic;
  auto path = profile.provider() == Provider::Gemini ? "/v1beta/openai/chat/completions" :
      chat ? "/v1/chat/completions" : "/v1/messages";
  std::string source = "{\"descriptor_version\":1,\"revision\":1,\"id\":\"canary\",\"family\":" +
      json::quote(chat ? "openai.chat" : "anthropic.messages") + ",\"connection\":{\"base_url\":" +
      json::quote(profile.origin()) + ",\"paths\":{\"buffered\":" + json::quote(path) + ",\"streaming\":" + json::quote(path) + '}';
  if (!chat) source += ",\"headers\":{\"anthropic-version\":\"2023-06-01\"}";
  source += "}}";
  auto loaded = descriptor::load(source);
  if (!std::holds_alternative<descriptor::ValidatedDescriptor>(loaded)) detail::fail();
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
const Completion* completion(const runtime::Result& value) { return value ? std::get_if<Completion>(value.get()) : nullptr; }
bool has_text(const Completion& value) {
  for (const auto& message : value.messages) for (const auto& part : message.parts)
    if (const auto* text = std::get_if<Text>(&part); text && !text->value.empty()) return true;
  return false;
}
void usage(Case& target, const Usage& usage, const Bounds& bounds) {
  if (usage.input_total) target.input_tokens = usage.input_total->value;
  if (usage.output_total) target.output_tokens = usage.output_total->value;
  if (usage.input_uncached) target.input_uncached = usage.input_uncached->value;
  if (usage.cache_read) target.cache_read = usage.cache_read->value;
  if (usage.cache_write) target.cache_write = usage.cache_write->value;
  if (usage.reasoning) target.reasoning = usage.reasoning->value;
  target.usage_stage = usage.stage;
  target.usage_quality = usage.quality;
  if (target.input_tokens && target.output_tokens) {
    auto observed = bounds; observed.input_tokens = *target.input_tokens; observed.output_tokens = *target.output_tokens;
    try { target.estimated_micro_usd = reserved_cost(observed); } catch (...) {}
  }
}
Reason blocked_reason(Reason value) {
  switch (value) {
    case Reason::CallBudget: case Reason::TokenBudget: case Reason::CostBudget:
    case Reason::LedgerFailure: case Reason::MutationUnavailable: return value;
    default: return Reason::None;
  }
}
} // namespace
Report run(const Profile& profile, const std::string& ledger_path, std::string api_key) {
  Report report;
  report.provider = profile.provider(); report.test_only = profile.loopback();
  report.replay = profile.provider() == Provider::Anthropic ? Replay::ReplayAcceptanceUnobservable : Replay::NotApplicable;
  report.cases.reserve(profile.provider() == Provider::Gemini ? 2 : profile.provider() == Provider::Anthropic ? 5 : 4);
  for (auto name : {"text_buffered", "text_sse"}) report.cases.push_back(Case{name});
  if (profile.provider() != Provider::Gemini)
    for (auto name : {"tool_first", "tool_positive"}) report.cases.push_back(Case{name});
  if (profile.provider() == Provider::Anthropic) report.cases.push_back(Case{"signature_negative"});
  if (api_key.empty()) {
    for (auto& item : report.cases) item.reason = Reason::MissingCredential;
    return report;
  }
  if (api_key.size() > 4096) detail::fail();
  for (unsigned char c : api_key) if (c < 0x21 || c > 0x7e) detail::fail();
  auto wire = std::make_shared<WireState>();
  runtime::Options options; options.api_key = std::move(api_key); options.retry_tokens = 0;
  auto transport = std::make_shared<GuardedTransport>(profile, ledger_path, wire);
  auto client = runtime::detail::ClientAccess::make(descriptor_for(profile), std::move(options), {}, transport);
  auto call = [&](runtime::Request request, std::size_t index, bool streaming) {
    auto& item = report.cases[index]; wire->result = &item;
    item.reason = Reason::RuntimeFailure;
    runtime::RunOptions opts; opts.streaming = streaming; opts.retry.max_attempts = 1;
    auto value = client.complete(std::move(request), opts);
    report.reserved = wire->total;
    report.reserved_known = wire->total_known;
    if (const auto* good = completion(value)) {
      item.state = State::Passed; item.reason = Reason::None; usage(item, good->usage, profile.bounds());
      if (good->usage.stage != UsageStage::Final || good->usage.quality != UsageQuality::Consistent) {
        item.state = State::Failed; item.reason = Reason::InvalidUsage;
      }
    } else {
      item.state = blocked_reason(item.reason) != Reason::None ? State::NotRun : State::Failed;
      if (value) if (const auto* failure = std::get_if<Failure>(value.get())) {
        item.failure_kind = failure->error.kind;
        item.http_status = failure->error.http_status;
        usage(item, failure->partial.usage, profile.bounds());
      }
    }
    return value;
  };
  if (profile.provider() == Provider::Gemini) {
    chat::Request request; request.model = profile.model(); request.max_output_tokens = profile.bounds().output_tokens;
    request.messages.push_back({Role::User, "Reply with the single word OK."});
    for (std::size_t i = 0; i < 2; ++i) {
      auto outcome = call(request, i, i == 1);
      if (auto* good = completion(outcome); good && !has_text(*good)) {
        report.cases[i].state = State::Failed; report.cases[i].reason = Reason::MissingText;
      }
    }
    return report;
  }
  const auto schema = document(R"({"type":"object","properties":{"value":{"type":"integer"}},"required":["value"],"additionalProperties":false})");
  const std::string text_prompt = "Reply with the single word OK.";
  const std::string tool_prompt = "Call canary_echo exactly once with value 7. After its result, reply OK. This is a synthetic tool protocol check.";
  if (profile.provider() == Provider::OpenAI) {
    chat::Request request; request.model = profile.model(); request.max_output_tokens = profile.bounds().output_tokens;
    request.messages.push_back({Role::User, text_prompt});
    for (std::size_t i = 0; i < 2; ++i) {
      auto outcome = call(request, i, i == 1);
      if (auto* good = completion(outcome); good && !has_text(*good)) { report.cases[i].state = State::Failed; report.cases[i].reason = Reason::MissingText; }
    }
    request.messages[0].text = tool_prompt;
    request.tools.push_back({"canary_echo", "Return an inert synthetic result; no code is executed.", schema});
    auto first = call(request, 2, false);
    const auto* good = completion(first);
    if (!good) return report;
    chat::InputMessage assistant; assistant.role = Role::Assistant;
    bool invalid = false;
    for (const auto& message : good->messages) for (const auto& part : message.parts) {
      if (const auto* text = std::get_if<Text>(&part)) assistant.text += text->value;
      else if (const auto* tool = std::get_if<ToolCall>(&part)) {
        if (tool->kind != ToolCallKind::ClientExecuted || tool->name != "canary_echo") invalid = true;
        assistant.tool_calls.push_back(*tool);
      } else if (std::holds_alternative<InvalidToolCall>(part)) invalid = true;
    }
    if (invalid || assistant.tool_calls.size() != 1) {
      report.cases[2].state = State::Failed; report.cases[2].reason = invalid ? Reason::InvalidTool : Reason::MissingTool; return report;
    }
    auto id = assistant.tool_calls.front().id;
    request.messages.push_back(std::move(assistant));
    chat::InputMessage result; result.role = Role::Tool; result.text = R"({"value":7})"; result.tool_call_id = std::move(id);
    request.messages.push_back(std::move(result));
    auto positive = call(request, 3, false);
    if (auto* done = completion(positive); done && !has_text(*done)) { report.cases[3].state = State::Failed; report.cases[3].reason = Reason::MissingText; }
    return report;
  }
  messages::Request request; request.model = profile.model(); request.max_tokens = profile.bounds().output_tokens;
  request.account_scope = "canary-process";
  request.messages.push_back(Message{{}, Role::User, {Text{text_prompt}}});
  for (std::size_t i = 0; i < 2; ++i) {
    auto outcome = call(request, i, i == 1);
    if (auto* good = completion(outcome); good && !has_text(*good)) { report.cases[i].state = State::Failed; report.cases[i].reason = Reason::MissingText; }
  }
  request.messages[0].parts = {Text{tool_prompt}};
  request.thinking_budget = profile.thinking_budget();
  request.tools.push_back({"canary_echo", "Return an inert synthetic result; no code is executed.", schema, {}, {}});
  auto first = call(request, 2, false);
  const auto* good = completion(first);
  if (!good) return report;
  if (good->messages.size() != 1 || !good->messages.front().native || !good->messages.front().native->complete()) {
    report.cases[2].state = State::Failed; report.cases[2].reason = Reason::MissingTool; return report;
  }
  const auto& original = good->messages.front();
  std::vector<ToolResult> results;
  bool invalid = false, signed_block = false;
  for (const auto& part : original.parts) {
    if (const auto* tool = std::get_if<ToolCall>(&part)) {
      if (tool->kind != ToolCallKind::ClientExecuted || tool->name != "canary_echo") invalid = true;
      results.push_back(ToolResult{tool->id, R"({"value":7})", false});
    } else if (std::holds_alternative<InvalidToolCall>(part)) invalid = true;
    else if (const auto* thinking = std::get_if<Thinking>(&part)) {
      ++report.native_leaves;
      if (thinking->signature && !thinking->signature->empty()) signed_block = true;
    } else if (std::holds_alternative<RedactedThinking>(part)) ++report.native_leaves;
  }
  if (invalid || results.size() != 1) {
    report.cases[2].state = State::Failed; report.cases[2].reason = invalid ? Reason::InvalidTool : Reason::MissingTool; return report;
  }
  request.messages.push_back(original);
  Message tool_result; tool_result.role = Role::User;
  tool_result.parts.push_back(std::move(results.front()));
  request.messages.push_back(std::move(tool_result));
  wire->expected = &original;
  auto positive = call(request, 3, false);
  report.positive_retained = wire->retained && report.cases[3].dispatched;
  if (auto* done = completion(positive); done && !has_text(*done)) { report.cases[3].state = State::Failed; report.cases[3].reason = Reason::MissingText; }
  if (!signed_block) { report.cases[4].reason = Reason::MissingSignature; return report; }
  // A negative control uses the SAME native-admitted replay, not the next turn.
  if (!report.cases[3].dispatched || !report.positive_retained) return report;
  wire->negative = true;
  auto negative = call(request, 4, false);
  report.signature_mutated = wire->mutated && report.cases[4].dispatched;
  auto& control = report.cases[4];
  if (control.dispatched) {
    if (wire->rejected && report.signature_mutated) {
      control.state = State::Passed; control.reason = Reason::SignatureRejected;
      if (report.cases[3].state == State::Passed) report.replay = Replay::ReplayVerified;
    } else if (completion(negative)) {
      control.state = State::Failed; control.reason = Reason::NegativeAccepted;
    } else { control.state = State::Failed; control.reason = Reason::NegativeInconclusive; }
  }
  return report;
}
} // namespace sp::canary
