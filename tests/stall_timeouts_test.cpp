// Stall bounds of the transport (connect, first byte, idle): pin the default (all disabled), the three
// bounds themselves, their interaction with the deadline/cancel/teardown, the HTTP/2 and TLS cells, and
// the runtime's typed outcome per provider family. Peers are local: a Node process (stall_server.mjs)
// and, for the TCP black hole and a half-written response head, raw loopback sockets owned here.
//
// Usage: sp_stall_tests <node> <stall_server.mjs> <suite>
//   suites: transport | tls_h2 | connect | runtime
// A suite whose environment cannot provide its peer prints STALL_SKIPPED (ctest then reports a skip).
#include "descriptor/descriptor.h"
#include "json/json.h"
#include "runtime/client.h"
#include "support/runtime_peer.h"
#include "transport/http_transport.h"
#include "configuration/runtime_policy.h"
#include "sp/config_defaults.h"

#include "support/portable.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;
namespace tr = sp::transport;
using runtime_test::Peer;

#define CHECK(cond) \
  do { if (!(cond)) throw std::runtime_error(std::string(__FILE__ ":") + std::to_string(__LINE__) + " " #cond); } while (0)

double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

struct Skip { std::string why; };

// ---------------------------------------------------------------------------------------------
// Raw loopback sockets
// ---------------------------------------------------------------------------------------------
// Sockets are owned through portable::Socket (POSIX descriptors, Winsock SOCKETs).
using portable::PollFd;
using portable::Socket;

sockaddr_in loopback(std::uint16_t port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return addr;
}

// A listener whose accept queue is full, so further SYNs are dropped and a connect() neither completes
// nor fails: a TCP black hole that needs no routable address. The Linux queue limit is backlog + 1, so a
// listen backlog of 0 is saturated by the first connection that nobody accepts.
class Blackhole {
 public:
  Blackhole() {
    listener_ = Socket(::socket(AF_INET, SOCK_STREAM, 0));
    CHECK(listener_);
    auto addr = loopback(0);
    CHECK(::bind(listener_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    CHECK(::listen(listener_.get(), 0) == 0);
    portable::socklen len = sizeof addr;
    CHECK(::getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    port = ntohs(addr.sin_port);
    // Fill the queue: connections that complete are parked; the first one that stays pending proves
    // saturation. Every later connect hangs until the listener goes away.
    for (int i = 0; i < 6; ++i) {
      Socket probe = begin_connect();
      if (!completes_within(probe, 250ms)) {
        pending_.push_back(std::move(probe));
        break;
      }
      parked_.push_back(std::move(probe));
    }
    // Confirm: a fresh connect must not complete over a longer window.
    Socket confirm = begin_connect();
    if (completes_within(confirm, 400ms)) throw Skip{"this kernel does not drop SYNs for a full accept queue"};
  }
  std::uint16_t port = 0;
 private:
  Socket begin_connect() const {
    Socket c(::socket(AF_INET, SOCK_STREAM, 0));
    CHECK(c);
    CHECK(portable::set_nonblocking(c.get()));
    auto addr = loopback(port);
    const int rc = ::connect(c.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    CHECK(rc == 0 || portable::connect_in_progress());
    return c;
  }
  static bool completes_within(const Socket& c, Ms limit) {
    PollFd p{c.get(), POLLOUT, 0};
    const auto ready = portable::poll(&p, 1, static_cast<int>(limit.count()));
    CHECK(ready >= 0);
    if (ready == 0) return false;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
      throw Skip{"full accept queue rejects connects instead of leaving them pending"};
    CHECK(p.revents & POLLOUT);
    int error = 0;
    portable::socklen size = sizeof(error);
    CHECK(::getsockopt(c.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) == 0);
    if (error != 0) throw Skip{"full accept queue rejects connects instead of leaving them pending"};
    return true;
  }
  Socket listener_;
  std::vector<Socket> parked_, pending_;
};

// Accepts connections, reads each request head, writes a fragment of a status line and then stays
// silent until the client closes. Counts requests.
class HalfHeadServer {
 public:
  HalfHeadServer() {
    listener_ = Socket(::socket(AF_INET, SOCK_STREAM, 0));
    CHECK(listener_);
    int one = 1;
    ::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
    auto addr = loopback(0);
    CHECK(::bind(listener_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    CHECK(::listen(listener_.get(), 16) == 0);
    CHECK(portable::set_nonblocking(listener_.get()));
    portable::socklen len = sizeof addr;
    CHECK(::getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    port = ntohs(addr.sin_port);
    thread_ = std::thread([this] { run(); });
  }
  ~HalfHeadServer() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }
  std::uint16_t port = 0;
  std::atomic<int> requests{0};
 private:
  void run() {
    std::vector<Socket> held;
    while (!stop_) {
      PollFd p{listener_.get(), POLLIN, 0};
      if (portable::poll(&p, 1, 50) <= 0 || !(p.revents & POLLIN) || stop_) continue;
      Socket c(::accept(listener_.get(), nullptr, nullptr));
      if (!c) continue;
      if (!portable::set_nonblocking(c.get())) continue;
      std::string seen;
      char buffer[1024];
      const auto deadline = Clock::now() + 2s;
      while (!stop_ && Clock::now() < deadline && seen.size() <= (64U << 10) &&
             seen.find("\r\n\r\n") == std::string::npos) {
        PollFd q{c.get(), POLLIN, 0};
        const auto ready = portable::poll(&q, 1, 50);
        if (ready < 0 || (q.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        if (ready == 0) continue;
        const auto n = portable::receive(c.get(), buffer, sizeof buffer);
        if (n <= 0) break;
        seen.append(buffer, static_cast<std::size_t>(n));
      }
      if (stop_ || seen.size() > (64U << 10) || seen.find("\r\n\r\n") == std::string::npos) continue;
      ++requests;
      // An incomplete status line: libcurl's header callback runs per complete line, so no head yet.
      constexpr std::string_view fragment = "HTTP/1.1 20";
      std::string_view remaining = fragment;
      while (!remaining.empty() && !stop_ && Clock::now() < deadline) {
        PollFd q{c.get(), POLLOUT, 0};
        if (portable::poll(&q, 1, 50) <= 0) continue;
        const auto n = portable::send_quiet(c.get(), remaining.data(), remaining.size());
        if (n <= 0) break;
        remaining.remove_prefix(static_cast<std::size_t>(n));
      }
      if (!remaining.empty()) continue;
      held.push_back(std::move(c));
    }
  }
  std::atomic<bool> stop_{false};
  Socket listener_;
  std::thread thread_;
};

// ---------------------------------------------------------------------------------------------
// Transport helpers
// ---------------------------------------------------------------------------------------------
struct Ports {
  std::uint64_t http = 0, h2c = 0, tls = 0, handshake = 0;
  std::string ca;
};
Ports ports_of(Peer& peer) {
  auto doc = peer.command("{\"ports\":true}");
  const auto root = doc.root();
  return {root.get("port").as_uint(), root.get("h2c").as_uint(), root.get("tls").as_uint(),
          root.get("handshake").as_uint(), std::string(root.get("ca").as_string())};
}

struct Fetch {
  Clock::time_point start = Clock::now();
  std::atomic<std::int64_t> done_ns{0};
  std::atomic<bool> done{false};
  std::atomic<int> body_calls{0};
  std::atomic<int> done_calls{0};
  std::atomic<int> callbacks_after_done{0};
  std::atomic<std::size_t> body_bytes{0};
  std::atomic<bool> head{false};
  std::atomic<bool> refuse_body{false};  // on_body returns false (backpressure) while set
  std::weak_ptr<int> callback_storage;
  tr::Result result;
  double ms() const {
    return std::chrono::duration<double, std::milli>(Clock::time_point(Clock::duration(done_ns.load())) - start).count();
  }
};

struct Req {
  std::string url;
  std::string body = "{}";
  Clock::duration deadline = 8s;
  std::optional<Ms> connect = std::nullopt, first_byte = std::nullopt, idle = std::nullopt;
  tr::HttpVersion version = tr::HttpVersion::Http1_1;
  std::string ca{};
};

tr::Operation begin(tr::Transport& transport, const Req& r, const std::shared_ptr<Fetch>& fetch) {
  tr::HttpRequest request;
  request.method = "POST";
  request.url = r.url;
  request.body = r.body;
  request.headers = {{"content-type", "application/json"}};
  request.deadline = Clock::now() + r.deadline;
  request.connect_timeout = r.connect;
  request.first_byte_timeout = r.first_byte;
  request.idle_timeout = r.idle;
  request.http_version = r.version;
  request.ca_file = r.ca;
  auto callback_storage = std::make_shared<int>(0);
  fetch->callback_storage = callback_storage;
  tr::Callbacks callbacks;
  callbacks.on_head = [fetch](const tr::ResponseHead&) {
    if (fetch->done) ++fetch->callbacks_after_done;
    fetch->head = true;
  };
  callbacks.on_body = [fetch](std::string_view bytes) {
    if (fetch->done) ++fetch->callbacks_after_done;
    ++fetch->body_calls;
    if (fetch->refuse_body) return false;
    fetch->body_bytes += bytes.size();
    return true;
  };
  callbacks.on_done = [fetch, callback_storage](const tr::Result& result) {
    if (++fetch->done_calls != 1) return;
    fetch->result = result;
    fetch->done_ns = Clock::now().time_since_epoch().count();
    fetch->done = true;
  };
  fetch->start = Clock::now();
  return transport.start(std::move(request), std::move(callbacks));
}

std::shared_ptr<Fetch> run(tr::Transport& transport, const Req& r) {
  auto fetch = std::make_shared<Fetch>();
  auto op = begin(transport, r, fetch);
  (void)op.join();
  CHECK(fetch->done && fetch->done_calls == 1 && fetch->callbacks_after_done == 0);
  CHECK(fetch->callback_storage.expired());  // Retaining a completed handle must not retain its callbacks.
  return fetch;
}

std::string url_for(std::uint64_t port, const std::string& name, const char* scheme = "http") {
  return std::string(scheme) + "://127.0.0.1:" + std::to_string(port) + "/t/" + name;
}

void expect_failed(const Fetch& f, tr::FailureKind kind, double lo_ms, double hi_ms) {
  const auto& r = f.result;
  if (!(r.status == tr::Status::Failed && r.failure == kind)) {
    std::fprintf(stderr, "    status=%d failure=%d detail=%s ms=%.1f\n", static_cast<int>(r.status),
                 static_cast<int>(r.failure), r.detail.c_str(), f.ms());
  }
  CHECK(r.status == tr::Status::Failed && r.failure == kind);
  CHECK(f.ms() >= lo_ms && f.ms() <= hi_ms);
}
void expect_deadline(const Fetch& f, double deadline_ms, double slack_ms = 900) {
  const auto& r = f.result;
  if (r.status != tr::Status::DeadlineExceeded)
    std::fprintf(stderr, "    status=%d failure=%d detail=%s ms=%.1f\n", static_cast<int>(r.status),
                 static_cast<int>(r.failure), r.detail.c_str(), f.ms());
  CHECK(r.status == tr::Status::DeadlineExceeded && r.failure == tr::FailureKind::None);
  CHECK(f.ms() >= deadline_ms - 5 && f.ms() <= deadline_ms + slack_ms);
}

// What reached the peer's wire is the oracle for request counts and uploaded bytes.
std::uint64_t peer_stat(Peer& peer, const std::string& name, std::string_view key) {
  auto doc = peer.stats(name);
  return doc.root().get(key).as_uint();
}

// ---------------------------------------------------------------------------------------------
// Suite: transport (plain HTTP/1.1)
// ---------------------------------------------------------------------------------------------
struct Env {
  Peer& peer;
  Ports ports;
};

// Default behaviour pinned: with every bound disabled the deadline is the only time bound, however
// lively or silent the peer is.
void pin_default_trickle_runs_to_deadline(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("trickle:100");
  Req r{.url = url_for(e.ports.http, name), .deadline = 900ms};
  auto f = run(transport, r);
  expect_deadline(*f, 900);
  CHECK(f->head && f->result.attempt.response_head_seen);
  CHECK(f->body_calls >= 4);  // the trickle was flowing the whole time
}
void pin_default_head_silent_runs_to_deadline(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("head-silent");
  auto f = run(transport, Req{.url = url_for(e.ports.http, name), .deadline = 700ms});
  expect_deadline(*f, 700);
  CHECK(f->head);
}
void pin_default_never_responding_runs_to_deadline(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("none");
  auto f = run(transport, Req{.url = url_for(e.ports.http, name), .deadline = 700ms});
  expect_deadline(*f, 700);
  CHECK(!f->head);
  CHECK(f->result.attempt.reached >= tr::Stage::RequestStarted);
  e.peer.count(name, 1);
}

void pin_default_comments_run_to_deadline(Env& e) {
  tr::Transport transport;
  auto f = run(transport, Req{.url = url_for(e.ports.http, e.peer.arm("comments:100")), .deadline = 900ms});
  expect_deadline(*f, 900);
  CHECK(f->body_calls >= 4);
}

void idle_request_headers_count_as_activity(Env& e) {
  tr::TransportOptions options;
  options.resolve = [](const std::string&) {
    std::this_thread::sleep_for(400ms);
    return std::vector<std::string>{"127.0.0.1"};
  };
  tr::Transport transport(options);
  const auto name = e.peer.arm("none");
  Req r{.url = "http://stall.test:" + std::to_string(e.ports.http) + "/t/" + name,
        .body = "", .deadline = 8s, .idle = 1000ms};
  auto f = run(transport, r);
  // No response bytes and an empty upload: only sending the request head resets the idle clock.
  expect_failed(*f, tr::FailureKind::IdleTimeout, 1395, 4000);
  CHECK(f->result.attempt.request_body_bytes == 0 && !f->head);
  e.peer.count(name, 1);
}

void idle_response_header_lines_count_as_activity(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("slow-head:100");
  auto f = run(transport, Req{.url = url_for(e.ports.http, name), .deadline = 8s, .idle = 400ms});
  expect_failed(*f, tr::FailureKind::IdleTimeout, 1195, 3000);
  CHECK(f->head && f->body_calls == 0);
}

void idle_head_silent(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("head-silent");
  Req r{.url = url_for(e.ports.http, name), .deadline = 8s, .idle = 400ms};
  auto f = run(transport, r);
  expect_failed(*f, tr::FailureKind::IdleTimeout, 395, 2400);
  CHECK(f->head && f->result.attempt.response_head_seen && f->result.attempt.reached == tr::Stage::ResponseStarted);
  CHECK(f->result.curl_code != 0);
  // The transfer was torn down at once, not left to the peer: the peer sees the connection close.
  e.peer.wait(name, "closed");
  e.peer.count(name, 1);
}
void idle_never_responding_is_idle_not_first_byte(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("none");
  auto f = run(transport, Req{.url = url_for(e.ports.http, name), .deadline = 8s, .idle = 400ms});
  expect_failed(*f, tr::FailureKind::IdleTimeout, 395, 2400);
  CHECK(!f->head && f->result.attempt.reached >= tr::Stage::RequestStarted);
}
void idle_trickle_of_events_is_not_cut(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("trickle:100");
  auto f = run(transport, Req{.url = url_for(e.ports.http, name), .deadline = 1600ms, .idle = 400ms});
  expect_deadline(*f, 1600);  // alive for 4x the idle bound, then the deadline ends it
  CHECK(f->body_calls >= 8);
}
void idle_comments_count_as_activity(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("comments:100");
  auto f = run(transport, Req{.url = url_for(e.ports.http, name), .deadline = 1600ms, .idle = 400ms});
  expect_deadline(*f, 1600);
  CHECK(f->body_calls >= 8);
}
void idle_slow_upload_counts_as_activity(Env& e) {
  tr::Transport transport;
  // The peer drains the request body at ~6 MB/s, far slower than the socket would carry it, so for
  // seconds the only traffic is upload progress. The idle bound is shorter than the whole upload.
  const auto name = e.peer.arm("slow-read:131072:20");
  Req r{.url = url_for(e.ports.http, name), .body = std::string(24u << 20, 'a'), .deadline = 40s, .idle = 1500ms};
  auto f = run(transport, r);
  if (f->result.status != tr::Status::Completed)
    std::fprintf(stderr, "    status=%d failure=%d detail=%s ms=%.1f\n", static_cast<int>(f->result.status),
                 static_cast<int>(f->result.failure), f->result.detail.c_str(), f->ms());
  CHECK(f->result.status == tr::Status::Completed && f->result.http_status == 200);
  CHECK(f->ms() > 1500 * 1.3);  // the upload outlasted the idle bound: the test is meaningful
  CHECK(f->result.attempt.request_body_bytes == static_cast<std::int64_t>(24u << 20));
  CHECK(peer_stat(e.peer, name, "upload_bytes") == (24u << 20));
}
void idle_normal_completion_and_reset_unaffected(Env& e) {
  tr::Transport transport;
  const auto ok = e.peer.arm("ok");
  auto f = run(transport, Req{.url = url_for(e.ports.http, ok), .deadline = 5s, .first_byte = 300ms, .idle = 300ms});
  CHECK(f->result.status == tr::Status::Completed && f->result.failure == tr::FailureKind::None && f->body_bytes == 2);
  const auto reset = e.peer.arm("reset");
  // A new transport, so the reset hits a fresh connection (on the reused one libcurl would try a resend,
  // which the transport refuses as ResendRefused, a path that is unrelated to the stall bounds).
  tr::Transport fresh;
  auto g = run(fresh, Req{.url = url_for(e.ports.http, reset), .deadline = 5s, .connect = 300ms,
                              .first_byte = 300ms, .idle = 300ms});
  if (!(g->result.status == tr::Status::Failed && g->result.failure == tr::FailureKind::Receive))
    std::fprintf(stderr, "    status=%d failure=%d curl=%d detail=%s\n", static_cast<int>(g->result.status),
                 static_cast<int>(g->result.failure), g->result.curl_code, g->result.detail.c_str());
  CHECK(g->result.status == tr::Status::Failed && g->result.failure == tr::FailureKind::Receive);
  CHECK(g->ms() < 300);  // the reset is reported when it happens, not as a timeout
}
void idle_pause_is_not_peer_silence(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("trickle:50");
  auto fetch = std::make_shared<Fetch>();
  fetch->refuse_body = true;  // the first body chunk is refused: libcurl pauses the transfer
  Req r{.url = url_for(e.ports.http, name), .deadline = 10s, .idle = 300ms};
  auto op = begin(transport, r, fetch);
  const auto limit = Clock::now() + 5s;
  while (fetch->body_calls == 0 && Clock::now() < limit) std::this_thread::sleep_for(5ms);
  CHECK(fetch->body_calls >= 1);
  std::this_thread::sleep_for(1000ms);  // > 3x the idle bound, all of it application backpressure
  CHECK(!fetch->done);
  fetch->refuse_body = false;
  op.resume();
  std::this_thread::sleep_for(500ms);  // the stream moves again; still no timeout
  CHECK(!fetch->done);
  op.cancel();
  (void)op.join();
  CHECK(fetch->result.status == tr::Status::Cancelled);
}
void idle_rearms_after_a_pause_ends(Env& e) {
  tr::Transport transport;
  // A valid event, then silence. Refusing the first body chunk pauses the transfer, so the application
  // is idle for longer than the bound while the peer has said everything it will say. After the resume
  // the refused chunk must be delivered and the idle bound must be armed again from that moment, so a
  // watchdog that stayed disabled after the pause would run to the 10 s deadline instead.
  const auto name = e.peer.arm("prefix-silent");
  auto fetch = std::make_shared<Fetch>();
  fetch->refuse_body = true;
  Req r{.url = url_for(e.ports.http, name), .deadline = 10s, .idle = 300ms};
  auto op = begin(transport, r, fetch);
  const auto limit = Clock::now() + 5s;
  while (fetch->body_calls == 0 && Clock::now() < limit) std::this_thread::sleep_for(5ms);
  CHECK(fetch->body_calls >= 1);
  std::this_thread::sleep_for(1000ms);  // more than three idle bounds of application backpressure
  CHECK(!fetch->done && fetch->body_bytes == 0);
  fetch->refuse_body = false;
  const auto resumed = Clock::now();
  op.resume();
  (void)op.join();
  const double since_resume = ms_since(resumed);
  CHECK(fetch->body_bytes > 0);  // the chunk refused before the pause was delivered after the resume
  CHECK(fetch->result.status == tr::Status::Failed && fetch->result.failure == tr::FailureKind::IdleTimeout);
  CHECK(since_resume >= 250 && since_resume < 3000);
}

void first_byte_never_responding(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("none");
  auto f = run(transport, Req{.url = url_for(e.ports.http, name), .deadline = 8s, .first_byte = 400ms});
  expect_failed(*f, tr::FailureKind::FirstByteTimeout, 395, 2400);
  CHECK(!f->head && !f->result.attempt.response_head_seen);
  CHECK(f->result.attempt.reached >= tr::Stage::RequestStarted && f->result.attempt.reached < tr::Stage::ResponseStarted);
  e.peer.wait(name, "closed");
  e.peer.count(name, 1);
}
void first_byte_head_in_time_completes(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("late-head:200");
  auto f = run(transport, Req{.url = url_for(e.ports.http, name), .deadline = 8s, .first_byte = 1500ms});
  CHECK(f->result.status == tr::Status::Completed);
  CHECK(f->ms() >= 195 && f->ms() < 1500);
}
void first_byte_head_too_late_fails(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("late-head:1500");
  auto f = run(transport, Req{.url = url_for(e.ports.http, name), .deadline = 8s, .first_byte = 400ms});
  expect_failed(*f, tr::FailureKind::FirstByteTimeout, 395, 1450);
}
void first_byte_slow_body_after_head_unaffected(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("slow-body:60");  // head at once, 40 bytes over 2.4 s
  auto f = run(transport, Req{.url = url_for(e.ports.http, name), .deadline = 10s, .first_byte = 300ms});
  CHECK(f->result.status == tr::Status::Completed && f->body_bytes == 40);
  CHECK(f->ms() > 1500);
}
void first_byte_half_a_status_line_is_not_a_head(Env& e) {
  (void)e;
  HalfHeadServer server;
  tr::Transport transport;
  Req r{.url = "http://127.0.0.1:" + std::to_string(server.port) + "/x", .deadline = 8s, .first_byte = 400ms};
  auto f = run(transport, r);
  expect_failed(*f, tr::FailureKind::FirstByteTimeout, 395, 2400);
  CHECK(!f->head && server.requests == 1);
}
void first_byte_cleared_by_the_first_header_line(Env& e) {
  tr::Transport transport;
  // The status line arrives at once, the rest of the head takes about 800 ms, and then the body is
  // silent. The first byte is the first response header line, so the bound is satisfied immediately
  // and only the deadline ends the attempt; a bound that waited for the complete head would fire at 400 ms.
  const auto name = e.peer.arm("slow-head:100");
  auto f = run(transport, Req{.url = url_for(e.ports.http, name), .deadline = 1500ms, .first_byte = 400ms});
  expect_deadline(*f, 1500);
}

void deadline_earlier_than_every_bound_wins(Env& e) {
  tr::Transport transport;
  const auto silent = e.peer.arm("head-silent");
  auto f = run(transport, Req{.url = url_for(e.ports.http, silent), .deadline = 500ms, .connect = 3000ms,
                              .first_byte = 3000ms, .idle = 3000ms});
  expect_deadline(*f, 500);
  const auto none = e.peer.arm("none");
  auto g = run(transport, Req{.url = url_for(e.ports.http, none), .deadline = 500ms, .connect = 3000ms,
                              .first_byte = 3000ms, .idle = 3000ms});
  expect_deadline(*g, 500);
  // A bound equal to the deadline never pre-empts it either.
  auto h = run(transport, Req{.url = url_for(e.ports.http, e.peer.arm("none")), .deadline = 500ms, .first_byte = 500ms,
                              .idle = 500ms});
  expect_deadline(*h, 500);
}
void bound_wins_when_earlier_than_deadline(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("head-silent");
  auto f = run(transport, Req{.url = url_for(e.ports.http, name), .deadline = 1200ms, .idle = 300ms});
  expect_failed(*f, tr::FailureKind::IdleTimeout, 295, 1100);
}
void cancel_wins_over_armed_bounds(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("head-silent");
  auto fetch = std::make_shared<Fetch>();
  Req r{.url = url_for(e.ports.http, name), .deadline = 30s, .connect = 20s, .first_byte = 20s, .idle = 20s};
  auto op = begin(transport, r, fetch);
  e.peer.wait(name, "count");
  std::this_thread::sleep_for(150ms);
  const auto t0 = Clock::now();
  op.cancel();
  (void)op.join();
  CHECK(fetch->result.status == tr::Status::Cancelled && ms_since(t0) < 500);
}
void transport_defaults_and_request_overrides(Env& e) {
  tr::TransportOptions options;
  options.idle_timeout = 300ms;
  tr::Transport transport(options);
  const auto name = e.peer.arm("head-silent");
  const auto url = url_for(e.ports.http, name);
  auto inherited = run(transport, Req{.url = url, .deadline = 5s});  // nullopt: the transport default applies
  expect_failed(*inherited, tr::FailureKind::IdleTimeout, 295, 2000);
  auto disabled = run(transport, Req{.url = url, .deadline = 700ms, .idle = 0ms});  // zero disables it
  expect_deadline(*disabled, 700);
  auto tighter = run(transport, Req{.url = url, .deadline = 5s, .idle = 100ms});
  expect_failed(*tighter, tr::FailureKind::IdleTimeout, 95, 1500);
  auto negative = run(transport, Req{.url = url, .deadline = 600ms, .idle = Ms(-5)});  // nonsense disables
  expect_deadline(*negative, 600);
}
void extreme_bounds_do_not_overflow(Env& e) {
  tr::Transport transport;
  // A bound too large to precede the deadline stays disabled without converting it to clock ticks.
  const auto name = e.peer.arm("none");
  auto f = run(transport, Req{.url = url_for(e.ports.http, name), .deadline = 500ms,
                            .connect = Ms::max(), .first_byte = Ms::max(), .idle = Ms::max()});
  expect_deadline(*f, 500);
  tr::HttpRequest request;
  request.url = url_for(e.ports.http, name);
  request.deadline = Clock::time_point::min();
  request.connect_timeout = request.first_byte_timeout = request.idle_timeout = Ms::max();
  std::atomic<int> terminals{0};
  tr::Callbacks callbacks;
  callbacks.on_done = [&](const tr::Result&) { ++terminals; };
  auto op = transport.start(std::move(request), std::move(callbacks));
  const auto result = op.join();
  CHECK(result.status == tr::Status::DeadlineExceeded && terminals == 1);
  e.peer.count(name, 1);  // The already-expired attempt was rejected before dispatch.
}

void timers_do_not_outlive_the_operation(Env& e) {
  // Long bounds armed under a far deadline. If any timer survived the outcome, the transport's
  // destructor (which waits for the event loop to drain) would block for a minute.
  const auto ok = e.peer.arm("ok");
  const auto silent = e.peer.arm("head-silent");
  const auto t0 = Clock::now();
  {
    tr::Transport transport;
    Req normal{.url = url_for(e.ports.http, ok), .deadline = 120s, .connect = 60s, .first_byte = 60s, .idle = 60s};
    CHECK(run(transport, normal)->result.status == tr::Status::Completed);
    auto fetch = std::make_shared<Fetch>();
    auto op = begin(transport, Req{.url = url_for(e.ports.http, silent), .deadline = 120s, .connect = 60s,
                                   .first_byte = 60s, .idle = 60s}, fetch);
    e.peer.wait(silent, "count");
    op.cancel();
    (void)op.join();
    CHECK(fetch->result.status == tr::Status::Cancelled);
  }
  CHECK(ms_since(t0) < 5000);
}
void transport_destroyed_mid_wait(Env& e) {
  const auto name = e.peer.arm("head-silent");
  auto transport = std::make_unique<tr::Transport>();
  auto fetch = std::make_shared<Fetch>();
  auto op = begin(*transport, Req{.url = url_for(e.ports.http, name), .deadline = 120s, .connect = 60s,
                                  .first_byte = 60s, .idle = 60s}, fetch);
  e.peer.wait(name, "count");
  const auto t0 = Clock::now();
  transport.reset();
  CHECK(ms_since(t0) < 3000);
  (void)op.join();
  CHECK(fetch->result.status == tr::Status::Cancelled);
}
void operation_handle_dropped_mid_wait(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("head-silent");
  auto fetch = std::make_shared<Fetch>();
  {
    auto op = begin(transport, Req{.url = url_for(e.ports.http, name), .deadline = 120s, .idle = 60s}, fetch);
    e.peer.wait(name, "count");
  }  // ~Operation cancels
  const auto limit = Clock::now() + 3s;
  while (!fetch->done && Clock::now() < limit) std::this_thread::sleep_for(5ms);
  CHECK(fetch->done && fetch->result.status == tr::Status::Cancelled);
}

void connect_cleared_once_connected(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("head-silent");
  // The connection is up long before 300 ms; a connect bound that stayed armed would cut this off.
  auto f = run(transport, Req{.url = url_for(e.ports.http, name), .deadline = 900ms, .connect = 300ms});
  expect_deadline(*f, 900);
}
void first_byte_cleared_by_the_head(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("trickle:100");
  // The head arrives at once; a first-byte bound that stayed armed would cut the stream at 300 ms.
  auto f = run(transport, Req{.url = url_for(e.ports.http, name), .deadline = 1000ms, .first_byte = 300ms});
  expect_deadline(*f, 1000);
  CHECK(f->body_calls >= 5);
}

void terminal_races_complete_exactly_once(Env& e) {
  std::vector<std::shared_ptr<Fetch>> results;
  {
    tr::Transport transport;
    std::vector<tr::Operation> operations;
    for (int i = 0; i < 32; ++i) {
      auto fetch = std::make_shared<Fetch>();
      const auto name = e.peer.arm("none");
      operations.push_back(begin(transport, Req{.url = url_for(e.ports.http, name), .deadline = 120ms,
                                              .first_byte = 80ms, .idle = 80ms}, fetch));
      results.push_back(std::move(fetch));
    }
    // Only this thread mutates each handle; joins happen after it has stopped.
    std::thread canceller([&] {
      std::this_thread::sleep_for(80ms);
      for (auto& operation : operations) operation.cancel();
    });
    canceller.join();
    for (auto& operation : operations) (void)operation.join();
  }  // Drain queued timer/cancel handlers before examining the terminal counts.
  for (const auto& fetch : results) {
    CHECK(fetch->done && fetch->done_calls == 1 && fetch->callbacks_after_done == 0);
    CHECK(fetch->callback_storage.expired());
    const auto& result = fetch->result;
    CHECK(result.status == tr::Status::Cancelled || result.status == tr::Status::DeadlineExceeded ||
          (result.status == tr::Status::Failed && (result.failure == tr::FailureKind::IdleTimeout ||
                                                   result.failure == tr::FailureKind::FirstByteTimeout)));
  }
}

// ---------------------------------------------------------------------------------------------
// Suite: tls_h2 (cleartext HTTP/2, TLS with ALPN h2 and http/1.1, and the handshake black hole)
// ---------------------------------------------------------------------------------------------
void h2c_idle_head_silent(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("head-silent");
  auto f = run(transport, Req{.url = url_for(e.ports.h2c, name), .deadline = 8s, .idle = 400ms,
                              .version = tr::HttpVersion::Http2PriorKnowledge});
  expect_failed(*f, tr::FailureKind::IdleTimeout, 395, 2400);
  CHECK(f->result.attempt.version == tr::ResponseVersion::Http2 && f->head);
}
void h2c_trickle_is_not_cut(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("trickle:100");
  auto f = run(transport, Req{.url = url_for(e.ports.h2c, name), .deadline = 1600ms, .first_byte = 300ms, .idle = 400ms,
                              .version = tr::HttpVersion::Http2PriorKnowledge});
  expect_deadline(*f, 1600);
  CHECK(f->result.attempt.version == tr::ResponseVersion::Http2 && f->body_calls >= 8);
}
void h2c_activity_is_per_transfer_not_per_connection(Env& e) {
  tr::Transport transport;
  const auto live = e.peer.arm("trickle:100");
  const auto quiet = e.peer.arm("head-silent");
  Req a{.url = url_for(e.ports.h2c, live), .deadline = 20s, .idle = 500ms, .version = tr::HttpVersion::Http2PriorKnowledge};
  Req b{.url = url_for(e.ports.h2c, quiet), .deadline = 20s, .idle = 500ms, .version = tr::HttpVersion::Http2PriorKnowledge};
  auto fa = std::make_shared<Fetch>();
  auto fb = std::make_shared<Fetch>();
  auto opa = begin(transport, a, fa);
  const auto limit = Clock::now() + 5s;
  while (!fa->head && Clock::now() < limit) std::this_thread::sleep_for(5ms);
  CHECK(fa->head);
  auto opb = begin(transport, b, fb);  // same origin: libcurl multiplexes it onto A's connection
  (void)opb.join();
  expect_failed(*fb, tr::FailureKind::IdleTimeout, 495, 2500);
  // The oracle confirms both transfers rode ONE HTTP/2 connection: activity is judged per transfer.
  CHECK(peer_stat(e.peer, live, "connection") != 0 && peer_stat(e.peer, live, "connection") == peer_stat(e.peer, quiet, "connection"));
  CHECK(!fa->done);  // the neighbour on the same connection kept flowing and was not cut
  CHECK(fa->body_calls >= 5);
  opa.cancel();
  (void)opa.join();
  CHECK(fa->result.status == tr::Status::Cancelled);
}
void tls_h2_idle_head_silent(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("head-silent");
  auto f = run(transport, Req{.url = url_for(e.ports.tls, name, "https"), .deadline = 8s, .idle = 400ms,
                              .version = tr::HttpVersion::Auto, .ca = e.ports.ca});
  expect_failed(*f, tr::FailureKind::IdleTimeout, 395, 2400);
  CHECK(f->result.attempt.version == tr::ResponseVersion::Http2 && f->head);
}
void tls_http1_idle_head_silent(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("head-silent");
  auto f = run(transport, Req{.url = url_for(e.ports.tls, name, "https"), .deadline = 8s, .idle = 400ms,
                              .version = tr::HttpVersion::Http1_1, .ca = e.ports.ca});
  expect_failed(*f, tr::FailureKind::IdleTimeout, 395, 2400);
  CHECK(f->result.attempt.version == tr::ResponseVersion::Http1_1 && f->head);
}
void tls_trickle_is_not_cut_and_connect_is_cleared(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("trickle:100");
  auto f = run(transport, Req{.url = url_for(e.ports.tls, name, "https"), .deadline = 1600ms, .connect = 300ms,
                              .first_byte = 300ms, .idle = 400ms, .version = tr::HttpVersion::Auto, .ca = e.ports.ca});
  expect_deadline(*f, 1600);
  CHECK(f->body_calls >= 8);
}
void tls_normal_completion_with_all_bounds(Env& e) {
  tr::Transport transport;
  const auto name = e.peer.arm("ok");
  auto f = run(transport, Req{.url = url_for(e.ports.tls, name, "https"), .deadline = 8s, .connect = 3s,
                              .first_byte = 3s, .idle = 3s, .version = tr::HttpVersion::Auto, .ca = e.ports.ca});
  CHECK(f->result.status == tr::Status::Completed && f->body_bytes == 2);
}
void tls_handshake_black_hole_connect_timeout(Env& e) {
  tr::Transport transport;
  Req r{.url = "https://127.0.0.1:" + std::to_string(e.ports.handshake) + "/x", .deadline = 8s, .connect = 400ms,
        .version = tr::HttpVersion::Auto, .ca = e.ports.ca};
  auto f = run(transport, r);
  expect_failed(*f, tr::FailureKind::ConnectTimeout, 395, 2400);
  // libcurl marks "connected" only once TCP and TLS are both up, so the stalled handshake is
  // observable only as "not past name lookup" (Stage documents the limitation).
  CHECK(f->result.attempt.reached < tr::Stage::Connected && !f->head);
  CHECK(!f->result.attempt.response_head_seen && f->result.attempt.request_body_bytes == 0);
}
void tls_handshake_black_hole_other_bounds(Env& e) {
  tr::Transport transport;
  const std::string url = "https://127.0.0.1:" + std::to_string(e.ports.handshake) + "/x";
  auto first = run(transport, Req{.url = url, .deadline = 8s, .first_byte = 400ms, .version = tr::HttpVersion::Auto, .ca = e.ports.ca});
  expect_failed(*first, tr::FailureKind::FirstByteTimeout, 395, 2400);  // first byte includes connect
  auto idle = run(transport, Req{.url = url, .deadline = 8s, .idle = 400ms, .version = tr::HttpVersion::Auto, .ca = e.ports.ca});
  expect_failed(*idle, tr::FailureKind::IdleTimeout, 395, 2400);  // counted from the attempt's start
  auto pinned = run(transport, Req{.url = url, .deadline = 600ms, .version = tr::HttpVersion::Auto, .ca = e.ports.ca});
  expect_deadline(*pinned, 600);  // default: nothing but the deadline
}

// ---------------------------------------------------------------------------------------------
// Suite: connect (TCP black hole) at transport and runtime level
// ---------------------------------------------------------------------------------------------
void connect_black_hole_times_out(Env&) {
  Blackhole hole;
  tr::Transport transport;
  auto f = run(transport, Req{.url = "http://127.0.0.1:" + std::to_string(hole.port) + "/x", .deadline = 8s, .connect = 400ms});
  expect_failed(*f, tr::FailureKind::ConnectTimeout, 395, 2400);
  CHECK(f->result.attempt.reached < tr::Stage::Connected && !f->head && f->result.attempt.request_body_bytes == 0);
}
void connect_black_hole_pinned_default_and_other_bounds(Env&) {
  Blackhole hole;
  tr::Transport transport;
  const std::string url = "http://127.0.0.1:" + std::to_string(hole.port) + "/x";
  expect_deadline(*run(transport, Req{.url = url, .deadline = 700ms}), 700);
  expect_failed(*run(transport, Req{.url = url, .deadline = 8s, .first_byte = 400ms}), tr::FailureKind::FirstByteTimeout, 395, 2400);
  expect_failed(*run(transport, Req{.url = url, .deadline = 8s, .idle = 400ms}), tr::FailureKind::IdleTimeout, 395, 2400);
  expect_deadline(*run(transport, Req{.url = url, .deadline = 500ms, .connect = 3s}), 500);
}

// ---------------------------------------------------------------------------------------------
// Runtime level: the typed outcome per provider family
// ---------------------------------------------------------------------------------------------
enum class Fam { Chat, Messages, Responses, Gemini, Interactions };
constexpr Fam kFamilies[] = {Fam::Chat, Fam::Messages, Fam::Responses, Fam::Gemini, Fam::Interactions};
const char* family_name(Fam f) {
  switch (f) {
    case Fam::Chat: return "chat";
    case Fam::Messages: return "messages";
    case Fam::Responses: return "responses";
    case Fam::Gemini: return "gemini";
    case Fam::Interactions: return "interactions";
  }
  return "";
}
constexpr std::string_view kKey = "STALL_SYNTHETIC_KEY";

std::string descriptor_source(Fam fam, std::uint64_t port, const std::string& model) {
  const auto base = "http://127.0.0.1:" + std::to_string(port);
  const auto head = [&](const char* family, const std::string& buffered, const std::string& streaming, const char* headers,
                        const char* bindings, const char* extra) {
    return std::string("{\"descriptor_version\":1,\"revision\":1,\"id\":\"stall-loopback\",\"family\":") + sp::json::quote(family) +
           extra + ",\"connection\":{\"base_url\":" + sp::json::quote(base) + ",\"paths\":{\"buffered\":" + sp::json::quote(buffered) +
           ",\"streaming\":" + sp::json::quote(streaming) + "}" + headers + "}," + bindings + "}";
  };
  const char* evidence = ",\"evidence\":{\"urls\":[\"https://example.com/spec\"],\"verified_at\":\"2026-10-01\"}";
  switch (fam) {
    case Fam::Chat:
      return head("openai.chat", "/v1/chat/completions", "/v1/chat/completions", "",
                  "\"bindings\":{\"model\":\"model\",\"messages\":\"messages\",\"stream\":\"stream\",\"max_output_tokens\":\"max_tokens\",\"usage\":[\"usage\"]},"
                  "\"stop_reasons\":{\"stop\":\"EndTurn\",\"end_turn\":\"EndTurn\",\"tool_use\":\"ToolUse\"}", evidence);
    case Fam::Messages:
      return head("anthropic.messages", "/v1/messages", "/v1/messages", ",\"headers\":{\"anthropic-version\":\"2023-06-01\"}",
                  "\"bindings\":{\"model\":\"model\",\"messages\":\"messages\",\"stream\":\"stream\",\"max_output_tokens\":\"max_tokens\",\"usage\":[\"usage\"]},"
                  "\"stop_reasons\":{\"stop\":\"EndTurn\",\"end_turn\":\"EndTurn\",\"tool_use\":\"ToolUse\"}", evidence);
    case Fam::Responses:
      return head("openai.responses", "/v1/responses", "/v1/responses", "",
                  "\"bindings\":{\"model\":\"model\",\"messages\":\"input\",\"stream\":\"stream\",\"max_output_tokens\":\"max_output_tokens\",\"usage\":[\"usage\"]}", "");
    case Fam::Gemini:
      return head("google.generate", "/v1beta/models/" + model + ":generateContent",
                  "/v1beta/models/" + model + ":streamGenerateContent?alt=sse", "",
                  "\"bindings\":{\"model\":\"model\",\"messages\":\"contents\",\"stream\":\"stream\",\"max_output_tokens\":\"maxOutputTokens\",\"usage\":[\"usageMetadata\"]}", "");
    case Fam::Interactions:
      return head("google.interactions", "/v1beta/interactions", "/v1beta/interactions", "",
                  "\"bindings\":{\"model\":\"model\",\"messages\":\"input\",\"stream\":\"stream\",\"max_output_tokens\":\"max_output_tokens\",\"usage\":[\"usage\"]}", "");
  }
  return {};
}
sp::descriptor::ValidatedDescriptor make_descriptor(Fam fam, std::uint64_t port, const std::string& model) {
  auto loaded = sp::descriptor::load(descriptor_source(fam, port, model));
  CHECK(std::holds_alternative<sp::descriptor::ValidatedDescriptor>(loaded));
  return std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded));
}
sp::runtime::Request make_request(Fam fam, const std::string& model) {
  sp::Message user;
  user.role = sp::Role::User;
  user.parts.emplace_back(sp::Text{"synthetic"});
  switch (fam) {
    case Fam::Chat: { sp::chat::Request r; r.model = model; r.messages.push_back({sp::Role::User, "synthetic"}); return r; }
    case Fam::Messages: { sp::messages::Request r; r.model = model; r.account_scope = "synthetic-account"; r.messages.push_back(user); return r; }
    case Fam::Responses: { sp::responses::Request r; r.model = model; r.account_scope = "synthetic-account"; r.messages.push_back(user); return r; }
    case Fam::Gemini: { sp::gemini::Request r; r.model = model; r.account_scope = "synthetic-account"; r.messages.push_back(user); return r; }
    case Fam::Interactions: { sp::interactions::Request r; r.model = model; r.account_scope = "synthetic-account"; r.messages.push_back(user); return r; }
  }
  return sp::chat::Request{};
}
sp::runtime::Options base_options() {
  sp::runtime::Options o;
  o.api_key = std::string(kKey);
  o.default_timeout = 8s;
  o.retry_tokens_per_second = 0;
  return o;
}
sp::runtime::RunOptions run_options(bool retry = false, bool risk = false, std::uint32_t attempts = 3) {
  sp::runtime::RunOptions r;
  r.streaming = true;
  r.retry = sp::runtime::RetryPolicy{retry, risk, attempts, 0ms, 0ms};
  return r;
}

struct Outcome {
  sp::runtime::Result result;
  double ms = 0;
  Clock::time_point ended;
};
Outcome complete(Fam fam, std::uint64_t port, const std::string& model, sp::runtime::Options options, sp::runtime::RunOptions run) {
  sp::runtime::Client client(make_descriptor(fam, port, model), std::move(options));
  const auto t0 = Clock::now();
  auto result = client.complete(make_request(fam, model), std::move(run));
  return {result, ms_since(t0), Clock::now()};
}
const sp::Failure& failure_of(const sp::runtime::Result& result, sp::ErrorKind kind) {
  CHECK(result && std::holds_alternative<sp::Failure>(*result));
  const auto& f = std::get<sp::Failure>(*result);
  if (f.error.kind != kind)
    std::fprintf(stderr, "    kind=%d expected=%d msg=%s\n", static_cast<int>(f.error.kind), static_cast<int>(kind), f.error.safe_message.c_str());
  CHECK(f.error.kind == kind);
  CHECK(f.error.safe_message.find(kKey) == std::string::npos);
  return f;
}
std::string text_of(const std::vector<sp::Message>& messages) {
  std::string out;
  for (const auto& m : messages) for (const auto& p : m.parts) if (const auto* t = std::get_if<sp::Text>(&p)) out += t->value;
  return out;
}

void family_idle_stall_after_output(Env& e) {
  // utf8-silent stops inside a multi-byte character: the stall, not malformed UTF-8, is the cause.
  for (const char* scenario : {"prefix-silent", "utf8-silent"}) {
    for (const Fam fam : kFamilies) {
      std::fprintf(stderr, "  family %s (%s)\n", family_name(fam), scenario);
      const auto model = e.peer.arm(scenario);
      auto options = base_options();
      options.transport.idle_timeout = 400ms;  // client-wide default
      // Retries are allowed and permissive: only the attempt evidence may stop one.
      auto out = complete(fam, e.ports.http, model, options, run_options(true, true, 3));
      const auto& f = failure_of(out.result, sp::ErrorKind::Truncated);
      CHECK(out.ms >= 395 && out.ms < 4000);
      CHECK(f.error.retry_class == sp::RetryClass::Transient);
      CHECK(f.error.retry_safety == sp::RetrySafety::OutputObserved);
      CHECK(f.error.attempt.request_may_have_left && f.error.attempt.response_head_seen && f.error.attempt.attempts == 1);
      CHECK(text_of(f.partial.messages).find("partial") != std::string::npos);  // the partial message survives
      e.peer.count(model, 1);  // no automatic retry after output was observed
    }
  }
}
void family_first_byte_stall_by_run_override(Env& e) {
  for (const Fam fam : kFamilies) {
    std::fprintf(stderr, "  family %s\n", family_name(fam));
    {
      const auto model = e.peer.arm("none");
      auto run = run_options(true, true, 2);  // duplicate-billing risk accepted: the evidence allows a retry
      run.first_byte_timeout = 400ms;         // per-run override; the client default is disabled
      auto out = complete(fam, e.ports.http, model, base_options(), run);
      const auto& f = failure_of(out.result, sp::ErrorKind::Transport);
      CHECK(f.error.retry_class == sp::RetryClass::Transient);
      CHECK(f.error.retry_safety == sp::RetrySafety::PossiblyAccepted);
      CHECK(f.error.attempt.request_may_have_left && !f.error.attempt.response_head_seen && f.error.attempt.attempts == 2);
      CHECK(out.ms >= 2 * 395 && out.ms < 5000);
      e.peer.count(model, 2);
    }
    {
      const auto model = e.peer.arm("none");
      auto run = run_options(true, false, 3);  // the request may have been accepted: no retry without the risk flag
      run.first_byte_timeout = 400ms;
      auto out = complete(fam, e.ports.http, model, base_options(), run);
      const auto& f = failure_of(out.result, sp::ErrorKind::Transport);
      CHECK(f.error.retry_safety == sp::RetrySafety::PossiblyAccepted && f.error.attempt.attempts == 1);
      CHECK(out.ms >= 395 && out.ms < 3000);
      e.peer.count(model, 1);
    }
  }
}
void family_connect_stall(Env&) {
  Blackhole hole;
  for (const Fam fam : kFamilies) {
    std::fprintf(stderr, "  family %s\n", family_name(fam));
    auto options = base_options();
    options.transport.connect_timeout = 300ms;
    // Nothing was written, so a retry is safe even without accepting duplicate-billing risk.
    auto out = complete(fam, hole.port, "connect-" + std::string(family_name(fam)), options, run_options(true, false, 2));
    const auto& f = failure_of(out.result, sp::ErrorKind::Transport);
    CHECK(f.error.retry_class == sp::RetryClass::Transient);
    CHECK(f.error.retry_safety == sp::RetrySafety::NotSent);
    CHECK(!f.error.attempt.request_may_have_left && !f.error.attempt.response_head_seen && f.error.attempt.attempts == 2);
    CHECK(out.ms >= 2 * 295 && out.ms < 6000);
  }
}
void runtime_run_connect_override_alone(Env&) {
  Blackhole hole;
  {  // only the run enables a connect bound
    auto run = run_options();
    run.connect_timeout = 300ms;
    auto out = complete(Fam::Chat, hole.port, "connect-run", base_options(), run);
    const auto& f = failure_of(out.result, sp::ErrorKind::Transport);
    CHECK(f.error.retry_safety == sp::RetrySafety::NotSent && !f.error.attempt.request_may_have_left);
    CHECK(out.ms >= 295 && out.ms < 3000);
  }
  {  // an explicit zero in the run disables the enabled client default
    auto options = base_options();
    options.transport.connect_timeout = 300ms;
    auto run = run_options();
    run.connect_timeout = 0ms;
    run.deadline = Clock::now() + 900ms;
    auto out = complete(Fam::Chat, hole.port, "connect-zero", options, run);
    failure_of(out.result, sp::ErrorKind::DeadlineExceeded);
    CHECK(out.ended >= *run.deadline && out.ended < *run.deadline + 2s);
  }
}

void runtime_pin_default_behaviour(Env& e) {
  {
    const auto model = e.peer.arm("trickle:100");
    auto run = run_options(true, true, 3);
    run.deadline = Clock::now() + 900ms;
    auto out = complete(Fam::Chat, e.ports.http, model, base_options(), run);
    const auto& f = failure_of(out.result, sp::ErrorKind::DeadlineExceeded);
    CHECK(out.ended >= *run.deadline && out.ended < *run.deadline + 2s);
    CHECK(f.error.retry_safety == sp::RetrySafety::OutputObserved);
    const auto partial = text_of(f.partial.messages);
    CHECK(partial.find("partial") != std::string::npos && partial.size() > std::string("partial").size() + 3);
    e.peer.count(model, 1);
  }
  for (const char* scenario : {"head-silent", "none"}) {
    const auto model = e.peer.arm(scenario);
    auto run = run_options();
    run.deadline = Clock::now() + 700ms;
    auto out = complete(Fam::Chat, e.ports.http, model, base_options(), run);
    failure_of(out.result, sp::ErrorKind::DeadlineExceeded);
    CHECK(out.ended >= *run.deadline && out.ended < *run.deadline + 2s);
  }
}
void runtime_trickle_and_comments_are_not_cut(Env& e) {
  for (const char* scenario : {"trickle:100", "comments:100"}) {
    const auto model = e.peer.arm(scenario);
    auto options = base_options();
    options.transport.idle_timeout = 400ms;
    auto run = run_options();
    run.deadline = Clock::now() + 1600ms;
    auto out = complete(Fam::Chat, e.ports.http, model, options, run);
    failure_of(out.result, sp::ErrorKind::DeadlineExceeded);
    CHECK(out.ended >= *run.deadline && out.ended < *run.deadline + 2s);
  }
}
void runtime_run_override_beats_client_default(Env& e) {
  {  // default disabled, run enables
    const auto model = e.peer.arm("head-silent");
    auto run = run_options();
    run.idle_timeout = 300ms;
    auto out = complete(Fam::Chat, e.ports.http, model, base_options(), run);
    failure_of(out.result, sp::ErrorKind::Truncated);
    CHECK(out.ms >= 295 && out.ms < 2500);
  }
  {  // default enabled, run disables (zero)
    const auto model = e.peer.arm("head-silent");
    auto options = base_options();
    options.transport.idle_timeout = 300ms;
    auto run = run_options();
    run.idle_timeout = 0ms;
    run.deadline = Clock::now() + 800ms;
    auto out = complete(Fam::Chat, e.ports.http, model, options, run);
    failure_of(out.result, sp::ErrorKind::DeadlineExceeded);
    CHECK(out.ended >= *run.deadline && out.ended < *run.deadline + 2s);
  }
  {  // default long, run tighter
    const auto model = e.peer.arm("head-silent");
    auto options = base_options();
    options.transport.idle_timeout = 5s;
    auto run = run_options();
    run.idle_timeout = 200ms;
    auto out = complete(Fam::Chat, e.ports.http, model, options, run);
    failure_of(out.result, sp::ErrorKind::Truncated);
    CHECK(out.ms >= 195 && out.ms < 2500);
  }
  {  // idle before the head is a transport failure, not a truncation
    const auto model = e.peer.arm("none");
    auto run = run_options();
    run.idle_timeout = 300ms;
    auto out = complete(Fam::Messages, e.ports.http, model, base_options(), run);
    const auto& f = failure_of(out.result, sp::ErrorKind::Transport);
    CHECK(f.error.retry_safety == sp::RetrySafety::PossiblyAccepted && !f.error.attempt.response_head_seen);
  }
}
void runtime_client_default_first_byte_alone(Env& e) {
  auto options = base_options();
  options.transport.first_byte_timeout = 400ms;  // the client-wide default is the only bound enabled
  {
    const auto model = e.peer.arm("none");
    auto out = complete(Fam::Chat, e.ports.http, model, options, run_options());
    const auto& f = failure_of(out.result, sp::ErrorKind::Transport);
    CHECK(f.error.retry_safety == sp::RetrySafety::PossiblyAccepted && !f.error.attempt.response_head_seen);
    CHECK(out.ms >= 395 && out.ms < 3000);
    e.peer.count(model, 1);
  }
  {  // an explicit zero in the run disables the enabled default
    const auto model = e.peer.arm("none");
    auto run = run_options();
    run.first_byte_timeout = 0ms;
    run.deadline = Clock::now() + 800ms;
    auto out = complete(Fam::Chat, e.ports.http, model, options, run);
    failure_of(out.result, sp::ErrorKind::DeadlineExceeded);
    CHECK(out.ended >= *run.deadline && out.ended < *run.deadline + 2s);
  }
}
void runtime_retries_keep_the_original_deadline(Env& e) {
  for (const Fam fam : kFamilies) {
    const auto model = e.peer.arm("none");
    sp::runtime::Client client(make_descriptor(fam, e.ports.http, model), base_options());
    auto run = run_options(true, true, 8);
    run.first_byte_timeout = 300ms;
    const auto t0 = Clock::now();
    run.deadline = t0 + 750ms;
    auto result = client.complete(make_request(fam, model), run);
    const auto& failure = failure_of(result, sp::ErrorKind::DeadlineExceeded);
    CHECK(ms_since(t0) >= 745 && ms_since(t0) < 2500);
    CHECK(failure.error.attempt.attempts >= 2 && failure.error.attempt.attempts <= 3);
    CHECK(failure.error.retry_safety == sp::RetrySafety::PossiblyAccepted);
    e.peer.count(model, failure.error.attempt.attempts);
  }
}

void runtime_deadline_wins_and_cancel(Env& e) {
  {
    const auto model = e.peer.arm("head-silent");
    auto options = base_options();
    options.transport.idle_timeout = 3s;
    options.transport.first_byte_timeout = 3s;
    options.transport.connect_timeout = 3s;
    auto run = run_options();
    run.deadline = Clock::now() + 500ms;
    auto out = complete(Fam::Chat, e.ports.http, model, options, run);
    failure_of(out.result, sp::ErrorKind::DeadlineExceeded);
    CHECK(out.ended >= *run.deadline && out.ended < *run.deadline + 2s);
  }
  {
    const auto model = e.peer.arm("head-silent");
    auto options = base_options();
    options.transport.idle_timeout = 20s;
    sp::runtime::Client client(make_descriptor(Fam::Chat, e.ports.http, model), options);
    auto op = client.start(make_request(Fam::Chat, model), run_options());
    e.peer.wait(model, "count");
    std::this_thread::sleep_for(150ms);
    const auto t0 = Clock::now();
    op.cancel();
    auto result = op.join();
    failure_of(result, sp::ErrorKind::Cancelled);
    CHECK(ms_since(t0) < 1000);
  }
}
void runtime_normal_completion_unaffected(Env& e) {
  auto options = base_options();
  options.transport.connect_timeout = 2s;
  options.transport.first_byte_timeout = 2s;
  options.transport.idle_timeout = 2s;
  for (const Fam fam : {Fam::Chat, Fam::Messages}) {
    for (const bool streaming : {true, false}) {
      const auto model = e.peer.arm("ok");
      auto run = run_options();
      run.streaming = streaming;
      auto out = complete(fam, e.ports.http, model, options, run);
      CHECK(out.result && std::holds_alternative<sp::Completion>(*out.result));
      CHECK(text_of(std::get<sp::Completion>(*out.result).messages) == "hello");
      e.peer.count(model, 1);
    }
  }
}
void runtime_option_validation(Env& e) {
  struct Knob { const char* pointer; Ms sp::transport::TransportOptions::* member; };
  const Knob knobs[] = {{"/connect_timeout_ms", &tr::TransportOptions::connect_timeout},
                        {"/first_byte_timeout_ms", &tr::TransportOptions::first_byte_timeout},
                        {"/idle_timeout_ms", &tr::TransportOptions::idle_timeout}};
  for (const auto& knob : knobs) {
    auto options = base_options();
    options.transport.*(knob.member) = Ms(-1);
    bool rejected = false;
    try { sp::runtime::Client client(make_descriptor(Fam::Chat, e.ports.http, "m"), options); }
    catch (const sp::descriptor::ConfigError& error) { rejected = error.pointer == knob.pointer; }
    CHECK(rejected);
  }
  // A custom admitted policy: the stall bounds share the default_timeout_ms admission ceiling.
  std::string policy(sp::config_defaults::runtime_defaults_json);
  const std::string open = "\"default_timeout_ms\":9223372036854775807";
  const auto at = policy.find(open);
  CHECK(at != std::string::npos);
  policy.replace(at, open.size(), "\"default_timeout_ms\":40000");
  auto loaded = sp::configuration::load_runtime_policy(policy, sp::config_defaults::error_policy_json);
  CHECK(std::holds_alternative<sp::configuration::PolicySnapshot>(loaded));
  const auto snapshot = std::get<sp::configuration::PolicySnapshot>(loaded);
  for (const auto& knob : knobs) {
    sp::runtime::Options options(snapshot);
    options.api_key = std::string(kKey);
    options.default_timeout = 8s;
    options.transport.*(knob.member) = 40001ms;
    bool rejected = false;
    try { sp::runtime::Client client(make_descriptor(Fam::Chat, e.ports.http, "m"), options); }
    catch (const sp::descriptor::ConfigError& error) { rejected = error.pointer == knob.pointer; }
    CHECK(rejected);
    options.transport.*(knob.member) = 40000ms;  // the ceiling itself is admitted
    sp::runtime::Client client(make_descriptor(Fam::Chat, e.ports.http, "m"), options);
  }
  // Per-run overrides: nonsense is an InvalidRequest before anything is sent.
  sp::runtime::Options options(snapshot);
  options.api_key = std::string(kKey);
  options.default_timeout = 8s;
  options.retry_tokens_per_second = 0;
  const auto model = e.peer.arm("ok");
  sp::runtime::Client client(make_descriptor(Fam::Chat, e.ports.http, model), options);
  using Setter = void (*)(sp::runtime::RunOptions&, Ms);
  const Setter setters[] = {[](sp::runtime::RunOptions& r, Ms v) { r.connect_timeout = v; },
                            [](sp::runtime::RunOptions& r, Ms v) { r.first_byte_timeout = v; },
                            [](sp::runtime::RunOptions& r, Ms v) { r.idle_timeout = v; }};
  for (const auto set : setters) {
    for (const Ms bad : {Ms(-1), 40001ms}) {
      auto run = run_options();
      set(run, bad);
      auto result = client.complete(make_request(Fam::Chat, model), run);
      const auto& f = failure_of(result, sp::ErrorKind::InvalidRequest);
      CHECK(f.error.retry_safety == sp::RetrySafety::NotSent && !f.error.attempt.request_may_have_left);
    }
    auto run = run_options();
    set(run, 40000ms);
    auto result = client.complete(make_request(Fam::Chat, model), run);
    CHECK(result && std::holds_alternative<sp::Completion>(*result));
  }
  e.peer.count(model, 3);  // only the three valid runs reached the wire
}
void runtime_client_destroyed_mid_wait(Env& e) {
  const auto model = e.peer.arm("head-silent");
  auto options = base_options();
  options.transport.idle_timeout = 60s;
  options.transport.first_byte_timeout = 60s;
  options.transport.connect_timeout = 60s;
  options.default_timeout = 120s;
  auto client = std::make_unique<sp::runtime::Client>(make_descriptor(Fam::Chat, e.ports.http, model), options);
  auto op = client->start(make_request(Fam::Chat, model), run_options());
  e.peer.wait(model, "count");
  const auto t0 = Clock::now();
  client.reset();
  CHECK(ms_since(t0) < 3000);
  failure_of(op.join(), sp::ErrorKind::Cancelled);
}

// ---------------------------------------------------------------------------------------------
// Registry and main
// ---------------------------------------------------------------------------------------------
struct Case { const char* suite; const char* name; void (*fn)(Env&); };
const Case kCases[] = {
    {"transport", "stall_pin_default_trickle_runs_to_deadline", pin_default_trickle_runs_to_deadline},
    {"transport", "stall_pin_default_head_silent_runs_to_deadline", pin_default_head_silent_runs_to_deadline},
    {"transport", "stall_pin_default_never_responding_runs_to_deadline", pin_default_never_responding_runs_to_deadline},
    {"transport", "stall_pin_default_comments_run_to_deadline", pin_default_comments_run_to_deadline},
    {"transport", "stall_idle_request_headers_count_as_activity", idle_request_headers_count_as_activity},
    {"transport", "stall_idle_response_header_lines_count_as_activity", idle_response_header_lines_count_as_activity},
    {"transport", "stall_idle_head_silent", idle_head_silent},
    {"transport", "stall_idle_never_responding_is_idle_not_first_byte", idle_never_responding_is_idle_not_first_byte},
    {"transport", "stall_idle_trickle_of_events_is_not_cut", idle_trickle_of_events_is_not_cut},
    {"transport", "stall_idle_comments_count_as_activity", idle_comments_count_as_activity},
    {"transport", "stall_idle_slow_upload_counts_as_activity", idle_slow_upload_counts_as_activity},
    {"transport", "stall_idle_normal_completion_and_reset_unaffected", idle_normal_completion_and_reset_unaffected},
    {"transport", "stall_idle_pause_is_not_peer_silence", idle_pause_is_not_peer_silence},
    {"transport", "stall_idle_rearms_after_a_pause_ends", idle_rearms_after_a_pause_ends},
    {"transport", "stall_first_byte_never_responding", first_byte_never_responding},
    {"transport", "stall_first_byte_head_in_time_completes", first_byte_head_in_time_completes},
    {"transport", "stall_first_byte_head_too_late_fails", first_byte_head_too_late_fails},
    {"transport", "stall_first_byte_slow_body_after_head_unaffected", first_byte_slow_body_after_head_unaffected},
    {"transport", "stall_first_byte_half_a_status_line_is_not_a_head", first_byte_half_a_status_line_is_not_a_head},
    {"transport", "stall_first_byte_cleared_by_the_first_header_line", first_byte_cleared_by_the_first_header_line},
    {"transport", "stall_first_byte_cleared_by_the_head", first_byte_cleared_by_the_head},
    {"transport", "stall_connect_cleared_once_connected", connect_cleared_once_connected},
    {"transport", "stall_deadline_earlier_than_every_bound_wins", deadline_earlier_than_every_bound_wins},
    {"transport", "stall_bound_wins_when_earlier_than_deadline", bound_wins_when_earlier_than_deadline},
    {"transport", "stall_cancel_wins_over_armed_bounds", cancel_wins_over_armed_bounds},
    {"transport", "stall_transport_defaults_and_request_overrides", transport_defaults_and_request_overrides},
    {"transport", "stall_extreme_bounds_do_not_overflow", extreme_bounds_do_not_overflow},
    {"transport", "stall_timers_do_not_outlive_the_operation", timers_do_not_outlive_the_operation},
    {"transport", "stall_transport_destroyed_mid_wait", transport_destroyed_mid_wait},
    {"transport", "stall_operation_handle_dropped_mid_wait", operation_handle_dropped_mid_wait},
    {"transport", "stall_terminal_races_complete_exactly_once", terminal_races_complete_exactly_once},
    {"tls_h2", "stall_h2c_idle_head_silent", h2c_idle_head_silent},
    {"tls_h2", "stall_h2c_trickle_is_not_cut", h2c_trickle_is_not_cut},
    {"tls_h2", "stall_h2c_activity_is_per_transfer_not_per_connection", h2c_activity_is_per_transfer_not_per_connection},
    {"tls_h2", "stall_tls_h2_idle_head_silent", tls_h2_idle_head_silent},
    {"tls_h2", "stall_tls_http1_idle_head_silent", tls_http1_idle_head_silent},
    {"tls_h2", "stall_tls_trickle_is_not_cut_and_connect_is_cleared", tls_trickle_is_not_cut_and_connect_is_cleared},
    {"tls_h2", "stall_tls_normal_completion_with_all_bounds", tls_normal_completion_with_all_bounds},
    {"tls_h2", "stall_tls_handshake_black_hole_connect_timeout", tls_handshake_black_hole_connect_timeout},
    {"tls_h2", "stall_tls_handshake_black_hole_other_bounds", tls_handshake_black_hole_other_bounds},
    {"connect", "stall_connect_black_hole_times_out", connect_black_hole_times_out},
    {"connect", "stall_connect_black_hole_pinned_default_and_other_bounds", connect_black_hole_pinned_default_and_other_bounds},
    {"connect", "stall_family_connect_stall", family_connect_stall},
    {"connect", "stall_runtime_run_connect_override_alone", runtime_run_connect_override_alone},
    {"runtime", "stall_family_idle_stall_after_output", family_idle_stall_after_output},
    {"runtime", "stall_family_first_byte_stall_by_run_override", family_first_byte_stall_by_run_override},
    {"runtime", "stall_runtime_pin_default_behaviour", runtime_pin_default_behaviour},
    {"runtime", "stall_runtime_trickle_and_comments_are_not_cut", runtime_trickle_and_comments_are_not_cut},
    {"runtime", "stall_runtime_run_override_beats_client_default", runtime_run_override_beats_client_default},
    {"runtime", "stall_runtime_client_default_first_byte_alone", runtime_client_default_first_byte_alone},
    {"runtime", "stall_runtime_deadline_wins_and_cancel", runtime_deadline_wins_and_cancel},
    {"runtime", "stall_runtime_retries_keep_the_original_deadline", runtime_retries_keep_the_original_deadline},
    {"runtime", "stall_runtime_normal_completion_unaffected", runtime_normal_completion_unaffected},
    {"runtime", "stall_runtime_option_validation", runtime_option_validation},
    {"runtime", "stall_runtime_client_destroyed_mid_wait", runtime_client_destroyed_mid_wait},
};

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  runtime_test::Arguments arguments(argc, argv);
  argc = arguments.argc(); argv = arguments.argv();
#endif
  if (argc < 4) {
    std::fprintf(stderr, "usage: sp_stall_tests <node> <stall_server.mjs> <suite> [case-substring]\n");
    return 2;
  }
  const std::string suite = argv[3];
  const std::string only = argc > 4 ? argv[4] : "";
  int failed = 0, skipped = 0, ran = 0;
  try {
    Peer peer(argv[1], argv[2]);
    Env env{peer, ports_of(peer)};
    for (const Case& c : kCases) {
      if (suite != c.suite || (!only.empty() && std::string(c.name).find(only) == std::string::npos)) continue;
      ++ran;
      const auto t0 = Clock::now();
      try {
        c.fn(env);
        std::fprintf(stderr, "[ PASS ] %s (%.0f ms)\n", c.name, ms_since(t0));
      } catch (const Skip& s) {
        ++skipped;
        std::fprintf(stderr, "[ SKIP ] %s: %s\n", c.name, s.why.c_str());
      } catch (const std::exception& error) {
        ++failed;
        std::fprintf(stderr, "[ FAIL ] %s (%.0f ms): %s\n", c.name, ms_since(t0), error.what());
      }
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "setup failed: %s\n", error.what());
    return 1;
  }
  if (ran == 0) {
    std::fprintf(stderr, "no case matched suite '%s'\n", suite.c_str());
    return 2;
  }
  std::fprintf(stderr, "%d ran, %d failed, %d skipped\n", ran, failed, skipped);
  if (failed != 0) return 1;
  if (skipped != 0) std::fprintf(stderr, "STALL_SKIPPED: %d case(s) could not run in this environment\n", skipped);
  return 0;
}
