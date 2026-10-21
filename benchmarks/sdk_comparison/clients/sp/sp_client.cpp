// SchemaProvider in the SDK comparison: sp::runtime::Client with start() and callbacks.
//
// Concurrency model: one Client; the closed loop keeps `concurrency` operations in flight by starting the next request
// from the completion callback of the previous one, so no thread is parked per request. The runtime's own worker pool
// delivers the callbacks. Every operation is detached (the callbacks own its state).
#include <runtime/client.h>

#include <curl/curl.h>

#include <condition_variable>
#include <memory>
#include <variant>

#include "../cpp_common/harness.hpp"

namespace {

struct Origin {
  std::string origin, path;
};

Origin split_base(const std::string& base) {
  const auto slash = base.find('/', base.find("://") + 3);
  return {base.substr(0, slash), slash == std::string::npos ? "" : base.substr(slash)};
}

sp::descriptor::ValidatedDescriptor make_descriptor(const sdkb::Args& a) {
  const auto base = split_base(a.base_prefix);
  const bool openai = a.api == "openai";
  const std::string path = base.path + "/v1/" + (openai ? "chat/completions" : "messages");
  const std::string source = std::string("{\"descriptor_version\":1,\"revision\":1,\"id\":\"sdk-comparison\",\"family\":\"") +
                             (openai ? "openai.chat" : "anthropic.messages") + "\",\"connection\":{\"base_url\":" +
                             sdkb::json_string(base.origin) + ",\"paths\":{\"buffered\":" + sdkb::json_string(path) +
                             ",\"streaming\":" + sdkb::json_string(path) + "}}}";
  auto loaded = sp::descriptor::load(source);
  if (auto* error = std::get_if<sp::descriptor::ConfigError>(&loaded)) {
    std::cerr << "descriptor rejected at " << error->pointer << ": " << error->message << "\n";
    std::exit(2);
  }
  return std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded));
}

struct State {
  std::atomic<int> next{0}, done{0};
  std::mutex mutex;
  std::condition_variable finished;
};

struct Pending {
  std::int64_t t0 = 0;
  std::string text;
  int deltas = 0;
};

}  // namespace

int main(int argc, char** argv) {
  const sdkb::Args a = sdkb::parse_args(argc, argv);
  try {
    sp::runtime::require_interface_contract(sp::EXPECTED_INTERFACE_REVISION, sp::capability::RequiredProvider);
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 4;
  }

  sp::runtime::Options options;
  options.http_version = sp::transport::HttpVersion::Http1_1;
  options.ca_file = a.ca;
  options.api_key = a.api_key;
  options.transport.max_host_connections = a.concurrency;  // pool parity: as many connections as requests in flight
  // Optional overrides, for experiments only (`--io-threads N`, `--workers N`); the comparison runs the SDK's defaults.
  if (a.extra.count("io-threads")) options.transport.io_threads = static_cast<unsigned>(std::stoul(a.extra.at("io-threads")));
  if (a.extra.count("workers")) options.workers = std::stoul(a.extra.at("workers"));
  if (a.extra.count("max-host-connections")) options.transport.max_host_connections = std::stol(a.extra.at("max-host-connections"));
  sp::runtime::Client client(make_descriptor(a), options);

  auto make_request = [&]() -> sp::runtime::Request {
    if (a.api == "openai") return sp::chat::Request::simple(a.model, a.user, static_cast<std::uint64_t>(a.max_tokens), a.system);
    return sp::messages::Request::simple(a.model, a.user, static_cast<std::uint64_t>(a.max_tokens), a.system);
  };

  const sdkb::RunFn run_requests = [&](const sdkb::Args& args, int count, bool record, sdkb::Phase& phase) {
    auto state = std::make_shared<State>();
    std::function<void(int)> launch;
    launch = [&, state](int index) {
      auto pending = std::make_shared<Pending>();
      sp::runtime::RunOptions run;
      run.streaming = args.stream();
      sp::runtime::Callbacks callbacks;
      if (args.stream()) {
        callbacks.on_event = [pending](const sp::Event& event) {
          if (const auto* delta = std::get_if<sp::PartDelta>(&event))
            if (delta->payload.kind == sp::PartKind::Text && delta->payload.channel == sp::DeltaChannel::Content) {
              pending->text.append(delta->payload.bytes);
              ++pending->deltas;
            }
        };
      }
      callbacks.on_outcome = [&, state, pending, index, record](sp::runtime::Result result) {
        const auto t1 = sdkb::now_ns();
        if (const auto* error = sp::as_error(result)) {
          phase.fail(error->safe_message + " (" + error->vendor_code + ")");
        } else if (record) {
          phase.latency_ns[index] = t1 - pending->t0;
          if (args.stream()) {
            phase.text[index] = std::move(pending->text);
            phase.deltas[index] = pending->deltas;
          } else if (const auto* completion = sp::as_completion(result)) {
            phase.text[index] = completion->joined_text();  // after the clock has stopped
          }
        }
        const int following = state->next.fetch_add(1);
        if (following < count) launch(following);
        if (state->done.fetch_add(1) + 1 == count) {
          std::lock_guard lock(state->mutex);
          state->finished.notify_all();
        }
      };
      pending->t0 = sdkb::now_ns();
      try {
        client.start(make_request(), run, std::move(callbacks)).detach();
      } catch (const std::exception& e) {
        phase.fail(std::string("start: ") + e.what());
        const int following = state->next.fetch_add(1);
        if (following < count) launch(following);
        if (state->done.fetch_add(1) + 1 == count) {
          std::lock_guard lock(state->mutex);
          state->finished.notify_all();
        }
      }
    };
    const int first = std::min(args.concurrency, count);
    state->next = first;
    for (int i = 0; i < first; ++i) launch(i);
    std::unique_lock lock(state->mutex);
    state->finished.wait(lock, [&] { return state->done.load() >= count; });
  };

  const std::string config =
      std::string("{\"concurrency_model\":\"one Client; start() with callbacks, the next request starts from the previous one's "
                  "completion callback; the runtime's worker pool (workers=") + std::to_string(options.workers) +
      ") runs the callbacks, libcurl multi on io_threads=" + std::to_string(options.transport.io_threads) +
      "\",\"http\":\"1.1 (http_version=Http1_1)\",\"pool\":\"transport.max_host_connections=" + std::to_string(a.concurrency) +
      "\",\"ca\":\"Options::ca_file\",\"retries\":\"off (the client default)\",\"telemetry\":\"none\","
      "\"admission\":\"max_operations=" + std::to_string(options.limits.max_operations) + "\",\"libcurl\":\"" + curl_version() + "\"}";
  return sdkb::run(a, "sp", SDKB_CLIENT_VERSION, config, run_requests);
}
