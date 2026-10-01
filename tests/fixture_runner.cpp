// M2 fixture runner: independent golden projections plus a real model-free HTTP peer.
// Usage: sp_fixture_runner <node> <chat_fixture_server.mjs> <fixture-directory> [--offline-only]
#include "codecs/chat.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include "transport/http_transport.h"
#include "transport/sse_framer.h"

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
#include <poll.h>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
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
  require(static_cast<bool>(input), "cannot read fixture file: " + path.string());
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
    case sp::ErrorKind::Transport: return "Transport";
    case sp::ErrorKind::ProtocolCorrupt: return "ProtocolCorrupt";
    case sp::ErrorKind::Truncated: return "Truncated";
    case sp::ErrorKind::RemoteFailure: return "RemoteFailure";
    case sp::ErrorKind::Cancelled: return "Cancelled";
    case sp::ErrorKind::DeadlineExceeded: return "DeadlineExceeded";
    case sp::ErrorKind::ResourceLimit: return "ResourceLimit";
    case sp::ErrorKind::Misuse: return "Misuse";
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
  return "{\"kind\":" + sp::json::quote(stop_name(reason.kind)) + ",\"raw\":" + sp::json::quote(reason.raw) + "}";
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
  require(string(root.get("transport").get("scheme")) == "http", "M2 fixture peer is loopback HTTP only");
  require(string(root.get("provenance").get("kind")) == "synthetic", "M2 corpus must not masquerade as vendor captures");
  require(root.get("expect").get("request_count").as_uint() == 1, "each M2 attempt expects one request");
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
    else require(kind == "delay_ms", "unsupported M2 fixture entry");
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
struct Pipeline {
  size_t terminal_events = 0, events = 0;
  sp::Accumulator accumulator;
  sp::chat::Codec codec;
  sp::transport::SseFramer framer;
  sp::transport::SseFramer::Sink sink;
  std::string buffered;
  bool streaming;
  Pipeline(const sp::descriptor::ValidatedDescriptor& descriptor, bool is_streaming)
      : accumulator({}, [&](const sp::Event& event) {
          ++events;
          if (std::holds_alternative<sp::Commit>(event) || std::holds_alternative<sp::Fail>(event)) ++terminal_events;
        }), codec(descriptor, is_streaming ? sp::chat::Mode::Sse : sp::chat::Mode::Buffered, accumulator),
        sink([&](const auto& frame) { return codec.frame(frame.event, frame.data); }), streaming(is_streaming) {}
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
    } else if (!accumulator.terminal()) codec.buffered(buffered, {normal, error});
    codec.finish({normal, error});
    require(accumulator.outcome().has_value(), "missing terminal outcome");
    require(terminal_events == 1, "outcome notification count is not one");
    const size_t before = events;
    codec.finish({false, sp::ErrorKind::Cancelled});
    if (streaming) codec.frame("message", "[DONE]");
    require(events == before, "callbacks after outcome");
  }
};

void verify_close_precedence(const sp::descriptor::ValidatedDescriptor& descriptor) {
  for (auto error : {sp::ErrorKind::Cancelled, sp::ErrorKind::DeadlineExceeded, sp::ErrorKind::Truncated}) {
    Pipeline buffered(descriptor, false);
    buffered.bytes("{\"choices\":[");
    buffered.finish(false, error);
    require(std::get<sp::Failure>(*buffered.accumulator.outcome()).error.kind == error,
            "buffered EOF parsing hid transport failure");
    Pipeline sse(descriptor, true);
    sse.bytes("data: \xe2");
    sse.finish(false, error);
    require(std::get<sp::Failure>(*sse.accumulator.outcome()).error.kind == error,
            "SSE EOF validation hid transport failure");
  }
  Pipeline normal(descriptor, true);
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
std::string offline(const Fixture& fixture, const sp::descriptor::ValidatedDescriptor& descriptor, const std::vector<size_t>& cuts) {
  Pipeline pipeline(descriptor, fixture.streaming);
  size_t offset = 0;
  for (size_t cut : cuts) {
    require(cut >= offset && cut <= fixture.body.size(), "invalid partition");
    pipeline.bytes(std::string_view(fixture.body).substr(offset, cut - offset));
    offset = cut;
  }
  pipeline.bytes(std::string_view(fixture.body).substr(offset));
  const bool normal = fixture.close == "content_length_met" || fixture.close == "chunked_terminator";
  pipeline.finish(normal);
  return verify(fixture, pipeline);
}

class Peer {
 public:
  Peer(const char* node, const char* script, const std::filesystem::path& fixtures, const std::filesystem::path& descriptor) {
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
    size_t sent = 0;
    while (sent < text.size()) {
      const auto count = write(input_, text.data() + sent, text.size() - sent);
      if (count < 0 && errno == EINTR) continue;
      require(count > 0, "fixture peer control write failed");
      sent += static_cast<size_t>(count);
    }
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
      pollfd fd{output_, POLLIN, 0};
      const int result = poll(&fd, 1, static_cast<int>(left));
      if (result < 0 && errno == EINTR) continue;
      require(result > 0, "fixture peer did not respond");
      char buffer[4096];
      const auto count = read(output_, buffer, sizeof(buffer));
      if (count < 0 && errno == EINTR) continue;
      require(count > 0, "fixture peer exited before response");
      pending_.append(buffer, static_cast<size_t>(count));
      require(pending_.size() < (1U << 20), "fixture peer response exceeds bound");
    }
  }
  void stop() {
    if (pid_ <= 0) return;
    close(input_); close(output_);
    int status;
    while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
    pid_ = -1;
  }
  pid_t pid_ = -1;
  int input_ = -1, output_ = -1;
  std::string pending_;
};
sp::transport::HttpRequest wire_request(const Fixture& fixture, const sp::descriptor::ValidatedDescriptor& descriptor, const Peer& peer) {
  auto encoded = sp::chat::encode(descriptor, input_request(fixture), fixture.streaming);
  if (auto error = std::get_if<sp::Error>(&encoded)) throw std::runtime_error("request encode failed: " + error->safe_message);
  auto request = std::get<sp::chat::EncodedRequest>(std::move(encoded));
  sp::transport::HttpRequest wire;
  wire.method = std::move(request.method);
  // Test-only peer origin override; descriptor path/headers/body still pass through the encoder.
  wire.url = "http://127.0.0.1:" + std::to_string(peer.port) + request.path;
  for (auto& [name, value] : request.headers) wire.headers.push_back({std::move(name), std::move(value)});
  wire.body = std::move(request.body);
  wire.deadline = Clock::now() + std::chrono::seconds(5);
  return wire;
}
void run_wire(const Fixture& fixture, const sp::descriptor::ValidatedDescriptor& descriptor, Peer& peer, sp::transport::Transport& transport) {
  // The independent peer verifies embedded descriptor digests when loading the corpus.
  if (!fixture.descriptor_override)
    require(string(fixture.root().get("descriptor_digest")) == peer.digest, "descriptor digest does not match corpus");
  peer.arm(fixture);
  Pipeline pipeline(descriptor, fixture.streaming);
  size_t outcomes = 0;
  auto operation = transport.start(wire_request(fixture, descriptor, peer), {
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
void verify_matcher(const Fixture& fixture, const sp::descriptor::ValidatedDescriptor& descriptor, Peer& peer, sp::transport::Transport& transport) {
  for (std::string_view mutation : {"method", "path", "header:content-type", "body"}) {
    peer.arm(fixture, true);
    auto request = wire_request(fixture, descriptor, peer);
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
  for (int i = 0; i < 2; ++i) transport.start(wire_request(fixture, descriptor, peer), {{}, {}, [](const auto&) {}}).join();
  auto repeated = peer.stats();
  require(repeated.root().get("request_count").as_uint() == 2 && repeated.root().get("unexpected_requests").as_uint() == 1, "repeat playback was not refused");
  std::cout << "[PASS] wire matcher rejects method/path/semantic-header/body mutations, unused fixtures and repeat playback\n";
}
} // namespace

int main(int argc, char** argv) {
  if (argc < 4 || argc > 5) { std::cerr << "usage: sp_fixture_runner <node> <peer.mjs> <fixture-dir> [--offline-only]\n"; return 2; }
  std::signal(SIGPIPE, SIG_IGN);
  try {
    const std::filesystem::path directory = argv[3];
    const auto descriptor_path = directory.parent_path() / "openai-chat-descriptor.json";
    auto loaded = sp::descriptor::load(read_file(descriptor_path));
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&loaded)) throw std::runtime_error("descriptor rejected at " + error->pointer + ": " + error->expected);
    const auto& descriptor = std::get<sp::descriptor::ValidatedDescriptor>(loaded);
    verify_close_precedence(descriptor);
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) if (entry.is_regular_file() && entry.path().extension() == ".json") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    require(!files.empty(), "empty fixture corpus");
    std::vector<Fixture> fixtures;
    fixtures.reserve(files.size());
    for (const auto& file : files) fixtures.push_back(load_fixture(file));
    std::map<std::string, std::pair<std::string, unsigned>> parity;
    size_t partitions = 0;
    for (const auto& fixture : fixtures) {
      const auto& selected_descriptor = fixture.configuration(descriptor);
      const auto expected = offline(fixture, selected_descriptor, {});
      if (fixture.streaming) {
        const size_t cap = std::min<size_t>(fixture.body.size(), 4096);
        for (size_t cut = 0; cut <= cap; ++cut) {
          require(offline(fixture, selected_descriptor, {cut}) == expected, fixture.id + ": single split changed outcome");
          ++partitions;
        }
        std::vector<size_t> bytewise;
        for (size_t i = 0; i < fixture.body.size(); ++i) bytewise.push_back(i);
        require(offline(fixture, selected_descriptor, bytewise) == expected, fixture.id + ": bytewise partition changed outcome");
        std::mt19937 random(12345);
        for (unsigned trial = 0; trial < 8; ++trial) {
          std::vector<size_t> cuts;
          for (size_t i = random() % 17; i < fixture.body.size(); i += 1 + random() % 17) cuts.push_back(i);
          require(offline(fixture, selected_descriptor, cuts) == expected, fixture.id + ": multiway partition changed outcome");
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
    if (argc == 5) require(std::string_view(argv[4]) == "--offline-only", "unknown runner argument");
    else {
      Peer peer(argv[1], argv[2], directory, descriptor_path);
      sp::transport::Transport transport;
      for (const auto& fixture : fixtures) run_wire(fixture, fixture.configuration(descriptor), peer, transport);
      verify_matcher(fixtures.front(), fixtures.front().configuration(descriptor), peer, transport);
    }
    std::cout << "M2_FIXTURE_PASS fixtures=" << fixtures.size() << " partitions=" << partitions << " parity_groups=" << parity.size() << " wire=" << (argc == 4 ? "yes" : "no") << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "M2_FIXTURE_FAIL: " << error.what() << '\n';
    return 1;
  }
}
