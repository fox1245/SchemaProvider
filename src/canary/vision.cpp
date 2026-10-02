#include "canary/canary.h"
#include "canary/io.h"
#include "core/native.h"
#include "json/json.h"
#include "runtime/testing.h"
#include "transport/sse_framer.h"
#include "../../tests/support/vision_fixture.h"
#include <algorithm>
#include <memory>
#include <type_traits>

namespace sp::canary {
namespace {
constexpr std::string_view tool_prompt = "Call vision_score exactly once with value equal to weighted. After tool result return the same three-field JSON.";
struct Wire {
  Case* result{};
  const Message* expected{};
  const vision_test::Scene* scene{};
  Totals lane{}, campaign{};
  bool totals_known = false, negative = false, omission = false, retained = false, changed = false, rejected = false;
  std::string positive;
};
std::shared_ptr<const json::Document> document(std::string_view value) {
  auto parsed = json::parse(value, {16U << 20, 64});
  auto* result = std::get_if<json::Document>(&parsed);
  if (!result) detail::fail();
  return std::make_shared<const json::Document>(std::move(*result));
}
json::Value image_leaf(json::Value root, Provider provider) {
  if (provider == Provider::VisionMessages || provider == Provider::VisionChat || provider == Provider::VisionResponses) {
    auto content = root.get(provider == Provider::VisionResponses ? "input" : "messages").at(0).get("content");
    for (auto part : content.elements()) if (part.get("type").as_string() == "image" || part.get("type").as_string() == "input_image" || part.get("type").as_string() == "image_url") return part;
  } else if (provider == Provider::VisionGemini) {
    for (auto part : root.get("contents").at(0).get("parts").elements()) if (part.get("inlineData").valid()) return part;
  } else {
    for (auto part : root.get("input").at(0).get("content").elements()) if (part.get("type").as_string() == "image") return part;
  }
  return {};
}
bool image_exact(json::Value root, Provider provider, const vision_test::Scene& scene) {
  auto part = image_leaf(root, provider);
  const auto expected = std::string_view(*scene.base64);
  if (provider == Provider::VisionMessages) return part.get("source").get("media_type").as_string() == "image/png" && part.get("source").get("data").as_string() == expected;
  if (provider == Provider::VisionGemini) return part.get("inlineData").get("mimeType").as_string() == "image/png" && part.get("inlineData").get("data").as_string() == expected;
  if (provider == Provider::VisionInteractions) return part.get("mime_type").as_string() == "image/png" && part.get("data").as_string() == expected;
  auto uri = provider == Provider::VisionChat ? part.get("image_url").get("url").as_string() : part.get("image_url").as_string();
  constexpr std::string_view prefix = "data:image/png;base64,";
  return uri.starts_with(prefix) && uri.substr(prefix.size()) == expected;
}
json::Value captured_array(json::Value root, Provider provider) {
  if (provider == Provider::VisionResponses || provider == Provider::VisionInteractions) return root.get("input");
  if (provider == Provider::VisionGemini) return root.get("contents").at(1).get("parts");
  return root.get("messages").at(1).get("content");
}
bool native_retained(json::Value root, Provider provider, const Message& expected) {
  const auto input = captured_array(root, provider);
  if (!input.is_array()) return false;
  if (expected.wire_output) {
    const auto output = expected.wire_output->root();
    const size_t offset = provider == Provider::VisionResponses || provider == Provider::VisionInteractions ? 1 : 0;
    if (!output.is_array() || input.size() < output.size() + offset) return false;
    for (size_t i = 0; i < output.size(); ++i) if (!json::equal(input.at(i + offset), output.at(i))) return false;
    return true;
  }
  if (provider != Provider::VisionMessages || input.size() != expected.parts.size()) return false;
  for (size_t i = 0; i < expected.parts.size(); ++i) {
    const auto leaf = input.at(i);
    if (const auto* thinking = std::get_if<Thinking>(&expected.parts[i])) {
      if (leaf.get("type").as_string() != "thinking" || leaf.get("thinking").as_string() != thinking->text) return false;
      const auto signature = leaf.get("signature");
      if (thinking->signature ? !signature.is_string() || signature.as_string() != *thinking->signature : signature.valid()) return false;
    } else if (const auto* redacted = std::get_if<RedactedThinking>(&expected.parts[i])) {
      if (leaf.get("type").as_string() != "redacted_thinking" || leaf.get("data").as_string() != redacted->data) return false;
    } else if (const auto* tool = std::get_if<ToolCall>(&expected.parts[i])) {
      if (leaf.get("id").as_string() != tool->id || leaf.get("name").as_string() != tool->name || !tool->input || !json::equal(leaf.get("input"), tool->input->root())) return false;
    } else if (const auto* text = std::get_if<Text>(&expected.parts[i])) {
      if (leaf.get("text").as_string() != text->value) return false;
    } else return false;
  }
  return true;
}
bool mutate_signature(std::string& body, json::Value root, Provider provider) {
  const auto input = captured_array(root, provider);
  const std::string_view key = provider == Provider::VisionResponses ? "encrypted_content" : provider == Provider::VisionGemini ? "thoughtSignature" : "signature";
  json::Value selected;
  size_t selected_index = 0;
  // Prefer the signature bound to the pending call. Thought and call carriers
  // may legitimately contain the same opaque value; change exactly one owner.
  for (bool associated_only : {true, false}) {
    size_t index = 0;
    for (auto leaf : input.elements()) {
      const bool associated = provider == Provider::VisionInteractions ? leaf.get("type").as_string() == "function_call" :
          provider == Provider::VisionGemini && leaf.get("functionCall").is_object();
      const auto signature = leaf.get(key);
      if ((!associated_only || associated) && signature.is_string() && !signature.as_string().empty()) {
        const auto value = signature.as_string();
        const auto byte = value.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=_-");
        const bool escaped = std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 0x20 || c == '"' || c == '\\'; });
        if (byte != std::string_view::npos && !escaped) { selected = leaf; selected_index = index; break; }
      }
      ++index;
    }
    if (selected.valid()) break;
  }
  if (!selected.valid()) return false;
  const auto value = selected.get(key).as_string();
  size_t ordinal = 0, index = 0;
  for (auto leaf : input.elements()) {
    if (index++ == selected_index) break;
    const auto prior = leaf.get(key);
    if (prior.is_string() && prior.as_string() == value) ++ordinal;
  }
  const auto prefix = json::quote(key) + ":\"";
  size_t position = 0;
  while ((position = body.find(prefix, position)) != std::string::npos) {
    const auto start = position + prefix.size();
    if (value.size() < body.size() - start && body[start + value.size()] == '"' &&
        std::string_view(body).substr(start, value.size()) == value) {
      if (!ordinal) {
        const auto byte = value.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=_-");
        body[start + byte] = body[start + byte] == 'A' ? 'B' : 'A';
        return true;
      }
      --ordinal;
    }
    position = start;
  }
  return false;
}
bool native_leaf(json::Value leaf, Provider provider) {
  if (provider == Provider::VisionMessages) return leaf.get("type").as_string() == "thinking" || leaf.get("type").as_string() == "redacted_thinking";
  if (provider == Provider::VisionResponses) return leaf.get("type").as_string() == "reasoning";
  if (provider == Provider::VisionInteractions) return leaf.get("type").as_string() == "thought";
  return leaf.get("thought").is_bool() && leaf.get("thought").as_bool();
}
bool omit_native(std::string& body, json::Value root, Provider provider) {
  bool removed = false;
  auto build = [&](auto&& self, json::BoundedWriter& writer, json::Value value, size_t depth, bool target, bool native_item) -> void {
    if (value.is_array()) {
      writer.raw("["); bool comma = false;
      for (auto element : value.elements()) {
        if (target && native_leaf(element, provider)) { removed = true; continue; }
        if (comma) writer.raw(",");
        comma = true;
        self(self, writer, element, depth + 1, false, target);
      }
      writer.raw("]");
    } else if (value.is_object()) {
      writer.raw("{"); bool comma = false;
      for (auto member : value.members()) {
        const bool associated_signature = native_item &&
            ((provider == Provider::VisionGemini && member.key == "thoughtSignature") ||
             (provider == Provider::VisionInteractions && value.get("type").as_string() == "function_call" && member.key == "signature"));
        if (associated_signature) { removed = true; continue; }
        if (comma) writer.raw(",");
        comma = true; writer.quoted(member.key).raw(":");
        const bool selection = member.key == "input" || member.key == "content" || member.key == "parts";
        self(self, writer, member.value, depth + 1, selection, false);
      }
      writer.raw("}");
    } else writer.value(value, depth);
  };
  json::BoundedWriter measured({16U << 20, 64}); build(build, measured, root, 0, false, false);
  if (!measured.ok() || !removed) return false;
  std::string changed; changed.reserve(measured.size());
  json::BoundedWriter writer({measured.size(), 64}, &changed); build(build, writer, root, 0, false, false);
  if (!writer.ok()) return false;
  body = std::move(changed); return true;
}
bool native_rejected(Provider provider, int status, std::string_view body, bool omission = false) {
  // Removing a reasoning block is not a signature mutation. A signature
  // diagnostic cannot establish why an omission failed.
  if (status != 400 || body.size() > 65536) return false;
  auto diagnostic = [&](json::Value root) {
    if (!root.is_object()) return false;
    const auto error = root.get("error");
    if (!error.is_object()) return false;
    if (provider == Provider::VisionResponses) {
      if (error.get("type").as_string() != "invalid_request_error") return false;
      if (!omission) return error.get("code").as_string() == "invalid_encrypted_content";
      if (!error.get("message").is_string()) return false;
      // Reviewed required-reasoning diagnostic. Match the whole bounded item
      // path, not a phrase embedded in an unrelated field's error.
      auto message = error.get("message").as_string();
      auto take = [&](std::string_view prefix) {
        if (!message.starts_with(prefix)) return false;
        message.remove_prefix(prefix.size()); return true;
      };
      auto item = [&](std::string_view prefix) {
        const auto end = message.find('\'');
        if (end == std::string_view::npos || end > 256 || end <= prefix.size() ||
            !message.starts_with(prefix)) return false;
        for (char c : message.substr(prefix.size(), end - prefix.size()))
          if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
        message.remove_prefix(end + 1); return true;
      };
      if (!take("Item '") || !item("fc_")) return false;
      if (message.starts_with(" of type 'function_call'")) take(" of type 'function_call'");
      if (!take(" was provided without its required 'reasoning' item: '") || !item("rs_")) return false;
      return message.empty() || message == ".";
    }
    if (omission) return false;
    if (!error.get("message").is_string()) return false;
    std::string message(error.get("message").as_string());
    for (auto& c : message) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (provider == Provider::VisionMessages) {
      if (error.get("type").as_string() != "invalid_request_error") return false;
      if (message.starts_with("messages.")) {
        const auto colon = message.find(':');
        if (colon == std::string::npos) return false;
        std::string_view path(message.data(), colon); path.remove_prefix(9);
        auto index = [&] {
          const auto count = path.find_first_not_of("0123456789");
          if (path.empty() || count == 0) return false;
          path.remove_prefix(count == std::string_view::npos ? path.size() : count);
          return true;
        };
        if (!index() || !path.starts_with(".content.")) return false;
        path.remove_prefix(9);
        if (!index() || (!path.empty() && path != ".signature" && path != ".thinking")) return false;
        message.erase(0, colon + 1);
      }
      message.erase(std::remove_if(message.begin(), message.end(), [](char c) { return c == '`' || c == '\''; }), message.end());
      const auto first = message.find_first_not_of(" \t"); if (first == std::string::npos) return false; message.erase(0, first);
      while (!message.empty() && (message.back() == '.' || message.back() == ' ')) message.pop_back();
      return message == "invalid signature in thinking block" || message == "signature verification failed" || message == "signature does not match thinking block";
    }
    // The GenerateContent protobuf error and Interactions Error are different
    // schemas. Neither INVALID_ARGUMENT nor invalid_request alone is evidence.
    if (provider == Provider::VisionGemini) {
      if (!error.get("code").is_number() || error.get("code").as_uint() != 400 ||
          error.get("status").as_string() != "INVALID_ARGUMENT") return false;
    } else if (provider == Provider::VisionInteractions) {
      if (error.get("code").as_string() != "invalid_request" || error.get("status").valid()) return false;
      const auto type = root.get("event_type"), id = root.get("event_id");
      if ((type.valid() && type.as_string() != "error") || (id.valid() && !id.is_string())) return false;
    } else return false;
    return message == "invalid thought signature" || message == "invalid thought signature." ||
        message == "thought signature is invalid" || message == "thought signature is invalid.";
  };
  auto parsed = json::parse(body, {65536, 32});
  if (const auto* doc = std::get_if<json::Document>(&parsed)) return diagnostic(doc->root());
  if (provider != Provider::VisionInteractions) return false;
  // EOF discards pending SSE data. Require the final event delimiter as well
  // as normal transport completion so a complete error plus truncated tail
  // cannot be mistaken for a complete single-envelope response.
  if (!body.ends_with("\n\n") && !body.ends_with("\r\n\r\n") && !body.ends_with("\r\r")) return false;
  transport::SseFramer framer({65536, 65536, 65536});
  size_t envelopes = 0; bool specific = false;
  const bool fed = framer.feed(body, [&](const transport::SseFrame& frame) {
    ++envelopes;
    auto value = json::parse(frame.data, {65536, 32});
    const auto* doc = std::get_if<json::Document>(&value);
    specific = envelopes == 1 && frame.event == "error" && doc &&
        doc->root().get("event_type").as_string() == "error" && diagnostic(doc->root());
    return true; // Validate every byte, including subsequent error envelopes.
  });
  framer.finish();
  return fed && framer.error() == transport::SseError::None && envelopes == 1 && specific;
}
class Attempt final : public runtime::detail::Attempt {
 public: explicit Attempt(transport::Operation op) : operation_(std::move(op)) {}
  void cancel() noexcept override { operation_.cancel(); }
  void resume() noexcept override { operation_.resume(); }
 private: transport::Operation operation_;
};
class Backend final : public runtime::detail::AttemptTransport {
 public:
  Backend(Profile profile, std::string ledger, std::shared_ptr<Wire> wire)
      : profile_(std::move(profile)), ledger_(std::move(ledger)), wire_(std::move(wire)), backend_(std::make_unique<transport::Transport>()) {}
  std::unique_ptr<runtime::detail::Attempt> start(transport::HttpRequest request, transport::Callbacks callbacks) override {
    auto& wire = *wire_; auto& result = *wire.result;
    if (profile_.provider() == Provider::VisionChat || profile_.provider() == Provider::VisionResponses) {
      if (request.body.empty() || request.body.back() != '}') detail::fail();
      request.body.pop_back(); request.body.append(",\"service_tier\":\"default\"}");
    }
    auto parsed = json::parse(request.body, {16U << 20, 64}); auto* doc = std::get_if<json::Document>(&parsed); if (!doc) detail::fail();
    result.image_sent = image_exact(doc->root(), profile_.provider(), *wire.scene);
    if (!result.image_sent) { result.reason = Reason::RetentionMismatch; detail::fail(); }
    if (wire.expected) {
      wire.retained = native_retained(doc->root(), profile_.provider(), *wire.expected);
      if (!wire.retained) { result.reason = Reason::RetentionMismatch; detail::fail(); }
      if (!wire.negative) wire.positive = request.body;
      else {
        if (wire.positive != request.body) { result.reason = Reason::RetentionMismatch; detail::fail(); }
        wire.changed = wire.omission ? omit_native(request.body, doc->root(), profile_.provider()) : mutate_signature(request.body, doc->root(), profile_.provider());
        if (!wire.changed) { result.reason = Reason::MutationUnavailable; detail::fail(); }
      }
    }
    Debit debit;
    try { debit = reserve(profile_, ledger_); }
    catch (...) { wire.totals_known = false; result.reason = Reason::LedgerFailure; throw; }
    wire.lane = debit.total; wire.totals_known = true; wire.campaign = vision_campaign_totals(ledger_);
    if (debit.result != Reservation::Allowed) {
      result.reason = debit.result == Reservation::Calls ? Reason::CallBudget : debit.result == Reservation::Tokens ? Reason::TokenBudget : Reason::CostBudget;
      detail::fail();
    }
    ++result.attempts;
    if (wire.negative) {
      struct Response { int status{}; std::string bytes; bool overflow{}; };
      auto response = std::make_shared<Response>();
      auto head = std::move(callbacks.on_head);
      auto done = std::move(callbacks.on_done);
      auto body = std::move(callbacks.on_body);
      callbacks.on_head = [response, head = std::move(head)](const transport::ResponseHead& value) { response->status = value.status; if (head) head(value); };
      callbacks.on_body = [response, body = std::move(body)](std::string_view bytes) {
        if (response->status == 400 && !response->overflow) {
          if (bytes.size() > 65536 - response->bytes.size()) { response->overflow = true; response->bytes.clear(); }
          else response->bytes.append(bytes);
        }
        if (body && !body(bytes)) return false;
        return true;
      };
      callbacks.on_done = [response, done = std::move(done), wire = wire_, provider = profile_.provider(), omission = wire.omission](const transport::Result& value) {
        try { wire->rejected = value.status == transport::Status::Completed && !response->overflow && native_rejected(provider, response->status, response->bytes, omission); }
        catch (...) { wire->rejected = false; }
        response->bytes.clear(); if (done) done(value);
      };
    }
    auto op = backend_->start(std::move(request), std::move(callbacks)); result.dispatched = true;
    return std::make_unique<Attempt>(std::move(op));
  }
  void shutdown() noexcept override { backend_.reset(); }
 private: Profile profile_; std::string ledger_; std::shared_ptr<Wire> wire_; std::unique_ptr<transport::Transport> backend_;
};
descriptor::ValidatedDescriptor descriptor_for(const Profile& profile) {
  std::string family, buffered, streaming;
  switch (profile.provider()) {
    case Provider::VisionChat: family = "openai.chat"; buffered = streaming = "/v1/chat/completions"; break;
    case Provider::VisionResponses: family = "openai.responses"; buffered = streaming = "/v1/responses"; break;
    case Provider::VisionMessages: family = "anthropic.messages"; buffered = streaming = "/v1/messages"; break;
    case Provider::VisionGemini: family = "google.generate"; buffered = "/v1beta/models/" + profile.model() + ":generateContent"; streaming = "/v1beta/models/" + profile.model() + ":streamGenerateContent?alt=sse"; break;
    case Provider::VisionInteractions: family = "google.interactions"; buffered = streaming = "/v1beta/interactions"; break;
    default: detail::fail();
  }
  auto source = "{\"descriptor_version\":1,\"revision\":1,\"id\":\"vision-canary\",\"family\":" + json::quote(family) + ",\"connection\":{\"base_url\":" + json::quote(profile.origin()) + ",\"paths\":{\"buffered\":" + json::quote(buffered) + ",\"streaming\":" + json::quote(streaming) + "}}";
  if (profile.provider() == Provider::VisionChat) source += ",\"bindings\":{\"max_output_tokens\":\"max_completion_tokens\"}";
  source += '}';
  auto loaded = descriptor::load(source); if (!std::holds_alternative<descriptor::ValidatedDescriptor>(loaded)) detail::fail();
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
void usage(Case& item, const Usage& value, const Bounds& bounds) {
  if (value.input_total) item.input_tokens = value.input_total->value;
  if (value.output_total) item.output_tokens = value.output_total->value;
  if (value.input_uncached) item.input_uncached = value.input_uncached->value;
  if (value.cache_read) item.cache_read = value.cache_read->value;
  if (value.cache_write) item.cache_write = value.cache_write->value;
  if (value.reasoning) item.reasoning = value.reasoning->value;
  item.usage_stage = value.stage; item.usage_quality = value.quality;
  if (item.input_tokens && item.output_tokens) { auto seen = bounds; seen.input_tokens = *item.input_tokens; seen.output_tokens = *item.output_tokens; try { item.estimated_micro_usd = reserved_cost(seen); } catch (...) {} }
}
std::string text(const Completion& outcome) { std::string result; for (const auto& m : outcome.messages) for (const auto& p : m.parts) if (const auto* value = std::get_if<Text>(&p)) result += value->value; return result; }
bool integer_equals(json::Value value, unsigned expected) {
  // JSON Schema integers include integral decimal notation such as 8.0.
  return value.is_number() && value.as_double() == static_cast<double>(expected);
}
std::optional<bool> oracle(const Completion& outcome, const vision_test::Scene& scene) {
  const auto answer = text(outcome);
  const auto begin = answer.find('{'), end = answer.rfind('}');
  if (begin == std::string::npos || end == std::string::npos || end < begin) return {};
  // Presentation is not the oracle: one JSON object may be fenced or surrounded
  // by prose. Multiple/contradictory objects still fail strict parsing.
  auto parsed = json::parse(std::string_view(answer).substr(begin, end - begin + 1), {65536, 16});
  const auto* document = std::get_if<json::Document>(&parsed); if (!document) return {};
  const auto root = document->root();
  return root.is_object() && root.size() == 3 && integer_equals(root.get("red_circles"), scene.red) &&
      integer_equals(root.get("blue_squares"), scene.blue) && integer_equals(root.get("weighted"), scene.weighted);
}
void observation(Case& item, const Completion& outcome) {
  for (const auto& m : outcome.messages) {
    item.native_complete = item.native_complete || (m.native && m.native->complete());
    for (const auto& p : m.parts) {
      if (const auto* r = std::get_if<Reasoning>(&p)) { ++item.reasoning_items; item.summary_items += r->summary.size(); item.encrypted_present = item.encrypted_present || (r->encrypted_content && !r->encrypted_content->empty()); for (const auto& v : r->summary) item.visible_reasoning = item.visible_reasoning || !v.empty(); }
      if (const auto* r = std::get_if<Thinking>(&p)) { ++item.reasoning_items; item.summary_items += !r->text.empty(); item.visible_reasoning = item.visible_reasoning || !r->text.empty(); item.native_present = item.native_present || (r->signature && !r->signature->empty()); }
      if (const auto* r = std::get_if<Thought>(&p)) { ++item.reasoning_items; item.summary_items += r->summary.size(); item.native_present = item.native_present || (r->signature && !r->signature->empty()); for (const auto& v : r->summary) item.visible_reasoning = item.visible_reasoning || !v.empty(); }
    }
    if (m.wire_output) for (auto leaf : m.wire_output->root().elements()) if (leaf.get("thoughtSignature").is_string() && !leaf.get("thoughtSignature").as_string().empty()) item.native_present = true;
  }
  item.native_present = item.native_present || item.encrypted_present;
}
} // namespace
Report run_vision(const Profile& profile, const std::string& ledger, std::string api_key) {
  Report report; report.provider = profile.provider(); report.test_only = profile.loopback();
  const bool chat = profile.provider() == Provider::VisionChat;
  report.replay = chat ? Replay::NotApplicable : Replay::ReplayAcceptanceUnobservable;
  report.cases.reserve(chat ? 6 : 8);
  for (auto name : {"vision_off_buffered", "vision_on_buffered", "vision_on_sse", "vision_changed", "vision_tool_first", "vision_tool_positive"}) report.cases.push_back(Case{name});
  if (!chat) { report.cases.push_back(Case{"vision_signature_negative"}); report.cases.push_back(Case{"vision_reasoning_missing"}); }
  if (api_key.empty()) { for (auto& item : report.cases) item.reason = Reason::MissingCredential; return report; }
  if (api_key.size() > 4096) detail::fail();
  for (unsigned char c : api_key) if (c < 0x21 || c > 0x7e) detail::fail();
  auto wire = std::make_shared<Wire>(); auto backend = std::make_shared<Backend>(profile, ledger, wire);
  runtime::Options opts; opts.api_key = std::move(api_key); opts.retry_tokens = 0; opts.default_timeout = std::chrono::seconds(120);
  auto client = runtime::detail::ClientAccess::make(descriptor_for(profile), std::move(opts), {}, backend);
  const auto schema = document(R"({"type":"object","properties":{"value":{"type":"integer"}},"required":["value"],"additionalProperties":false})");
  auto build = [&](const vision_test::Scene& scene, bool on, bool tools) -> runtime::Request {
    if (chat) {
      chat::Request r; r.model = profile.model(); r.max_output_tokens = profile.bounds().output_tokens;
      // GPT-6 Luna Chat function calling only supports reasoning_effort:none.
      r.reasoning_effort = on && !tools ? "low" : "none";
      chat::InputMessage m{Role::User, std::string(vision_test::question)};
      m.images.push_back(scene.image());
      if (tools) {
        m.text += '\n'; m.text += tool_prompt;
        r.tools.push_back({"vision_score", "Return the inert weighted image score.", schema});
      }
      r.messages.push_back(std::move(m)); return r;
    }
    Message user{{}, Role::User, {scene.image(), Text{std::string(vision_test::question)}}};
    if (tools) user.parts.emplace_back(Text{std::string(tool_prompt)});
    switch (profile.provider()) {
      case Provider::VisionResponses: {
        responses::Request r; r.model = profile.model(); r.account_scope = "vision-campaign";
        r.max_output_tokens = profile.bounds().output_tokens;
        // Low may emit a direct tool call with no native payload. Medium is a
        // separate on control; absence still stays unobservable, never repaired.
        r.reasoning = responses::ReasoningOptions{on ? "medium" : "none", "auto"};
        r.messages.push_back(std::move(user));
        if (tools) {
          r.tools.push_back({"vision_score", "Return the inert weighted image score.", schema});
          r.required_tool = "vision_score";
        }
        return r;
      }
      case Provider::VisionMessages: {
        messages::Request r; r.model = profile.model(); r.account_scope = "vision-campaign";
        r.max_tokens = profile.bounds().output_tokens;
        if (on) r.thinking_budget = profile.thinking_budget();
        r.messages.push_back(std::move(user));
        if (tools) r.tools.push_back({"vision_score", "Return the inert weighted image score.", schema, {}, {}});
        return r;
      }
      case Provider::VisionGemini: {
        gemini::Request r; r.model = profile.model(); r.account_scope = "vision-campaign";
        r.max_output_tokens = profile.bounds().output_tokens;
        r.thinking_budget = on ? 1024 : 0; r.include_thoughts = on;
        r.messages.push_back(std::move(user));
        if (tools) {
          r.tools.push_back({"vision_score", "Return the inert weighted image score.", schema});
          r.required_tool = "vision_score";
        }
        return r;
      }
      case Provider::VisionInteractions: {
        interactions::Request r; r.model = profile.model(); r.account_scope = "vision-campaign";
        r.max_output_tokens = profile.bounds().output_tokens;
        // Flash-Lite's omitted level is documented default-off, not an explicit disable.
        // Live low maps to256 (<512 minimum), and medium was rejected. High
        // is provider-reported allowed; prior failed controls stay recorded.
        if (on) r.thinking_level = "high";
        r.thinking_summaries = on; r.messages.push_back(std::move(user));
        if (tools) {
          r.tools.push_back({"vision_score", "Return the inert weighted image score.", schema});
          r.required_tool = "vision_score";
        }
        return r;
      }
      default: detail::fail();
    }
  };
  auto call = [&](runtime::Request request, size_t position, bool streaming, const vision_test::Scene& scene, bool on) {
    auto& item = report.cases[position]; wire->result = &item; wire->scene = &scene; item.reason = Reason::RuntimeFailure;
    item.reasoning_requested = on; item.reasoning_disabled = !on && profile.provider() != Provider::VisionInteractions; item.default_off = !on && profile.provider() == Provider::VisionInteractions;
    runtime::RunOptions run; run.streaming = streaming; run.retry.max_attempts = 1;
    auto value = client.complete(std::move(request), run);
    report.reserved = wire->lane; report.reserved_known = wire->totals_known; if (wire->totals_known) report.campaign_reserved = wire->campaign;
    if (const auto* complete = std::get_if<Completion>(value.get())) {
      item.state = State::Passed; item.reason = Reason::None; usage(item, complete->usage, profile.bounds()); observation(item, *complete);
      if (complete->usage.stage != UsageStage::Final || complete->usage.quality != UsageQuality::Consistent) { item.state = State::Failed; item.reason = Reason::InvalidUsage; }
      if (position != 4 && !wire->negative) {
        item.vision_correct = oracle(*complete, scene);
        if (!item.vision_correct || !*item.vision_correct) {
          item.state = State::Failed;
          item.reason = item.vision_correct ? Reason::IncorrectVision : Reason::UnreadableVision;
        }
      }
    } else {
      item.state = item.reason == Reason::CallBudget || item.reason == Reason::TokenBudget || item.reason == Reason::CostBudget || item.reason == Reason::LedgerFailure || item.reason == Reason::MutationUnavailable ? State::NotRun : State::Failed;
      const auto& failure = std::get<Failure>(*value); item.failure_kind = failure.error.kind; item.http_status = failure.error.http_status; usage(item, failure.partial.usage, profile.bounds());
    }
    return value;
  };
  const auto& a = vision_test::scene_a(); const auto& b = vision_test::scene_b();
  for (size_t i = 0; i < 4; ++i) call(build(i == 3 ? b : a, i == 1 || i == 2, false), i, i == 2, i == 3 ? b : a, i == 1 || i == 2);
  auto request = build(a, !chat, true); auto first = call(request, 4, true, a, !chat);
  const auto* first_complete = std::get_if<Completion>(first.get()); if (!first_complete) return report;
  const ToolCall* selected = nullptr; const Message* original = nullptr; bool invalid = false;
  for (const auto& m : first_complete->messages) for (const auto& part : m.parts) {
    if (const auto* tool = std::get_if<ToolCall>(&part)) { if (selected || tool->kind != ToolCallKind::ClientExecuted || tool->name != "vision_score" || !tool->input || !integer_equals(tool->input->root().get("value"), a.weighted)) invalid = true; selected = tool; original = &m; }
    if (std::holds_alternative<InvalidToolCall>(part)) invalid = true;
  }
  if (!selected || !original || invalid || (!chat && (!original->native || !original->native->complete()))) { report.cases[4].state = State::Failed; report.cases[4].reason = invalid ? Reason::InvalidTool : Reason::MissingTool; return report; }
  report.cases[4].vision_correct = true;
  std::visit([&](auto& typed) {
    using T = std::decay_t<decltype(typed)>;
    if constexpr (std::is_same_v<T, chat::Request>) {
      chat::InputMessage m; m.role = Role::Assistant; m.tool_calls.push_back(*selected);
      for (const auto& part : original->parts) if (const auto* t = std::get_if<Text>(&part)) m.text += t->value;
      typed.messages.push_back(std::move(m));
      chat::InputMessage result; result.role = Role::Tool; result.tool_call_id = selected->id;
      result.text = "{\"value\":" + std::to_string(a.weighted) + '}';
      typed.messages.push_back(std::move(result));
    } else {
      typed.messages.push_back(*original);
      typed.messages.push_back(Message{{}, Role::User,
          {ToolResult{selected->id, "{\"value\":" + std::to_string(a.weighted) + '}', false}}});
      if constexpr (!std::is_same_v<T, messages::Request>) typed.required_tool.reset();
    }
  }, request);
  if (!chat) wire->expected = original;
  auto positive = call(request, 5, true, a, !chat); (void)positive;
  report.positive_retained = !chat && wire->retained && report.cases[5].dispatched;
  if (chat || !report.positive_retained || report.cases[5].state != State::Passed) return report;
  report.native_leaves = report.cases[4].reasoning_items;
  wire->negative = true; wire->omission = false; wire->changed = wire->rejected = false;
  auto negative = call(request, 6, true, a, true);
  report.ciphertext_mutated = profile.provider() == Provider::VisionResponses && wire->changed && report.cases[6].dispatched;
  report.signature_mutated = profile.provider() != Provider::VisionResponses && wire->changed && report.cases[6].dispatched;
  if (report.cases[6].dispatched) {
    auto& control = report.cases[6];
    if (wire->rejected && wire->changed) { control.state = State::Passed; control.reason = profile.provider() == Provider::VisionResponses ? Reason::CiphertextRejected : Reason::SignatureRejected; if (!profile.loopback()) report.replay = Replay::ReplayVerified; }
    else if (std::holds_alternative<Completion>(*negative)) { control.state = State::Passed; control.reason = Reason::NegativeAccepted; }
    else { control.state = State::Failed; control.reason = Reason::NegativeInconclusive; }
  }
  wire->omission = true; wire->changed = wire->rejected = false;
  auto missing = call(request, 7, true, a, true); report.reasoning_removed = wire->changed && report.cases[7].dispatched;
  if (report.cases[7].dispatched && report.reasoning_removed) {
    auto& control = report.cases[7];
    if (wire->rejected) { control.state = State::Passed; control.reason = Reason::OmissionRejected; }
    else if (std::holds_alternative<Completion>(*missing)) { control.state = State::Passed; control.reason = Reason::OmissionAccepted; }
    else { control.state = State::Failed; control.reason = Reason::NegativeInconclusive; }
  }
  return report;
}
} // namespace sp::canary
