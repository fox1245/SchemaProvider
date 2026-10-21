// ClickHouse/ai-sdk-cpp in the SDK comparison: ai::openai::create_client and ai::anthropic::create_client.
//
// Concurrency model: the SDK has blocking calls only (generate_text, and stream_text whose iterator polls an event queue).
// One Client is shared by `concurrency` OS threads, each running requests back to back. A stream additionally starts a
// thread of its own inside the SDK. The SDK opens a new connection for every request; there is no pool to size.
#include <ai/anthropic.h>
#include <ai/core.h>
#include <ai/openai.h>

#include <thread>

#include "../cpp_common/harness.hpp"

int main(int argc, char** argv) {
  const sdkb::Args a = sdkb::parse_args(argc, argv);
  ai::Client client = a.api == "openai" ? ai::openai::create_client(a.api_key, a.base_prefix)
                                        : ai::anthropic::create_client(a.api_key, a.base_prefix);

  const sdkb::RunFn run_requests = [&](const sdkb::Args& args, int count, bool record, sdkb::Phase& phase) {
    std::atomic<int> next{0};
    auto worker = [&] {
      for (;;) {
        const int i = next.fetch_add(1);
        if (i >= count) return;
        const auto t0 = sdkb::now_ns();
        ai::GenerateOptions options(args.model, args.system, args.user);
        options.max_tokens = args.max_tokens;
        if (!args.stream()) {
          auto result = client.generate_text(options);
          const auto t1 = sdkb::now_ns();
          if (!result) { phase.fail(result.error_message()); continue; }
          if (record) { phase.latency_ns[i] = t1 - t0; phase.text[i] = std::move(result.text); }
        } else {
          std::string text, error;
          int deltas = 0;
          auto stream = client.stream_text(ai::StreamOptions(options));
          for (const auto& event : stream) {
            if (event.is_text_delta() && !event.text_delta.empty()) { text += event.text_delta; ++deltas; }
            else if (event.is_error()) error = event.error.value_or("stream error");
          }
          const auto t1 = sdkb::now_ns();
          if (!error.empty()) { phase.fail(error); continue; }
          if (record) { phase.latency_ns[i] = t1 - t0; phase.text[i] = std::move(text); phase.deltas[i] = deltas; }
        }
      }
    };
    std::vector<std::thread> threads;
    for (int t = 0; t < std::min(args.concurrency, count); ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();
  };

  const std::string config =
      "{\"concurrency_model\":\"one shared Client; " + std::to_string(a.concurrency) +
      " OS threads, each calling the blocking generate_text or iterating stream_text (the SDK starts one more thread per stream "
      "and the iterator sleeps 1 ms when its queue is empty)\",\"http\":\"1.1 (cpp-httplib)\","
      "\"pool\":\"none: cpp-httplib builds a new client, and so a new connection, for every request\","
      "\"ca\":\"SSL_CERT_FILE through OpenSSL's default verify paths (the SDK sets no CA path)\","
      "\"retries\":\"SDK default RetryConfig (retries only on retryable failures), not triggered\",\"telemetry\":\"none; the logger is off\"}";
  return sdkb::run(a, "ai-sdk-cpp", SDKB_CLIENT_VERSION, config, run_requests);
}
