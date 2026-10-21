// Shared measurement loop for the C++ contenders; the C++ twin of ../py/harness.py.
//
// A client supplies a RunFn: it issues `count` requests with at most `args.concurrency` in flight, using its SDK the way
// that SDK is meant to be used, and fills the Phase: per request the latency (from just before the SDK call to its
// return, or to the end of the stream), and the assembled text with the number of text deltas. Verification of the text
// happens here, after the clock has stopped.
#pragma once

#include <sys/resource.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace sdkb {

using Clock = std::chrono::steady_clock;
inline std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

struct Args {
  std::string api, mode, base_prefix, ca, model, system, user, expect, api_key;
  int concurrency = 1, warmup = 0, requests = 1, max_tokens = 256, chunks = 1;
  bool cold = false;
  std::map<std::string, std::string> extra;  // client-specific `--name value` options
  bool stream() const { return mode == "stream"; }
};

inline Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) { std::cerr << "missing value for " << key << "\n"; std::exit(2); }
      return argv[++i];
    };
    if (key == "--api") a.api = value();
    else if (key == "--mode") a.mode = value();
    else if (key == "--base-prefix") a.base_prefix = value();
    else if (key == "--ca") a.ca = value();
    else if (key == "--model") a.model = value();
    else if (key == "--system") a.system = value();
    else if (key == "--user") a.user = value();
    else if (key == "--expect") a.expect = value();
    else if (key == "--api-key") a.api_key = value();
    else if (key == "--concurrency") a.concurrency = std::stoi(value());
    else if (key == "--warmup") a.warmup = std::stoi(value());
    else if (key == "--requests") a.requests = std::stoi(value());
    else if (key == "--max-tokens") a.max_tokens = std::stoi(value());
    else if (key == "--chunks") a.chunks = std::stoi(value());
    else if (key == "--cold") a.cold = true;
    else if (key.rfind("--", 0) == 0) a.extra[key.substr(2)] = value();
    else { std::cerr << "unknown argument " << key << "\n"; std::exit(2); }
  }
  return a;
}

// Results of one batch of requests.
struct Phase {
  std::vector<std::int64_t> latency_ns;
  std::vector<std::string> text;
  std::vector<int> deltas;
  std::atomic<int> failed{0};
  std::mutex mutex;
  std::string first_error;

  Phase(int count, bool record)
      : latency_ns(record ? count : 0), text(record ? count : 0), deltas(record ? count : 0) {}

  void fail(const std::string& what) {
    std::lock_guard lock(mutex);
    if (failed.fetch_add(1) == 0) first_error = what.substr(0, 300);
  }
};

using RunFn = std::function<void(const Args&, int count, bool record, Phase&)>;

inline long status_kb(const char* field) {
  std::ifstream f("/proc/self/status");
  std::string line;
  const std::size_t n = std::strlen(field);
  while (std::getline(f, line))
    if (line.compare(0, n, field) == 0) return std::strtol(line.c_str() + n, nullptr, 10);
  return -1;
}

inline std::string json_string(const std::string& s) {
  std::string out = "\"";
  for (const unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += static_cast<char>(c);
    }
  }
  return out + "\"";
}

inline bool verify(const Args& a, const std::string& text, int deltas) {
  return text == a.expect && (!a.stream() || deltas == a.chunks);
}

inline void usage_us(long& user, long& sys) {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  user = ru.ru_utime.tv_sec * 1000000L + ru.ru_utime.tv_usec;
  sys = ru.ru_stime.tv_sec * 1000000L + ru.ru_stime.tv_usec;
}

// `config_json` is a JSON object describing the settings the client actually used.
inline int run(Args a, const std::string& client, const std::string& version, const std::string& config_json, const RunFn& run_requests) {
  if (a.cold) {
    a.concurrency = 1;
    Phase one(1, true);
    run_requests(a, 1, true, one);
    const bool ok = one.failed == 0 && verify(a, one.text[0], one.deltas[0]);
    std::printf("{\"event\":\"first\",\"ok\":%s}\n", ok ? "true" : "false");
    std::fflush(stdout);
    std::_Exit(ok ? 0 : 1);
  }
  int warm_failed = 0;
  std::string warm_error;
  long rss_ready = 0, hwm_ready = 0;
  char line[64];
  for (;;) {  // warm-up rounds: the runner answers each `ready` with MORE or GO
    Phase warm(a.warmup, false);
    if (a.warmup) run_requests(a, a.warmup, false, warm);
    warm_failed += warm.failed;
    if (warm_error.empty()) warm_error = warm.first_error;
    rss_ready = status_kb("VmRSS:");
    hwm_ready = status_kb("VmHWM:");
    std::printf("{\"event\":\"ready\"}\n");
    std::fflush(stdout);
    if (!std::fgets(line, sizeof line, stdin)) return 3;
    if (!std::strncmp(line, "GO", 2)) break;
    if (std::strncmp(line, "MORE", 4)) return 3;
  }

  Phase phase(a.requests, true);
  long u0, s0, u1, s1;
  usage_us(u0, s0);
  const auto t0 = now_ns();
  run_requests(a, a.requests, true, phase);
  const auto t1 = now_ns();
  usage_us(u1, s1);

  // Outside the measured window.
  int bad = 0;
  for (int i = 0; i < a.requests; ++i)
    if (phase.latency_ns[i] != 0 && !verify(a, phase.text[i], phase.deltas[i]) && ++bad == 1)
      std::fprintf(stderr, "first mismatch: %zu bytes in %d deltas, expected %zu bytes in %d deltas; got \"%.60s\"\n", phase.text[i].size(),
                   phase.deltas[i], a.expect.size(), a.stream() ? a.chunks : 0, phase.text[i].c_str());
  const int failed = phase.failed + warm_failed;
  std::ostringstream out;
  out << "{\"event\":\"result\",\"schema\":1,\"client\":" << json_string(client) << ",\"version\":" << json_string(version)
      << ",\"api\":" << json_string(a.api) << ",\"mode\":" << json_string(a.mode) << ",\"concurrency\":" << a.concurrency
      << ",\"warmup\":" << a.warmup << ",\"requests\":" << a.requests << ",\"ok\":" << (a.requests - phase.failed)
      << ",\"failed\":" << failed << ",\"first_error\":"
      << (failed ? json_string(phase.first_error.empty() ? warm_error : phase.first_error) : std::string("null"))
      << ",\"verify_failed\":" << bad << ",\"wall_ns\":" << (t1 - t0) << ",\"cpu_user_us\":" << (u1 - u0)
      << ",\"cpu_sys_us\":" << (s1 - s0) << ",\"rss_ready_kb\":" << rss_ready << ",\"hwm_ready_kb\":" << hwm_ready
      << ",\"rss_end_kb\":" << status_kb("VmRSS:") << ",\"hwm_end_kb\":" << status_kb("VmHWM:") << ",\"config\":" << config_json
      << ",\"latency_ns\":[";
  for (int i = 0; i < a.requests; ++i) out << (i ? "," : "") << phase.latency_ns[i];
  out << "]}\n";
  std::fputs(out.str().c_str(), stdout);
  std::fflush(stdout);
  std::_Exit(0);
}

}  // namespace sdkb
