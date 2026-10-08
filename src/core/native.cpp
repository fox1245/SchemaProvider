#include "core/native.h"
#include "core/interface_contract.h"
#include "crypto/crypto.h"
#include "json/json.h"
#include <algorithm>
#include <bit>
#include <map>
#include <type_traits>

namespace sp {
namespace {
using Digest = std::array<unsigned char, 32>;
struct Domains {
  std::string_view family, origin, content, prefix, output;
};
const Domains* domains(std::string_view family) {
  static constexpr Domains values[]{
      {"anthropic.messages", "sp.messages.origin.v1", "sp.messages.content.v1", "sp.messages.prefix.v1", "sp.messages.output.v1"},
      {"openai.responses", "sp.responses.origin.v1", "sp.responses.content.v1", "sp.responses.prefix.v1", "sp.responses.output.v1"},
      {"google.generate", "sp.gemini.origin.v1", "sp.gemini.content.v1", "sp.gemini.prefix.v1", "sp.gemini.output.v1"},
      {"google.interactions", "sp.interactions.origin.v1", "sp.interactions.content.v1", "sp.interactions.prefix.v1", "sp.interactions.output.v1"},
      {"openai.chat", "sp.chat.origin.v1", "sp.chat.content.v1", "sp.chat.prefix.v1", "sp.chat.output.v1"}};
  for (const auto& value : values) if (value.family == family) return &value;
  return nullptr;
}
// Domain-separated, typed, length-prefixed values avoid delimiter collisions.
// Digests are implementation-private and grant no provenance.
class Hash {
 public:
  explicit Hash(const descriptor::CodecResources& limits) : limits_(limits) {}
  void bytes(const void* data, size_t size) {
    if (!valid_ || size > limits_.native_bytes - consumed_) { valid_ = false; return; }
    consumed_ += size;
    context_.update(data, size);
  }
  void number(uint64_t value) {
    unsigned char data[8];
    for (unsigned i = 0; i < 8; ++i) data[7 - i] = static_cast<unsigned char>(value >> (8 * i));
    bytes(data, sizeof data);
  }
  void text(std::string_view value) { number(value.size()); bytes(value.data(), value.size()); }
  void document(const std::shared_ptr<const json::Document>& document) {
    number(document ? 1 : 0);
    if (document) value(document->root(), 0);
  }
  void value(json::Value v, size_t depth) {
    if (depth > limits_.native_depth || !v.valid()) { valid_ = false; return; }
    if (v.is_null()) number(0);
    else if (v.is_bool()) { number(1); number(v.as_bool()); }
    else if (v.is_string()) { number(2); text(v.as_string()); }
    else if (v.is_uint()) { number(3); number(v.as_uint()); }
    else if (v.is_int()) { number(4); number(static_cast<uint64_t>(v.as_int())); }
    else if (v.is_number()) { number(5); number(std::bit_cast<uint64_t>(v.as_double())); }
    else if (v.is_array()) {
      number(6); number(v.size());
      if (v.size() > limits_.native_members) { valid_ = false; return; }
      for (auto element : v.elements()) { if (!valid_) break; value(element, depth + 1); }
    } else if (v.is_object()) {
      number(7); number(v.size());
      if (v.size() > limits_.native_members) { valid_ = false; return; }
      std::vector<json::Member> members; members.reserve(v.size());
      for (auto member : v.members()) members.push_back(member);
      std::sort(members.begin(), members.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
      for (auto member : members) { if (!valid_) break; text(member.key); value(member.value, depth + 1); }
    } else valid_ = false;
  }
  void message(const Message& message, std::string_view output_domain) {
    text(message.id); number(static_cast<uint64_t>(message.role)); number(message.parts.size());
    for (const auto& part : message.parts) {
      if (!valid_) break;
      number(part.index());
      std::visit([&](const auto& p) {
        using P = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<P, Text>) text(p.value);
        else if constexpr (std::is_same_v<P, Refusal>) { text(p.text); text(p.raw_code); }
        else if constexpr (std::is_same_v<P, ToolCall> || std::is_same_v<P, InvalidToolCall>) {
          text(p.id); text(p.name); number(static_cast<uint64_t>(p.kind));
          text(p.wire_type); document(p.wire_metadata);
          if constexpr (std::is_same_v<P, ToolCall>) document(p.input);
          else { text(p.raw_fragment); number(static_cast<uint64_t>(p.reason)); }
        } else if constexpr (std::is_same_v<P, Thinking>) {
          text(p.text); number(p.signature.has_value()); if (p.signature) text(*p.signature);
        } else if constexpr (std::is_same_v<P, RedactedThinking>) text(p.data);
        else if constexpr (std::is_same_v<P, ServerToolResult>) { text(p.tool_use_id); text(p.wire_type); document(p.content); }
        else if constexpr (std::is_same_v<P, ToolResult>) {
          text(p.tool_use_id); text(p.content); number(p.is_error); number(p.host.has_value());
          if (p.host) { text(p.host->name); text(p.host->status); number(p.host->retryable); number(p.host->effect_uncertain); }
        }
        else if constexpr (std::is_same_v<P, Reasoning>) {
          text(p.id); number(p.summary.size()); for (const auto& s : p.summary) text(s);
          number(p.encrypted_content.has_value()); if (p.encrypted_content) text(*p.encrypted_content);
          number(p.status.has_value()); if (p.status) text(*p.status);
          number(p.content.size()); for (const auto& s : p.content) text(s);
        } else if constexpr (std::is_same_v<P, Opaque>) { text(p.wire_type); document(p.wire_metadata); }
        else if constexpr (std::is_same_v<P, Image>) {
          text(p.mime); number(p.data != nullptr); if (p.data) text(*p.data);
          number(static_cast<uint64_t>(p.detail));
        } else if constexpr (std::is_same_v<P, Thought>) {
          number(p.summary.size());
          for (const auto& summary : p.summary) { if (!valid_) break; text(summary); }
          number(p.signature.has_value()); if (p.signature) text(*p.signature);
        } else valid_ = false;
      }, part);
    }
    // No native array leaves established Messages content bytes unchanged.
    // The full ordered group also binds signatures on nonthinking Google parts.
    if (message.wire_output) { text(output_domain); document(message.wire_output); }
  }
  bool finish(Digest& digest) {
    if (!valid_) return false;
    digest = context_.finish();
    return true;
  }
 private:
  const descriptor::CodecResources& limits_;
  crypto::Sha256 context_;
  size_t consumed_ = 0;
  bool valid_ = true;
};
std::string lower(std::string_view text) {
  std::string result(text);
  for (char& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  return result;
}
bool origin_digest(const descriptor::ValidatedDescriptor& descriptor, Digest& digest) {
  const auto* domain = domains(descriptor.family());
  if (!domain) return false;
  const bool messages = descriptor.family() == "anthropic.messages";
  Hash hash(descriptor.policy()->resources()); hash.text(domain->origin);
  hash.text(descriptor.policy()->identity());
  hash.text(descriptor.family()); hash.text(descriptor.id()); hash.text(descriptor.base_url());
  if (descriptor.family() == "google.generate") {
    // Both literal endpoints bind the surface; transport mode is not lineage.
    hash.text(descriptor.path(false)); hash.text(descriptor.path(true));
  }
  hash.text(descriptor.request_model_member()); hash.text(descriptor.request_messages_member());
  hash.text(descriptor.request_stream_member()); hash.text(descriptor.max_output_tokens_member());
  hash.number(descriptor.usage_path().size());
  for (const auto& segment : descriptor.usage_path()) hash.text(segment);
  hash.number(descriptor.stop_mappings().size());
  for (const auto& [raw, kind] : descriptor.stop_mappings()) { hash.text(raw); hash.number(static_cast<uint64_t>(kind)); }
  std::vector<std::pair<std::string, std::string_view>> headers;
  headers.reserve(descriptor.headers().size());
  for (const auto& [name, value] : descriptor.headers()) {
    auto key = lower(name);
    if (key != "accept" && key != "content-type" && (!messages || key != "anthropic-version")) headers.emplace_back(std::move(key), value);
  }
  std::sort(headers.begin(), headers.end());
  hash.number(headers.size());
  for (const auto& [key, value] : headers) { hash.text(key); hash.text(value); }
  if (messages) {
    std::string_view version = *descriptor.family_policy().header_version;
    for (const auto& [name, value] : descriptor.headers()) if (lower(name) == "anthropic-version") version = value;
    hash.text("anthropic-version"); hash.text(version);
  }
  return hash.finish(digest);
}
void effective_digest(Hash& hash, const descriptor::EffectiveChoices& choices) {
  auto integer = [&](const auto& value) { hash.number(value.has_value()); if (value) hash.number(*value); };
  auto text = [&](const auto& value) { hash.number(value.has_value()); if (value) hash.text(*value); };
  auto number = [&](const auto& value) { hash.number(value.has_value()); if (value) hash.number(std::bit_cast<uint64_t>(*value)); };
  // Generation cap governs this dispatch's resource admission, not consumption
  // of already-sealed history. Reasoning budget and every lineage control remain
  // bound; the encoded request separately retains its actual generation cap.
  integer(choices.thinking_budget);
  integer(choices.include_thoughts); integer(choices.thinking_summaries);
  hash.number(choices.reasoning_enabled);
  if (choices.reasoning_enabled) { text(choices.reasoning_effort); text(choices.reasoning_summary); }
  text(choices.thinking_level); text(choices.service_tier);
  number(choices.temperature); number(choices.top_p);
}
void routing_digest(Hash& hash, const std::optional<OpenRouterRouting>& routing) {
  hash.number(routing.has_value());
  if (!routing) return;
  auto boolean = [&](const auto& value) { hash.number(value.has_value()); if (value) hash.number(*value); };
  auto list = [&](const auto& values) {
    hash.number(values.size());
    for (const auto& value : values) hash.text(value);
  };
  boolean(routing->zdr); boolean(routing->allow_fallbacks); boolean(routing->require_parameters);
  list(routing->only); list(routing->order); list(routing->ignore);
  hash.number(routing->data_collection.has_value());
  if (routing->data_collection) hash.text(*routing->data_collection);
}
void response_format_digest(Hash& hash, const std::optional<ResponseFormat>& format) {
  hash.number(format.has_value());
  if (!format) return;
  hash.number(static_cast<unsigned>(format->kind));
  hash.text(format->name); hash.text(format->description); hash.document(format->schema);
  hash.number(format->strict.has_value()); if (format->strict) hash.number(*format->strict);
}
bool configuration_digest(const descriptor::ValidatedDescriptor& descriptor, const chat::Request& request,
                          const descriptor::EffectiveChoices& choices, Digest& digest) {
  Hash hash(descriptor.policy()->resources()); hash.text("sp.chat.configuration.v1");
  effective_digest(hash, choices);
  hash.text(request.model);
  routing_digest(hash, request.provider);
  response_format_digest(hash, request.response_format);
  hash.number(request.reasoning.has_value());
  if (request.reasoning) {
    hash.number(request.reasoning->effort.has_value()); if (request.reasoning->effort) hash.text(*request.reasoning->effort);
    hash.number(request.reasoning->max_tokens.has_value()); if (request.reasoning->max_tokens) hash.number(*request.reasoning->max_tokens);
    hash.number(request.reasoning->exclude.has_value()); if (request.reasoning->exclude) hash.number(*request.reasoning->exclude);
    hash.number(request.reasoning->enabled.has_value()); if (request.reasoning->enabled) hash.number(*request.reasoning->enabled);
  }
  hash.number(request.include_reasoning.has_value()); if (request.include_reasoning) hash.number(*request.include_reasoning);
  hash.number(request.usage_include.has_value()); if (request.usage_include) hash.number(*request.usage_include);
  hash.number(request.models.size()); for (const auto& model : request.models) hash.text(model);
  hash.number(request.tools.size());
  for (const auto& tool : request.tools) {
    hash.text(tool.name); hash.text(tool.description); hash.document(tool.parameters);
  }
  return hash.finish(digest);
}
bool configuration_digest(const descriptor::ValidatedDescriptor& descriptor, const messages::Request& request, const descriptor::EffectiveChoices& choices, Digest& digest) {
  Hash hash(descriptor.policy()->resources()); hash.text("sp.messages.configuration.v1");
  effective_digest(hash, choices);
  hash.text(request.model); hash.text(request.account_scope); hash.text(request.system);
  hash.number(request.thinking_mode.has_value()); if (request.thinking_mode) hash.number(static_cast<unsigned>(*request.thinking_mode));
  hash.number(request.output_effort.has_value()); if (request.output_effort) hash.number(static_cast<unsigned>(*request.output_effort));
  hash.number(request.cache_control.has_value());
  if (request.cache_control) {
    hash.number(request.cache_control->ttl.has_value());
    if (request.cache_control->ttl) hash.number(static_cast<unsigned>(*request.cache_control->ttl));
  }
  routing_digest(hash, request.provider);
  hash.number(request.tools.size());
  for (const auto& tool : request.tools) {
    hash.text(tool.name); hash.text(tool.description); hash.document(tool.input_schema); hash.text(tool.type);
    hash.number(tool.max_uses.has_value()); if (tool.max_uses) hash.number(*tool.max_uses);
  }
  return hash.finish(digest);
}
bool configuration_digest(const descriptor::ValidatedDescriptor& descriptor, const responses::Request& request, const descriptor::EffectiveChoices& choices, Digest& digest) {
  Hash hash(descriptor.policy()->resources()); hash.text("sp.responses.configuration.v1");
  effective_digest(hash, choices);
  hash.text(request.model); hash.text(request.account_scope); hash.text(request.instructions);
  hash.number(request.store.value_or(false));
  hash.number(request.max_tool_calls.has_value()); if (request.max_tool_calls) hash.number(*request.max_tool_calls);
  routing_digest(hash, request.provider);
  response_format_digest(hash, request.response_format);
  hash.number(request.parallel_tool_calls.has_value()); if (request.parallel_tool_calls) hash.number(*request.parallel_tool_calls);
  hash.number(request.verbosity.has_value()); if (request.verbosity) hash.number(static_cast<unsigned>(*request.verbosity));
  hash.number(request.truncation.has_value()); if (request.truncation) hash.number(static_cast<unsigned>(*request.truncation));
  hash.number(request.include.has_value());
  if (request.include) {
    hash.number(request.include->size());
    for (const auto value : *request.include) hash.number(static_cast<unsigned>(value));
  }
  hash.number(request.hosted_tools.size());
  for (const auto& hosted : request.hosted_tools) {
    hash.number(hosted.index());
    std::visit([&](const auto& tool) {
      using T = std::decay_t<decltype(tool)>;
      if constexpr (std::is_same_v<T, responses::ImageGenerationTool>) {
        hash.number(tool.size.has_value()); if (tool.size) hash.text(*tool.size);
        hash.number(tool.quality.has_value()); if (tool.quality) hash.text(*tool.quality);
      } else if constexpr (std::is_same_v<T, responses::FileSearchTool>) {
        hash.number(tool.vector_store_ids.size());
        for (const auto& id : tool.vector_store_ids) hash.text(id);
      } else if constexpr (std::is_same_v<T, responses::ShellTool>) {
        hash.number(tool.environment.skills.size());
        for (const auto& skill : tool.environment.skills) hash.text(skill.skill_id);
      }
    }, hosted);
  }
  // Per-turn tool selection can be cleared after the forced client call.
  hash.number(request.tools.size());
  for (const auto& tool : request.tools) {
    const auto& defaults = descriptor.policy()->defaults(descriptor.family(), request.model);
    hash.text(tool.name); hash.text(tool.description); hash.document(tool.parameters);
    const auto strict = tool.strict ? tool.strict : defaults.strict_tools;
    hash.number(strict.has_value()); if (strict) hash.number(*strict);
    hash.number(tool.defer_loading.value_or(false));
  }
  return hash.finish(digest);
}
bool configuration_digest(const descriptor::ValidatedDescriptor& descriptor, const gemini::Request& request, const descriptor::EffectiveChoices& choices, Digest& digest) {
  Hash hash(descriptor.policy()->resources()); hash.text("sp.gemini.configuration.v1");
  effective_digest(hash, choices);
  hash.text(request.model); hash.text(request.account_scope); hash.text(request.system);
  hash.number(request.safety_settings.size());
  for (const auto& setting : request.safety_settings) {
    hash.number(static_cast<unsigned>(setting.category));
    hash.number(static_cast<unsigned>(setting.threshold));
  }
  // required_tool is a per-turn choice, not continuation lineage.
  hash.number(request.tools.size());
  for (const auto& tool : request.tools) {
    hash.text(tool.name); hash.text(tool.description); hash.document(tool.parameters);
  }
  return hash.finish(digest);
}
bool configuration_digest(const descriptor::ValidatedDescriptor& descriptor, const interactions::Request& request, const descriptor::EffectiveChoices& choices, Digest& digest) {
  Hash hash(descriptor.policy()->resources()); hash.text("sp.interactions.configuration.v1");
  effective_digest(hash, choices);
  hash.text(request.model); hash.text(request.account_scope); hash.text(request.system);
  // required_tool is deliberately excluded just as in Responses.
  hash.number(request.tools.size());
  for (const auto& tool : request.tools) {
    hash.text(tool.name); hash.text(tool.description); hash.document(tool.parameters);
  }
  return hash.finish(digest);
}
bool content_digest(const Message& message, Digest& digest, std::string_view family, const descriptor::CodecResources& resources) {
  const auto* domain = domains(family);
  if (!domain) return false;
  Hash hash(resources); hash.text(domain->content);
  hash.message(message, domain->output); return hash.finish(digest);
}
bool append_prefix(Digest& prefix, const Digest& content, size_t index, std::string_view family, const descriptor::CodecResources& resources) {
  const auto* domain = domains(family);
  if (!domain) return false;
  Hash hash(resources); hash.text(domain->prefix); hash.number(index);
  hash.bytes(prefix.data(), prefix.size()); hash.bytes(content.data(), content.size());
  return hash.finish(prefix);
}
} // namespace
NativeContext::NativeContext(Family family, std::string model, std::string route, size_t prefix_count, descriptor::PolicySnapshot policy)
    : family_(family), model_(std::move(model)), route_(std::move(route)), prefix_count_(prefix_count), policy_(std::move(policy)) {}
std::shared_ptr<const NativeContext> NativeContext::decoding_only() const {
  auto context = std::shared_ptr<NativeContext>(new NativeContext(family_, model_, route_, prefix_count_, policy_));
  context->origin_ = origin_;
  context->prefix_ = prefix_;
  context->valid_ = valid_;
  context->history_valid_ = history_valid_;
  context->pending_server_tools_ = pending_server_tools_;
  context->client_tools_ = client_tools_;
  context->replay_eligible_ = false;
  context->thinking_disabled_ = thinking_disabled_;
  return context;
}
NativeReplay::NativeReplay(std::shared_ptr<const NativeContext> context, StopKind stop, Digest content, bool complete)
    : context_(std::move(context)), stop_(stop), content_(content), complete_(complete) {}
bool NativeReplay::archive_valid(const Message& message) const {
  Digest content{};
  if (!context_ || !context_->valid_ || !context_->policy_ || !context_->replay_eligible_) return false;
  // An incomplete genuine capsule remains incomplete; persisting its partial
  // bytes never upgrades it to replay authority.
  if (!complete_) return true;
  return context_->history_valid_ &&
      content_digest(message, content, NativeContext::family_name(context_->family_), context_->policy_->resources()) &&
      content == content_;
}
std::string_view NativeContext::family_name(Family family) {
  switch (family) {
    case Family::Messages: return "anthropic.messages";
    case Family::Responses: return "openai.responses";
    case Family::Gemini: return "google.generate";
    case Family::Interactions: return "google.interactions";
    case Family::Chat: return "openai.chat";
  }
  return {};
}
NativeContext::NativeContext(const descriptor::ValidatedDescriptor& descriptor, const messages::Request& request, const descriptor::EffectiveChoices& choices, bool streaming)
    : family_(Family::Messages), model_(request.model), route_(descriptor.path(streaming)), prefix_count_(request.messages.size()), policy_(descriptor.policy()) {
  valid_ = descriptor.family() == "anthropic.messages" && origin_digest(descriptor, origin_) && configuration_digest(descriptor, request, choices, prefix_);
  if (valid_) bind_history(request.messages);
}
NativeContext::NativeContext(const descriptor::ValidatedDescriptor& descriptor, const responses::Request& request, const descriptor::EffectiveChoices& choices, bool streaming)
    : family_(Family::Responses), model_(request.model), route_(descriptor.path(streaming)), prefix_count_(request.messages.size()), policy_(descriptor.policy()) {
  valid_ = descriptor.family() == "openai.responses" && origin_digest(descriptor, origin_) && configuration_digest(descriptor, request, choices, prefix_);
  if (!valid_) return;
  configuration_ = prefix_;
  if (request.previous_response_id) {
    replay_eligible_ = false;
    cursor_authority_ = true;
    if (!request.previous_response_history.empty()) {
      const auto& previous = request.previous_response_history.back();
      if (previous.role != Role::Assistant || previous.id != *request.previous_response_id ||
          !previous.native || !previous.wire_output || !previous.wire_output->root().is_array()) {
        history_valid_ = false; return;
      }
      const auto& seal = *previous.native;
      if (seal.context_ && seal.context_->cursor_authority_) {
        Digest content{};
        if (request.previous_response_history.size() != 1 || !seal.continuation_complete_ ||
            !seal.context_->valid_ || !seal.context_->history_valid_ ||
            seal.context_->family_ != family_ || seal.context_->model_ != model_ ||
            seal.context_->route_ != route_ || seal.context_->origin_ != origin_ ||
            seal.context_->configuration_ != configuration_ ||
            !content_digest(previous, content, family_name(family_), policy_->resources()) ||
            content != seal.content_) { history_valid_ = false; return; }
        prefix_ = seal.context_->prefix_;
        if (!append_prefix(prefix_, content, 0, family_name(family_), policy_->resources())) { valid_ = false; return; }
      } else {
        bind_history(request.previous_response_history);
        if (!valid_ || !history_valid_) return;
      }
    }
    Hash cursor(policy_->resources());
    cursor.text("sp.responses.cursor.v1"); cursor.bytes(prefix_.data(), prefix_.size());
    cursor.text(*request.previous_response_id);
    if (!cursor.finish(prefix_)) { valid_ = false; return; }
  }
  bind_history(request.messages);
}
NativeContext::NativeContext(const descriptor::ValidatedDescriptor& descriptor, const gemini::Request& request, const descriptor::EffectiveChoices& choices, bool)
    : family_(Family::Gemini), model_(request.model), route_(descriptor.path(false)), prefix_count_(request.messages.size()), policy_(descriptor.policy()) {
  valid_ = descriptor.family() == family_name(family_) && origin_digest(descriptor, origin_) && configuration_digest(descriptor, request, choices, prefix_);
  thinking_disabled_ = choices.thinking_budget && *choices.thinking_budget == 0 && !choices.thinking_level;
  if (valid_) {
    client_tools_.reserve(request.tools.size());
    for (const auto& tool : request.tools) client_tools_.push_back(tool.name);
    std::sort(client_tools_.begin(), client_tools_.end());
    bind_history(request.messages, request.history_mode == gemini::HistoryMode::PortableForeign);
  }
}
NativeContext::NativeContext(const descriptor::ValidatedDescriptor& descriptor, const interactions::Request& request, const descriptor::EffectiveChoices& choices, bool streaming)
    : family_(Family::Interactions), model_(request.model), route_(descriptor.path(streaming)), prefix_count_(request.messages.size()), policy_(descriptor.policy()) {
  valid_ = descriptor.family() == family_name(family_) && origin_digest(descriptor, origin_) && configuration_digest(descriptor, request, choices, prefix_);
  if (valid_) {
    client_tools_.reserve(request.tools.size());
    for (const auto& tool : request.tools) client_tools_.push_back(tool.name);
    std::sort(client_tools_.begin(), client_tools_.end());
    bind_history(request.messages);
  }
}
NativeContext::NativeContext(const descriptor::ValidatedDescriptor& descriptor, const chat::Request& request, const descriptor::EffectiveChoices& choices, bool streaming)
    : family_(Family::Chat), model_(request.model), route_(descriptor.path(streaming)),
      prefix_count_(request.canonical_messages.empty() ? request.messages.size() : request.canonical_messages.size()), policy_(descriptor.policy()) {
  valid_ = descriptor.family() == family_name(family_) && origin_digest(descriptor, origin_) && configuration_digest(descriptor, request, choices, prefix_);
  if (!valid_) return;
  if (!request.canonical_messages.empty()) { bind_history(request.canonical_messages); return; }
  // Hash typed portable inputs using the same normalized ordered Message
  // representation without allocating/copying strings or image payloads.
  const auto* domain = domains(family_name(family_));
  for (size_t index = 0; index < request.messages.size(); ++index) {
    const auto& m = request.messages[index];
    Hash hash(policy_->resources()); hash.text(domain->content); hash.text(""); hash.number(static_cast<uint64_t>(m.role));
    if (m.role == Role::Tool) {
      hash.number(1); hash.number(7); hash.text(m.tool_call_id); hash.text(m.text); hash.number(0); hash.number(0);
    } else {
      const bool text = m.images.empty() || !m.text.empty();
      hash.number(m.images.size() + (text ? 1 : 0) + m.tool_calls.size());
      for (const auto& image : m.images) {
        hash.number(10); hash.text(image.mime); hash.number(image.data != nullptr);
        if (image.data) hash.text(*image.data);
        hash.number(static_cast<uint64_t>(image.detail));
      }
      if (text) { hash.number(0); hash.text(m.text); }
      for (const auto& call : m.tool_calls) {
        hash.number(2); hash.text(call.id); hash.text(call.name); hash.number(static_cast<uint64_t>(call.kind));
        hash.text(call.wire_type); hash.document(call.wire_metadata); hash.document(call.input);
      }
    }
    Digest content{};
    if (!hash.finish(content) || !append_prefix(prefix_, content, index, family_name(family_), policy_->resources())) { valid_ = false; return; }
  }
}
void NativeContext::bind_history(const std::vector<Message>& messages, bool portable_gemini) {
  std::map<std::string, std::string> pending;
  for (size_t index = 0; index < messages.size(); ++index) {
    const auto& message = messages[index];
    if (family_ == Family::Messages && (message.wire_output || std::any_of(message.parts.begin(), message.parts.end(), [](const Part& part) {
          return std::holds_alternative<Reasoning>(part) || std::holds_alternative<Opaque>(part) || std::holds_alternative<Thought>(part);
        }))) { history_valid_ = false; return; }
    if (family_ != Family::Messages && family_ != Family::Chat) {
      if (message.native) {
        if (message.role != Role::Assistant || !message.wire_output || !message.wire_output->root().is_array()) {
          history_valid_ = false; return;
        }
      } else {
        const bool portable = family_ == Family::Gemini && portable_gemini && message.role == Role::Assistant;
        if (message.wire_output || (message.role == Role::Assistant && !portable) ||
            std::any_of(message.parts.begin(), message.parts.end(), [portable](const Part& part) {
              if (portable) {
                if (std::holds_alternative<Text>(part)) return false;
                const auto* call = std::get_if<ToolCall>(&part);
                return !call || call->kind != ToolCallKind::ClientExecuted ||
                    !call->wire_type.empty() || call->wire_metadata;
              }
              return !std::holds_alternative<Text>(part) && !std::holds_alternative<Image>(part) && !std::holds_alternative<ToolResult>(part);
            })) { history_valid_ = false; return; }
      }
    }
    Digest content{};
    if (!content_digest(message, content, family_name(family_), policy_->resources())) { valid_ = false; return; }
    if (message.native) {
      const auto& seal = *message.native;
      if (!seal.complete_ || !seal.context_ || !seal.context_->valid_ || !seal.context_->replay_eligible_ ||
          seal.context_->family_ != family_ || seal.context_->prefix_count_ != index || seal.context_->route_ != route_ ||
          seal.context_->origin_ != origin_ || seal.context_->prefix_ != prefix_ ||
          seal.content_ != content) { history_valid_ = false; return; }
    }
    // One bounded content hash per history message, not one rehash per capsule.
    if (!append_prefix(prefix_, content, index, family_name(family_), policy_->resources())) { valid_ = false; return; }
    for (const auto& part : message.parts) {
      if (const auto* call = std::get_if<ToolCall>(&part); call && call->kind == ToolCallKind::ServerExecuted) pending.emplace(call->id, call->name);
      if (const auto* result = std::get_if<ServerToolResult>(&part)) pending.erase(result->tool_use_id);
    }
  }
  pending_server_tools_.reserve(pending.size());
  for (auto& entry : pending) pending_server_tools_.emplace_back(entry.first, std::move(entry.second));
}
bool NativeContext::matches_descriptor(const descriptor::ValidatedDescriptor& descriptor) const {
  Digest digest{};
  return valid_ && (route_ == descriptor.path(false) || route_ == descriptor.path(true)) && origin_digest(descriptor, digest) && digest == origin_;
}
std::optional<std::string_view> NativeContext::server_tool_name(std::string_view id) const {
  const auto found = std::lower_bound(pending_server_tools_.begin(), pending_server_tools_.end(), id,
      [](const auto& entry, std::string_view key) { return std::string_view(entry.first) < key; });
  if (found != pending_server_tools_.end() && found->first == id) return found->second;
  return std::nullopt;
}
bool NativeContext::client_tool_declared(std::string_view name) const {
  const auto found = std::lower_bound(client_tools_.begin(), client_tools_.end(), name,
      [](const auto& entry, std::string_view key) { return std::string_view(entry) < key; });
  return found != client_tools_.end() && *found == name;
}
NativeReplay::NativeReplay(std::shared_ptr<const NativeContext> context, const Message& message, const StopReason* stop, bool complete)
    : context_(std::move(context)), stop_(stop ? stop->kind : StopKind::Unknown) {
  const bool atomic = context_ && context_->family_ != NativeContext::Family::Messages && context_->family_ != NativeContext::Family::Chat;
  const bool eligible = !atomic || (message.role == Role::Assistant && message.wire_output && message.wire_output->root().is_array() &&
      stop && (stop->kind == StopKind::EndTurn || stop->kind == StopKind::ToolUse || stop->kind == StopKind::Refusal));
  const bool invalid_call = context_ && context_->family_ == NativeContext::Family::Gemini &&
      std::any_of(message.parts.begin(), message.parts.end(), [](const Part& part) {
        return std::holds_alternative<InvalidToolCall>(part);
      });
  const bool terminal = complete && eligible && !invalid_call && context_ && context_->valid_ &&
      context_->history_valid_ && stop &&
      content_digest(message, content_, NativeContext::family_name(context_->family_), context_->policy_->resources());
  complete_ = terminal && context_->replay_eligible_;
  continuation_complete_ = terminal && context_->family_ == NativeContext::Family::Responses &&
      context_->cursor_authority_;
}
InterfaceContract core_interface_contract() noexcept {
  std::uint64_t capabilities = capability::NullableUsage |
      capability::All5Native | capability::TrustedArchive | capability::OrderedWireEvents;
  if (json::retained_size_contract() == 1) capabilities |= capability::RetainedJsonSize;
#if defined(__unix__) || defined(__APPLE__) || defined(_WIN32)
  return {5, capabilities | capability::NativeSocketRuntime};
#else
  return {5, capabilities};
#endif
}
} // namespace sp
