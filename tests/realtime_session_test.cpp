// Realtime session properties (D8) against tests/support/ws_server.mjs, whose `/v1/realtime` path answers in the
// shape of OpenAI Realtime and records the order of what it received.
//
// Usage: sp_realtime_session_tests <node> <ws_server.mjs>
#include "transport/realtime_session.h"
#include "descriptor/policy.h"
#include "sp/config_defaults.h"
#include "json/json.h"
#include "support/runtime_peer.h"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace sp;
using namespace std::chrono_literals;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)
runtime_test::Peer* g_peer = nullptr;

descriptor::ValidatedDescriptor local() {
  const std::string origin = "http://127.0.0.1:" + std::to_string(g_peer->port);
  auto parsed = json::parse(config_defaults::descriptor_policy_json);
  CHECK(std::holds_alternative<json::Document>(parsed));
  std::string text = "{";
  for (auto member : std::get<json::Document>(parsed).root().members()) {
    if (text.size() > 1) text += ',';
    text += json::quote(member.key) + ":" + (member.key == "endpoints"
        ? R"([{"endpoint":"realtime","shape":"openai","origins":[)" + json::quote(origin) + R"(],"paths":{"connect":"/v1/realtime"}}])"
        : member.value.dump());
  }
  auto policy = descriptor::load_policy(text + "}", config_defaults::codec_defaults_json);
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(policy));
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"realtime-local","family":"openai.chat","connection":{"base_url":)" + json::quote(origin) +
                                 R"(,"paths":{"buffered":"/v1/chat/completions","streaming":"/v1/chat/completions"}}})",
                                 std::get<descriptor::PolicySnapshot>(policy));
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
std::vector<std::string> received() {
  const auto stats = g_peer->command("{\"stats\":true}");
  std::vector<std::string> out;
  for (auto item : stats.root().get("realtime").elements()) out.emplace_back(item.as_string());
  return out;
}
realtime::Setup setup() {
  realtime::Setup value;
  value.model = "gpt-realtime-2.1-mini";
  value.instructions = "Answer briefly.";
  return value;
}
realtime::Session opened(transport::Transport& transport, const descriptor::ValidatedDescriptor& d, realtime::Options options) {
  auto session = realtime::open(transport, d, "test-key", setup(), std::move(options));
  if (auto* error = std::get_if<Error>(&session)) throw std::runtime_error("open refused: " + error->reason);
  return std::get<realtime::Session>(std::move(session));
}
// Pulls events until one of type T arrives or the session ends.
template <class T> std::optional<T> await(realtime::Session& session, std::chrono::seconds limit = 10s) {
  const auto until = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < until) {
    auto event = session.next_event(100ms);
    if (event && std::holds_alternative<T>(*event)) return std::get<T>(std::move(*event));
    if (!event && session.ended()) return std::nullopt;
  }
  return std::nullopt;
}
std::string pcm(std::size_t bytes) { return std::string(bytes, '\x01'); }

// A tool result goes ahead of queued audio; the end of the audio, a text turn and a request to answer wait for it.
void lanes_and_fences(transport::Transport& transport, const descriptor::ValidatedDescriptor& d) {
  const auto before = received().size();
  realtime::Options options;
  options.lead = 0ms;
  auto session = opened(transport, d, options);
  CHECK(await<realtime::Ready>(session));
  for (int i = 0; i < 5; ++i) CHECK(session.send(realtime::Audio{pcm(4800)}));  // 0.5 s at 24 kHz
  CHECK(session.send(realtime::EndAudio{}));
  CHECK(session.send(realtime::Text{"And in Seoul?"}));
  CHECK(session.send(realtime::ToolResult{"call-1", "get_weather", nullptr}));
  CHECK(session.send(realtime::Respond{}));
  const auto turn = await<realtime::TurnCommitted>(session);
  CHECK(turn && turn->turn.usage.input_total->value == 10 && turn->turn.usage.output_total->value == 5);
  const auto all = received();
  const std::vector<std::string> mine(all.begin() + static_cast<std::ptrdiff_t>(before), all.end());
  const auto first = [&](std::string_view type) { return std::find(mine.begin(), mine.end(), type) - mine.begin(); };
  const auto last = [&](std::string_view type) { return std::find(mine.rbegin(), mine.rend(), type).base() - mine.begin() - 1; };
  CHECK(mine.front() == "session.update");
  CHECK(std::count(mine.begin(), mine.end(), "input_audio_buffer.append") == 5);
  // The tool result (the first item) overtook the audio still queued; the commit, the text and the answer did not.
  CHECK(first("conversation.item.create") < last("input_audio_buffer.append"));
  CHECK(first("input_audio_buffer.commit") > last("input_audio_buffer.append"));
  CHECK(last("conversation.item.create") > first("input_audio_buffer.commit") && first("response.create") > last("conversation.item.create"));
  session.close();
  const auto outcome = session.join();
  CHECK(!outcome.error && outcome.clean && outcome.media_dropped == 0);
}

// The host's budget refuses to renew the lease after a turn: the session closes with that reason.
void lease_refused(transport::Transport& transport, const descriptor::ValidatedDescriptor& d) {
  realtime::Options options;
  std::atomic<int> renewals{0};
  options.renew = [&](const Usage& usage) { ++renewals; return usage.total && usage.total->value < 10; };
  auto session = opened(transport, d, options);
  CHECK(session.send(realtime::Respond{}));
  CHECK(await<realtime::TurnCommitted>(session));
  const auto outcome = session.join();
  CHECK(renewals == 1 && outcome.error && outcome.error->reason == "runtime.realtime.lease_refused" && outcome.close_code == 1000);
  CHECK(!session.send(realtime::Respond{}));
}

// Model audio the host does not read is bounded: past the bound the session ends rather than drop it.
void inbound_overflow(transport::Transport& transport, const descriptor::ValidatedDescriptor& d) {
  realtime::Options options;
  options.inbound_media_bytes = 200000;
  auto session = opened(transport, d, options);
  CHECK(session.send(realtime::Text{"flood"}));
  const auto outcome = session.join();
  CHECK(outcome.error && outcome.error->reason == "runtime.realtime.inbound_overflow" && outcome.error->kind == ErrorKind::ResourceLimit);
}

// A full audio ring drops its oldest audio, or refuses new audio, by policy.
void media_ring(transport::Transport& transport, const descriptor::ValidatedDescriptor& d) {
  realtime::Options options;
  options.lead = 0ms;
  options.media_bytes = 9600;
  auto session = opened(transport, d, options);
  for (int i = 0; i < 6; ++i) CHECK(session.send(realtime::Audio{pcm(4800)}));
  session.close();
  CHECK(session.join().media_dropped >= 4800);
  options.media_policy = realtime::MediaPolicy::Refuse;
  auto strict = opened(transport, d, options);
  int refused = 0;
  for (int i = 0; i < 6; ++i) refused += strict.send(realtime::Audio{pcm(4800)}) ? 0 : 1;
  CHECK(refused >= 1);
  strict.close();
  CHECK(strict.join().media_dropped == 0);
}

// A setup outside the fact is refused before any connection.
void refused_setup(transport::Transport& transport, const descriptor::ValidatedDescriptor& d) {
  const auto handshakes = g_peer->command("{\"stats\":true}").root().get("handshakes").as_uint();
  auto bad = setup();
  auto parsed = json::parse(R"({"temperature":0.5})");
  bad.extensions = std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
  auto session = realtime::open(transport, d, "test-key", bad, {});
  CHECK(std::holds_alternative<Error>(session) && std::get<Error>(session).reason == "request.realtime.options_not_admitted");
  CHECK(g_peer->command("{\"stats\":true}").root().get("handshakes").as_uint() == handshakes);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { std::cerr << "usage: sp_realtime_session_tests <node> <ws_server.mjs>\n"; return 2; }
  try {
    runtime_test::Peer peer(argv[1], argv[2]);
    g_peer = &peer;
    transport::Transport transport;
    const auto d = local();
    lanes_and_fences(transport, d);
    lease_refused(transport, d);
    inbound_overflow(transport, d);
    media_ring(transport, d);
    refused_setup(transport, d);
    g_peer = nullptr;
    std::cout << "realtime session properties passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "realtime session test failed: " << error.what() << '\n';
    return 1;
  }
}
