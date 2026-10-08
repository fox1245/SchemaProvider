// Independent family goldens plus a real model-free HTTP peer.
// Usage: sp_fixture_runner <node> <fixture_server.mjs> <fixture-directory> <descriptor> [--offline-only]
#include "codecs/chat.h"
#include "codecs/messages.h"
#include "codecs/messages_request.h"
#include "core/native.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include "transport/http_transport.h"
#include "transport/sse_framer.h"
#ifdef _WIN32
#include "support/win_owner.h"
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <limits>
#include <optional>
#ifndef _WIN32
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <random>
#include <stdexcept>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {
using sp::json::Value;
using Clock = std::chrono::steady_clock;
void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
std::string read_file(const std::filesystem::path& path) {
  require(std::filesystem::file_size(path) <= (16U << 20), "fixture file exceeds size bound");
  std::ifstream input(path, std::ios::binary);
  const auto utf8_path = path.u8string();
  require(static_cast<bool>(input), "cannot read fixture file: " +
      std::string(reinterpret_cast<const char*>(utf8_path.data()), utf8_path.size()));
  std::string result((std::istreambuf_iterator<char>(input)), {});
  require(result.size() <= (16U << 20), "fixture file exceeds size bound");
  return result;
}
sp::json::Document parse(std::string_view text) {
  auto parsed = sp::json::parse(text, {16U << 20, 64});
  if (auto error = std::get_if<sp::json::ParseError>(&parsed))
    throw std::runtime_error("fixture JSON error: " + error->message);
  return std::get<sp::json::Document>(std::move(parsed));
}
std::string string(Value value) {
  require(value.is_string(), "fixture expected a string");
  return std::string(value.as_string());
}
const char* error_name(sp::ErrorKind kind) {
  switch (kind) {
    case sp::ErrorKind::InvalidConfig: return "InvalidConfig";
    case sp::ErrorKind::InvalidRequest: return "InvalidRequest";
    case sp::ErrorKind::Unsupported: return "Unsupported";
    case sp::ErrorKind::ReplayIneligible: return "ReplayIneligible";
    case sp::ErrorKind::Transport: return "Transport";
    case sp::ErrorKind::ProtocolCorrupt: return "ProtocolCorrupt";
    case sp::ErrorKind::Truncated: return "Truncated";
    case sp::ErrorKind::RemoteFailure: return "RemoteFailure";
    case sp::ErrorKind::Cancelled: return "Cancelled";
    case sp::ErrorKind::DeadlineExceeded: return "DeadlineExceeded";
    case sp::ErrorKind::ResourceLimit: return "ResourceLimit";
    case sp::ErrorKind::Misuse: return "Misuse";
    case sp::ErrorKind::Authentication: return "Authentication";
    case sp::ErrorKind::Permission: return "Permission";
    case sp::ErrorKind::NotFound: return "NotFound";
    case sp::ErrorKind::RateLimited: return "RateLimited";
    case sp::ErrorKind::QuotaExhausted: return "QuotaExhausted";
    case sp::ErrorKind::LimitUnknown: return "LimitUnknown";
    case sp::ErrorKind::Overloaded: return "Overloaded";
  }
  throw std::runtime_error("unprojected error kind");
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
  throw std::runtime_error("unprojected stop kind");
}
const char* invalid_name(sp::InvalidReason reason) {
  switch (reason) {
    case sp::InvalidReason::Truncated: return "Truncated";
    case sp::InvalidReason::NotJson: return "NotJson";
    case sp::InvalidReason::DuplicateKey: return "DuplicateKey";
    case sp::InvalidReason::DepthExceeded: return "DepthExceeded";
    case sp::InvalidReason::Empty: return "Empty";
    case sp::InvalidReason::Other: return "Other";
  }
  throw std::runtime_error("unprojected invalid-tool reason");
}
const char* role_name(sp::Role role) {
  switch (role) {
    case sp::Role::System: return "system";
    case sp::Role::Developer: return "developer";
    case sp::Role::User: return "user";
    case sp::Role::Assistant: return "assistant";
    case sp::Role::Tool: return "tool";
  }
  throw std::runtime_error("unprojected role");
}
const char* tool_kind_name(sp::ToolCallKind kind) {
  switch (kind) {
    case sp::ToolCallKind::ClientExecuted: return "ClientExecuted";
    case sp::ToolCallKind::ServerExecuted: return "ServerExecuted";
    case sp::ToolCallKind::ApprovalRequest: return "ApprovalRequest";
  }
  throw std::runtime_error("unprojected tool kind");
}
std::string stop_projection(const sp::StopReason& reason) {
  std::string out = "{\"kind\":" + sp::json::quote(stop_name(reason.kind)) + ",\"raw\":" + sp::json::quote(reason.raw);
  if (reason.sequence) out += ",\"sequence\":" + sp::json::quote(*reason.sequence);
  if (reason.details) out += ",\"details\":" + reason.details->root().dump();
  return out + '}';
}
std::string count_projection(const std::optional<sp::Count>& value) {
  return value ? std::to_string(value->value) : "null";
}
std::string usage_projection(const sp::Usage& usage) {
  std::string out = "{";
  const auto add = [&](std::string_view key, const std::optional<sp::Count>& value) {
    if (out.size() > 1) out += ',';
    out += sp::json::quote(key) + ':' + count_projection(value);
  };
  add("input_total", usage.input_total); add("output_total", usage.output_total);
  add("total", usage.total); add("provider_reported_total", usage.provider_reported_total);
  add("input_uncached", usage.input_uncached); add("cache_read", usage.cache_read);
  add("cache_write", usage.cache_write); add("reasoning", usage.reasoning);
  const char* stage = usage.stage == sp::UsageStage::Missing ? "Missing" : usage.stage == sp::UsageStage::Partial ? "Partial" : "Final";
  out += ",\"stage\":" + sp::json::quote(stage) + ",\"quality\":" + sp::json::quote(usage.quality == sp::UsageQuality::Consistent ? "Consistent" : "Inconsistent");
  out += ",\"extra\":{";
  bool first = true;
  for (const auto& [key, value] : usage.extra) {
    if (!first) out += ',';
    first = false;
    out += sp::json::quote(key) + ':' + std::to_string(value.value);
  }
  return out + "}}";
}
std::string part_projection(const sp::Part& part) {
  return std::visit([](const auto& p) -> std::string {
    using T = std::decay_t<decltype(p)>;
    if constexpr (std::is_same_v<T, sp::Text>) return "{\"type\":\"text\",\"text\":" + sp::json::quote(p.value) + "}";
    else if constexpr (std::is_same_v<T, sp::Refusal>) return "{\"type\":\"refusal\",\"text\":" + sp::json::quote(p.text) + ",\"raw_code\":" + sp::json::quote(p.raw_code) + "}";
    else if constexpr (std::is_same_v<T, sp::Thinking>)
      return "{\"type\":\"thinking\",\"text\":" + sp::json::quote(p.text) + ",\"signature\":" + (p.signature ? sp::json::quote(*p.signature) : "null") + '}';
    else if constexpr (std::is_same_v<T, sp::RedactedThinking>)
      return "{\"type\":\"redacted_thinking\",\"data\":" + sp::json::quote(p.data) + '}';
    else if constexpr (std::is_same_v<T, sp::Reasoning>) {
      std::string out = "{\"type\":\"reasoning\",\"id\":" + sp::json::quote(p.id) + ",\"summary\":[";
      for (size_t i = 0; i < p.summary.size(); ++i) {
        if (i) out += ',';
        out += sp::json::quote(p.summary[i]);
      }
      out += "],\"content\":[";
      for (size_t i = 0; i < p.content.size(); ++i) {
        if (i) out += ',';
        out += sp::json::quote(p.content[i]);
      }
      return out + "],\"encrypted_content\":" + (p.encrypted_content ? sp::json::quote(*p.encrypted_content) : "null") +
          ",\"status\":" + (p.status ? sp::json::quote(*p.status) : "null") + '}';
    } else if constexpr (std::is_same_v<T, sp::Opaque>) {
      require(p.wire_metadata != nullptr, "opaque output has no native metadata");
      return "{\"type\":\"opaque\",\"wire_type\":" + sp::json::quote(p.wire_type) +
          ",\"wire_metadata\":" + p.wire_metadata->root().dump() + '}';
    }
    else if constexpr (std::is_same_v<T, sp::ServerToolResult>) {
      require(p.content != nullptr, "server result has no native block");
      return "{\"type\":\"server_tool_result\",\"tool_use_id\":" + sp::json::quote(p.tool_use_id)
           + ",\"wire_type\":" + sp::json::quote(p.wire_type) + ",\"content\":" + p.content->root().dump() + '}';
    } else if constexpr (std::is_same_v<T, sp::ToolResult> || std::is_same_v<T, sp::Image>)
      throw std::runtime_error("request-only part in model output");
    else if constexpr (std::is_same_v<T, sp::Thought>) {
      std::string out = "{\"type\":\"thought\",\"summary\":[";
      for (size_t i = 0; i < p.summary.size(); ++i) {
        if (i) out += ',';
        out += sp::json::quote(p.summary[i]);
      }
      return out + "],\"signature\":" + (p.signature ? sp::json::quote(*p.signature) : "null") + '}';
    }
    else {
      std::string out = std::is_same_v<T, sp::ToolCall> ? "{\"type\":\"tool_call\"" : "{\"type\":\"invalid_tool_call\"";
      out += ",\"id\":" + sp::json::quote(p.id) + ",\"name\":" + sp::json::quote(p.name) + ",\"kind\":" + sp::json::quote(tool_kind_name(p.kind));
      if constexpr (std::is_same_v<T, sp::ToolCall>) {
        require(p.input != nullptr, "sealed tool has no input");
        out += ",\"input\":" + p.input->root().dump();
      } else out += ",\"raw_fragment\":" + sp::json::quote(p.raw_fragment) + ",\"reason\":" + sp::json::quote(invalid_name(p.reason));
      return out + '}';
    }
  }, part);
}
std::string projection(const sp::Outcome& outcome) {
  if (const auto* completion = std::get_if<sp::Completion>(&outcome)) {
    std::string out = "{\"messages\":[";
    bool first_message = true;
    for (const auto& message : completion->messages) {
      if (!first_message) out += ',';
      first_message = false;
      out += "{\"role\":" + sp::json::quote(role_name(message.role)) + ",\"parts\":[";
      bool first_part = true;
      for (const auto& part : message.parts) {
        if (!first_part) out += ',';
        first_part = false;
        out += part_projection(part);
      }
      out += "]}";
    }
    return out + "],\"stop\":" + stop_projection(completion->stop) + ",\"usage\":" + usage_projection(completion->usage) + '}';
  }
  const auto& failure = std::get<sp::Failure>(outcome);
  std::string out = "{\"class\":" + sp::json::quote(error_name(failure.error.kind)) + ",\"partial_text\":[";
  bool first = true;
  for (const auto& message : failure.partial.messages) for (const auto& part : message.parts) {
    if (const auto* text = std::get_if<sp::Text>(&part)) {
      if (!first) out += ',';
      first = false;
      out += sp::json::quote(text->value);
    }
  }
  return out + "],\"stop\":" + (failure.partial.stop ? stop_projection(*failure.partial.stop) : "null") + '}';
}

struct Fixture {
  std::filesystem::path path;
  sp::json::Document document;
  std::string id, body, close, parity_group;
  bool streaming;
  std::optional<sp::descriptor::ValidatedDescriptor> descriptor_override;
  Value root() const { return document.root(); }
  const sp::descriptor::ValidatedDescriptor& configuration(const sp::descriptor::ValidatedDescriptor& base) const {
    return descriptor_override ? *descriptor_override : base;
  }
};
Fixture load_fixture(const std::filesystem::path& path) {
  auto document = parse(read_file(path));
  auto root = document.root();
  require(root.get("fixture_version").as_uint() == 2, "fixture version must be 2");
  const auto mode = string(root.get("transport").get("mode"));
  require(mode == "buffered" || mode == "sse", "unsupported fixture transport");
  require(string(root.get("transport").get("scheme")) == "http", "fixture peer is loopback HTTP only");
  require(string(root.get("provenance").get("kind")) == "synthetic", "corpus must not masquerade as vendor captures");
  require(root.get("expect").get("request_count").as_uint() == 1, "each fixture attempt expects one request");
  Fixture fixture{path, std::move(document), string(root.get("case_id")), {}, {}, {}, mode == "sse", {}};
  if (auto source = root.get("descriptor_source"); source.valid()) {
    auto loaded = sp::descriptor::load(string(source));
    require(std::holds_alternative<sp::descriptor::ValidatedDescriptor>(loaded), "fixture descriptor rejected");
    fixture.descriptor_override.emplace(std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded)));
  }
  auto schedule = root.get("transport").get("schedule");
  require(schedule.is_array() && schedule.size() > 0, "missing fixture schedule");
  for (auto entry : schedule.elements()) {
    const auto kind = string(entry.get("kind"));
    if (kind == "bytes") fixture.body += string(entry.get("utf8"));
    else if (kind == "close") fixture.close = string(entry.get("how"));
    else if (kind == "reset") fixture.close = "rst";
    else require(kind == "delay_ms", "unsupported fixture entry");
  }
  require(!fixture.close.empty(), "fixture has no explicit close");
  if (root.get("parity_group").valid()) fixture.parity_group = string(root.get("parity_group"));
  return fixture;
}
sp::chat::Request input_request(const Fixture& fixture) {
  const auto input = fixture.root().get("input");
  sp::chat::Request request;
  request.model = string(input.get("model"));
  require(input.get("messages").is_array(), "fixture messages must be an array");
  for (auto message : input.get("messages").elements()) {
    const auto role = string(message.get("role"));
    sp::Role typed;
    if (role == "user") typed = sp::Role::User;
    else if (role == "system") typed = sp::Role::System;
    else if (role == "developer") typed = sp::Role::Developer;
    else if (role == "assistant") typed = sp::Role::Assistant;
    else throw std::runtime_error("unsupported fixture input role");
    request.messages.push_back({typed, string(message.get("content"))});
  }
  return request;
}
using Captures = std::map<std::string, sp::Message>;
sp::messages::Request messages_request(const Fixture& fixture, const Captures& captures) {
  const auto input = fixture.root().get("input");
  sp::messages::Request request;
  request.model = string(input.get("model"));
  request.account_scope = string(input.get("account_scope"));
  if (auto system = input.get("system"); system.valid()) request.system = string(system);
  if (auto maximum = input.get("max_tokens"); maximum.valid()) {
    require(maximum.is_uint(), "fixture max_tokens must be unsigned");
    request.max_tokens = maximum.as_uint();
  }
  if (auto budget = input.get("thinking_budget"); budget.valid()) {
    require(budget.is_uint(), "fixture thinking budget must be unsigned");
    request.thinking_budget = budget.as_uint();
  }
  require(input.get("messages").is_array(), "fixture messages must be an array");
  for (auto message : input.get("messages").elements()) {
    const auto role = string(message.get("role"));
    require(role == "user" || role == "assistant", "unsupported Messages fixture role");
    request.messages.push_back({{}, role == "user" ? sp::Role::User : sp::Role::Assistant,
                                {sp::Text{string(message.get("content"))}}});
  }
  if (auto tools = input.get("tools"); tools.valid()) {
    require(tools.is_array(), "fixture tools must be an array");
    for (auto tool : tools.elements()) {
      sp::messages::ToolDefinition definition;
      definition.name = string(tool.get("name"));
      if (auto description = tool.get("description"); description.valid()) definition.description = string(description);
      if (auto type = tool.get("type"); type.valid()) definition.type = string(type);
      if (auto schema = tool.get("input_schema"); schema.valid())
        definition.input_schema = std::make_shared<const sp::json::Document>(parse(schema.dump()));
      if (auto maximum = tool.get("max_uses"); maximum.valid()) {
        require(maximum.is_uint(), "fixture max_uses must be unsigned");
        definition.max_uses = maximum.as_uint();
      }
      request.tools.push_back(std::move(definition));
    }
  }
  if (auto source = fixture.root().get("replay_from"); source.valid()) {
    const auto found = captures.find(string(source));
    require(found != captures.end(), "replay source must complete before its continuation");
    request.messages.push_back(found->second);
    if (auto results = input.get("tool_results"); results.valid()) {
      require(results.is_array(), "fixture tool_results must be an array");
      sp::Message user{{}, sp::Role::User, {}};
      for (auto result : results.elements()) {
        const auto error = result.get("is_error");
        require(!error.valid() || error.is_bool(), "fixture is_error must be boolean");
        user.parts.push_back(sp::ToolResult{string(result.get("tool_use_id")), string(result.get("content")), error.valid() && error.as_bool()});
      }
      if (!user.parts.empty()) request.messages.push_back(std::move(user));
    }
  }
  return request;
}
struct PreparedRequest {
  std::string method, path;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::shared_ptr<const sp::NativeContext> context;
};
PreparedRequest prepare_request(const Fixture& fixture, const sp::descriptor::ValidatedDescriptor& descriptor, const Captures& captures) {
  auto take = [](auto result) {
    return std::visit([](auto&& request) -> PreparedRequest {
      using T = std::decay_t<decltype(request)>;
      if constexpr (std::is_same_v<T, sp::Error>) throw std::runtime_error("request encode failed: " + request.safe_message);
      else {
        PreparedRequest prepared{std::move(request.method), std::move(request.path), std::move(request.headers), std::move(request.body), {}};
        if constexpr (std::is_same_v<T, sp::messages::EncodedRequest>) prepared.context = std::move(request.context);
        return prepared;
      }
    }, std::move(result));
  };
  if (descriptor.family() == "anthropic.messages")
    return take(sp::messages::encode(descriptor, messages_request(fixture, captures), fixture.streaming));
  return take(sp::chat::encode(descriptor, input_request(fixture), fixture.streaming));
}
struct Pipeline {
  size_t terminal_events = 0, events = 0;
  sp::Accumulator accumulator;
  using Codecs = std::variant<sp::chat::Codec, sp::messages::Codec>;
  Codecs codec;
  sp::transport::SseFramer framer;
  sp::transport::SseFramer::Sink sink;
  std::string buffered;
  bool streaming;
  static Codecs make_codec(const sp::descriptor::ValidatedDescriptor& descriptor, bool streaming, sp::Accumulator& accumulator,
                           std::shared_ptr<const sp::NativeContext> context) {
    if (descriptor.family() == "anthropic.messages")
      return Codecs{std::in_place_type<sp::messages::Codec>, descriptor,
                    streaming ? sp::messages::Mode::Sse : sp::messages::Mode::Buffered, accumulator, std::move(context)};
    return Codecs{std::in_place_type<sp::chat::Codec>, descriptor,
                  streaming ? sp::chat::Mode::Sse : sp::chat::Mode::Buffered, accumulator};
  }
  Pipeline(const sp::descriptor::ValidatedDescriptor& descriptor, bool is_streaming, std::shared_ptr<const sp::NativeContext> context = {})
      : accumulator({}, [&](const sp::Event& event) {
          ++events;
          if (std::holds_alternative<sp::Commit>(event) || std::holds_alternative<sp::Fail>(event)) ++terminal_events;
        }), codec(make_codec(descriptor, is_streaming, accumulator, std::move(context))),
        sink([&](const auto& frame) { return std::visit([&](auto& selected) { return selected.frame(frame.event, frame.data); }, codec); }),
        streaming(is_streaming) {}
  void bytes(std::string_view bytes) {
    if (streaming) {
      framer.feed(bytes, sink);
      if (framer.error() != sp::transport::SseError::None && !accumulator.terminal())
        accumulator.accept(sp::Fail{{framer.error() == sp::transport::SseError::ResourceLimit ? sp::ErrorKind::ResourceLimit : sp::ErrorKind::ProtocolCorrupt, "SSE framing rejected"}});
    } else if (!accumulator.terminal()) {
      if (bytes.size() > (16U << 20) - buffered.size()) accumulator.accept(sp::Fail{{sp::ErrorKind::ResourceLimit, "buffered response exceeds limit"}});
      else buffered.append(bytes);
    }
  }
  void finish(bool normal, sp::ErrorKind error = sp::ErrorKind::Truncated) {
    if (streaming) {
      framer.finish();
      // A failure known from the transport precedes errors discovered only while finalizing EOF.
      if (normal && framer.error() != sp::transport::SseError::None && !accumulator.terminal())
        accumulator.accept(sp::Fail{{sp::ErrorKind::ProtocolCorrupt, "incomplete SSE encoding"}});
    } else if (!accumulator.terminal()) std::visit([&](auto& selected) { selected.buffered(buffered, {normal, error}); }, codec);
    std::visit([&](auto& selected) { selected.finish({normal, error}); }, codec);
    require(accumulator.outcome().has_value(), "missing terminal outcome");
    require(terminal_events == 1, "outcome notification count is not one");
    const size_t before = events;
    std::visit([&](auto& selected) {
      selected.finish({false, sp::ErrorKind::Cancelled});
      if (streaming) selected.frame("message", "[DONE]");
    }, codec);
    require(events == before, "callbacks after outcome");
  }
};

void verify_close_precedence(const sp::descriptor::ValidatedDescriptor& descriptor, const std::shared_ptr<const sp::NativeContext>& context) {
  for (auto error : {sp::ErrorKind::Cancelled, sp::ErrorKind::DeadlineExceeded, sp::ErrorKind::Truncated}) {
    Pipeline buffered(descriptor, false, context);
    buffered.bytes("{\"choices\":[");
    buffered.finish(false, error);
    require(std::get<sp::Failure>(*buffered.accumulator.outcome()).error.kind == error,
            "buffered EOF parsing hid transport failure");
    Pipeline sse(descriptor, true, context);
    sse.bytes("data: \xe2");
    sse.finish(false, error);
    require(std::get<sp::Failure>(*sse.accumulator.outcome()).error.kind == error,
            "SSE EOF validation hid transport failure");
  }
  Pipeline normal(descriptor, true, context);
  normal.bytes("data: \xe2");
  normal.finish(true);
  require(std::get<sp::Failure>(*normal.accumulator.outcome()).error.kind == sp::ErrorKind::ProtocolCorrupt,
          "normal EOF hid incomplete UTF-8");
  std::cout << "[PASS] abnormal close precedes deferred JSON/UTF-8 EOF validation\n";
}
std::string verify(const Fixture& fixture, const Pipeline& pipeline) {
  const auto& outcome = *pipeline.accumulator.outcome();
  const bool success = std::holds_alternative<sp::Completion>(outcome);
  if ((string(fixture.root().get("expect").get("outcome")) == "completion") != success)
    throw std::runtime_error(fixture.id + ": wrong outcome: " + projection(outcome));
  const auto expected = fixture.root().get("expect").get(success ? "completion" : "failure");
  const auto actual_text = projection(outcome);
  auto actual = parse(actual_text);
  if (!sp::json::equal(actual.root(), expected))
    throw std::runtime_error(fixture.id + ": projection mismatch\n expected=" + expected.dump() + "\n actual=" + actual_text);
  return actual_text;
}
void capture(const Fixture& fixture, const Pipeline& pipeline, Captures* captures) {
  if (!captures) return;
  const auto* completion = std::get_if<sp::Completion>(&*pipeline.accumulator.outcome());
  require(completion && completion->messages.size() == 1, "native capture must contain one complete message");
  const auto& message = completion->messages.front();
  require(message.native && message.native->complete(), "captured message lacks complete native provenance");
  require(captures->emplace(fixture.id, message).second, "duplicate capture identity");
}
std::string offline(const Fixture& fixture, const sp::descriptor::ValidatedDescriptor& descriptor,
                    const std::shared_ptr<const sp::NativeContext>& context, const std::vector<size_t>& cuts, Captures* captures = nullptr) {
  Pipeline pipeline(descriptor, fixture.streaming, context);
  size_t offset = 0;
  for (size_t cut : cuts) {
    require(cut >= offset && cut <= fixture.body.size(), "invalid partition");
    pipeline.bytes(std::string_view(fixture.body).substr(offset, cut - offset));
    offset = cut;
  }
  pipeline.bytes(std::string_view(fixture.body).substr(offset));
  const bool normal = fixture.close == "content_length_met" || fixture.close == "chunked_terminator";
  pipeline.finish(normal);
  auto result = verify(fixture, pipeline);
  capture(fixture, pipeline, captures);
  return result;
}

class Peer {
 public:
  Peer(const char* node, const char* script, const std::filesystem::path& fixtures, const std::filesystem::path& descriptor) {
#ifdef _WIN32
    const auto fixture_path = fixtures.u8string();
    const auto descriptor_path = descriptor.u8string();
    auto child = runtime_test::spawn_with_pipes(node, {
        script,
        std::string_view(reinterpret_cast<const char*>(fixture_path.data()), fixture_path.size()),
        std::string_view(reinterpret_cast<const char*>(descriptor_path.data()), descriptor_path.size())});
    process_ = std::move(child.process);
    input_ = std::move(child.to_child);
    output_ = std::move(child.from_child);
#else
    int to_child[2], from_child[2];
    require(pipe(to_child) == 0, "creating child stdin failed");
    if (pipe(from_child) != 0) { close(to_child[0]); close(to_child[1]); throw std::runtime_error("creating child stdout failed"); }
    pid_ = fork();
    if (pid_ == 0) {
      dup2(to_child[0], STDIN_FILENO); dup2(from_child[1], STDOUT_FILENO);
      close(to_child[0]); close(to_child[1]); close(from_child[0]); close(from_child[1]);
      execl(node, node, script, fixtures.c_str(), descriptor.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    close(to_child[0]); close(from_child[1]);
    input_ = to_child[1]; output_ = from_child[0];
    if (pid_ < 0) { close(input_); close(output_); throw std::runtime_error("fork failed"); }
#endif
    try {
      auto ready = line();
      require(ready.root().get("port").is_uint(), "fixture peer did not start");
      port = ready.root().get("port").as_uint();
      digest = string(ready.root().get("descriptor_digest"));
    } catch (...) { stop(); throw; }
  }
  ~Peer() { stop(); }
  Peer(const Peer&) = delete;
  Peer& operator=(const Peer&) = delete;
  sp::json::Document command(std::string text) {
    text += '\n';
#ifdef _WIN32
    runtime_test::write_pipe(input_.get(), text);
#else
    size_t sent = 0;
    while (sent < text.size()) {
      const auto count = write(input_, text.data() + sent, text.size() - sent);
      if (count < 0 && errno == EINTR) continue;
      require(count > 0, "fixture peer control write failed");
      sent += static_cast<size_t>(count);
    }
#endif
    return line();
  }
  void arm(const Fixture& fixture, bool negative = false) {
    auto ack = command("{\"arm\":" + sp::json::quote(fixture.id) + ",\"negative_control\":" + (negative ? "true}" : "false}"));
    require(ack.root().get("armed").as_string() == fixture.id, "fixture peer rejected arm: " + ack.root().dump());
  }
  sp::json::Document stats() { return command("{\"stats\":true}"); }
  uint64_t port = 0;
  std::string digest;
 private:
  sp::json::Document line() {
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (true) {
      if (const auto end = pending_.find('\n'); end != std::string::npos) {
        auto document = parse(std::string_view(pending_).substr(0, end));
        pending_.erase(0, end + 1);
        return document;
      }
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      require(left > 0, "fixture peer control timeout");
      char buffer[4096];
#ifdef _WIN32
      const auto count = runtime_test::read_pipe(output_.get(), buffer, sizeof(buffer), left);
      require(count > 0, "fixture peer did not respond");
#else
      pollfd fd{output_, POLLIN, 0};
      const int result = poll(&fd, 1, static_cast<int>(left));
      if (result < 0 && errno == EINTR) continue;
      require(result > 0, "fixture peer did not respond");
      const auto count = read(output_, buffer, sizeof(buffer));
      if (count < 0 && errno == EINTR) continue;
      require(count > 0, "fixture peer exited before response");
#endif
      pending_.append(buffer, static_cast<size_t>(count));
      require(pending_.size() < (1U << 20), "fixture peer response exceeds bound");
    }
  }
  void stop() {
#ifdef _WIN32
    input_.reset();
    output_.reset();
    process_.reset(); // Terminate the contained tree; never wait for Node to notice closed stdin.
#else
    if (pid_ <= 0) return;
    close(input_); close(output_);
    int status;
    while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
    pid_ = -1;
#endif
  }
#ifdef _WIN32
  runtime_test::Process process_;
  runtime_test::Handle input_, output_;
#else
  pid_t pid_ = -1;
  int input_ = -1, output_ = -1;
#endif
  std::string pending_;
};
template<class Encoded>
sp::transport::HttpRequest wire_request(const Encoded& request, const Peer& peer) {
  sp::transport::HttpRequest wire;
  wire.method = request.method;
  // Test-only peer origin override; descriptor path/headers/body still pass through the encoder.
  wire.url = "http://127.0.0.1:" + std::to_string(peer.port) + request.path;
  for (const auto& [name, value] : request.headers) wire.headers.push_back({name, value});
  wire.body = request.body;
  wire.deadline = Clock::now() + std::chrono::seconds(5);
  return wire;
}
void verify_replay_gates(const Fixture& fixture, const sp::descriptor::ValidatedDescriptor& descriptor,
                         const Captures& captures, Peer& peer, sp::transport::Transport& transport) {
  if (!fixture.root().get("replay_from").valid()) return;
  const auto original = messages_request(fixture, captures);
  const auto native = std::find_if(original.messages.begin(), original.messages.end(), [](const auto& message) { return !!message.native; });
  require(native != original.messages.end(), "replay fixture lacks captured native message");
  const size_t index = static_cast<size_t>(native - original.messages.begin());
  size_t controls = 0;
  auto reject = [&](std::string_view name, const sp::messages::Request& changed,
                    const sp::descriptor::ValidatedDescriptor& selected) {
    auto encoded = sp::messages::encode(selected, changed, fixture.streaming);
    if (auto* accepted = std::get_if<sp::messages::EncodedRequest>(&encoded)) {
      transport.start(wire_request(*accepted, peer), {{}, {}, [](const auto&) {}}).join();
      throw std::runtime_error("replay mutation reached dispatch: " + std::string(name));
    }
    require(std::get<sp::Error>(encoded).kind == sp::ErrorKind::ReplayIneligible, "replay mutation has wrong rejection class: " + std::string(name));
    auto report = peer.stats();
    require(report.root().get("request_count").as_uint() == 0 && report.root().get("unexpected_requests").as_uint() == 0,
            "rejected replay emitted a wire request");
    ++controls;
  };
  auto attempt = [&](std::string_view name, auto mutate) {
    auto changed = original;
    mutate(changed);
    reject(name, changed, descriptor);
  };
  attempt("model", [](auto& request) { request.model += "-foreign"; });
  attempt("account", [](auto& request) { request.account_scope += "-foreign"; });
  attempt("system", [](auto& request) { request.system += "Changed instructions."; });
  attempt("prefix", [](auto& request) { std::get<sp::Text>(request.messages.front().parts.front()).value += "Changed prefix."; });
  attempt("native provenance removed", [&](auto& request) { request.messages[index].native.reset(); });
  if (!original.tools.empty()) attempt("tool definition", [](auto& request) { request.tools.front().name += "_changed"; });
  if (native->parts.size() > 1)
    attempt("native block ordering", [&](auto& request) { std::swap(request.messages[index].parts.front(), request.messages[index].parts.back()); });
  for (size_t part = 0; part < native->parts.size(); ++part) {
    if (std::holds_alternative<sp::Thinking>(native->parts[part])) {
      attempt("thinking text", [&](auto& request) { std::get<sp::Thinking>(request.messages[index].parts[part]).text += "tampered"; });
      attempt("thinking signature", [&](auto& request) { std::get<sp::Thinking>(request.messages[index].parts[part]).signature = "tampered"; });
    } else if (std::holds_alternative<sp::RedactedThinking>(native->parts[part])) {
      attempt("redacted bytes", [&](auto& request) { std::get<sp::RedactedThinking>(request.messages[index].parts[part]).data += "tampered"; });
    } else if (std::holds_alternative<sp::ToolCall>(native->parts[part])) {
      attempt("tool input", [&](auto& request) {
        std::get<sp::ToolCall>(request.messages[index].parts[part]).input =
            std::make_shared<const sp::json::Document>(parse("{\"tampered\":true}"));
      });
    } else if (std::holds_alternative<sp::ServerToolResult>(native->parts[part])) {
      attempt("server native result", [&](auto& request) {
        std::get<sp::ServerToolResult>(request.messages[index].parts[part]).content =
            std::make_shared<const sp::json::Document>(parse("{\"tampered\":true}"));
      });
    }
  }
  // A second admitted origin is still not allowed to replay this origin's native response.
  auto foreign = sp::descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"foreign-messages","family":"anthropic.messages","connection":{"base_url":"https://foreign.example.test","paths":{"buffered":"/v1/messages","streaming":"/v1/messages"}}})");
  require(std::holds_alternative<sp::descriptor::ValidatedDescriptor>(foreign), "foreign-origin control is not admitted");
  reject("origin", original, std::get<sp::descriptor::ValidatedDescriptor>(foreign));
  std::cout << "[PASS] replay gates " << fixture.id << " mutations=" << controls << " requests=0\n";
}
void run_wire(const Fixture& fixture, const sp::descriptor::ValidatedDescriptor& descriptor, const PreparedRequest& request,
              Peer& peer, sp::transport::Transport& transport, const Captures& sources, Captures* captures) {
  // The independent peer verifies embedded descriptor digests when loading the corpus.
  if (!fixture.descriptor_override)
    require(string(fixture.root().get("descriptor_digest")) == peer.digest, "descriptor digest does not match corpus");
  peer.arm(fixture);
  verify_replay_gates(fixture, descriptor, sources, peer, transport);
  Pipeline pipeline(descriptor, fixture.streaming, request.context);
  size_t outcomes = 0;
  auto operation = transport.start(wire_request(request, peer), {
    {}, [&](std::string_view bytes) { pipeline.bytes(bytes); return true; },
    [&](const sp::transport::Result& result) {
      ++outcomes;
      const bool normal = result.status == sp::transport::Status::Completed && result.http_status >= 200 && result.http_status < 300;
      auto error = sp::ErrorKind::Truncated;
      if (result.status == sp::transport::Status::Cancelled) error = sp::ErrorKind::Cancelled;
      else if (result.status == sp::transport::Status::DeadlineExceeded) error = sp::ErrorKind::DeadlineExceeded;
      else if (result.http_status >= 300) error = sp::ErrorKind::RemoteFailure;
      pipeline.finish(normal, error);
    }});
  const auto result = operation.join();
  require(result.failure != sp::transport::FailureKind::CallbackError, fixture.id + ": pipeline callback threw");
  require(outcomes == 1, fixture.id + ": transport outcome count differs from one");
  verify(fixture, pipeline);
  capture(fixture, pipeline, captures);
  auto report = peer.stats();
  const auto stats = report.root();
  require(stats.get("request_count").as_uint() == 1 && stats.get("consumed").as_bool(), fixture.id + ": missing/extra wire request");
  require(stats.get("mismatches").size() == 0 && stats.get("unexpected_requests").as_uint() == 0, fixture.id + ": request matcher failed: " + stats.dump());
  require(stats.get("oracle_error").as_string().empty() && !stats.get("active").as_bool(), fixture.id + ": peer schedule not completed");
  require(stats.get("close_fired").as_string() == fixture.close, fixture.id + ": scripted close did not fire");
  const bool fault = fixture.close == "short_body" || fixture.close == "eof_unmarked" || fixture.close == "rst";
  require(stats.get("fault_fired").as_bool() == fault, fixture.id + ": injected framing fault not witnessed");
  std::cout << "[PASS] wire " << fixture.id << " requests=1 close=" << fixture.close << '\n';
}
void verify_matcher(const Fixture& fixture, const PreparedRequest& prepared, Peer& peer, sp::transport::Transport& transport) {
  for (std::string_view mutation : {"method", "path", "header:content-type", "body"}) {
    peer.arm(fixture, true);
    auto request = wire_request(prepared, peer);
    if (mutation == "method") request.method = "PUT";
    else if (mutation == "path") request.url += "/unexpected";
    else if (mutation == "body") request.body = "{\"model\":\"wrong-model\"}";
    else for (auto& header : request.headers) if (header.name == "Content-Type") header.value = "text/plain";
    auto result = transport.start(std::move(request), {{}, {}, [](const auto&) {}}).join();
    auto report = peer.stats();
    bool rejected = false;
    for (auto value : report.root().get("mismatches").elements()) if (value.as_string() == mutation) rejected = true;
    require(result.http_status == 400 && rejected && !report.root().get("consumed").as_bool(), "request matcher failed negative control");
  }
  peer.arm(fixture, true);
  auto unused = peer.command("{\"arm\":" + sp::json::quote(fixture.id) + '}');
  require(unused.root().get("error").is_string(), "unused interaction was silently replaced");
  peer.arm(fixture, true);
  for (int i = 0; i < 2; ++i) transport.start(wire_request(prepared, peer), {{}, {}, [](const auto&) {}}).join();
  auto repeated = peer.stats();
  require(repeated.root().get("request_count").as_uint() == 2 && repeated.root().get("unexpected_requests").as_uint() == 1, "repeat playback was not refused");
  std::cout << "[PASS] wire matcher rejects method/path/semantic-header/body mutations, unused fixtures and repeat playback\n";
}
} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  runtime_test::Arguments arguments(argc, argv);
  argc = arguments.argc(); argv = arguments.argv();
#endif
  if (argc < 5 || argc > 6) { std::cerr << "usage: sp_fixture_runner <node> <peer.mjs> <fixture-dir> <descriptor> [--offline-only]\n"; return 2; }
#ifndef _WIN32
  std::signal(SIGPIPE, SIG_IGN);
#endif
  try {
    const auto directory = std::filesystem::u8path(argv[3]);
    const auto descriptor_path = std::filesystem::u8path(argv[4]);
    auto loaded = sp::descriptor::load(read_file(descriptor_path));
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&loaded)) throw std::runtime_error("descriptor rejected at " + error->pointer + ": " + error->expected);
    const auto& descriptor = std::get<sp::descriptor::ValidatedDescriptor>(loaded);
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) if (entry.is_regular_file() && entry.path().extension() == ".json") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    require(!files.empty(), "empty fixture corpus");
    std::vector<Fixture> fixtures;
    fixtures.reserve(files.size());
    for (const auto& file : files) fixtures.push_back(load_fixture(file));
    std::set<std::string> replay_sources;
    for (const auto& fixture : fixtures)
      if (auto source = fixture.root().get("replay_from"); source.valid()) replay_sources.insert(string(source));
    Captures offline_captures;
    const auto first_request = prepare_request(fixtures.front(), fixtures.front().configuration(descriptor), offline_captures);
    verify_close_precedence(descriptor, first_request.context);
    std::map<std::string, std::pair<std::string, unsigned>> parity;
    size_t partitions = 0;
    for (const auto& fixture : fixtures) {
      const auto& selected_descriptor = fixture.configuration(descriptor);
      const auto prepared = prepare_request(fixture, selected_descriptor, offline_captures);
      const auto expected = offline(fixture, selected_descriptor, prepared.context, {},
                                    replay_sources.contains(fixture.id) ? &offline_captures : nullptr);
      if (fixture.streaming) {
        const size_t cap = std::min<size_t>(fixture.body.size(), 4096);
        for (size_t cut = 0; cut <= cap; ++cut) {
          require(offline(fixture, selected_descriptor, prepared.context, {cut}) == expected, fixture.id + ": single split changed outcome");
          ++partitions;
        }
        std::vector<size_t> bytewise;
        for (size_t i = 0; i < fixture.body.size(); ++i) bytewise.push_back(i);
        require(offline(fixture, selected_descriptor, prepared.context, bytewise) == expected, fixture.id + ": bytewise partition changed outcome");
        std::mt19937 random(12345);
        for (unsigned trial = 0; trial < 8; ++trial) {
          std::vector<size_t> cuts;
          for (size_t i = random() % 17; i < fixture.body.size(); i += 1 + random() % 17) cuts.push_back(i);
          require(offline(fixture, selected_descriptor, prepared.context, cuts) == expected, fixture.id + ": multiway partition changed outcome");
          ++partitions;
        }
      }
      if (!fixture.parity_group.empty()) {
        auto [it, fresh] = parity.try_emplace(fixture.parity_group, expected, 0);
        require(fresh || it->second.first == expected, fixture.parity_group + ": buffered/SSE projection mismatch");
        it->second.second |= fixture.streaming ? 2 : 1;
      }
      std::cout << "[PASS] golden " << fixture.id << '\n';
    }
    for (const auto& [name, value] : parity) require(value.second == 3, name + ": parity group lacks a transport");
    if (argc == 6) require(std::string_view(argv[5]) == "--offline-only", "unknown runner argument");
    else {
      Peer peer(argv[1], argv[2], directory, descriptor_path);
      sp::transport::Transport transport;
      Captures wire_captures;
      for (const auto& fixture : fixtures) {
        const auto& selected = fixture.configuration(descriptor);
        const auto prepared = prepare_request(fixture, selected, wire_captures);
        run_wire(fixture, selected, prepared, peer, transport, wire_captures,
                 replay_sources.contains(fixture.id) ? &wire_captures : nullptr);
      }
      verify_matcher(fixtures.front(), first_request, peer, transport);
    }
    std::cout << "FIXTURE_PASS family=" << descriptor.family() << " fixtures=" << fixtures.size() << " partitions=" << partitions
              << " parity_groups=" << parity.size() << " wire=" << (argc == 5 ? "yes" : "no") << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FIXTURE_FAIL: " << error.what() << '\n';
    return 1;
  }
}
