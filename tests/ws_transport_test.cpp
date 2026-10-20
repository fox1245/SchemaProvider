// WebSocket session properties (ws_transport.h) against tests/support/ws_server.mjs, a separate
// hand-written RFC 6455 peer. The oracle for what reached the wire is the peer's own record.
//
// Usage: sp_ws_transport_tests <node> <ws_server.mjs> [test-name-substring]
#include "transport/ws_transport.h"

#include "support/runtime_peer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace sp::transport;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {
int failures = 0;
std::vector<std::pair<const char*, std::function<void()>>>& registry() {
  static std::vector<std::pair<const char*, std::function<void()>>> tests;
  return tests;
}
struct Register {
  Register(const char* name, std::function<void()> body) { registry().emplace_back(name, std::move(body)); }
};
#define TEST(name)                               \
  void name();                                   \
  const Register register_##name(#name, name);   \
  void name()
#define CHECK(condition)                                                                 \
  do {                                                                                   \
    if (!(condition)) {                                                                  \
      ++failures;                                                                        \
      std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
    }                                                                                    \
  } while (0)

runtime_test::Peer* g_peer = nullptr;
std::string g_ca;
std::uint64_t g_tls_port = 0;

// Records every callback of one session; waits are bounded.
struct Recorder {
  std::mutex mu;
  std::condition_variable cv;
  bool opened = false;
  int open_status = 0;
  std::vector<std::pair<WsMessageKind, std::string>> messages;
  std::vector<std::pair<std::uint16_t, std::string>> closes;
  int done_calls = 0;
  WsResult result;

  WsCallbacks callbacks() {
    WsCallbacks cb;
    cb.on_open = [this](const ResponseHead& head) {
      std::lock_guard lock(mu);
      opened = true;
      open_status = head.status;
      cv.notify_all();
    };
    cb.on_message = [this](WsMessageKind kind, std::string_view payload) {
      std::lock_guard lock(mu);
      messages.emplace_back(kind, std::string(payload));
      cv.notify_all();
    };
    cb.on_close = [this](std::uint16_t code, std::string_view reason) {
      std::lock_guard lock(mu);
      closes.emplace_back(code, std::string(reason));
      cv.notify_all();
    };
    cb.on_done = [this](const WsResult& r) {
      std::lock_guard lock(mu);
      ++done_calls;
      result = r;
      cv.notify_all();
    };
    return cb;
  }
  bool wait_messages(std::size_t count) {
    std::unique_lock lock(mu);
    return cv.wait_for(lock, 10s, [&] { return messages.size() >= count; });
  }
  bool wait_open() {
    std::unique_lock lock(mu);
    return cv.wait_for(lock, 10s, [&] { return opened; });
  }
};

WsRequest request(const std::string& path) {
  WsRequest r;
  r.url = "ws://127.0.0.1:" + std::to_string(g_peer->port) + path;
  r.deadline = Clock::now() + 20s;
  return r;
}

sp::json::Document stats() { return g_peer->command("{\"stats\":true}"); }

bool contains(const sp::json::Document& doc, const char* key, std::uint64_t value) {
  for (const auto& element : doc.root().get(key).elements())
    if (element.as_uint() == value) return true;
  return false;
}

TEST(echo_text_and_binary_then_client_close) {
  Transport transport;
  Recorder rec;
  auto session = open_websocket(transport, request("/echo"), rec.callbacks());
  CHECK(session.send_text("hello"));
  CHECK(session.send_binary(std::string("\x00\xff\x01", 3)));
  CHECK(rec.wait_messages(2));
  {
    std::lock_guard lock(rec.mu);
    CHECK(rec.open_status == 101);
    CHECK(rec.messages.size() == 2);
    if (rec.messages.size() == 2) {
      CHECK(rec.messages[0].first == WsMessageKind::Text && rec.messages[0].second == "hello");
      CHECK(rec.messages[1].first == WsMessageKind::Binary && rec.messages[1].second == std::string("\x00\xff\x01", 3));
    }
  }
  session.close(ws_close::Normal, "done");
  CHECK(!session.send_text("late"));
  const auto r = session.join();
  CHECK(r.transport.status == Status::Completed);
  CHECK(r.clean && !r.closed_by_peer && r.close_code == ws_close::Normal);
  CHECK(r.http_status == 101);
  CHECK(rec.done_calls == 1);
  // on_close reports a Close the server sent first; the answer to the client's own is not reported.
  {
    std::lock_guard lock(rec.mu);
    CHECK(rec.closes.empty());
  }
  const auto seen = stats();
  CHECK(contains(seen, "closes", 1000));
  CHECK(seen.root().get("unmasked").as_uint() == 0);
}

TEST(handshake_sends_target_host_and_headers) {
  Transport transport;
  Recorder rec;
  auto req = request("/headers?x=1#fragment");
  req.headers.push_back(Header{"Authorization", "Bearer test-token"});
  req.headers.push_back(Header{"X-Test", "value"});
  auto session = open_websocket(transport, std::move(req), rec.callbacks());
  CHECK(rec.wait_messages(1));
  std::string text;
  {
    std::lock_guard lock(rec.mu);
    if (!rec.messages.empty()) text = rec.messages[0].second;
  }
  CHECK(text.find("\"target\":\"/headers?x=1\"") != std::string::npos);
  CHECK(text.find("\"authorization\":\"Bearer test-token\"") != std::string::npos);
  CHECK(text.find("\"test\":\"value\"") != std::string::npos);
  CHECK(text.find("\"host\":\"127.0.0.1:" + std::to_string(g_peer->port) + "\"") != std::string::npos);
  session.close();
  CHECK(session.join().clean);
}

// A header the handshake sets itself, given again in the request, is not sent: the server sees one of each, the key it
// accepts is the client's, and a protocol or extension the client never offered is not asked for.
TEST(handshake_owns_its_headers) {
  Transport transport;
  Recorder rec;
  auto req = request("/headers");
  req.headers.push_back(Header{"Upgrade", "h2c"});
  req.headers.push_back(Header{"Sec-WebSocket-Key", "AAAAAAAAAAAAAAAAAAAAAA=="});
  req.headers.push_back(Header{"Sec-WebSocket-Protocol", "chat"});
  req.headers.push_back(Header{"Sec-WebSocket-Extensions", "permessage-deflate"});
  req.headers.push_back(Header{"X-Test", "kept"});
  auto session = open_websocket(transport, std::move(req), rec.callbacks());
  CHECK(rec.wait_messages(1));
  std::string text;
  {
    std::lock_guard lock(rec.mu);
    if (!rec.messages.empty()) text = rec.messages[0].second;
  }
  CHECK(text.find("\"test\":\"kept\"") != std::string::npos);
  CHECK(text.find("\"owned\":{\"upgrade\":1,\"connection\":1,\"sec-websocket-key\":1,\"sec-websocket-version\":1}") != std::string::npos);
  session.close();
  const auto r = session.join();
  CHECK(r.clean && r.http_status == 101);
}

TEST(fragments_reassemble_and_interleaved_ping_is_answered) {
  Transport transport;
  Recorder rec;
  auto session = open_websocket(transport, request("/fragmented"), rec.callbacks());
  CHECK(session.send_text("go"));
  CHECK(rec.wait_messages(1));
  {
    std::lock_guard lock(rec.mu);
    CHECK(!rec.messages.empty() && rec.messages[0].second == "Hello");
  }
  session.close();
  CHECK(session.join().clean);
  const auto seen = stats();
  bool pong = false;
  for (const auto& element : seen.root().get("pongs").elements()) pong = pong || element.as_string() == "p";
  CHECK(pong);
}

TEST(ping_is_answered_with_its_payload) {
  Transport transport;
  Recorder rec;
  auto session = open_websocket(transport, request("/ping"), rec.callbacks());
  CHECK(rec.wait_messages(1));
  {
    std::lock_guard lock(rec.mu);
    CHECK(!rec.messages.empty() && rec.messages[0].second == "pong:probe");
  }
  session.close();
  CHECK(session.join().clean);
}

TEST(server_close_is_answered_and_completes) {
  Transport transport;
  Recorder rec;
  auto session = open_websocket(transport, request("/server-close"), rec.callbacks());
  const auto r = session.join();
  CHECK(r.transport.status == Status::Completed);
  CHECK(r.clean && r.closed_by_peer && r.close_code == 4001 && r.close_reason == "bye");
  {
    std::lock_guard lock(rec.mu);
    CHECK(rec.closes.size() == 1 && rec.closes[0].first == 4001);
  }
  CHECK(contains(stats(), "closes", 4001));
}

TEST(server_close_without_status_is_answered_without_one) {
  Transport transport;
  Recorder rec;
  auto session = open_websocket(transport, request("/server-close-nostatus"), rec.callbacks());
  const auto r = session.join();
  CHECK(r.transport.status == Status::Completed && r.clean);
  CHECK(r.close_code == ws_close::NoStatus);
  CHECK(contains(stats(), "closes", 0));
}

TEST(rejected_handshake_reports_the_status) {
  Transport transport;
  Recorder rec;
  auto session = open_websocket(transport, request("/reject"), rec.callbacks());
  const auto r = session.join();
  CHECK(r.transport.status == Status::Failed && r.transport.failure == FailureKind::Protocol);
  CHECK(r.http_status == 401);
  CHECK(!rec.opened);
}

TEST(wrong_accept_value_fails_the_handshake) {
  Transport transport;
  Recorder rec;
  auto session = open_websocket(transport, request("/bad-accept"), rec.callbacks());
  const auto r = session.join();
  CHECK(r.transport.status == Status::Failed && r.transport.failure == FailureKind::Protocol);
  CHECK(r.transport.detail.find("Accept") != std::string::npos);
  CHECK(!rec.opened);
}

TEST(frame_in_the_same_segment_as_the_head_is_delivered) {
  Transport transport;
  Recorder rec;
  auto session = open_websocket(transport, request("/early"), rec.callbacks());
  CHECK(rec.wait_messages(1));
  {
    std::lock_guard lock(rec.mu);
    CHECK(!rec.messages.empty() && rec.messages[0].second == "early");
  }
  session.close();
  CHECK(session.join().clean);
}

TEST(messages_sent_before_open_are_queued_in_order) {
  Transport transport;
  Recorder rec;
  auto session = open_websocket(transport, request("/echo"), rec.callbacks());
  for (int i = 0; i < 5; ++i) CHECK(session.send_text("m" + std::to_string(i)));
  CHECK(rec.wait_messages(5));
  {
    std::lock_guard lock(rec.mu);
    for (int i = 0; i < 5 && i < static_cast<int>(rec.messages.size()); ++i) CHECK(rec.messages[i].second == "m" + std::to_string(i));
  }
  session.close();
  CHECK(session.join().clean);
}

TEST(large_messages_in_both_directions) {
  Transport transport;
  Recorder rec;
  auto session = open_websocket(transport, request("/echo"), rec.callbacks());
  std::string big(3 * 1024 * 1024 + 17, 'x');
  for (std::size_t i = 0; i < big.size(); i += 4096) big[i] = static_cast<char>('a' + (i / 4096) % 26);
  CHECK(session.send_binary(big));
  CHECK(rec.wait_messages(1));
  {
    std::lock_guard lock(rec.mu);
    CHECK(!rec.messages.empty() && rec.messages[0].second == big);
  }
  session.close();
  CHECK(session.join().clean);
}

TEST(idle_timeout_ends_a_silent_session) {
  Transport transport;
  Recorder rec;
  auto req = request("/silent");
  req.idle_timeout = 300ms;
  const auto started = Clock::now();
  auto session = open_websocket(transport, std::move(req), rec.callbacks());
  const auto r = session.join();
  CHECK(r.transport.status == Status::Failed && r.transport.failure == FailureKind::IdleTimeout);
  CHECK(Clock::now() - started < 5s);
  CHECK(r.close_code == ws_close::Abnormal);
}

TEST(deadline_ends_the_session) {
  Transport transport;
  Recorder rec;
  auto req = request("/silent");
  req.deadline = Clock::now() + 400ms;
  auto session = open_websocket(transport, std::move(req), rec.callbacks());
  CHECK(session.join().transport.status == Status::DeadlineExceeded);
}

TEST(handshake_timeout_bounds_a_server_that_never_answers) {
  Transport transport;
  Recorder rec;
  auto req = request("/hold-handshake");
  req.handshake_timeout = 300ms;
  auto session = open_websocket(transport, std::move(req), rec.callbacks());
  const auto r = session.join();
  CHECK(r.transport.status == Status::Failed && r.transport.failure == FailureKind::FirstByteTimeout);
}

TEST(cancel_ends_an_open_session_promptly) {
  Transport transport;
  Recorder rec;
  auto session = open_websocket(transport, request("/silent"), rec.callbacks());
  CHECK(rec.wait_open());
  const auto started = Clock::now();
  session.cancel();
  const auto r = session.join();
  CHECK(r.transport.status == Status::Cancelled);
  CHECK(Clock::now() - started < 2s);
  CHECK(rec.done_calls == 1);
}

TEST(destroying_the_handle_cancels) {
  Transport transport;
  Recorder rec;
  {
    auto session = open_websocket(transport, request("/silent"), rec.callbacks());
    CHECK(rec.wait_open());
  }
  std::unique_lock lock(rec.mu);
  CHECK(rec.cv.wait_for(lock, 5s, [&] { return rec.done_calls == 1; }));
  CHECK(rec.result.transport.status == Status::Cancelled);
}

TEST(oversized_message_fails_with_1009) {
  Transport transport;
  Recorder rec;
  auto req = request("/big");
  req.limits.max_message_bytes = 1024 * 1024;
  auto session = open_websocket(transport, std::move(req), rec.callbacks());
  CHECK(session.send_text("go"));
  const auto r = session.join();
  CHECK(r.transport.status == Status::Failed && r.transport.failure == FailureKind::ResponseTooLarge);
  CHECK(r.close_code == ws_close::TooBig);
  CHECK(contains(stats(), "closes", 1009));
}

TEST(masked_server_frame_fails_with_1002) {
  Transport transport;
  Recorder rec;
  auto session = open_websocket(transport, request("/masked"), rec.callbacks());
  CHECK(session.send_text("go"));
  const auto r = session.join();
  CHECK(r.transport.status == Status::Failed && r.transport.failure == FailureKind::Protocol);
  CHECK(r.close_code == ws_close::ProtocolError);
  CHECK(contains(stats(), "closes", 1002));
  CHECK(rec.messages.empty());
}

TEST(dropped_connection_is_abnormal) {
  Transport transport;
  Recorder rec;
  auto session = open_websocket(transport, request("/drop"), rec.callbacks());
  const auto r = session.join();
  CHECK(r.transport.status == Status::Failed && r.transport.failure == FailureKind::Receive);
  CHECK(r.close_code == ws_close::Abnormal && !r.clean);
  std::lock_guard lock(rec.mu);
  CHECK(rec.messages.size() == 1 && rec.messages[0].second == "bye");
}

TEST(unanswered_close_ends_after_close_timeout) {
  Transport transport;
  Recorder rec;
  auto req = request("/ignore-close");
  req.close_timeout = 300ms;
  auto session = open_websocket(transport, std::move(req), rec.callbacks());
  CHECK(rec.wait_open());
  const auto started = Clock::now();
  session.close(4000, "bye");
  const auto r = session.join();
  CHECK(r.transport.status == Status::Failed && r.transport.failure == FailureKind::Receive);
  CHECK(!r.clean && r.close_code == 4000);
  CHECK(Clock::now() - started < 3s);
}

TEST(invalid_requests_fail_without_a_connection) {
  Transport transport;
  {
    Recorder rec;
    auto req = request("/echo");
    req.url = "http://127.0.0.1:" + std::to_string(g_peer->port) + "/echo";
    auto session = open_websocket(transport, std::move(req), rec.callbacks());
    const auto r = session.join();
    CHECK(r.transport.status == Status::Failed && r.transport.failure == FailureKind::Misuse);
  }
  {
    Recorder rec;
    auto req = request("/echo");
    req.headers.push_back(Header{"X-Bad", "a\r\nInjected: 1"});
    auto session = open_websocket(transport, std::move(req), rec.callbacks());
    CHECK(session.join().transport.failure == FailureKind::Misuse);
  }
  Recorder rec;
  auto session = open_websocket(transport, request("/echo"), rec.callbacks());
  CHECK(!session.send_text(std::string("\xff\xfe", 2)));  // text must be UTF-8
  session.close();
  session.join();
}

TEST(tls_session_with_a_private_ca) {
  if (g_tls_port == 0) {
    std::fprintf(stderr, "note: TLS peer unavailable (no openssl); skipped\n");
    return;
  }
  Transport transport;
  Recorder rec;
  WsRequest req;
  req.url = "wss://localhost:" + std::to_string(g_tls_port) + "/echo";
  req.ca_file = g_ca;
  req.deadline = Clock::now() + 20s;
  auto session = open_websocket(transport, std::move(req), rec.callbacks());
  CHECK(session.send_text("over tls"));
  CHECK(rec.wait_messages(1));
  session.close();
  const auto r = session.join();
  CHECK(r.clean);
  std::lock_guard lock(rec.mu);
  CHECK(!rec.messages.empty() && rec.messages[0].second == "over tls");
}

// A peer that stops reading leaves a TLS write unfinished (CURLE_AGAIN), and OpenSSL refuses to retry it with another
// buffer. Messages queued meanwhile, and the Pong and the Close after them, wait behind the pending write and arrive
// in order.
TEST(tls_send_backpressure_keeps_the_pending_write) {
  if (g_tls_port == 0) {
    std::fprintf(stderr, "note: TLS peer unavailable (no openssl); skipped\n");
    return;
  }
  Transport transport;
  Recorder rec;
  WsRequest req;
  req.url = "wss://localhost:" + std::to_string(g_tls_port) + "/hold-reading";
  req.ca_file = g_ca;
  req.deadline = Clock::now() + 30s;
  auto session = open_websocket(transport, std::move(req), rec.callbacks());
  CHECK(rec.wait_open());
  std::vector<std::string> expected;
  // 32 MiB: more than the connection holds while the peer reads nothing.
  for (int i = 0; i < 64; ++i) {
    CHECK(session.send_text(std::string(512 * 1024, 'a')));
    expected.push_back("text:" + std::to_string(512 * 1024));
  }
  std::this_thread::sleep_for(300ms);
  for (int i = 0; i < 8; ++i) {
    CHECK(session.send_text(std::string(100 + i, 'b')));
    expected.push_back("text:" + std::to_string(100 + i));
  }
  std::this_thread::sleep_for(100ms);
  CHECK(g_peer->command("{\"release\":true}").root().get("released").as_uint() == 1);
  expected.push_back("pong:held");
  const auto held = [] {
    const auto seen = stats();
    std::vector<std::string> out;
    for (const auto& item : seen.root().get("held").elements()) out.emplace_back(item.as_string());
    return out;
  };
  const auto finished = [&] {
    std::lock_guard lock(rec.mu);
    return rec.done_calls > 0;
  };
  // The Pong is the last frame before the Close; a failed session ends the wait.
  for (const auto until = Clock::now() + 20s; Clock::now() < until && !finished();) {
    const auto seen = held();
    if (!seen.empty() && seen.back() == "pong:held") break;
    std::this_thread::sleep_for(50ms);
  }
  session.close();
  const auto r = session.join();
  expected.push_back("close:1000");
  CHECK(r.transport.status == Status::Completed && r.clean);
  CHECK(held() == expected);
}

TEST(untrusted_certificate_fails_tls) {
  if (g_tls_port == 0) return;
  Transport transport;
  Recorder rec;
  WsRequest req;
  req.url = "wss://localhost:" + std::to_string(g_tls_port) + "/echo";
  req.deadline = Clock::now() + 20s;
  auto session = open_websocket(transport, std::move(req), rec.callbacks());
  const auto r = session.join();
  CHECK(r.transport.status == Status::Failed && r.transport.failure == FailureKind::Tls);
  CHECK(!rec.opened);
}

TEST(many_sessions_share_the_transport_threads) {
  TransportOptions options;
  options.io_threads = 2;
  Transport transport(options);
  constexpr int count = 24;
  std::vector<std::unique_ptr<Recorder>> recs;
  std::vector<WsSession> sessions;
  for (int i = 0; i < count; ++i) {
    recs.push_back(std::make_unique<Recorder>());
    sessions.push_back(open_websocket(transport, request("/echo"), recs.back()->callbacks()));
    CHECK(sessions.back().send_text("s" + std::to_string(i)));
  }
  for (int i = 0; i < count; ++i) CHECK(recs[i]->wait_messages(1));
  for (auto& s : sessions) s.close();
  for (auto& s : sessions) CHECK(s.join().clean);
  CHECK(transport.io_threads() == 2);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: sp_ws_transport_tests <node> <ws_server.mjs> [filter]\n");
    return 2;
  }
  try {
    runtime_test::Peer peer(argv[1], argv[2]);
    g_peer = &peer;
    const auto info = peer.command("{\"info\":true}");
    g_tls_port = info.root().get("tls_port").as_uint();
    g_ca = std::string(info.root().get("ca").as_string());
    const std::string filter = argc > 3 ? argv[3] : "";
    for (const auto& [name, body] : registry()) {
      if (!filter.empty() && std::string(name).find(filter) == std::string::npos) continue;
      const int before = failures;
      body();
      std::fprintf(stderr, "%s %s\n", failures == before ? "PASS" : "FAIL", name);
    }
    g_peer = nullptr;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "peer failure: %s\n", error.what());
    return 1;
  }
  if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
