#pragma once
// A scripted runtime for endpoint tests: an executor that runs posted tasks and due timers on the caller's thread,
// and a transport whose replies a test script writes. A reply honours the runtime's pause and resume, as the real
// transport does.
#include "runtime/testing.h"
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sp::test {
class QueueExecutor final : public runtime::detail::Executor {
 public:
  void post(Task task) override { std::lock_guard lock(mutex_); ready_.push_back(std::move(task)); }
  Timer schedule(runtime::SteadyTime at, Task task) override { std::lock_guard lock(mutex_); timers_.push_back({++last_, at, std::move(task)}); return last_; }
  void cancel(Timer id) noexcept override {
    std::lock_guard lock(mutex_);
    std::erase_if(timers_, [&](const Pending& timer) { return timer.id == id; });
  }
  runtime::SteadyTime now() const noexcept override { return now_; }
  runtime::WallTime wall_now() const noexcept override { return std::chrono::system_clock::now(); }
  bool in_thread() const noexcept override { return active_; }
  void shutdown() noexcept override {}
  void drain() {
    for (int i = 0; i < 100000; ++i) {
      Task task;
      {
        std::lock_guard lock(mutex_);
        if (ready_.empty()) {
          // Due timers (retry backoff without delay) run once nothing else is ready.
          // A moved-from std::function need not be empty, so a fired timer leaves the list.
          for (auto it = timers_.begin(); it != timers_.end(); ++it)
            if (it->at <= now_) { task = std::move(it->task); timers_.erase(it); break; }
          if (!task) return;
        } else { task = std::move(ready_.front()); ready_.pop_front(); }
      }
      active_ = true; task(); active_ = false;
    }
    throw std::runtime_error("executor did not quiesce");
  }
 private:
  std::mutex mutex_;
  std::deque<Task> ready_;
  struct Pending { Timer id; runtime::SteadyTime at; Task task; };
  std::vector<Pending> timers_;
  Timer last_ = 0;
  runtime::SteadyTime now_ = std::chrono::steady_clock::now();
  inline static thread_local bool active_ = false;
};
struct Exchange {
  transport::HttpRequest request;
  transport::Callbacks callbacks;
  transport::Result result{};
  std::deque<std::string> pending{};
  bool ending = false, done = false;
  void head(int status, std::vector<transport::Header> headers) {
    result.http_status = status;
    result.attempt.reached = transport::Stage::ResponseStarted;
    result.attempt.response_head_seen = true;
    transport::ResponseHead value;
    value.status = status;
    value.version = transport::ResponseVersion::Http1_1;
    value.framing = transport::BodyFraming::ContentLength;
    value.headers = std::move(headers);
    callbacks.on_head(value);
  }
  // The body in chunks; a chunk the runtime refuses waits for its resume, as the transport's pause does.
  void reply(int status, std::string_view type, std::string_view body, std::size_t chunk = 65536) {
    head(status, {{"content-type", std::string(type)}});
    for (std::size_t at = 0; at < body.size(); at += chunk) pending.emplace_back(body.substr(at, chunk));
    ending = true;
    flush();
  }
  void flush() {
    while (!done && !pending.empty()) {
      if (!callbacks.on_body(pending.front())) return;
      pending.pop_front();
    }
    if (ending) finish(transport::Status::Completed);
  }
  void finish(transport::Status status, transport::FailureKind failure = transport::FailureKind::None) {
    if (std::exchange(done, true)) return;
    result.status = status;
    result.failure = failure;
    callbacks.on_done(result);
  }
};
class ScriptTransport final : public runtime::detail::AttemptTransport {
  struct Handle final : runtime::detail::Attempt {
    explicit Handle(std::shared_ptr<Exchange> e) : exchange(std::move(e)) {}
    void cancel() noexcept override { exchange->finish(transport::Status::Cancelled); }
    void resume() noexcept override { exchange->flush(); }
    std::shared_ptr<Exchange> exchange;
  };
 public:
  std::unique_ptr<runtime::detail::Attempt> start(transport::HttpRequest request, transport::Callbacks callbacks) override {
    auto exchange = std::make_shared<Exchange>(Exchange{std::move(request), std::move(callbacks)});
    exchange->result.attempt.reached = transport::Stage::RequestStarted;
    exchange->result.attempt.request_body_bytes = static_cast<std::int64_t>(exchange->request.body.size());
    exchanges.push_back(exchange);
    script(*exchange);
    return std::make_unique<Handle>(exchange);
  }
  void shutdown() noexcept override {}
  std::function<void(Exchange&)> script;
  std::vector<std::shared_ptr<Exchange>> exchanges;
};
inline std::string_view header(const transport::HttpRequest& request, std::string_view name) {
  for (const auto& [key, value] : request.headers) if (key == name) return value;
  return {};
}
}  // namespace sp::test
