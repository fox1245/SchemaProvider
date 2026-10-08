// Transport property tests (docs/POC_PLAN.md milestones M1 and M1b). Each test names the CONFORMANCE.md
// property or POC_PLAN experiment it exercises. The oracle is server-side evidence from the
// separate sp_loopback_server process wherever the property is about what reached the wire.
//
// Usage: sp_transport_tests <path-to-sp_loopback_server> <fixture-directory> [test-name-substring]
#include "transport/http_transport.h"

#include "support/portable.h"
#ifdef _WIN32
#include "support/win_owner.h"
#include <psapi.h>
#include <tlhelp32.h>
#else
#include <sys/wait.h>
#include "support/posix_owner.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace sp::transport;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

// ------------------------------------------------------------------------------------------
// Minimal harness
// ------------------------------------------------------------------------------------------
namespace {

int g_failures = 0;
std::string g_current;

struct TestCase {
  const char* name;
  void (*fn)();
};
std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}
struct Registrar {
  Registrar(const char* n, void (*f)()) { registry().push_back({n, f}); }
};

#define TEST(name)                                  \
  static void name();                               \
  static Registrar registrar_##name(#name, &name);  \
  static void name()

#define CHECK(cond)                                                                      \
  do {                                                                                   \
    if (!(cond)) {                                                                       \
      ++g_failures;                                                                      \
      std::fprintf(stderr, "  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                                    \
  } while (0)

#define CHECK_MSG(cond, ...)                                                             \
  do {                                                                                   \
    if (!(cond)) {                                                                       \
      ++g_failures;                                                                      \
      std::fprintf(stderr, "  CHECK FAILED %s:%d: %s -- ", __FILE__, __LINE__, #cond);   \
      std::fprintf(stderr, __VA_ARGS__);                                                 \
      std::fprintf(stderr, "\n");                                                        \
    }                                                                                    \
  } while (0)

#if defined(__GNUC__) || defined(__clang__)
void note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#endif
void note(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::fprintf(stderr, "  note: ");
  std::vfprintf(stderr, fmt, ap);
  std::fprintf(stderr, "\n");
  va_end(ap);
}

template <class Pred>
bool wait_until(Pred pred, int timeout_ms) {
  const auto end = steady_clock::now() + milliseconds(timeout_ms);
  while (steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(milliseconds(2));
  }
  return pred();
}

double ms_since(steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(steady_clock::now() - t).count();
}

int thread_count() {
#ifdef _WIN32
  runtime_test::Handle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
  THREADENTRY32 entry{};
  entry.dwSize = sizeof entry;
  if (!snapshot || !::Thread32First(snapshot.get(), &entry)) {
    CHECK_MSG(false, "cannot enumerate native process threads (Win32 error %lu)", ::GetLastError());
    return -1;
  }
  int count = 0;
  do {
    if (entry.th32OwnerProcessID == ::GetCurrentProcessId()) ++count;
    entry.dwSize = sizeof entry;
  } while (::Thread32Next(snapshot.get(), &entry));
  CHECK_MSG(::GetLastError() == ERROR_NO_MORE_FILES, "native thread enumeration failed");
  return count;
#elif defined(__APPLE__)
  return portable::darwin_thread_count();
#else
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("Threads:", 0) == 0) return std::atoi(line.c_str() + 8);
  }
  return -1;
#endif
}

long long rss_bytes() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof counters;
  if (!::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof counters)) {
    CHECK_MSG(false, "cannot read native process working set (Win32 error %lu)", ::GetLastError());
    return 0;
  }
  return static_cast<long long>(counters.WorkingSetSize);
#elif defined(__APPLE__)
  return portable::darwin_resident_bytes();
#else
  // statm/VmRSS are approximate kernel counters. Keep the existing memory gates,
  // but measure actual resident pages, including sanitizer overhead.
  std::ifstream in("/proc/self/smaps_rollup");
  std::string line;
  while (std::getline(in, line)) {
    long kib = 0;
    char unit[3]{};
    if (std::sscanf(line.c_str(), "Rss: %ld %2s", &kib, unit) == 2 &&
        kib > 0 && std::strcmp(unit, "kB") == 0) {
      return static_cast<long long>(kib) * 1024;
    }
  }
  CHECK_MSG(false, "cannot read RSS from /proc/self/smaps_rollup");
  return 0;
#endif
}

// ------------------------------------------------------------------------------------------
// Server process + raw stats client (shares no code with the transport)
// ------------------------------------------------------------------------------------------
struct Server {
#ifdef _WIN32
  runtime_test::Spawned child;
#else
  pid_t pid = -1;
  int stdin_fd = -1;
#endif
  unsigned http = 0, silent = 0, blackhole = 0;
  std::string blackhole_reason;

  void start(const char* path) {
#ifdef _WIN32
    child = runtime_test::spawn_with_pipes(path, std::initializer_list<std::string_view>{});
    std::string line;
    const auto deadline = steady_clock::now() + std::chrono::seconds(5);
    while (line.find('\n') == std::string::npos && line.size() < 8192) {
      const auto remaining = std::chrono::duration_cast<milliseconds>(deadline - steady_clock::now()).count();
      if (remaining <= 0) break;
      char c;
      if (runtime_test::read_pipe(child.from_child.get(), &c, 1, remaining) == 0) break;
      line.push_back(c);
    }
#else
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) std::abort();
    pid = fork();
    if (pid == 0) {
      dup2(in_pipe[0], 0);
      dup2(out_pipe[1], 1);
      close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]);
      execl(path, path, static_cast<char*>(nullptr));
      _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    stdin_fd = in_pipe[1];
    std::string line;
    pollfd pfd{out_pipe[0], POLLIN, 0};
    while (line.find('\n') == std::string::npos) {
      if (poll(&pfd, 1, 5000) <= 0) break;
      char c;
      if (read(out_pipe[0], &c, 1) != 1) break;
      line.push_back(c);
    }
    close(out_pipe[0]);
#endif
    if (std::sscanf(line.c_str(), "PORTS http=%u silent=%u blackhole=%u", &http, &silent, &blackhole) != 3) {
      std::fprintf(stderr, "loopback server did not report ports: '%s'\n", line.c_str());
      std::exit(2);
    }
    const auto reason = line.find(" blackhole_reason=");
    if (reason != std::string::npos) {
      blackhole_reason = line.substr(reason + std::strlen(" blackhole_reason="));
      if (!blackhole_reason.empty() && blackhole_reason.back() == '\n') blackhole_reason.pop_back();
    }
    if (blackhole == 0 && blackhole_reason.empty()) {
      std::fprintf(stderr, "loopback server omitted the unsupported blackhole reason\n");
      stop();
      std::exit(2);
    }
  }
  void stop() {
#ifdef _WIN32
    child.to_child.reset();
    child.from_child.reset();
    child.process.reset();
#else
    if (stdin_fd >= 0) { close(stdin_fd); stdin_fd = -1; }
    if (pid > 0) { waitpid(pid, nullptr, 0); pid = -1; }
#endif
  }
  std::string url(const std::string& path_and_query) const {
    return "http://127.0.0.1:" + std::to_string(http) + path_and_query;
  }
};
Server g_server;

struct Stats {
  std::map<std::string, long> v;
  long operator[](const std::string& k) const {
    auto it = v.find(k);
    return it == v.end() ? 0 : it->second;
  }
};

bool socket_retry() {
#ifdef _WIN32
  const auto error = ::WSAGetLastError();
  return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

bool socket_ready(portable::socket_t socket, short events, steady_clock::time_point deadline) {
  for (;;) {
    const auto remaining = std::chrono::duration_cast<milliseconds>(deadline - steady_clock::now()).count();
    if (remaining <= 0) return false;
    portable::PollFd fd{socket, events, 0};
    const int ready = portable::poll(&fd, 1, static_cast<int>(remaining));
    if (ready > 0) return (fd.revents & (events | POLLERR | POLLHUP)) != 0;
    if (ready == 0 || !socket_retry()) return false;
  }
}

// A separate, bounded raw HTTP client: nonblocking connect/send/receive share one deadline.
// Socket ownership also covers every error path; failure is never mistaken for empty peer counters.
Stats stats_request(unsigned port, const char* path) {
  const auto deadline = steady_clock::now() + std::chrono::seconds(3);
  portable::Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
  auto fail = [&](const char* phase) {
    CHECK_MSG(false, "raw stats request to port %u failed or timed out during %s", port, phase);
    return Stats{};
  };
  if (!socket || !portable::set_nonblocking(socket.get())) return fail("socket setup");
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::connect(socket.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    if (!portable::connect_in_progress() || !socket_ready(socket.get(), POLLOUT, deadline))
      return fail("connect");
    int error = 0;
    portable::socklen size = sizeof error;
    if (::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) != 0 || error)
      return fail("connect completion");
  }
  const std::string request = std::string("GET ") + path + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  std::size_t sent = 0;
  while (sent < request.size()) {
    if (!socket_ready(socket.get(), POLLOUT, deadline)) return fail("send readiness");
    const auto n = portable::send_quiet(socket.get(), request.data() + sent, request.size() - sent);
    if (n > 0) sent += static_cast<std::size_t>(n);
    else if (n == 0 || !socket_retry()) return fail("send");
  }
  std::string response;
  char buffer[2048];
  for (;;) {
    if (!socket_ready(socket.get(), POLLIN, deadline)) return fail("receive readiness");
    const auto n = portable::receive(socket.get(), buffer, sizeof buffer);
    if (n == 0) break;
    if (n < 0) {
      if (socket_retry()) continue;
      return fail("receive");
    }
    response.append(buffer, static_cast<std::size_t>(n));
    if (response.size() > (1U << 20)) return fail("response bound");
  }
  const auto body = response.find("\r\n\r\n");
  if (body == std::string::npos || response.rfind("HTTP/1.1 200 ", 0) != 0) return fail("HTTP response");
  std::istringstream in(response.substr(body + 4));
  Stats result;
  std::string key;
  long value;
  while (in >> key >> value) result.v[key] = value;
  if (result.v.empty()) return fail("stats body");
  return result;
}

Stats fetch_stats() { return stats_request(g_server.http, "/__stats"); }

// ------------------------------------------------------------------------------------------
// Callback collector: records everything and counts contract violations
// ------------------------------------------------------------------------------------------
struct Collector {
  std::mutex mu;
  std::string body;
  std::optional<ResponseHead> head;
  std::atomic<int> outcomes{0};
  std::atomic<int> late_callbacks{0};  // any callback after on_done
  std::atomic<bool> done{false};
  std::atomic<bool> head_seen{false};
  std::atomic<long> body_bytes{0};
  Result result;

  Callbacks callbacks() {
    Callbacks cb;
    cb.on_head = [this](const ResponseHead& h) {
      if (done) ++late_callbacks;
      std::lock_guard lock(mu);
      head = h;
      head_seen = true;
    };
    cb.on_body = [this](std::string_view d) {
      if (done) ++late_callbacks;
      body_bytes += static_cast<long>(d.size());
      std::lock_guard lock(mu);
      body.append(d);
      return true;
    };
    cb.on_done = [this](const Result& r) {
      {
        std::lock_guard lock(mu);
        result = r;
      }
      ++outcomes;
      done = true;
    };
    return cb;
  }
};

HttpRequest post(const std::string& url, std::string body = "{}", int deadline_ms = 10000) {
  HttpRequest r;
  r.method = "POST";
  r.url = url;
  r.headers = {{"Content-Type", "application/json"}};
  r.body = std::move(body);
  r.deadline = steady_clock::now() + milliseconds(deadline_ms);
  return r;
}

const char* name_of(Status s) {
  switch (s) {
    case Status::Completed: return "Completed";
    case Status::Cancelled: return "Cancelled";
    case Status::DeadlineExceeded: return "DeadlineExceeded";
    case Status::Failed: return "Failed";
  }
  return "?";
}
const char* name_of(FailureKind k) {
  switch (k) {
    case FailureKind::None: return "None";
    case FailureKind::Resolve: return "Resolve";
    case FailureKind::Connect: return "Connect";
    case FailureKind::Tls: return "Tls";
    case FailureKind::Send: return "Send";
    case FailureKind::Receive: return "Receive";
    case FailureKind::Truncated: return "Truncated";
    case FailureKind::Protocol: return "Protocol";
    case FailureKind::ResendRefused: return "ResendRefused";
    case FailureKind::ResponseTooLarge: return "ResponseTooLarge";
    case FailureKind::CallbackError: return "CallbackError";
    case FailureKind::Misuse: return "Misuse";
    case FailureKind::Other: return "Other";
  }
  return "?";
}
const char* name_of(Stage s) {
  switch (s) {
    case Stage::Queued: return "Queued";
    case Stage::Resolved: return "Resolved";
    case Stage::Connected: return "Connected";
    case Stage::RequestStarted: return "RequestStarted";
    case Stage::ResponseStarted: return "ResponseStarted";
  }
  return "?";
}

// ------------------------------------------------------------------------------------------
// M1 experiments
// ------------------------------------------------------------------------------------------

TEST(basic_roundtrip_and_connection_reuse) {
  Transport t;
  const RuntimeInfo info = t.runtime_info();
  note("libcurl %s, %s, http2=%d, async_dns=%d", info.curl_version.c_str(), info.ssl_backend.c_str(), info.http2,
       info.async_dns);

  const Stats before = fetch_stats();
  auto c1 = std::make_shared<Collector>();
  Result r1 = t.start(post(g_server.url("/ok")), c1->callbacks()).join();
  CHECK_MSG(r1.status == Status::Completed, "%s/%s", name_of(r1.status), name_of(r1.failure));
  CHECK(r1.http_status == 200);
  CHECK(c1->body == "ok");
  CHECK(c1->head && c1->head->framing == BodyFraming::ContentLength);
  CHECK(r1.attempt.reached == Stage::ResponseStarted);
  CHECK(!r1.attempt.connection_reused);
  CHECK(r1.attempt.transport_internal_resends == 0);

  auto c2 = std::make_shared<Collector>();
  Result r2 = t.start(post(g_server.url("/ok")), c2->callbacks()).join();
  CHECK(r2.status == Status::Completed);
  CHECK(r2.attempt.connection_reused);
  const Stats after = fetch_stats();
  // two requests, one TCP connection (the stats fetch itself is one more accept)
  CHECK_MSG(after["accepted"] - before["accepted"] - 1 == 1, "connections=%ld", after["accepted"] - before["accepted"] - 1);
  CHECK(after["ok"] - before["ok"] == 2);

  auto c3 = std::make_shared<Collector>();
  Result r3 = t.start(post(g_server.url("/echo-len"), std::string(1 << 20, 'u')), c3->callbacks()).join();
  CHECK(r3.status == Status::Completed);
  CHECK_MSG(c3->body == std::to_string(1 << 20), "server saw '%s'", c3->body.c_str());
  CHECK_MSG(r3.attempt.request_body_bytes == (1 << 20), "uploaded %lld bytes", static_cast<long long>(r3.attempt.request_body_bytes));
}

TEST(sse_chunked_stream_framing) {
  Transport t;
  auto c = std::make_shared<Collector>();
  Result r = t.start(post(g_server.url("/sse?n=5&gap=3")), c->callbacks()).join();
  CHECK(r.status == Status::Completed);
  CHECK(c->head && c->head->framing == BodyFraming::Chunked);
  CHECK_MSG(c->body == "data: 0\n\ndata: 1\n\ndata: 2\n\ndata: 3\n\ndata: 4\n\n", "body='%s'", c->body.c_str());
}

// NoTerminalNoSuccess at the transport level: abnormal ends are never Completed.
TEST(abnormal_close_is_never_success) {
  struct Row {
    const char* path;
    Status status;
    FailureKind kind;
    bool kind_must_match;
  };
  const Row rows[] = {
      {"/truncate-chunked", Status::Failed, FailureKind::Truncated, true},
      {"/truncate-length", Status::Failed, FailureKind::Truncated, true},
      {"/close-delimited", Status::Failed, FailureKind::Truncated, true},
      {"/reset-mid-body", Status::Failed, FailureKind::Truncated, true},
      {"/bighead", Status::Failed, FailureKind::ResponseTooLarge, true},
      {"/hugeheader", Status::Failed, FailureKind::ResponseTooLarge, true},
      {"/die", Status::Failed, FailureKind::Receive, true},
      {"/status500", Status::Completed, FailureKind::None, true},  // the transport does not judge HTTP status
  };
  Transport t;
  for (const Row& row : rows) {
    auto c = std::make_shared<Collector>();
    Result r = t.start(post(g_server.url(row.path)), c->callbacks()).join();
    CHECK_MSG(r.status == row.status && (!row.kind_must_match || r.failure == row.kind),
              "%s -> %s/%s (curl %d: %s)", row.path, name_of(r.status), name_of(r.failure), r.curl_code,
              r.detail.c_str());
    CHECK(c->outcomes == 1);
    if (row.kind == FailureKind::ResponseTooLarge) {
      CHECK(!c->head);
      CHECK(c->body.empty());
      CHECK(c->body_bytes == 0);
    }
    note("%-18s -> %s/%s reached=%s", row.path, name_of(r.status), name_of(r.failure), name_of(r.attempt.reached));
  }
}

struct WaitingState {
  const char* name;
  std::string url;
  // blocks until the scripted peer has reached the state, so the peer provably sends nothing more
  std::function<void(Collector&, const Stats&)> reached;
  Stage max_stage;
  Stage min_stage;
  std::string unsupported_reason;
};

std::vector<WaitingState> waiting_states() {
  std::vector<WaitingState> s;
  s.push_back({"header_wait", g_server.url("/stall-header"),
               [](Collector&, const Stats& before) {
                 CHECK(wait_until([&] { return fetch_stats()["stall_header"] > before["stall_header"]; }, 3000));
               },
               Stage::RequestStarted, Stage::RequestStarted});
  s.push_back({"partial_body_then_stall", g_server.url("/hold"),
               [](Collector& c, const Stats&) { CHECK(wait_until([&] { return c.head_seen.load(); }, 3000)); },
               Stage::ResponseStarted, Stage::ResponseStarted});
  s.push_back({"connect_stall", "http://127.0.0.1:" + std::to_string(g_server.blackhole) + "/",
               [](Collector&, const Stats&) { std::this_thread::sleep_for(milliseconds(250)); }, Stage::Resolved,
               Stage::Queued, g_server.blackhole_reason});
  s.push_back({"tls_handshake_stall", "https://127.0.0.1:" + std::to_string(g_server.silent) + "/",
               [](Collector&, const Stats& before) {
                 CHECK(wait_until([&] { return fetch_stats()["silent_accepted"] > before["silent_accepted"]; }, 3000));
                 std::this_thread::sleep_for(milliseconds(100));
               },
               Stage::Resolved, Stage::Resolved});  // libcurl reports Connected only after TLS completes
  return s;
}

// DeadlineExceeded in every waiting state, no peer progress, bounded overshoot.
TEST(deadline_in_each_waiting_state) {
  Transport t;
  for (const WaitingState& s : waiting_states()) {
    if (!s.unsupported_reason.empty()) {
      note("SKIP CELL %s/%s: %s", g_current.c_str(), s.name, s.unsupported_reason.c_str());
      continue;
    }
    auto c = std::make_shared<Collector>();
    const auto start = steady_clock::now();
    Result r = t.start(post(s.url, "{}", 300), c->callbacks()).join();
    const double took = ms_since(start);
    CHECK_MSG(r.status == Status::DeadlineExceeded, "%s -> %s/%s", s.name, name_of(r.status), name_of(r.failure));
    CHECK_MSG(took >= 290 && took < 800, "%s took %.0f ms", s.name, took);
    CHECK_MSG(r.attempt.reached >= s.min_stage && r.attempt.reached <= s.max_stage, "%s reached %s", s.name,
              name_of(r.attempt.reached));
    CHECK(c->outcomes == 1 && c->late_callbacks == 0);
    note("%-24s deadline fired after %.0f ms at stage %s", s.name, took, name_of(r.attempt.reached));
  }
}

// CancelWithoutPeerProgress: the peer sends nothing after the state is reached; cancel alone ends it.
TEST(cancel_without_peer_progress) {
  Transport t;
  for (const WaitingState& s : waiting_states()) {
    if (!s.unsupported_reason.empty()) {
      note("SKIP CELL %s/%s: %s", g_current.c_str(), s.name, s.unsupported_reason.c_str());
      continue;
    }
    auto c = std::make_shared<Collector>();
    const Stats before = fetch_stats();
    Operation op = t.start(post(s.url, "{}", 30000), c->callbacks());
    s.reached(*c, before);
    CHECK(!c->done);
    const auto start = steady_clock::now();
    op.cancel();
    Result r = op.join();
    const double took = ms_since(start);
    CHECK_MSG(r.status == Status::Cancelled, "%s -> %s/%s", s.name, name_of(r.status), name_of(r.failure));
    CHECK_MSG(took < 1000, "%s: cancel took %.0f ms (anti-hang guard 1000)", s.name, took);
    CHECK(c->outcomes == 1 && c->late_callbacks == 0);
    note("%-24s cancelled in %.1f ms at stage %s", s.name, took, name_of(r.attempt.reached));
  }
}

// ExactlyOneOutcome under cancel/deadline/completion/handle-drop races.
TEST(exactly_one_outcome_under_races) {
  Transport t;
  std::mt19937 rng(12345);
  const int kBatches = 20, kPerBatch = 100;
  long total = 0, completed = 0, cancelled = 0, deadline = 0, failed = 0;
  for (int b = 0; b < kBatches; ++b) {
    std::vector<std::shared_ptr<Collector>> collectors;
    std::vector<Operation> ops;
    for (int i = 0; i < kPerBatch; ++i) {
      auto c = std::make_shared<Collector>();
      const bool streaming = (i % 3) == 0;
      HttpRequest req = post(g_server.url(streaming ? "/sse?n=3&gap=1" : "/ok"), "{}", 5000);
      if (i % 7 == 0) req.deadline = steady_clock::now() + std::chrono::microseconds(rng() % 3000);
      collectors.push_back(c);
      ops.push_back(t.start(std::move(req), c->callbacks()));
      std::this_thread::sleep_for(std::chrono::microseconds(rng() % 200));
      switch (rng() % 4) {
        case 0: ops.back().cancel(); break;
        case 1: { Operation dropped = std::move(ops.back()); break; }  // handle dropped while running
        default: break;
      }
    }
    for (auto& c : collectors) CHECK(wait_until([&] { return c->done.load(); }, 10000));
    for (auto& c : collectors) {
      ++total;
      CHECK_MSG(c->outcomes == 1, "outcomes=%d", c->outcomes.load());
      CHECK(c->late_callbacks == 0);
      switch (c->result.status) {
        case Status::Completed: ++completed; break;
        case Status::Cancelled: ++cancelled; break;
        case Status::DeadlineExceeded: ++deadline; break;
        case Status::Failed: ++failed; break;
      }
    }
  }
  note("%ld ops: %ld completed, %ld cancelled, %ld deadline, %ld failed", total, completed, cancelled, deadline, failed);
  CHECK_MSG(failed == 0, "unexpected failures: %ld", failed);  // /ok and /sse never fail on their own
  CHECK(cancelled > 0 && completed > 0);                       // the race window was actually exercised
}

TEST(callback_exception_fails_the_operation_only) {
  Transport t;
  {
    auto c = std::make_shared<Collector>();
    Callbacks cb = c->callbacks();
    cb.on_body = [](std::string_view) -> bool { throw std::runtime_error("boom"); };
    Result r = t.start(post(g_server.url("/sse?n=3&gap=2")), std::move(cb)).join();
    CHECK_MSG(r.status == Status::Failed && r.failure == FailureKind::CallbackError, "%s/%s", name_of(r.status),
              name_of(r.failure));
    CHECK(c->outcomes == 1);
  }
  {
    auto c = std::make_shared<Collector>();
    Callbacks cb = c->callbacks();
    cb.on_head = [](const ResponseHead&) { throw std::runtime_error("boom"); };
    Result r = t.start(post(g_server.url("/ok")), std::move(cb)).join();
    CHECK_MSG(r.status == Status::Failed && r.failure == FailureKind::CallbackError, "%s/%s", name_of(r.status),
              name_of(r.failure));
    CHECK(c->outcomes == 1);
  }
  auto c = std::make_shared<Collector>();
  Result r = t.start(post(g_server.url("/ok")), c->callbacks()).join();
  CHECK(r.status == Status::Completed);  // the transport survived
}

TEST(join_from_io_thread_is_misuse) {
  Transport t;
  auto holder = std::make_shared<Operation>();
  std::promise<Result> inner;
  auto inner_future = inner.get_future();
  auto c = std::make_shared<Collector>();
  Callbacks cb = c->callbacks();
  cb.on_done = [&, c](const Result& r) {
    c->result = r;
    inner.set_value(holder->join());  // must return immediately, not deadlock
    ++c->outcomes;
    c->done = true;
  };
  *holder = t.start(post(g_server.url("/hold"), "{}", 30000), std::move(cb));
  CHECK(wait_until([&] { return c->head_seen.load(); }, 3000));
  holder->cancel();
  CHECK(inner_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  if (inner_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    CHECK(inner_future.get().failure == FailureKind::Misuse);
  CHECK(wait_until([&] { return c->done.load(); }, 2000));
  CHECK(holder->join().status == Status::Cancelled);  // from a non-I/O thread join() works
}

// libcurl may silently resend on a fresh connection when a reused one dies before any response
// byte. The one-attempt contract (DESIGN.md section 7) counts that as a second attempt.
TEST(one_attempt_no_internal_resend) {
  Transport t;
  auto warm = std::make_shared<Collector>();
  CHECK(t.start(post(g_server.url("/ok")), warm->callbacks()).join().status == Status::Completed);
  const Stats before = fetch_stats();
  auto c = std::make_shared<Collector>();
  Result r = t.start(post(g_server.url("/die"), "{\"x\":1}"), c->callbacks()).join();
  const Stats after = fetch_stats();
  const long die_requests = after["die"] - before["die"];
  note("reused=%d resends=%u -> %s/%s; server saw %ld /die request(s)", r.attempt.connection_reused,
       r.attempt.transport_internal_resends, name_of(r.status), name_of(r.failure), die_requests);
  CHECK_MSG(die_requests == 1, "the server observed %ld requests for one attempt", die_requests);
  CHECK(r.status == Status::Failed);
  CHECK(c->outcomes == 1);
}

// Backpressure: refusing bytes stops reading the socket, so the server's writes plateau and the
// client does not buffer the response; resume() then drains it completely.
TEST(backpressure_http1_pause_resume) {
  const long kTotal = 64L << 20;
  const long kAccept = 1L << 20;
  Transport t;
  std::atomic<bool> unlimited{false};
  std::atomic<long> received{0};
  auto c = std::make_shared<Collector>();
  Callbacks cb = c->callbacks();
  cb.on_body = [&](std::string_view d) {
    if (!unlimited && received + static_cast<long>(d.size()) > kAccept) return false;
    received += static_cast<long>(d.size());
    return true;
  };
  const Stats before = fetch_stats();
  const long long rss_before = rss_bytes();
  Operation op = t.start(post(g_server.url("/flood?total=" + std::to_string(kTotal)), "{}", 60000), std::move(cb));

  CHECK(wait_until([&] { return received.load() >= kAccept - (32 << 10); }, 5000));
  std::this_thread::sleep_for(milliseconds(700));
  const long w1 = fetch_stats()["flood_written"] - before["flood_written"];
  std::this_thread::sleep_for(milliseconds(500));
  const long w2 = fetch_stats()["flood_written"] - before["flood_written"];
  const long long rss_growth = rss_bytes() - rss_before;
  note("paused: client consumed %ld B; server wrote %ld then %ld B of %ld; RSS +%lld B", received.load(), w1, w2, kTotal,
       rss_growth);
  CHECK_MSG(w1 == w2, "server writes still growing while the client is paused (%ld -> %ld)", w1, w2);
  CHECK_MSG(w2 < kTotal / 2, "server was able to push %ld of %ld bytes into a paused client", w2, kTotal);
  CHECK_MSG(rss_growth < (24L << 20), "client RSS grew %lld B while paused", rss_growth);
  CHECK(!c->done);

  unlimited = true;
  op.resume();
  Result r = op.join();
  CHECK_MSG(r.status == Status::Completed, "%s/%s", name_of(r.status), name_of(r.failure));
  CHECK_MSG(received == kTotal, "received %ld of %ld", received.load(), kTotal);
  CHECK(fetch_stats()["flood_done"] - before["flood_done"] == 1);
}

// A bounded consumer queue with a slow consumer thread: no lost wakeups, bounded occupancy.
TEST(backpressure_bounded_queue_no_lost_wakeup) {
  const long kTotal = 8L << 20;
  const size_t kCapacity = 128 << 10;
  Transport t;
  for (int rep = 0; rep < 5; ++rep) {
    std::mutex mu;
    size_t queued = 0, max_queued = 0;
    long consumed = 0;
    std::atomic<bool> producer_done{false};
    Operation* op_ptr = nullptr;
    std::atomic<bool> op_ready{false};
    auto c = std::make_shared<Collector>();
    Callbacks cb = c->callbacks();
    cb.on_body = [&](std::string_view d) {
      std::lock_guard lock(mu);
      if (queued + d.size() > kCapacity) return false;
      queued += d.size();
      max_queued = std::max(max_queued, queued);
      return true;
    };
    Operation op = t.start(post(g_server.url("/flood?total=" + std::to_string(kTotal)), "{}", 60000), std::move(cb));
    op_ptr = &op;
    op_ready = true;
    std::thread consumer([&] {
      while (consumed < kTotal) {
        size_t take = 0;
        {
          std::lock_guard lock(mu);
          take = std::min<size_t>(queued, 16 << 10);
          queued -= take;
        }
        if (take == 0) {
          std::this_thread::sleep_for(std::chrono::microseconds(50));
          continue;
        }
        consumed += static_cast<long>(take);
        std::this_thread::sleep_for(std::chrono::microseconds(100));  // slow consumer
        op_ptr->resume();  // room was made; on_body re-checks, so a spurious resume is harmless
      }
      producer_done = true;
    });
    Result r = op.join();
    consumer.join();
    CHECK_MSG(r.status == Status::Completed, "rep %d: %s/%s", rep, name_of(r.status), name_of(r.failure));
    CHECK_MSG(consumed == kTotal, "rep %d: consumed %ld of %ld", rep, consumed, kTotal);
    CHECK_MSG(max_queued <= kCapacity, "rep %d: queue occupancy %zu exceeded capacity", rep, max_queued);
  }
}

// AdmissionIndependentOfHeldStreams: K = 4T + 64 streams held open, a short call still completes,
// and the process thread count does not depend on K.
TEST(admission_independent_of_held_streams) {
  const int base_threads = thread_count();
  TransportOptions options;
  options.io_threads = 2;
  Transport t(options);
  const int T = static_cast<int>(t.io_threads());
  const int K = 4 * T + 64;
  const Stats before = fetch_stats();

  std::vector<std::shared_ptr<Collector>> holders;
  std::vector<Operation> ops;
  auto open_streams = [&](int count) {
    for (int i = 0; i < count; ++i) {
      auto c = std::make_shared<Collector>();
      holders.push_back(c);
      ops.push_back(t.start(post(g_server.url("/hold"), "{}", 60000), c->callbacks()));
    }
    CHECK(wait_until([&] {
      for (auto& h : holders)
        if (!h->head_seen) return false;
      return true;
    }, 10000));
  };

  open_streams(K);
  CHECK(fetch_stats()["held"] >= K);
  const int threads_at_K = thread_count();

  auto probe = std::make_shared<Collector>();
  const auto start = steady_clock::now();
  Result r = t.start(post(g_server.url("/ok"), "{}", 2000), probe->callbacks()).join();
  const double short_call_ms = ms_since(start);
  CHECK_MSG(r.status == Status::Completed, "short call: %s/%s", name_of(r.status), name_of(r.failure));
  CHECK_MSG(short_call_ms < 1000, "short call took %.0f ms with %d held streams", short_call_ms, K);

  open_streams(K);  // double the held streams: 2K
  const int threads_at_2K = thread_count();
  note("threads: baseline %d, %d held -> %d, %d held -> %d; short call %.1f ms", base_threads, K, threads_at_K, 2 * K,
       threads_at_2K, short_call_ms);
  CHECK_MSG(threads_at_K - base_threads <= T + 8, "library-caused threads: %d", threads_at_K - base_threads);
  CHECK_MSG(threads_at_2K == threads_at_K, "thread count grew with held streams: %d -> %d", threads_at_K, threads_at_2K);

  for (auto& op : ops) op.cancel();
  for (auto& c : holders) CHECK(wait_until([&] { return c->done.load(); }, 5000));
  for (auto& c : holders) CHECK(c->outcomes == 1 && c->result.status == Status::Cancelled);
  CHECK(wait_until([&] { return fetch_stats()["held"] == 0; }, 5000));  // sockets really closed server-side
  (void)before;
}

// ------------------------------------------------------------------------------------------
// Name resolution (risk R2). libcurl's own threaded resolver starts one thread per concurrent
// lookup (measured: 72 simultaneous lookups of distinct names -> +72 threads), so the transport
// resolves names itself: single-flight per host, TTL cache, bounded resolver threads, injectable
// resolver. IP literals bypass it, which is why the earlier tests use 127.0.0.1.
// ------------------------------------------------------------------------------------------
struct FakeResolver {
  std::atomic<int> calls{0}, active{0}, max_active{0}, delay_ms{0};
  std::mutex mu;
  std::condition_variable cv;
  bool blocked = false;

  std::vector<std::string> addrs{"127.0.0.1"};  // set before the transport starts

  std::vector<std::string> operator()(const std::string&) {
    ++calls;
    const int now_active = ++active;
    int seen = max_active.load();
    while (now_active > seen && !max_active.compare_exchange_weak(seen, now_active)) {}
    {
      std::unique_lock lock(mu);
      cv.wait(lock, [this] { return !blocked; });
    }
    if (delay_ms > 0) std::this_thread::sleep_for(milliseconds(delay_ms));
    --active;
    return addrs;
  }
  void block() { std::lock_guard lock(mu); blocked = true; }
  void release() { { std::lock_guard lock(mu); blocked = false; } cv.notify_all(); }
};

TransportOptions options_with(const std::shared_ptr<FakeResolver>& fr, unsigned resolver_threads = 2) {
  TransportOptions o;
  o.io_threads = 2;
  o.resolver_threads = resolver_threads;
  o.resolve = [fr](const std::string& host) { return (*fr)(host); };
  return o;
}

struct ThreadPeak {
  std::atomic<bool> sampling{true};
  std::atomic<int> peak{0};
  std::thread sampler{[this] {
    while (sampling) {
      peak = std::max(peak.load(), thread_count());
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }};
  int stop() {
    sampling = false;
    sampler.join();
    return peak.load();
  }
};

std::string fake_url(const std::string& host, const std::string& path) {
  return "http://" + host + ":" + std::to_string(g_server.http) + path;
}

bool all_have_head_or_done(const std::vector<std::shared_ptr<Collector>>& cs) {
  for (auto& c : cs)
    if (!c->head_seen && !c->done) return false;
  return true;
}

TEST(dns_single_flight_and_ttl_cache) {
  auto fr = std::make_shared<FakeResolver>();
  fr->delay_ms = 50;  // widen the window in which concurrent lookups would overlap
  TransportOptions o = options_with(fr);
  o.dns_ttl = std::chrono::seconds(1);
  Transport t(o);

  std::vector<std::shared_ptr<Collector>> cs;
  std::vector<Operation> ops;
  for (int i = 0; i < 72; ++i) {
    auto c = std::make_shared<Collector>();
    cs.push_back(c);
    ops.push_back(t.start(post(fake_url("sp-fake.test", "/hold"), "{}", 20000), c->callbacks()));
  }
  CHECK(wait_until([&] { return all_have_head_or_done(cs); }, 10000));
  for (auto& c : cs) CHECK(c->head_seen);
  CHECK_MSG(fr->calls == 1, "72 concurrent operations for one host ran %d lookups", fr->calls.load());
  for (auto& op : ops) op.cancel();
  for (auto& c : cs) CHECK(wait_until([&] { return c->done.load(); }, 5000));

  auto warm = std::make_shared<Collector>();
  CHECK(t.start(post(fake_url("sp-fake.test", "/ok")), warm->callbacks()).join().status == Status::Completed);
  CHECK_MSG(fr->calls == 1, "a cached name was looked up again (%d lookups)", fr->calls.load());

  std::this_thread::sleep_for(milliseconds(1250));  // past the TTL
  auto expired = std::make_shared<Collector>();
  CHECK(t.start(post(fake_url("sp-fake.test", "/ok")), expired->callbacks()).join().status == Status::Completed);
  CHECK_MSG(fr->calls == 2, "expired entry was not refreshed (%d lookups)", fr->calls.load());
}

TEST(dns_threads_independent_of_distinct_names) {
  auto fr = std::make_shared<FakeResolver>();
  fr->delay_ms = 30;
  const int base = thread_count();
  ThreadPeak peak;
  Transport t(options_with(fr, 2));
  std::vector<std::shared_ptr<Collector>> cs;
  std::vector<Operation> ops;
  for (int i = 0; i < 72; ++i) {
    auto c = std::make_shared<Collector>();
    cs.push_back(c);
    ops.push_back(t.start(post(fake_url("sp-fake-" + std::to_string(i) + ".test", "/hold"), "{}", 20000), c->callbacks()));
  }
  CHECK(wait_until([&] { return all_have_head_or_done(cs); }, 20000));
  for (auto& c : cs) CHECK(c->head_seen);
  const int extra = peak.stop() - base - 1;  // minus the sampler thread
  note("72 distinct names: %d lookups, max %d concurrent; library threads at peak %d (2 I/O + 2 resolver)", fr->calls.load(),
       fr->max_active.load(), extra);
  CHECK(fr->calls == 72);
  CHECK_MSG(fr->max_active <= 2, "resolver concurrency %d exceeded its pool", fr->max_active.load());
  CHECK_MSG(extra <= 8, "library-caused threads: %d", extra);
  for (auto& op : ops) op.cancel();
  for (auto& c : cs) CHECK(wait_until([&] { return c->done.load(); }, 5000));
}

// CancelWithoutPeerProgress / deadline in the DNS state, and shutdown with a stuck lookup.
TEST(dns_state_cancel_deadline_and_shutdown) {
  auto fr = std::make_shared<FakeResolver>();
  fr->block();
  {
    Transport t(options_with(fr));
    auto c = std::make_shared<Collector>();
    const auto start = steady_clock::now();
    Result r = t.start(post(fake_url("sp-stuck.test", "/ok"), "{}", 300), c->callbacks()).join();
    CHECK_MSG(r.status == Status::DeadlineExceeded, "%s/%s", name_of(r.status), name_of(r.failure));
    CHECK(r.attempt.reached == Stage::Queued);
    CHECK_MSG(ms_since(start) < 800, "deadline in DNS state took %.0f ms", ms_since(start));

    auto c2 = std::make_shared<Collector>();
    Operation op = t.start(post(fake_url("sp-stuck.test", "/ok"), "{}", 30000), c2->callbacks());
    std::this_thread::sleep_for(milliseconds(100));
    const auto cancel_start = steady_clock::now();
    op.cancel();
    Result r2 = op.join();
    CHECK_MSG(r2.status == Status::Cancelled, "%s/%s", name_of(r2.status), name_of(r2.failure));
    CHECK_MSG(ms_since(cancel_start) < 1000, "cancel in DNS state took %.0f ms", ms_since(cancel_start));
    CHECK(c->outcomes == 1 && c2->outcomes == 1 && c->late_callbacks == 0 && c2->late_callbacks == 0);
  }
  // Transport destruction with a lookup still stuck must not hang or be touched afterwards.
  auto c3 = std::make_shared<Collector>();
  const auto shutdown_start = steady_clock::now();
  {
    auto t = std::make_unique<Transport>(options_with(fr));
    Operation op = t->start(post(fake_url("sp-stuck.test", "/ok"), "{}", 30000), c3->callbacks());
    std::this_thread::sleep_for(milliseconds(50));
    t.reset();
    CHECK(op.join().status == Status::Cancelled);
  }
  CHECK_MSG(ms_since(shutdown_start) < 1500, "shutdown with a stuck lookup took %.0f ms", ms_since(shutdown_start));
  CHECK(c3->outcomes == 1);
  fr->release();  // the abandoned resolver threads finish and find the transport gone
  std::this_thread::sleep_for(milliseconds(150));
}

// Real getaddrinfo: ".invalid" never resolves (RFC 2606), so each operation ends in Resolve.
TEST(dns_real_getaddrinfo_failures_are_bounded) {
  const int base = thread_count();
  ThreadPeak peak;
  Transport t;
  std::vector<std::shared_ptr<Collector>> cs;
  std::vector<Operation> ops;
  for (int i = 0; i < 72; ++i) {
    auto c = std::make_shared<Collector>();
    cs.push_back(c);
    ops.push_back(t.start(post(fake_url("sp-dns-" + std::to_string(i) + ".invalid", "/ok"), "{}", 20000), c->callbacks()));
  }
  CHECK(wait_until([&] { return all_have_head_or_done(cs); }, 30000));
  const int extra = peak.stop() - base - 1;
  int resolve_failed = 0;
  for (auto& c : cs)
    if (c->done && c->result.failure == FailureKind::Resolve) ++resolve_failed;
  note("72 real lookups of .invalid names: %d Resolve failures; library threads at peak %d", resolve_failed, extra);
  CHECK_MSG(resolve_failed == 72, "%d of 72 ended in Resolve", resolve_failed);
  CHECK_MSG(extra <= 8, "library-caused threads: %d", extra);
}

// ------------------------------------------------------------------------------------------
// Regression tests for the independent review of the transport (docs/POC_PLAN.md section 5).
// ------------------------------------------------------------------------------------------

// A transfer coding overrides Content-Length unless libcurl rejects that coding before
// completing the head (8.21 rejects unsolicited gzip with CURLE_BAD_CONTENT_ENCODING).
// An unusable Content-Length gives no framing; obs-fold belongs to the previous field.
// Neither early rejection nor close-delimited EOF may become success.
TEST(framing_follows_effective_message_boundaries) {
  Transport t;
  // Result exposes numeric CURLcodes without exposing libcurl headers to callers.
  constexpr int curl_bad_content_encoding = 61;
  for (const char* path : {"/te-gzip-cl", "/cl-overflow", "/folded-cl"}) {
    auto c = std::make_shared<Collector>();
    Result r = t.start(post(g_server.url(path)), c->callbacks()).join();
    if (std::strcmp(path, "/te-gzip-cl") == 0 && r.curl_code == curl_bad_content_encoding) {
      // content_encoding.c: Curl_build_unencoding_stack rejects an unsolicited coding
      // while http.c parses Transfer-Encoding, before the terminating blank line.
      CHECK(r.status == Status::Failed);
      CHECK(r.failure == FailureKind::Protocol);
      CHECK(!c->head);
      CHECK(!c->head_seen);
      CHECK(c->body.empty());
      CHECK(c->body_bytes == 0);
    } else {
      CHECK_MSG(r.status == Status::Failed && r.failure == FailureKind::Truncated, "%s -> %s/%s (curl %d)", path,
                name_of(r.status), name_of(r.failure), r.curl_code);
      CHECK_MSG(c->head && c->head->framing == BodyFraming::CloseDelimited, "%s framing", path);
    }
    CHECK(c->outcomes == 1);
    CHECK(c->late_callbacks == 0);
  }
  auto c = std::make_shared<Collector>();
  t.start(post(g_server.url("/folded-cl")), c->callbacks()).join();
  bool has_content_length = false;
  std::string note_value;
  if (c->head) {
    for (const Header& h : c->head->headers) {
      if (h.name == "content-length") has_content_length = true;
      if (h.name == "x-note") note_value = h.value;
    }
  }
  CHECK_MSG(!has_content_length, "a folded continuation became a Content-Length field");
  CHECK_MSG(note_value == "first Content-Length: 5", "x-note was '%s'", note_value.c_str());
}

TEST(request_bodies_are_sent_for_every_body_method) {
  Transport t;
  for (const char* method : {"PUT", "PATCH", "DELETE", "POST"}) {
    HttpRequest req = post(g_server.url("/echo-len"), std::string(1000, 'b'));
    req.method = method;
    auto c = std::make_shared<Collector>();
    Result r = t.start(std::move(req), c->callbacks()).join();
    CHECK_MSG(r.status == Status::Completed && c->body == "1000", "%s -> %s, server saw body length '%s'", method,
              name_of(r.status), c->body.c_str());
  }
  HttpRequest get = post(g_server.url("/ok"), "oops");
  get.method = "GET";
  auto c = std::make_shared<Collector>();
  Result r = t.start(std::move(get), c->callbacks()).join();
  CHECK_MSG(r.status == Status::Failed && c->outcomes == 1, "a GET with a body must be rejected, got %s", name_of(r.status));
}

// Ambient proxy variables must not redirect traffic, and so must not bypass the bounded resolver.
TEST(ambient_proxy_environment_is_ignored) {
  portable::set_env("http_proxy", "http://127.0.0.1:9");
  portable::set_env("all_proxy", "http://sp-proxy.invalid:3128");
  Transport t;
  auto c = std::make_shared<Collector>();
  Result r = t.start(post(g_server.url("/ok")), c->callbacks()).join();
  portable::unset_env("http_proxy");
  portable::unset_env("all_proxy");
  CHECK_MSG(r.status == Status::Completed && c->body == "ok", "%s/%s (curl %d)", name_of(r.status), name_of(r.failure), r.curl_code);
}

// A complete 1xx block is progress: a later stall must not be reported as an earlier stage.
TEST(informational_response_counts_as_response_started) {
  Transport t;
  auto c = std::make_shared<Collector>();
  Operation op = t.start(post(g_server.url("/continue-then-hold"), "{}", 30000), c->callbacks());
  CHECK(wait_until([&] { return fetch_stats()["held"] >= 1; }, 3000));
  std::this_thread::sleep_for(milliseconds(100));
  op.cancel();
  Result r = op.join();
  CHECK(r.status == Status::Cancelled);
  CHECK_MSG(r.attempt.reached == Stage::ResponseStarted, "reached %s", name_of(r.attempt.reached));
  CHECK(!r.attempt.response_head_seen);  // no final head was delivered
}

// An exception thrown while resume() redelivers retained bytes fails the operation: libcurl reports
// it only through curl_easy_pause()'s return value.
TEST(resume_redelivery_error_fails_the_operation) {
  Transport t;
  std::atomic<int> calls{0};
  std::atomic<bool> refused{false};
  auto c = std::make_shared<Collector>();
  Callbacks cb = c->callbacks();
  cb.on_body = [&](std::string_view) -> bool {
    const int call = calls++;
    if (call == 0) {
      refused = true;
      return false;  // pause; the bytes are retained and redelivered on resume
    }
    if (call == 1) throw std::runtime_error("redelivery failed");  // thrown from inside resume()
    return true;  // an implementation that swallowed the error would carry on and complete
  };
  Operation op = t.start(post(g_server.url("/flood?total=1048576")), std::move(cb));
  CHECK(wait_until([&] { return refused.load(); }, 3000));
  op.resume();
  CHECK(wait_until([&] { return c->done.load(); }, 3000));
  Result r = op.join();
  CHECK_MSG(r.status == Status::Failed && r.failure == FailureKind::CallbackError, "%s/%s", name_of(r.status),
            name_of(r.failure));
  CHECK(c->outcomes == 1 && c->late_callbacks == 0);
}

// Shared body of the pause probes: the consumer refuses everything after `accept` bytes; the
// server's writes must plateau and the client must not buffer; resume() must then drain it all.
void expect_paused_plateau(Transport& t, const std::string& flood_url, long accept, const char* label,
                           bool expect_reused) {
  const long kTotal = 64L << 20;
  std::atomic<bool> unlimited{false}, refused{false};
  std::atomic<long> received{0};
  auto c = std::make_shared<Collector>();
  Callbacks cb = c->callbacks();
  cb.on_body = [&](std::string_view d) {
    if (!unlimited && received + static_cast<long>(d.size()) > accept) {
      refused = true;
      return false;
    }
    received += static_cast<long>(d.size());
    return true;
  };
  const Stats before = fetch_stats();
  const long long rss_before = rss_bytes();
  Operation op = t.start(post(flood_url + "?total=" + std::to_string(kTotal), "{}", 60000), std::move(cb));
  CHECK_MSG(wait_until([&] { return refused.load(); }, 5000), "%s: the consumer was never asked to pause", label);
  // Pausing the consumer does not instantly stop writes into kernel socket buffers.
  // Require a measured plateau within a bounded wait, not an assumed 700 ms settle time.
  long w2 = fetch_stats()["flood_written"] - before["flood_written"];
  long w1 = w2;
  bool plateau = false;
  const auto settle_deadline = steady_clock::now() + milliseconds(5000);
  while (steady_clock::now() < settle_deadline) {
    w1 = w2;
    std::this_thread::sleep_for(milliseconds(500));
    w2 = fetch_stats()["flood_written"] - before["flood_written"];
    if (w2 >= kTotal / 2) break;
    if (w1 == w2) {
      plateau = w2 > 0;
      break;
    }
  }
  const long long growth = rss_bytes() - rss_before;
  note("%s: consumed %ld B; server wrote %ld then %ld of %ld; RSS +%lld B", label, received.load(), w1, w2, kTotal, growth);
  CHECK_MSG(plateau, "%s: server did not plateau while paused (last samples %ld -> %ld)", label, w1, w2);
  CHECK_MSG(w2 < kTotal / 2, "%s: server pushed %ld bytes into a paused client", label, w2);
  CHECK_MSG(growth < (24L << 20), "%s: client RSS grew %lld B while paused", label, growth);
  unlimited = true;
  op.resume();
  Result r = op.join();
  CHECK_MSG(r.status == Status::Completed && received == kTotal, "%s: %s/%s received %ld", label, name_of(r.status),
            name_of(r.failure), received.load());
  if (expect_reused) CHECK_MSG(r.attempt.connection_reused, "%s: the connection was not reused", label);
}

// The very first body delivery on a warmed connection is refused: the pause may arrive before the
// transport has learned which socket carries the response.
TEST(pause_on_first_delivery_of_a_reused_connection) {
  Transport t;
  auto warm = std::make_shared<Collector>();
  CHECK(t.start(post(g_server.url("/ok")), warm->callbacks()).join().status == Status::Completed);
  expect_paused_plateau(t, g_server.url("/flood"), 0, "reused connection, refuse from byte 0", true);
}

// Several candidate addresses, the first one dead. NOTE: on loopback a refused connect fails inside
// the connect() call, so libcurl never announces a socket for the dead candidate and this does NOT
// reproduce the stale-association case from the review (a candidate that stays in flight while a
// later one wins). That case is fixed by construction (the association follows read interest and is
// cleared on REMOVE) and is not covered by a killing test; mutating the fix leaves this test green.
// 127.0.0.2 is loopback with no listener.
TEST(pause_with_a_dead_first_candidate_address) {
  auto fr = std::make_shared<FakeResolver>();
  fr->addrs = {"127.0.0.2", "127.0.0.1"};
  Transport t(options_with(fr));
  expect_paused_plateau(t, fake_url("sp-race.test", "/flood"), 1L << 20, "dead first candidate", false);
}

// A callback may destroy the Transport it runs on (directly or through a captured owner). That
// must neither deadlock nor touch freed state.
TEST(transport_destroyed_from_its_own_callback) {
  auto t = std::make_unique<Transport>();
  std::promise<void> destroyed;
  auto destroyed_future = destroyed.get_future();
  auto c = std::make_shared<Collector>();
  Callbacks cb = c->callbacks();
  auto base_done = cb.on_done;
  cb.on_done = [&, base_done](const Result& r) {
    base_done(r);
    t.reset();  // runs on an I/O thread of this very transport
    destroyed.set_value();
  };
  Operation op = t->start(post(g_server.url("/ok")), std::move(cb));
  CHECK_MSG(destroyed_future.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
            "destroying the Transport from its own callback deadlocked");
  if (destroyed_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    CHECK(op.join().status == Status::Completed);
  std::this_thread::sleep_for(milliseconds(300));  // let the detached teardown finish before exit
  Transport again;  // the process is still healthy
  auto c2 = std::make_shared<Collector>();
  CHECK(again.start(post(g_server.url("/ok")), c2->callbacks()).join().status == Status::Completed);
}

// A lookup that never returns must not pin every cancelled operation (and its request body).
TEST(stuck_lookup_does_not_retain_cancelled_operations) {
  auto fr = std::make_shared<FakeResolver>();
  fr->block();
  Transport t(options_with(fr));
  const long long rss_before = rss_bytes();
  const std::string big(1 << 20, 'r');
  for (int i = 0; i < 200; ++i) {
    auto c = std::make_shared<Collector>();
    Operation op = t.start(post(fake_url("sp-stuck.test", "/ok"), big, 30000), c->callbacks());
    op.cancel();
    CHECK(op.join().status == Status::Cancelled);
  }
  const long long growth = rss_bytes() - rss_before;
  note("200 cancelled 1 MiB requests behind one stuck lookup: RSS +%lld B", growth);
#if defined(__SANITIZE_ADDRESS__)
  // AddressSanitizer parks freed blocks in a quarantine of up to 256 MB, so RSS cannot show a
  // release there; the plain and TSan builds carry this check.
  note("RSS bound not asserted under AddressSanitizer (quarantine)");
#else
  CHECK_MSG(growth < (64L << 20), "cancelled operations stay referenced by the stuck lookup (RSS +%lld B)", growth);
#endif
  fr->release();
}

// Destroying the Transport with live operations delivers one Cancelled outcome each.
TEST(shutdown_cancels_active_operations) {
  std::vector<std::shared_ptr<Collector>> cs;
  std::vector<Operation> ops;
  {
    Transport t;
    for (int i = 0; i < 10; ++i) {
      auto c = std::make_shared<Collector>();
      cs.push_back(c);
      ops.push_back(t.start(post(g_server.url("/hold"), "{}", 60000), c->callbacks()));
    }
    CHECK(wait_until([&] {
      for (auto& c : cs)
        if (!c->head_seen) return false;
      return true;
    }, 5000));
  }
  for (auto& c : cs) CHECK(c->outcomes == 1 && c->result.status == Status::Cancelled && c->late_callbacks == 0);
  for (auto& op : ops) CHECK(op.join().status == Status::Cancelled);
  ops.clear();  // handle destruction after the transport is gone must not crash
}

// ------------------------------------------------------------------------------------------
// HTTP/2 backpressure (risk R3): libcurl reads and buffers a paused stream because its socket is
// shared with live streams. The buffering must at least be bounded (curl_easy_pause documents the
// stream window); this experiment measures it. Node and HTTP/2 capability are required, not skipped.
// ------------------------------------------------------------------------------------------
struct NodeServer {
#ifdef _WIN32
  runtime_test::Spawned child;
#else
  pid_t pid = -1;
  int stdin_fd = -1;
#endif
  unsigned h2 = 0, stats = 0;
  std::map<std::string, std::string> fields;

  ~NodeServer() { stop(); }

  unsigned port(const char* key) const {
    const auto it = fields.find(key);
    return it == fields.end() ? 0 : static_cast<unsigned>(std::strtoul(it->second.c_str(), nullptr, 10));
  }

  bool start(const std::string& script) {
#ifdef _WIN32
    const auto configured_node = runtime_test::environment("SP_NODE");
    const char* node = configured_node.empty() ? "node.exe" : configured_node.c_str();
#else
    const char* node = std::getenv("SP_NODE");
    if (!node || !*node) node = "node";
#endif
#ifdef _WIN32
    try {
      child = runtime_test::spawn_with_pipes(node, {script});
    } catch (const std::exception& error) {
      note("node launch failed: %s", error.what());
      return false;
    }
    std::string line;
    const auto deadline = steady_clock::now() + std::chrono::seconds(20);
    try {
      while (line.find('\n') == std::string::npos && line.size() < 8192) {
        const auto remaining = std::chrono::duration_cast<milliseconds>(deadline - steady_clock::now()).count();
        if (remaining <= 0) break;
        char c;
        if (runtime_test::read_pipe(child.from_child.get(), &c, 1, remaining) == 0) break;
        line.push_back(c);
      }
    } catch (const std::exception& error) {
      note("node startup failed: %s", error.what());
      stop();
      return false;
    }
#else
    runtime_test::Pipe input, output;
    int in_pipe[2] = {input.reader.release(), input.writer.release()};
    int out_pipe[2] = {output.reader.release(), output.writer.release()};
    pid = fork();
    if (pid == 0) {
      dup2(in_pipe[0], 0);
      dup2(out_pipe[1], 1);
      close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]);
      execlp(node, node, script.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    if (pid < 0) {
      close(in_pipe[1]); close(out_pipe[0]);
      return false;
    }
    stdin_fd = in_pipe[1];
    std::string line;
    pollfd pfd{out_pipe[0], POLLIN, 0};
    const auto deadline = steady_clock::now() + std::chrono::seconds(20);
    while (line.find('\n') == std::string::npos && line.size() < 8192) {
      const auto remaining = std::chrono::duration_cast<milliseconds>(deadline - steady_clock::now()).count();
      if (remaining <= 0 || poll(&pfd, 1, static_cast<int>(remaining)) <= 0) break;
      char c;
      if (read(out_pipe[0], &c, 1) != 1) break;
      line.push_back(c);
    }
    close(out_pipe[0]);
#endif
    if (line.find('\n') == std::string::npos) { stop(); return false; }
    // TLS peers put ca= last; its remainder is a path, not whitespace-delimited tokens.
    const auto ca = line.find(" ca=");
    if (ca != std::string::npos) {
      auto path = line.substr(ca + 4);
      while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
      fields.emplace("ca", std::move(path));
    }
    std::istringstream tokens(line.substr(0, ca));
    std::string token;
    if (!(tokens >> token) || token != "PORTS") return false;
    while (tokens >> token) {
      const auto equal = token.find('=');
      if (equal != std::string::npos)
        fields.emplace(token.substr(0, equal), token.substr(equal + 1));
    }
    h2 = port("h2");
    stats = port("stats");
    return stats != 0 && (h2 != 0 || port("good") != 0);
  }
  void stop() {
#ifdef _WIN32
    child.to_child.reset();
    child.from_child.reset();
    child.process.reset();
#else
    if (stdin_fd >= 0) {
      close(stdin_fd);
      stdin_fd = -1;
    }
    if (pid > 0) {
      while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
      pid = -1;
    }
#endif
  }
  Stats stats_request(const char* path = "/") const {
    return ::stats_request(stats, path);
  }
  long stat(const char* key) const {
    const Stats snapshot = stats_request();
    const auto it = snapshot.v.find(key);
    return it == snapshot.v.end() ? -1 : it->second;
  }
};
std::string g_h2c_script, g_tls_script, g_h2_adversarial_script;

TEST(backpressure_http2_paused_stream_buffering_is_bounded) {
  NodeServer node;
  const bool started = node.start(g_h2c_script);
  CHECK_MSG(started, "could not start node peer %s", g_h2c_script.c_str());
  if (!started) return;
  const long kTotal = 64L << 20;
  const long kAccept = 1L << 20;
  Transport t;
  std::atomic<bool> unlimited{false};
  std::atomic<long> received{0};
  auto c = std::make_shared<Collector>();
  Callbacks cb = c->callbacks();
  cb.on_body = [&](std::string_view d) {
    if (!unlimited && received + static_cast<long>(d.size()) > kAccept) return false;
    received += static_cast<long>(d.size());
    return true;
  };
  HttpRequest req = post("http://127.0.0.1:" + std::to_string(node.h2) + "/flood?total=" + std::to_string(kTotal), "{}", 60000);
  req.http_version = HttpVersion::Http2PriorKnowledge;
  const long long rss_before = rss_bytes();
  Operation op = t.start(std::move(req), std::move(cb));
  CHECK(wait_until([&] { return received.load() >= kAccept - (32 << 10); }, 5000));
  std::this_thread::sleep_for(milliseconds(1000));
  const long w1 = node.stat("written");
  std::this_thread::sleep_for(milliseconds(500));
  const long w2 = node.stat("written");
  const long long rss_growth = rss_bytes() - rss_before;
  note("h2 paused: client consumed %ld B; server handed %ld then %ld B of %ld to its socket; RSS +%lld B", received.load(),
       w1, w2, kTotal, rss_growth);
  CHECK_MSG(rss_growth < (32L << 20), "paused HTTP/2 stream grew client RSS by %lld B", rss_growth);
  CHECK(!c->done);
  CHECK(c->head && c->head->framing == BodyFraming::Stream);

  unlimited = true;
  op.resume();
  Result r = op.join();
  CHECK_MSG(r.status == Status::Completed, "%s/%s", name_of(r.status), name_of(r.failure));
  CHECK_MSG(received == kTotal, "received %ld of %ld", received.load(), kTotal);
  CHECK(r.attempt.version == ResponseVersion::Http2);
  node.stop();
}

// M1b R4: a custom CA permits TLS without weakening certificate or hostname verification.
// Every rejection must occur before HTTP request bytes reach the peer, even after a trusted call.
TEST(tls_trust_matrix_and_http2_alpn) {
  NodeServer node;
  const bool started = node.start(g_tls_script);
  CHECK(started);
  if (!started) return;
  const std::string ca = node.fields.at("ca");
  const auto url = [&](const char* endpoint) {
    return "https://127.0.0.1:" + std::to_string(node.port(endpoint)) + "/echo";
  };
  const std::string payload("tls-body\0binary", 15);
  for (const auto version : {HttpVersion::Http1_1, HttpVersion::Auto}) {
    Transport t;
    CHECK(t.runtime_info().http2);
    for (int i = 0; i < 2; ++i) {
      auto c = std::make_shared<Collector>();
      HttpRequest req = post(url("good"), payload);
      req.http_version = version;
      req.ca_file = ca;
      const Result r = t.start(std::move(req), c->callbacks()).join();
      const auto expected = version == HttpVersion::Auto ? ResponseVersion::Http2 : ResponseVersion::Http1_1;
      CHECK_MSG(r.status == Status::Completed, "%s/%s curl=%d", name_of(r.status), name_of(r.failure), r.curl_code);
      CHECK(r.attempt.version == expected);
      CHECK(r.attempt.connection_reused == (i != 0));
      CHECK(r.attempt.transport_internal_resends == 0);
      CHECK(c->head && c->head->version == expected);
      CHECK(c->body == payload);
      CHECK(c->outcomes == 1 && c->late_callbacks == 0);
    }
    // A pooled connection validated against the custom CA cannot grant trust to a request
    // which deliberately uses only the system trust store.
    auto c = std::make_shared<Collector>();
    HttpRequest req = post(url("good"), payload);
    req.http_version = version;
    const Result r = t.start(std::move(req), c->callbacks()).join();
    CHECK(r.status == Status::Failed && r.failure == FailureKind::Tls);
    CHECK(r.attempt.reached < Stage::RequestStarted);
    CHECK(r.attempt.request_body_bytes == 0 && !r.attempt.response_head_seen);
    CHECK(c->body.empty() && c->outcomes == 1 && c->late_callbacks == 0);
  }
  struct Row { const char* name; const char* endpoint; bool missing_ca; };
  for (const Row row : {Row{"wrong-host", "wrong", false},
                        Row{"expired", "expired", false},
                        Row{"self-signed", "untrusted", false},
                        Row{"missing-ca-file", "good", true}}) {
    Transport t;
    auto c = std::make_shared<Collector>();
    HttpRequest req = post(url(row.endpoint), payload);
    req.ca_file = row.missing_ca ? ca + ".missing" : ca;
    const Result r = t.start(std::move(req), c->callbacks()).join();
    note("TLS %s: %s/%s, curl=%d, request bytes=%lld", row.name, name_of(r.status), name_of(r.failure),
         r.curl_code, static_cast<long long>(r.attempt.request_body_bytes));
    CHECK(r.status == Status::Failed && r.failure == FailureKind::Tls);
    CHECK(r.attempt.reached < Stage::RequestStarted);
    CHECK(r.attempt.request_body_bytes == 0 && !r.attempt.response_head_seen);
    CHECK(c->body.empty() && c->outcomes == 1 && c->late_callbacks == 0);
  }
  const Stats stats = node.stats_request();
  CHECK(stats["good_requests"] == 4);
  CHECK(stats["good_h1"] == 2 && stats["good_h2"] == 2);
  CHECK(stats["wrong_requests"] == 0);
  CHECK(stats["expired_requests"] == 0);
  CHECK(stats["untrusted_requests"] == 0);
  note("TLS oracle: trusted requests=%ld (h1=%ld h2=%ld); invalid peers saw %ld HTTP requests",
       stats["good_requests"], stats["good_h1"], stats["good_h2"],
       stats["wrong_requests"] + stats["expired_requests"] + stats["untrusted_requests"]);
}

HttpRequest h2_post(const NodeServer& node, const std::string& path, int deadline_ms = 15000,
                    const char* host = "127.0.0.1") {
  HttpRequest req = post("http://" + std::string(host) + ":" + std::to_string(node.h2) + path, "{}", deadline_ms);
  req.http_version = HttpVersion::Http2PriorKnowledge;
  return req;
}

// Count bytes without retaining them, so RSS measures the transport rather than the consumer.
struct FloodCollector {
  Collector result;
  std::atomic<bool> paused{false}, released{false}, valid{true};
  std::atomic<long> bytes{0};
  long accept_before_pause = 0;

  Callbacks callbacks() {
    Callbacks cb = result.callbacks();
    cb.on_body = [this](std::string_view data) {
      if (!released && bytes + static_cast<long>(data.size()) > accept_before_pause) {
        paused = true;
        return false;
      }
      if (result.done) ++result.late_callbacks;
      if (data.find_first_not_of('z') != std::string_view::npos) valid = false;
      bytes += static_cast<long>(data.size());
      return true;
    };
    return cb;
  }
};

// M1b R3: the oracle proves all transfers share one connection; paused-stream buffering must
// remain bounded while active siblings progress, and resume must deliver every byte exactly once.
TEST(http2_paused_stream_has_live_multiplexed_siblings) {
  NodeServer node;
  const bool started = node.start(g_h2_adversarial_script);
  CHECK(started);
  if (!started) return;
  TransportOptions options;
  options.max_host_connections = 1;
  Transport t(options);
  auto warm = std::make_shared<Collector>();
  CHECK(t.start(h2_post(node, "/ok?key=warm"), warm->callbacks()).join().status == Status::Completed);
  constexpr long total = 64L << 20, sibling_total = 4L << 20;
  FloodCollector held;
  held.accept_before_pause = 1L << 20;
  const long long rss_before = rss_bytes();
  Operation paused = t.start(h2_post(node, "/flood?key=held&total=" + std::to_string(total), 60000), held.callbacks());
  CHECK(wait_until([&] { return held.paused.load(); }, 5000));
  const long consumed_at_pause = held.bytes.load();
  std::array<FloodCollector, 3> siblings;
  std::vector<Operation> operations;
  for (std::size_t i = 0; i < siblings.size(); ++i) {
    siblings[i].released = true;
    operations.push_back(t.start(h2_post(node, "/flood?key=s" + std::to_string(i) +
                                              "&total=" + std::to_string(sibling_total)), siblings[i].callbacks()));
  }
  for (std::size_t i = 0; i < operations.size(); ++i) {
    const Result r = operations[i].join();
    CHECK_MSG(r.status == Status::Completed, "sibling %zu: %s/%s", i, name_of(r.status), name_of(r.failure));
    CHECK(r.attempt.version == ResponseVersion::Http2);
    CHECK(siblings[i].bytes == sibling_total && siblings[i].valid);
    CHECK(siblings[i].result.outcomes == 1 && siblings[i].result.late_callbacks == 0);
  }
  std::this_thread::sleep_for(milliseconds(200));
  const long written1 = node.stat("held_written");
  std::this_thread::sleep_for(milliseconds(200));
  const long written2 = node.stat("held_written");
  const long long growth = rss_bytes() - rss_before;
  const Stats stats = node.stats_request();
  CHECK(stats["sessions"] == 1);
  CHECK(stats["held_requests"] == 1 && stats["held_done"] == 0);
  CHECK(stats["held_session"] == stats["warm_session"]);
  for (std::size_t i = 0; i < siblings.size(); ++i) {
    const std::string key = "s" + std::to_string(i);
    CHECK(stats[key + "_session"] == stats["held_session"]);
    CHECK(stats[key + "_stream"] != stats["held_stream"]);
    CHECK(stats[key + "_requests"] == 1 && stats[key + "_done"] == 1);
  }
  CHECK(written1 == written2);
  CHECK(written2 > consumed_at_pause && written2 < (32L << 20));
  CHECK_MSG(growth < (32L << 20), "multiplexed pause grew RSS by %lld B", growth);
  CHECK(held.bytes == consumed_at_pause && !held.result.done);
  note("shared h2 session: 3 siblings completed %ld B each; paused received=%ld written=%ld->%ld RSS +%lld",
       sibling_total, consumed_at_pause, written1, written2, growth);
  held.released = true;
  paused.resume();
  const Result r = paused.join();
  CHECK(r.status == Status::Completed);
  CHECK(held.bytes == total && held.valid);
  CHECK(held.result.outcomes == 1 && held.result.late_callbacks == 0);
}

TEST(http2_cancelling_paused_stream_preserves_sibling) {
  NodeServer node;
  const bool started = node.start(g_h2_adversarial_script);
  CHECK(started);
  if (!started) return;
  TransportOptions options;
  options.max_host_connections = 1;
  Transport t(options);
  auto warm = std::make_shared<Collector>();
  CHECK(t.start(h2_post(node, "/ok?key=warm"), warm->callbacks()).join().status == Status::Completed);
  FloodCollector cancelled, survivor;
  Operation a = t.start(h2_post(node, "/flood?key=cancelled&total=67108864"), cancelled.callbacks());
  CHECK(wait_until([&] { return cancelled.paused.load(); }, 5000));
  Operation b = t.start(h2_post(node, "/flood?key=survivor&total=4194304"), survivor.callbacks());
  CHECK(wait_until([&] { return survivor.paused.load(); }, 5000));
  const auto before_cancel = steady_clock::now();
  a.cancel();
  const Result stopped = a.join();
  CHECK(stopped.status == Status::Cancelled);
  CHECK_MSG(ms_since(before_cancel) < 500, "HTTP/2 cancellation waited for peer progress");
  CHECK(!survivor.result.done);
  survivor.released = true;
  b.resume();
  CHECK(b.join().status == Status::Completed);
  CHECK(survivor.bytes == (4L << 20) && survivor.valid);
  CHECK(cancelled.result.outcomes == 1 && cancelled.result.late_callbacks == 0);
  CHECK(survivor.result.outcomes == 1 && survivor.result.late_callbacks == 0);
  const Stats stats = node.stats_request();
  CHECK(stats["sessions"] == 1);
  CHECK(stats["cancelled_session"] == stats["survivor_session"]);
}

// M1b R1: GOAWAY and REFUSED_STREAM can make libcurl resend internally. The independent peer
// must see precisely one request, regardless of whether this curl version tries that resend.
TEST(http2_one_attempt_survives_goaway_and_refused_stream) {
  NodeServer node;
  const bool started = node.start(g_h2_adversarial_script);
  CHECK(started);
  if (!started) return;
  for (const char* mode : {"goaway", "refused"}) {
    TransportOptions options;
    options.max_host_connections = 1;
    Transport t(options);
    auto warm = std::make_shared<Collector>();
    CHECK(t.start(h2_post(node, "/ok?key=warm"), warm->callbacks()).join().status == Status::Completed);
    auto c = std::make_shared<Collector>();
    const std::string key(mode);
    const Result r = t.start(h2_post(node, "/" + key + "?key=" + key), c->callbacks()).join();
    const long requests = node.stat((key + "_requests").c_str());
    note("h2 %s: %s/%s, curl=%d, internal resends=%u, peer requests=%ld", mode,
         name_of(r.status), name_of(r.failure), r.curl_code, r.attempt.transport_internal_resends, requests);
    CHECK(r.status == Status::Failed);
    CHECK(r.attempt.reached >= Stage::RequestStarted);
    CHECK(r.attempt.connection_reused);
    CHECK(requests == 1);
    CHECK(!r.attempt.response_head_seen && c->body.empty());
    if (r.attempt.transport_internal_resends != 0) CHECK(r.failure == FailureKind::ResendRefused);
    CHECK(c->outcomes == 1 && c->late_callbacks == 0);
  }
}

// M1b R2: refreshed addresses may leave a healthy pooled connection alive, but any newly
// established connection must use the fresh resolver answer rather than curl's stale DNS entry.
TEST(http2_dns_refresh_and_connection_retirement) {
  NodeServer node;
  const bool started = node.start(g_h2_adversarial_script);
  CHECK(started);
  if (!started) return;
  std::atomic<int> lookups{0};
  std::atomic<bool> second_address{false};
  TransportOptions options;
  options.dns_ttl = std::chrono::seconds(0);
  options.max_host_connections = 1;
  options.resolve = [&](const std::string&) {
    ++lookups;
    return std::vector<std::string>{second_address ? "127.0.0.2" : "127.0.0.1"};
  };
  Transport t(options);
  auto a = std::make_shared<Collector>();
  const Result first = t.start(h2_post(node, "/ok?key=first", 15000, "protocol-peer.invalid"), a->callbacks()).join();
  CHECK(first.status == Status::Completed && a->body == "peer A\n");
  CHECK(!first.attempt.connection_reused);
  second_address = true;
  auto reused = std::make_shared<Collector>();
  const Result second = t.start(h2_post(node, "/ok?key=second", 15000, "protocol-peer.invalid"), reused->callbacks()).join();
  CHECK(second.status == Status::Completed);
  CHECK(reused->body == (second.attempt.connection_reused ? "peer A\n" : "peer B\n"));
  const Stats retirement = node.stats_request("/close");
  CHECK(retirement["retired"] >= 1);
  CHECK(wait_until([&] { return node.stat("active_sessions") == 0; }, 5000));
  auto b = std::make_shared<Collector>();
  const Result third = t.start(h2_post(node, "/ok?key=third", 15000, "protocol-peer.invalid"), b->callbacks()).join();
  CHECK_MSG(third.status == Status::Completed, "%s/%s", name_of(third.status), name_of(third.failure));
  CHECK(b->body == "peer B\n");
  CHECK(!third.attempt.connection_reused && third.attempt.transport_internal_resends == 0);
  CHECK(lookups == 3);
  CHECK(a->outcomes == 1 && reused->outcomes == 1 && b->outcomes == 1);
  const Stats stats = node.stats_request();
  CHECK(stats["first_requests"] == 1 && stats["second_requests"] == 1 && stats["third_requests"] == 1);
  CHECK(stats["third_session"] != stats["first_session"]);
  note("DNS refresh: %d lookups, second reused=%d; retired=%ld; third=%s/%s curl=%d reused=%d resends=%u reached_peer_B=%d",
       lookups.load(), second.attempt.connection_reused, retirement["retired"],
       name_of(third.status), name_of(third.failure), third.curl_code, third.attempt.connection_reused,
       third.attempt.transport_internal_resends, b->body == "peer B\n");
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  runtime_test::Arguments arguments(argc, argv);
  argc = arguments.argc(); argv = arguments.argv();
#endif
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <sp_loopback_server> <fixture-directory> [filter]\n", argv[0]);
    return 2;
  }
  const std::string fixtures = argv[2];
  g_h2c_script = fixtures + "/h2c_flood_server.mjs";
  g_tls_script = fixtures + "/tls_test_server.mjs";
  g_h2_adversarial_script = fixtures + "/h2_adversarial_server.mjs";
  g_server.start(argv[1]);
  const std::string filter = argc > 3 ? argv[3] : "";
  int ran = 0, failed_tests = 0;
  for (const TestCase& tc : registry()) {
    if (!filter.empty() && std::string(tc.name).find(filter) == std::string::npos) continue;
    g_current = tc.name;
    const int before = g_failures;
    const auto start = steady_clock::now();
    std::fprintf(stderr, "[ RUN  ] %s\n", tc.name);
    tc.fn();
    const bool ok = g_failures == before;
    std::fprintf(stderr, "[ %s ] %s (%.0f ms)\n", ok ? " OK " : "FAIL", tc.name, ms_since(start));
    ++ran;
    if (!ok) ++failed_tests;
  }
  g_server.stop();
  std::fprintf(stderr, "%d tests, %d failed\n", ran, failed_tests);
  return failed_tests == 0 && ran > 0 ? 0 : 1;
}
