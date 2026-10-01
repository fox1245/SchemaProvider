#include "core/native.h"
#include "json/json.h"
#include <openssl/evp.h>
#include <algorithm>
#include <bit>
#include <map>
#include <type_traits>

namespace sp {
namespace {
using Digest = std::array<unsigned char, 32>;
// Domain-separated, typed, length-prefixed values avoid delimiter collisions.
// EVP is implementation-private; neither OpenSSL types nor digests grant provenance.
class Hash {
 public:
  Hash() : context_(EVP_MD_CTX_new(), EVP_MD_CTX_free) {
    valid_ = context_ && EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) == 1;
  }
  void bytes(const void* data, size_t size) {
    constexpr size_t limit = 32 << 20;
    if (!valid_ || size > limit - consumed_) { valid_ = false; return; }
    consumed_ += size;
    if (size && EVP_DigestUpdate(context_.get(), data, size) != 1) valid_ = false;
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
    if (depth > 64 || !v.valid()) { valid_ = false; return; }
    if (v.is_null()) number(0);
    else if (v.is_bool()) { number(1); number(v.as_bool()); }
    else if (v.is_string()) { number(2); text(v.as_string()); }
    else if (v.is_uint()) { number(3); number(v.as_uint()); }
    else if (v.is_int()) { number(4); number(static_cast<uint64_t>(v.as_int())); }
    else if (v.is_number()) { number(5); number(std::bit_cast<uint64_t>(v.as_double())); }
    else if (v.is_array()) {
      number(6); number(v.size());
      for (auto element : v.elements()) { if (!valid_) break; value(element, depth + 1); }
    } else if (v.is_object()) {
      number(7); number(v.size());
      if (v.size() > (1 << 20)) { valid_ = false; return; }
      std::vector<json::Member> members; members.reserve(v.size());
      for (auto member : v.members()) members.push_back(member);
      std::sort(members.begin(), members.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
      for (auto member : members) { if (!valid_) break; text(member.key); value(member.value, depth + 1); }
    } else valid_ = false;
  }
  void message(const Message& message) {
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
        else if constexpr (std::is_same_v<P, ToolResult>) { text(p.tool_use_id); text(p.content); number(p.is_error); }
        else if constexpr (std::is_same_v<P, Reasoning>) {
          text(p.id); number(p.summary.size()); for (const auto& s : p.summary) text(s);
          number(p.encrypted_content.has_value()); if (p.encrypted_content) text(*p.encrypted_content);
          number(p.status.has_value()); if (p.status) text(*p.status);
          number(p.content.size()); for (const auto& s : p.content) text(s);
        } else if constexpr (std::is_same_v<P, Opaque>) { text(p.wire_type); document(p.wire_metadata); }
        else valid_ = false;
      }, part);
    }
    // Absent Responses data leaves the established Messages content domain intact.
    if (message.wire_output) { text("sp.responses.output.v1"); document(message.wire_output); }
  }
  bool finish(Digest& digest) {
    unsigned size = 0;
    return valid_ && EVP_DigestFinal_ex(context_.get(), digest.data(), &size) == 1 && size == digest.size();
  }
 private:
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
  size_t consumed_ = 0;
  bool valid_ = false;
};
std::string lower(std::string_view text) {
  std::string result(text);
  for (char& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  return result;
}
bool origin_digest(const descriptor::ValidatedDescriptor& descriptor, Digest& digest) {
  const bool responses = descriptor.family() == "openai.responses";
  if (!responses && descriptor.family() != "anthropic.messages") return false;
  Hash hash; hash.text(responses ? "sp.responses.origin.v1" : "sp.messages.origin.v1");
  hash.text(descriptor.family()); hash.text(descriptor.id()); hash.text(descriptor.base_url());
  hash.text(descriptor.request_model_member()); hash.text(descriptor.request_messages_member());
  hash.text(descriptor.request_stream_member()); hash.text(descriptor.max_output_tokens_member());
  std::vector<std::pair<std::string, std::string_view>> headers;
  headers.reserve(descriptor.headers().size());
  for (const auto& [name, value] : descriptor.headers()) {
    auto key = lower(name);
    if (key != "accept" && key != "content-type" && (responses || key != "anthropic-version")) headers.emplace_back(std::move(key), value);
  }
  std::sort(headers.begin(), headers.end());
  hash.number(headers.size());
  for (const auto& [key, value] : headers) { hash.text(key); hash.text(value); }
  if (!responses) { hash.text("anthropic-version"); hash.text("2023-06-01"); }
  return hash.finish(digest);
}
bool configuration_digest(const messages::Request& request, Digest& digest) {
  Hash hash; hash.text("sp.messages.configuration.v1");
  hash.text(request.model); hash.text(request.account_scope); hash.text(request.system);
  hash.number(request.thinking_budget.has_value()); if (request.thinking_budget) hash.number(*request.thinking_budget);
  hash.number(request.tools.size());
  for (const auto& tool : request.tools) {
    hash.text(tool.name); hash.text(tool.description); hash.document(tool.input_schema); hash.text(tool.type);
    hash.number(tool.max_uses.has_value()); if (tool.max_uses) hash.number(*tool.max_uses);
  }
  return hash.finish(digest);
}
bool configuration_digest(const responses::Request& request, Digest& digest) {
  Hash hash; hash.text("sp.responses.configuration.v1");
  hash.text(request.model); hash.text(request.account_scope); hash.text(request.instructions);
  hash.number(request.max_output_tokens.has_value()); if (request.max_output_tokens) hash.number(*request.max_output_tokens);
  hash.number(request.reasoning.has_value());
  if (request.reasoning) { hash.text(request.reasoning->effort); hash.text(request.reasoning->summary); }
  // Per-turn tool selection can be cleared after the forced client call.
  hash.number(request.tools.size());
  for (const auto& tool : request.tools) {
    hash.text(tool.name); hash.text(tool.description); hash.document(tool.parameters); hash.number(tool.strict);
  }
  return hash.finish(digest);
}
bool content_digest(const Message& message, Digest& digest, bool responses) {
  Hash hash; hash.text(responses ? "sp.responses.content.v1" : "sp.messages.content.v1");
  hash.message(message); return hash.finish(digest);
}
bool append_prefix(Digest& prefix, const Digest& content, size_t index, bool responses) {
  Hash hash; hash.text(responses ? "sp.responses.prefix.v1" : "sp.messages.prefix.v1"); hash.number(index);
  hash.bytes(prefix.data(), prefix.size()); hash.bytes(content.data(), content.size());
  return hash.finish(prefix);
}
} // namespace
NativeContext::NativeContext(const descriptor::ValidatedDescriptor& descriptor, const messages::Request& request, bool streaming)
    : responses_(false), model_(request.model), route_(descriptor.path(streaming)), prefix_count_(request.messages.size()) {
  valid_ = descriptor.family() == "anthropic.messages" && origin_digest(descriptor, origin_) && configuration_digest(request, prefix_);
  if (valid_) bind_history(request.messages);
}
NativeContext::NativeContext(const descriptor::ValidatedDescriptor& descriptor, const responses::Request& request, bool streaming)
    : responses_(true), model_(request.model), route_(descriptor.path(streaming)), prefix_count_(request.messages.size()) {
  valid_ = descriptor.family() == "openai.responses" && origin_digest(descriptor, origin_) && configuration_digest(request, prefix_);
  if (valid_) bind_history(request.messages);
}
void NativeContext::bind_history(const std::vector<Message>& messages) {
  std::map<std::string, std::string> pending;
  for (size_t index = 0; index < messages.size(); ++index) {
    const auto& message = messages[index];
    if (!responses_ && (message.wire_output || std::any_of(message.parts.begin(), message.parts.end(), [](const Part& part) {
          return std::holds_alternative<Reasoning>(part) || std::holds_alternative<Opaque>(part);
        }))) { history_valid_ = false; return; }
    Digest content{};
    if (!content_digest(message, content, responses_)) { valid_ = false; return; }
    if (message.native) {
      const auto& seal = *message.native;
      if (!seal.complete_ || !seal.context_ || !seal.context_->valid_ ||
          seal.context_->responses_ != responses_ || seal.context_->prefix_count_ != index || seal.context_->route_ != route_ ||
          seal.context_->origin_ != origin_ || seal.context_->prefix_ != prefix_ ||
          seal.content_ != content) { history_valid_ = false; return; }
    }
    // One bounded content hash per history message, not one rehash per capsule.
    if (!append_prefix(prefix_, content, index, responses_)) { valid_ = false; return; }
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
NativeReplay::NativeReplay(std::shared_ptr<const NativeContext> context, const Message& message, const StopReason* stop, bool complete)
    : context_(std::move(context)), stop_(stop ? stop->kind : StopKind::Unknown) {
  const bool responses = context_ && context_->responses_;
  const bool eligible = !responses || (message.wire_output && message.wire_output->root().is_array() &&
      stop && (stop->kind == StopKind::EndTurn || stop->kind == StopKind::ToolUse || stop->kind == StopKind::Refusal));
  complete_ = complete && eligible && context_ && context_->valid_ && context_->history_valid_ && stop &&
      content_digest(message, content_, responses);
}
} // namespace sp
