// Actual loopback QUIC/TLS scenarios; never count an H1 fallback as H3 evidence.
// Usage: sp_http3_scenarios <isolated-python> <http3_peer.py> [all|capable|incapable|probe]
#include "transport/http_transport.h"
#include "transport/sse_framer.h"
#include "support/posix_owner.h"
#include "support/portable.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace sp::transport;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

void require(bool value, std::string_view message) {
  if (!value) throw std::runtime_error(std::string(message));
}

using Fields = std::map<std::string, std::string>;
Fields fields(std::string_view line, std::string_view prefix) {
  require(line.starts_with(prefix), "invalid peer reply");
  Fields result;
  std::istringstream input(std::string(line.substr(prefix.size())));
  std::string item;
  while (input >> item) {
    const auto equals = item.find('=');
    require(equals != std::string::npos, "invalid peer field");
    result.emplace(item.substr(0, equals), item.substr(equals + 1));
  }
  return result;
}
long number(const Fields& row, const std::string& key) {
  const auto found = row.find(key);
  return found == row.end() ? 0 : std::stol(found->second);
}

class Peer {
 public:
  Peer(const char* python, const char* script) {
    runtime_test::Pipe input, output;
    const auto pid = fork();
    if (pid == 0) {
      input.writer.reset(); output.reader.reset();
      if (!input.reader.redirect_to(STDIN_FILENO) || !output.writer.redirect_to(STDOUT_FILENO)) _exit(126);
      execl(python, python, "-u", script, static_cast<char*>(nullptr));
      _exit(127);
    }
    require(pid > 0, "peer fork failed");
    process_ = runtime_test::Process(pid);
    in_ = std::move(input.writer); out_ = std::move(output.reader);
    input.reader.reset(); output.writer.reset();
    const auto ready = fields(line(), "READY ");
    dual = number(ready, "dual"); fallback = number(ready, "fallback"); blackhole = number(ready, "blackhole");
    auto found = ready.find("ca");
    require(dual && fallback && blackhole && found != ready.end(), "peer readiness incomplete");
    ca = found->second;
  }
  Fields command(std::string text) {
    text += '\n';
    std::string_view pending(text);
    while (!pending.empty()) {
      const auto n = ::write(in_.get(), pending.data(), pending.size());
      if (n < 0 && errno == EINTR) continue;
      require(n > 0, "peer control write failed");
      pending.remove_prefix(static_cast<std::size_t>(n));
    }
    auto row = fields(line(), "OK ");
    require(number(row, "fixture_errors") == 0, "QUIC peer coroutine failed");
    return row;
  }
  Fields stats(const std::string& id) { return command("STATS " + id); }
  void wait(const std::string& id, const std::string& counter, long count = 1) {
    command("WAIT " + id + ' ' + counter + ' ' + std::to_string(count));
  }
  void release(const std::string& id) { command("RELEASE " + id); }
  void count(const std::string& id, long total, long h3, long tcp) {
    const auto row = stats(id);
    require(number(row, "requests") == total && number(row, "h3") == h3 && number(row, "tcp") == tcp,
            "wire request count/protocol mismatch");
    require(number(row, "invalid") == 0 && number(row, "request_bytes") == total * 2,
            "wire method/body mismatch");
    require(number(row, "early_data") == 0, "generation POST used early data");
  }
  long dual = 0, fallback = 0, blackhole = 0;
  std::string ca;
 private:
  std::string line() {
    const auto deadline = Clock::now() + 15s;
    for (;;) {
      if (auto pos = pending_.find('\n'); pos != std::string::npos) {
        auto line = pending_.substr(0, pos); pending_.erase(0, pos + 1); return line;
      }
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      require(left > 0, "peer control timed out");
      pollfd fd{out_.get(), POLLIN, 0};
      const auto ready = poll(&fd, 1, static_cast<int>(left));
      if (ready < 0 && errno == EINTR) continue;
      require(ready > 0, "peer control did not respond");
      char buffer[4096];
      const auto n = ::read(out_.get(), buffer, sizeof buffer);
      if (n < 0 && errno == EINTR) continue;
      require(n > 0, "peer exited before reply");
      pending_.append(buffer, static_cast<std::size_t>(n));
      require(pending_.size() <= 16384, "peer reply exceeded bound");
    }
  }
  runtime_test::Process process_;
  runtime_test::Fd in_, out_;
  std::string pending_;
};

struct Collector {
  std::optional<ResponseHead> head;
  std::string body;
  std::atomic<unsigned> outcomes{0}, late{0};
  std::atomic<bool> done{false};
  Callbacks callbacks() {
    return {
      [this](const ResponseHead& value) { if (done) ++late; head = value; },
      [this](std::string_view bytes) { if (done) ++late; body.append(bytes); return true; },
      [this](const Result&) { if (done.exchange(true)) ++late; ++outcomes; }
    };
  }
  void terminal() const { require(outcomes == 1 && late == 0 && done, "callback terminal contract broken"); }
};

struct StreamCollector {
  Collector collector;
  SseFramer framer;
  std::vector<std::string> events;
  std::atomic<unsigned> frames{0};
  Callbacks callbacks() {
    auto cb = collector.callbacks();
    cb.on_body = [this](std::string_view bytes) {
      require(!collector.done, "SSE callback after outcome");
      require(framer.feed(bytes, [this](const SseFrame& frame) {
        events.emplace_back(frame.data); ++frames; return true;
      }), "SSE framing rejected bytes");
      return true;
    };
    return cb;
  }
};

HttpRequest request(const Peer& peer, long port, std::string path, HttpVersion policy, std::chrono::milliseconds timeout = 10s) {
  HttpRequest req;
  req.url = "https://127.0.0.1:" + std::to_string(port) + path;
  req.body = "{}";
  req.ca_file = peer.ca;
  req.http_version = policy;
  req.deadline = Clock::now() + timeout;
  return req;
}

void success(const Result& result, const Collector& collector, ResponseVersion version) {
  collector.terminal();
  require(result.status == Status::Completed && result.failure == FailureKind::None && result.http_status == 200,
          "exchange did not complete successfully");
  require(result.attempt.version == version && collector.head && collector.head->version == version,
          "actual negotiated version mismatch");
  require(result.attempt.transport_internal_resends == 0, "successful exchange attempted resend");
  if (version == ResponseVersion::Http3) require(collector.head->framing == BodyFraming::Stream, "H3 stream framing lost");
}

void basic(Peer& peer, bool capable, bool probe) {
  Transport transport;
  const auto info = transport.runtime_info();
  std::printf("RUNTIME curl=%s tls=%s http2=%d http3=%d\n", info.curl_version.c_str(), info.ssl_backend.c_str(), info.http2, info.http3);
  const auto normal_version = info.http2 ? ResponseVersion::Http2 : ResponseVersion::Http1_1;
  Collector preferred;
  const auto p = transport.start(request(peer, peer.dual, "/ok/preferred", HttpVersion::Http3Preferred), preferred.callbacks()).join();
  success(p, preferred, capable ? ResponseVersion::Http3 : normal_version);
  require(preferred.body == "{\"ok\":true}", "buffered payload mismatch");
  peer.count("preferred", 1, capable, !capable);
  std::printf("EXCHANGE policy=preferred actual=%d requests=1\n", static_cast<int>(p.attempt.version));

  Collector only;
  const auto o = transport.start(request(peer, peer.dual, "/ok/only", HttpVersion::Http3Only), only.callbacks()).join();
  only.terminal();
  if (capable) {
    success(o, only, ResponseVersion::Http3);
    require(only.body == "{\"ok\":true}" && !o.attempt.connection_reused, "only lane reused pooled connection");
    peer.count("only", 1, 1, 0);
  } else {
    require(o.status == Status::Failed && o.failure == FailureKind::Protocol, "unsupported only lane not explicit");
    require(o.attempt.reached < Stage::RequestStarted && !only.head && only.body.empty(), "unsupported only lane dispatched");
    peer.count("only", 0, 0, 0);
  }
  std::printf("EXCHANGE policy=only status=%d actual=%d requests=%d\n", static_cast<int>(o.status), static_cast<int>(o.attempt.version), capable ? 1 : 0);
  if (probe) return;

  Transport normal; // Keep Auto's normal lane independent of the preferred H3 cache.
  Collector automatic;
  const auto a = normal.start(request(peer, peer.dual, "/ok/auto", HttpVersion::Auto), automatic.callbacks()).join();
  success(a, automatic, normal_version);
  peer.count("auto", 1, 0, 1);

  // Fresh policy lane on an origin already carrying pooled H2/H1 must still use H3.
  Collector after;
  const auto r = normal.start(request(peer, peer.dual, "/ok/afterauto", HttpVersion::Http3Only), after.callbacks()).join();
  if (capable) success(r, after, ResponseVersion::Http3);
  else require(r.status == Status::Failed && r.failure == FailureKind::Protocol, "incapable lane unexpectedly succeeded");
  after.terminal();
  peer.count("afterauto", capable ? 1 : 0, capable ? 1 : 0, 0);

  Collector plain;
  auto nonhttps = request(peer, peer.dual, "/ok/plain", HttpVersion::Http3Only);
  nonhttps.url.replace(0, 5, "http");
  const auto rejected = transport.start(std::move(nonhttps), plain.callbacks()).join();
  plain.terminal();
  require(rejected.status == Status::Failed && rejected.failure == FailureKind::Protocol && rejected.attempt.reached < Stage::RequestStarted,
          "non-HTTPS only lane dispatched");
  peer.count("plain", 0, 0, 0);

  Collector fallback;
  const auto f = transport.start(request(peer, peer.fallback, "/ok/fallback", HttpVersion::Http3Preferred), fallback.callbacks()).join();
  success(f, fallback, normal_version);
  peer.count("fallback", 1, 0, 1);
  require(f.attempt.transport_internal_resends == 0 && fallback.body == "{\"ok\":true}", "fallback resent POST");
  std::printf("FALLBACK actual=%d requests=1 h3_requests=0 internal_resends=0\n", static_cast<int>(f.attempt.version));
  Collector strict;
  const auto s = transport.start(request(peer, peer.fallback, "/ok/nodowngrade", HttpVersion::Http3Only, 350ms), strict.callbacks()).join();
  strict.terminal();
  require(s.status != Status::Completed && s.attempt.reached < Stage::RequestStarted && strict.body.empty(), "only lane downgraded to warm TCP");
  peer.count("nodowngrade", 0, 0, 0);
}

void stream_and_failures(Peer& peer) {
  Transport transport;
  StreamCollector stream;
  const auto r = transport.start(request(peer, peer.dual, "/sse/sse", HttpVersion::Http3Only), stream.callbacks()).join();
  stream.framer.finish();
  success(r, stream.collector, ResponseVersion::Http3);
  require(stream.framer.error() == SseError::None && stream.events == std::vector<std::string>{"token", "[DONE]"}, "SSE terminal event missing");
  peer.count("sse", 1, 1, 0);

  // Transport FIN is not semantic success: the shared SSE consumer must still
  // see a terminal event. A reset after output must never turn into completion.
  for (const std::string key : {"ssemissing", "ssetruncate"}) {
    StreamCollector incomplete;
    const auto outcome = transport.start(request(peer, peer.dual, '/' + key + '/' + key, HttpVersion::Http3Only),
                                         incomplete.callbacks()).join();
    incomplete.framer.finish();
    incomplete.collector.terminal();
    require(incomplete.events == std::vector<std::string>{"token"}, "incomplete SSE lost partial output or invented terminal");
    if (key == "ssemissing") success(outcome, incomplete.collector, ResponseVersion::Http3);
    else require(outcome.status == Status::Failed && outcome.failure == FailureKind::Truncated,
                 "reset after SSE output became successful completion");
    peer.count(key, 1, 1, 0);
  }

  for (const std::string mode : {"reset", "truncate", "short"}) {
    Collector broken;
    const auto result = transport.start(request(peer, peer.dual, '/' + mode + '/' + mode, HttpVersion::Http3Only), broken.callbacks()).join();
    broken.terminal();
    require(result.status == Status::Failed && result.attempt.reached >= Stage::RequestStarted, "abnormal QUIC end became success");
    require(result.failure == FailureKind::ResendRefused || result.failure == FailureKind::Truncated ||
            result.failure == FailureKind::Receive || result.failure == FailureKind::Protocol, "QUIC error lacks safe typed failure");
    if (mode == "truncate" || mode == "short") {
      require(broken.head && broken.head->version == ResponseVersion::Http3, "truncation lost H3 response head");
    }
    peer.count(mode, 1, 1, 0);
    require(number(peer.stats(mode), "faults") == 1, "peer fault did not fire");
    std::printf("FAULT mode=%s kind=%d curl=%d requests=1 resends=%u\n", mode.c_str(), static_cast<int>(result.failure), result.curl_code,
                result.attempt.transport_internal_resends);
  }
}

template <class Predicate> void wait_until(Predicate predicate, std::chrono::milliseconds timeout = 5s) {
  const auto deadline = Clock::now() + timeout;
  while (!predicate() && Clock::now() < deadline) std::this_thread::sleep_for(2ms);
  require(predicate(), "operation did not reach expected state");
}

void no_progress(Peer& peer) {
  Transport transport;
  Collector silent;
  auto cancelled = transport.start(request(peer, peer.blackhole, "/hold/silent", HttpVersion::Http3Only), silent.callbacks());
  // No response/header/upload progress exists at the UDP blackhole.
  std::this_thread::sleep_for(50ms);
  const auto before = Clock::now();
  cancelled.cancel();
  const auto c = cancelled.join();
  silent.terminal();
  require(c.status == Status::Cancelled && Clock::now() - before < 1s && c.attempt.reached < Stage::RequestStarted,
          "QUIC handshake cancellation waited for peer");
  Collector deadline;
  const auto start = Clock::now();
  const auto d = transport.start(request(peer, peer.blackhole, "/hold/deadline", HttpVersion::Http3Only, 150ms), deadline.callbacks()).join();
  deadline.terminal();
  require(d.status == Status::DeadlineExceeded && Clock::now() - start < 1s && d.attempt.reached < Stage::RequestStarted,
          "QUIC handshake deadline waited for peer");

  for (const std::string key : {"acceptedcancel", "accepteddeadline"}) {
    Collector held;
    auto op = transport.start(request(peer, peer.dual, "/hold/" + key, HttpVersion::Http3Only,
                                     key == "accepteddeadline" ? 500ms : 10s), held.callbacks());
    peer.wait(key, "requests");
    const auto before_finish = Clock::now();
    if (key == "acceptedcancel") op.cancel();
    const auto outcome = op.join();
    held.terminal();
    require(outcome.status == (key == "acceptedcancel" ? Status::Cancelled : Status::DeadlineExceeded), "accepted silent stream terminal mismatch");
    require(Clock::now() - before_finish < 1s && outcome.attempt.reached >= Stage::RequestStarted && !held.head,
            "accepted silent stream needed peer progress");
    peer.count(key, 1, 1, 0);
  }
  StreamCollector partial;
  auto stream = transport.start(request(peer, peer.dual, "/ssehold/partialcancel", HttpVersion::Http3Only), partial.callbacks());
  wait_until([&] { return partial.frames.load() == 1; });
  const auto cancel_start = Clock::now();
  stream.cancel();
  const auto outcome = stream.join();
  partial.framer.finish();
  partial.collector.terminal();
  require(outcome.status == Status::Cancelled && Clock::now() - cancel_start < 1s &&
          outcome.attempt.version == ResponseVersion::Http3 && partial.events == std::vector<std::string>{"token"},
          "cancel after H3 SSE output reconnected or invented a terminal");
  peer.count("partialcancel", 1, 1, 0);
}

long rss_bytes() {
#ifdef __APPLE__
  const auto observed = portable::darwin_resident_bytes();
  require(observed <= (std::numeric_limits<long>::max)(), "RSS exceeds observation range");
  return static_cast<long>(observed);
#else
  std::ifstream input("/proc/self/smaps_rollup");
  std::string line;
  while (std::getline(input, line)) {
    long kib = 0;
    if (std::sscanf(line.c_str(), "Rss: %ld kB", &kib) == 1) return kib * 1024;
  }
  throw std::runtime_error("cannot observe client RSS");
#endif
}

struct Flood {
  Collector collector;
  std::atomic<bool> paused{false}, released{false}, valid{true};
  std::atomic<long> consumed{0};
  Callbacks callbacks() {
    auto cb = collector.callbacks();
    cb.on_body = [this](std::string_view bytes) {
      if (collector.done) ++collector.late;
      if (!released) { paused = true; return false; }
      for (const char byte : bytes) if (byte != 'x') valid = false;
      consumed += static_cast<long>(bytes.size());
      return true;
    };
    return cb;
  }
};

void multiplexed_pause(Peer& peer) {
  TransportOptions options;
  options.max_host_connections = 1;
  Transport transport(options);
  Collector warm;
  success(transport.start(request(peer, peer.dual, "/ok/warm", HttpVersion::Http3Preferred), warm.callbacks()).join(), warm, ResponseVersion::Http3);
  const auto connection = number(peer.stats("warm"), "connection");
  const auto rss = rss_bytes();
  Flood held;
  auto a = transport.start(request(peer, peer.dual, "/flood/held", HttpVersion::Http3Preferred, 60s), held.callbacks());
  wait_until([&] { return held.paused.load(); });
  Collector sibling;
  success(transport.start(request(peer, peer.dual, "/sse/sibling", HttpVersion::Http3Preferred), sibling.callbacks()).join(), sibling, ResponseVersion::Http3);
  require(sibling.body == "data: token\n\ndata: [DONE]\n\n", "paused QUIC stream blocked sibling");
  require(number(peer.stats("held"), "connection") == connection && number(peer.stats("sibling"), "connection") == connection,
          "pause isolation was not on one QUIC connection");
  long previous = -1, produced = 0;
  unsigned stable = 0;
  const auto settle = Clock::now() + 8s;
  while (stable < 3 && Clock::now() < settle) {
    std::this_thread::sleep_for(300ms);
    produced = number(peer.stats("held"), "produced");
    stable = produced == previous ? stable + 1 : 0;
    previous = produced;
  }
  const auto growth = rss_bytes() - rss;
  require(stable >= 3 && produced > 0 && produced < (32L << 20), "paused H3 stream did not reach bounded plateau");
  require(growth < (32L << 20) && held.consumed == 0 && !held.collector.done, "paused H3 buffering exceeded bound");
  held.released = true;
  a.resume();
  const auto resumed = a.join();
  success(resumed, held.collector, ResponseVersion::Http3);
  require(held.consumed == (64L << 20) && held.valid, "resume did not deliver every flood byte exactly once");
  peer.count("held", 1, 1, 0); peer.count("sibling", 1, 1, 0);
  std::printf("PAUSE connection=%ld produced=%ld rss_growth=%ld resumed_bytes=%ld\n", connection, produced, growth, held.consumed.load());

  Flood cancelled, survivor;
  auto ca = transport.start(request(peer, peer.dual, "/flood/cancelled", HttpVersion::Http3Preferred, 60s), cancelled.callbacks());
  wait_until([&] { return cancelled.paused.load(); });
  auto cb = transport.start(request(peer, peer.dual, "/flood/survivor", HttpVersion::Http3Preferred, 60s), survivor.callbacks());
  wait_until([&] { return survivor.paused.load(); });
  require(number(peer.stats("cancelled"), "connection") == connection && number(peer.stats("survivor"), "connection") == connection,
          "cancellation isolation was not on one QUIC connection");
  const auto before = Clock::now();
  ca.cancel();
  const auto stopped = ca.join();
  cancelled.collector.terminal();
  require(stopped.status == Status::Cancelled && Clock::now() - before < 1s, "paused H3 cancellation blocked");
  survivor.released = true; cb.resume();
  success(cb.join(), survivor.collector, ResponseVersion::Http3);
  require(survivor.consumed == (64L << 20) && survivor.valid, "cancelling paused stream damaged sibling");
  peer.count("cancelled", 1, 1, 0); peer.count("survivor", 1, 1, 0);
  std::printf("ISOLATION connection=%ld cancelled=1 surviving_bytes=%ld\n", connection, survivor.consumed.load());
}
} // namespace

int main(int argc, char** argv) {
  if (argc < 3 || argc > 4) {
    std::fprintf(stderr, "usage: %s <isolated-python> <http3_peer.py> [all|capable|incapable|probe]\n", argv[0]);
    return 2;
  }
  try {
    const std::string mode = argc == 4 ? argv[3] : "all";
    require(mode == "all" || mode == "capable" || mode == "incapable" || mode == "probe", "invalid scenario mode");
    const bool capable = Transport().runtime_info().http3;
    require(mode != "capable" || capable, "capable suite linked to non-H3 libcurl");
    require(mode != "incapable" || !capable, "incapable suite linked to H3 libcurl");
    Peer peer(argv[1], argv[2]);
    basic(peer, capable, mode == "probe");
    if (capable && mode != "probe") {
      stream_and_failures(peer);
      no_progress(peer);
      multiplexed_pause(peer);
    }
    std::printf("PASS http3_scenarios capability=%d mode=%s\n", capable, mode.c_str());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL http3_scenarios: %s\n", error.what());
    return 1;
  }
}
