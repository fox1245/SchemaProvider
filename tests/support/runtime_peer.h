#pragma once
#include "json/json.h"
#ifdef _WIN32
#include "win_owner.h"
#else
#include "posix_owner.h"
#endif

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#ifndef _WIN32
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace runtime_test {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
inline void require(bool yes, std::string_view message) {
  if (!yes) throw std::runtime_error(std::string(message));
}
inline sp::json::Document parse(std::string_view text) {
  auto value = sp::json::parse(text, {1U << 20, 64});
  require(std::holds_alternative<sp::json::Document>(value), "invalid peer JSON");
  return std::get<sp::json::Document>(std::move(value));
}
#ifndef _WIN32
inline void write_all(int fd, std::string_view value) {
  while (!value.empty()) {
    auto n = ::write(fd, value.data(), value.size());
    if (n < 0 && errno == EINTR) continue;
    require(n > 0, "control write failed");
    value.remove_prefix(static_cast<std::size_t>(n));
  }
}
#endif
class Peer {
 public:
#ifdef _WIN32
  // Pipes have no outstanding kernel I/O when a call returns; destruction closes them before the job.
  Peer(const char* node, const char* script) {
    auto spawned = spawn_with_pipes(node, script);
    process_ = std::move(spawned.process);
    in_ = std::move(spawned.to_child);
    out_ = std::move(spawned.from_child);
    auto ready = line();
    port = ready.root().get("port").as_uint();
    require(port != 0, "peer startup failed");
  }
#else
  Peer(const char* node, const char* script) {
    Pipe input, output;
    const auto pid = fork();
    if (pid == 0) {
      input.writer.reset(); output.reader.reset();
      if (!input.reader.redirect_to(STDIN_FILENO) || !output.writer.redirect_to(STDOUT_FILENO)) _exit(126);
      execl(node, node, script, static_cast<char*>(nullptr));
      _exit(127);
    }
    require(pid > 0, "peer fork failed");
    process_ = Process(pid);
    in_ = std::move(input.writer);
    out_ = std::move(output.reader);
    input.reader.reset(); output.writer.reset();
    auto ready = line();
    port = ready.root().get("port").as_uint();
    require(port != 0, "peer startup failed");
  }
#endif
  ~Peer() = default;
  Peer(const Peer&) = delete;
  Peer& operator=(const Peer&) = delete;
  sp::json::Document command(std::string command) {
    write_control(command + '\n');
    auto result = line();
    require(!result.root().get("error").valid(), "peer rejected control command");
    return result;
  }
  std::string arm(std::string_view scenario, std::optional<std::string_view> text = {}) {
    auto model = "runtime-" + std::to_string(++sequence_);
    std::string cmd = "{\"arm\":" + sp::json::quote(model) + ",\"scenario\":" + sp::json::quote(scenario);
    if (text) cmd += ",\"text\":" + sp::json::quote(*text);
    command(cmd + '}');
    return model;
  }
  sp::json::Document stats(const std::string& model) { return command("{\"model\":" + sp::json::quote(model) + '}'); }
  void wait(const std::string& model, std::string_view key, std::size_t count = 1) {
    command("{\"model\":" + sp::json::quote(model) + ",\"wait\":{" + sp::json::quote(key) + ':' + std::to_string(count) + "}}");
  }
  void release(const std::string& model) { command("{\"model\":" + sp::json::quote(model) + ",\"release\":true}"); }
  void count(const std::string& model, std::size_t expected, std::size_t faults = 0) {
    auto report = stats(model); auto root = report.root();
    require(root.get("count").as_uint() == expected, "incorrect wire request count");
    require(root.get("faults").as_uint() == faults, "expected peer fault did not fire");
    require(root.get("invalid").as_uint() == 0 && root.get("unexpected").as_uint() == 0, "peer rejected request semantics or credentials");
  }
  std::uint64_t port = 0;
 private:
#ifdef _WIN32
  void write_control(const std::string& text) { write_pipe(in_.get(), text); }
  // Returns 0 only when the bounded wait found no bytes. No stack buffer or OVERLAPPED can outlive it.
  std::size_t read_some(char* buffer, std::size_t size, long long milliseconds) {
    const auto got = read_pipe(out_.get(), buffer, size, milliseconds);
    require(got > 0, "peer control did not respond");
    return got;
  }
#else
  void write_control(const std::string& text) { write_all(in_.get(), text); }
  // Returns the number of bytes read, or 0 when the wait or read was interrupted and must be retried.
  std::size_t read_some(char* buffer, std::size_t size, long long milliseconds) {
    pollfd fd{out_.get(), POLLIN, 0};
    auto ready = poll(&fd, 1, static_cast<int>(milliseconds));
    if (ready < 0 && errno == EINTR) return 0;
    require(ready > 0, "peer control did not respond");
    auto n = ::read(out_.get(), buffer, size);
    if (n < 0 && errno == EINTR) return 0;
    require(n > 0, "peer exited before reply");
    return static_cast<std::size_t>(n);
  }
#endif
  sp::json::Document line() {
    auto deadline = Clock::now() + 15s;
    for (;;) {
      if (auto pos = pending_.find('\n'); pos != std::string::npos) {
        auto result = parse(std::string_view(pending_).substr(0, pos)); pending_.erase(0, pos + 1); return result;
      }
      auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      require(left > 0, "peer control timed out");
      char buffer[4096];
      const auto n = read_some(buffer, sizeof(buffer), left);
      if (n == 0) continue;
      pending_.append(buffer, n);
      require(pending_.size() <= (1U << 20), "peer reply exceeded bound");
    }
  }
  Process process_;
#ifdef _WIN32
  Handle in_, out_;
#else
  Fd in_, out_;
#endif
  std::size_t sequence_ = 0;
  std::string pending_;
};
} // namespace runtime_test
