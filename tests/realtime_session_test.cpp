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
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace sp;
using namespace std::chrono_literals;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)
runtime_test::Peer* g_peer = nullptr;

// The audio formats of OpenAI Realtime: 24 kHz PCM and G.711.
const char* openai_audio = R"("audio":{"input":[{"mime":"audio/pcm;rate=24000","rate":24000,"sample_bytes":2},{"mime":"audio/pcmu","rate":8000,"sample_bytes":1}],)"
                           R"("output":[{"mime":"audio/pcm;rate=24000","rate":24000,"sample_bytes":2},{"mime":"audio/pcma","rate":8000,"sample_bytes":1}]})";

const char* google_audio = R"("audio":{"input":[{"mime":"audio/pcm;rate=16000","rate":16000,"sample_bytes":2}],)"
                           R"("output":[{"mime":"audio/pcm;rate=24000","rate":24000,"sample_bytes":2}]})";

// A policy whose realtime fact is `connect` on the test peer, with `more` members (each led by a comma) after the
// audio formats. The shape is OpenAI Realtime's, or Google Live's when `google` is set.
descriptor::ValidatedDescriptor local(const std::string& connect = "/v1/realtime", const std::string& more = {}, bool google = false) {
  const std::string origin = "http://127.0.0.1:" + std::to_string(g_peer->port);
  auto parsed = json::parse(config_defaults::descriptor_policy_json);
  CHECK(std::holds_alternative<json::Document>(parsed));
  std::string text = "{";
  for (auto member : std::get<json::Document>(parsed).root().members()) {
    if (text.size() > 1) text += ',';
    text += json::quote(member.key) + ":" + (member.key == "endpoints"
        ? std::string(R"([{"endpoint":"realtime","shape":")") + (google ? "google" : "openai") + R"(","origins":[)" + json::quote(origin) +
              R"(],"paths":{"connect":)" + json::quote(connect) + "}," + (google ? google_audio : openai_audio) + more + "}]"
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
realtime::Session opened(transport::Transport& transport, const descriptor::ValidatedDescriptor& d, realtime::Options options,
                         realtime::Setup with = setup()) {
  auto session = realtime::open(transport, d, "test-key", std::move(with), std::move(options));
  if (auto* error = std::get_if<Error>(&session)) throw std::runtime_error("open refused: " + error->reason);
  return std::get<realtime::Session>(std::move(session));
}
// The audio format types the peer received in session updates, in order (`input:audio/pcmu`).
std::vector<std::string> formats() {
  const auto stats = g_peer->command("{\"stats\":true}");
  std::vector<std::string> out;
  for (auto item : stats.root().get("formats").elements()) out.emplace_back(item.as_string());
  return out;
}
// The outcome of a session that must end by itself: a session that does not is cancelled and the test fails.
realtime::Outcome ended_within(realtime::Session& session, std::chrono::seconds limit = 10s) {
  auto outcome = std::async(std::launch::async, [&] { return session.join(); });
  if (outcome.wait_for(limit) != std::future_status::ready) {
    session.cancel();
    outcome.wait();
    throw std::runtime_error("the session did not end by itself");
  }
  return outcome.get();
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

// Model audio the host does not read is bounded: past the bound the session ends rather than drop it. So is every other
// event: a session that answers in text only is bounded too.
void inbound_overflow(transport::Transport& transport, const descriptor::ValidatedDescriptor& d) {
  realtime::Options options;
  options.inbound_media_bytes = 200000;
  auto session = opened(transport, d, options);
  CHECK(session.send(realtime::Text{"flood"}));
  const auto outcome = ended_within(session);
  CHECK(outcome.error && outcome.error->reason == "runtime.realtime.inbound_overflow" && outcome.error->kind == ErrorKind::ResourceLimit);
  realtime::Options text;
  text.inbound_event_bytes = 200000;
  auto talkative = opened(transport, d, text);
  CHECK(talkative.send(realtime::Text{"flood-text"}));
  const auto text_outcome = ended_within(talkative);
  CHECK(text_outcome.error && text_outcome.error->reason == "runtime.realtime.inbound_overflow" && text_outcome.error->kind == ErrorKind::ResourceLimit);
  // Events the host reads leave the bound: a host that keeps up is not cut off.
  realtime::Options generous;
  generous.inbound_event_bytes = 300000;
  auto reading = opened(transport, d, generous);
  CHECK(reading.send(realtime::Text{"flood-text"}));
  int deltas = 0;
  while (deltas < 8) {
    auto event = reading.next_event(5s);
    CHECK(event);
    deltas += std::holds_alternative<realtime::TextDelta>(*event) ? 1 : 0;
  }
  reading.close();
  CHECK(!reading.join().error);
}

// The session's end is `max_duration` after `open`, not after the `Options` were made, and an explicit deadline wins.
void deadline_counts_from_open(transport::Transport& transport, const descriptor::ValidatedDescriptor& d) {
  realtime::Options options;
  options.max_duration = 600ms;
  std::this_thread::sleep_for(700ms);
  const auto begun = std::chrono::steady_clock::now();
  auto session = opened(transport, d, options);
  CHECK(await<realtime::Ready>(session));
  const auto outcome = ended_within(session);
  CHECK(outcome.error && outcome.error->kind == ErrorKind::DeadlineExceeded);
  CHECK(std::chrono::steady_clock::now() - begun >= 500ms);
  options.max_duration = 1h;
  options.deadline = std::chrono::steady_clock::now() + 300ms;
  auto bounded = opened(transport, d, options);
  const auto early = ended_within(bounded);
  CHECK(early.error && early.error->kind == ErrorKind::DeadlineExceeded);
}

// A tool-call turn waits for the message that ends it and carries its usage. When the session ends first, the turn
// settles at the end, and the host's budget sees it.
void tool_turn_settles_at_the_end(transport::Transport& transport) {
  realtime::Options options;
  std::atomic<int> renewals{0};
  options.renew = [&](const Usage&) { ++renewals; return true; };
  auto settled = setup();
  settled.model = "tool-then-close";
  auto session = opened(transport, local("/realtime/google", {}, true), options, settled);
  const auto call = await<realtime::ToolCallEvent>(session);
  CHECK(call && call->name == "get_weather");
  const auto turn = await<realtime::TurnCommitted>(session);
  CHECK(turn && turn->turn.stop.kind == StopKind::ToolUse);
  const auto outcome = session.join();
  CHECK(!outcome.error && renewals == 1);
}

// A provider that ends the session itself over a failure says so with a close code of its own. The fact names the codes
// that are failures and the status each stands for, and the runtime error policy classifies that status.
void provider_close(transport::Transport& transport) {
  const std::string codes = R"(,"close_codes":[{"code":3000,"status":401},{"code":1007,"reason":"API key not valid","status":401},{"code":4003,"status":503}])";
  const auto closed_with = [&](int code, const std::string& reason) {
    auto session = opened(transport, local("/realtime/close/" + std::to_string(code) + "/" + reason, codes), {});
    return ended_within(session);
  };
  const auto wrong_key = closed_with(3000, "invalid_request_error.invalid_api_key");
  CHECK(wrong_key.clean && wrong_key.close_code == 3000 && wrong_key.error);
  CHECK(wrong_key.error->kind == ErrorKind::Authentication && wrong_key.error->retry_class == RetryClass::Never);
  CHECK(wrong_key.error->reason == "runtime.realtime.provider_closed" && wrong_key.error->vendor_code == "3000" && wrong_key.error->http_status == 0);
  const auto google_key = closed_with(1007, "API%20key%20not%20valid.%20Please%20pass%20a%20valid%20API%20key.");
  CHECK(google_key.error && google_key.error->kind == ErrorKind::Authentication && google_key.close_reason.starts_with("API key not valid"));
  // The same code with another reason is not a wrong key, and a code the fact does not list is an ordinary close.
  CHECK(!closed_with(1007, "bad%20payload").error);
  CHECK(!closed_with(4001, "bye").error);
  // The status is the policy's to classify.
  const auto busy = closed_with(4003, "busy");
  CHECK(busy.error && busy.error->kind == ErrorKind::Overloaded && busy.error->retry_class == RetryClass::Transient);
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

// Control goes ahead of audio even when the ring is full: a tool result reaches the socket while the ring still holds
// the audio queued before it.
void control_preempts_full_ring(transport::Transport& transport, const descriptor::ValidatedDescriptor& d) {
  const auto before = received().size();
  realtime::Options options;
  options.lead = 0ms;
  options.media_bytes = 9600;
  options.media_policy = realtime::MediaPolicy::Refuse;
  auto session = opened(transport, d, options);
  CHECK(await<realtime::Ready>(session));
  // The first chunk leaves at once and holds the rest back for 0.1 s; the ring takes two more and refuses the fourth.
  CHECK(session.send(realtime::Audio{pcm(4800)}));
  CHECK(session.send(realtime::Audio{pcm(4800)}));
  CHECK(session.send(realtime::Audio{pcm(4800)}));
  CHECK(!session.send(realtime::Audio{pcm(4800)}));
  CHECK(session.send(realtime::ToolResult{"call-1", "get_weather", nullptr}));
  // Queued audio moves only when the host calls the session, so what has arrived is what overtook it.
  std::vector<std::string> mine;
  for (const auto until = std::chrono::steady_clock::now() + 5s; std::chrono::steady_clock::now() < until;) {
    const auto all = received();
    mine.assign(all.begin() + static_cast<std::ptrdiff_t>(before), all.end());
    if (std::count(mine.begin(), mine.end(), "conversation.item.create")) break;
    std::this_thread::sleep_for(10ms);
  }
  CHECK(std::count(mine.begin(), mine.end(), "conversation.item.create") == 1);
  CHECK(std::count(mine.begin(), mine.end(), "input_audio_buffer.append") == 1);
  session.cancel();
  session.join();
}

// A session may choose another audio format of the fact's: G.711 is sent as such, and the input paces at its rate.
void audio_formats(transport::Transport& transport, const descriptor::ValidatedDescriptor& d) {
  const auto before = formats().size();
  auto chosen = setup();
  chosen.input_audio = "audio/pcmu";
  chosen.output_audio = "audio/pcma";
  realtime::Options options;
  options.lead = 0ms;
  const auto begun = received().size();
  auto session = opened(transport, d, options, chosen);
  CHECK(await<realtime::Ready>(session));
  const auto seen = formats();
  CHECK(seen.size() == before + 2 && seen[before] == "input:audio/pcmu" && seen[before + 1] == "output:audio/pcma");
  // 8000 bytes of G.711 are a second (a sixth of a second at the 24 kHz PCM rate): the second chunk waits for the first.
  CHECK(session.send(realtime::Audio{pcm(8000)}));
  CHECK(session.send(realtime::Audio{pcm(8000)}));
  const auto appended = [&] {
    const auto all = received();
    return std::count(all.begin() + static_cast<std::ptrdiff_t>(begun), all.end(), "input_audio_buffer.append");
  };
  for (const auto until = std::chrono::steady_clock::now() + 400ms; std::chrono::steady_clock::now() < until;) session.next_event(25ms);
  CHECK(appended() == 1);
  for (const auto until = std::chrono::steady_clock::now() + 5s; std::chrono::steady_clock::now() < until && appended() < 2;) session.next_event(25ms);
  CHECK(appended() == 2);
  session.close();
  CHECK(!session.join().error);
  // A format the fact does not list is refused before any connection.
  const auto handshakes = g_peer->command("{\"stats\":true}").root().get("handshakes").as_uint();
  auto foreign = setup();
  foreign.input_audio = "audio/pcm;rate=16000";
  const auto refused = realtime::open(transport, d, "test-key", foreign, {});
  CHECK(std::holds_alternative<Error>(refused) && std::get<Error>(refused).reason == "request.realtime.audio_format_invalid");
  CHECK(g_peer->command("{\"stats\":true}").root().get("handshakes").as_uint() == handshakes);
}

// The audio and turn markers the server received since `before`, in order: 'a' for audio, 'c' for the end of it. Pulling
// events moves queued audio, so the wait keeps the session going.
std::string audio_order(realtime::Session& session, std::size_t before, std::size_t count) {
  std::string order;
  for (const auto until = std::chrono::steady_clock::now() + 5s; std::chrono::steady_clock::now() < until && order.size() < count;) {
    session.next_event(25ms);
    order.clear();
    const auto all = received();
    for (auto i = before; i < all.size(); ++i) {
      if (all[i] == "input_audio_buffer.append") order += 'a';
      else if (all[i] == "input_audio_buffer.commit") order += 'c';
    }
  }
  return order;
}

// Audio ahead of a turn marker still in the ring is never dropped: when the ring holds nothing else, the new audio is
// refused. Audio after the last marker goes first.
void media_ring_fence(transport::Transport& transport, const descriptor::ValidatedDescriptor& d) {
  realtime::Options options;
  options.lead = 0ms;
  options.media_bytes = 24000;
  // The first chunk leaves at once and holds the rest back for 0.25 s at 24 kHz.
  const auto chunk = [] { return realtime::Audio{pcm(12000)}; };
  {
    const auto before = received().size();
    auto session = opened(transport, d, options);
    CHECK(await<realtime::Ready>(session));
    for (int i = 0; i < 3; ++i) CHECK(session.send(chunk()));
    CHECK(session.send(realtime::EndAudio{}));
    CHECK(!session.send(chunk()));  // the two chunks in the ring are fenced by the marker
    CHECK(audio_order(session, before, 4) == "aaac");
    session.close();
    const auto outcome = session.join();
    CHECK(!outcome.error && outcome.media_dropped == 0);
  }
  {
    const auto before = received().size();
    auto session = opened(transport, d, options);
    CHECK(await<realtime::Ready>(session));
    CHECK(session.send(chunk()));
    CHECK(session.send(chunk()));
    CHECK(session.send(realtime::EndAudio{}));
    CHECK(session.send(chunk()));
    CHECK(session.send(chunk()));  // the audio after the marker makes room; the one before it stays
    CHECK(audio_order(session, before, 4) == "aaca");
    session.close();
    const auto outcome = session.join();
    CHECK(!outcome.error && outcome.media_dropped == 12000);
  }
}

// A session that does not close cleanly says why: a cancel is `Cancelled`, and a refused handshake is classified by the
// runtime error policy as an HTTP answer is, with the server's status. Neither is a transport fault.
void ends_by_cause(transport::Transport& transport, const descriptor::ValidatedDescriptor& d) {
  auto session = opened(transport, d, {});
  CHECK(await<realtime::Ready>(session));
  session.cancel();
  const auto cancelled = session.join();
  CHECK(cancelled.error && cancelled.error->kind == ErrorKind::Cancelled && cancelled.error->reason == "runtime.realtime.cancelled");
  CHECK(!cancelled.clean);
  struct Refusal { int status; ErrorKind kind; RetryClass retry; };
  const Refusal refusals[]{{401, ErrorKind::Authentication, RetryClass::Never}, {403, ErrorKind::Permission, RetryClass::Never},
                           {429, ErrorKind::LimitUnknown, RetryClass::Unknown}, {503, ErrorKind::Overloaded, RetryClass::Transient},
                           {418, ErrorKind::RemoteFailure, RetryClass::Unknown}};
  const auto refused_by = [&](const Refusal& refusal, realtime::Options options) {
    const auto refusing = local("/reject/" + std::to_string(refusal.status));
    auto refused = opened(transport, refusing, std::move(options));
    const auto outcome = refused.join();
    CHECK(!outcome.clean && outcome.error && outcome.error->reason == "runtime.realtime.refused" && outcome.error->http_status == refusal.status);
    return outcome.error;
  };
  for (const auto& refusal : refusals) {
    const auto error = refused_by(refusal, {});
    CHECK(error->kind == refusal.kind && error->retry_class == refusal.retry);
  }
  // The policy is the host's to choose: here it rates 429 as a rate limit.
  std::string text(config_defaults::error_policy_json);
  const std::string listed = R"({"kind":"LimitUnknown","retry":"Unknown","status":429})";
  const auto at = text.find(listed);
  CHECK(at != std::string::npos);
  text.replace(at, listed.size(), R"({"kind":"RateLimited","retry":"Transient","status":429})");
  auto policy = configuration::load_runtime_policy(config_defaults::runtime_defaults_json, text);
  CHECK(std::holds_alternative<configuration::PolicySnapshot>(policy));
  realtime::Options options;
  options.runtime_policy = std::get<configuration::PolicySnapshot>(policy);
  const auto limited = refused_by({429, ErrorKind::RateLimited, RetryClass::Transient}, options);
  CHECK(limited->kind == ErrorKind::RateLimited && limited->retry_class == RetryClass::Transient);
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

// The executor of a host that calls the session: it runs nothing, and says whether the host pumps it on the calling
// thread (`pumped`) and whether a task of it is running (`running`).
class TestHost final : public runtime::Executor {
 public:
  void post(Task) override {}
  Timer schedule(runtime::SteadyTime, Task) override { return 0; }
  void cancel(Timer) noexcept override {}
  runtime::SteadyTime now() const noexcept override { return std::chrono::steady_clock::now(); }
  runtime::WallTime wall_now() const noexcept override { return std::chrono::system_clock::now(); }
  bool in_thread() const noexcept override { return running; }
  void shutdown() noexcept override {}
  bool host_pumped() const noexcept override { return pumped; }
  bool pumped = false, running = false;
};
void expect_blocking_refusal(const std::optional<Error>& error, std::string_view reason) {
  CHECK(error && error->kind == ErrorKind::Misuse && error->reason == reason);
}

// A wait for events needs another thread. It is refused at once, without waiting, when the host's executor is pumped by
// the calling thread, when called from a task of that executor, and from an I/O thread; the session is untouched, and
// waits again once the call is allowed.
void blocking_calls_refuse(transport::Transport& transport, const descriptor::ValidatedDescriptor& d) {
  const auto quick = [](auto&& call) {
    const auto begun = std::chrono::steady_clock::now();
    auto result = call();
    CHECK(std::chrono::steady_clock::now() - begun < 2s);
    return result;
  };
  const auto before = received().size();
  auto host = std::make_shared<TestHost>();
  realtime::Options options;
  options.executor = host;
  auto session = opened(transport, d, options);
  // The host is pumped by the calling thread: the pull API cannot be used, whatever the timeout.
  host->pumped = true;
  CHECK(!quick([&] { return session.next_event(10s); }));
  expect_blocking_refusal(session.last_error(), runtime::blocking_host_pumped_reason);
  const auto joined = quick([&] { return session.join(); });
  expect_blocking_refusal(joined.error, runtime::blocking_host_pumped_reason);
  CHECK(!joined.clean && !session.ended());
  // The session itself is alive: it connected and what is sent reaches the server.
  CHECK(session.send(realtime::Text{"still here"}));
  std::vector<std::string> seen;
  for (const auto until = std::chrono::steady_clock::now() + 10s; std::chrono::steady_clock::now() < until;) {
    seen = received();
    if (std::count(seen.begin() + static_cast<std::ptrdiff_t>(before), seen.end(), "conversation.item.create")) break;
    std::this_thread::sleep_for(5ms);
  }
  CHECK(std::count(seen.begin() + static_cast<std::ptrdiff_t>(before), seen.end(), "session.update") == 1);
  CHECK(std::count(seen.begin() + static_cast<std::ptrdiff_t>(before), seen.end(), "conversation.item.create") == 1);
  host->pumped = false;
  // A task of the host's executor is running on this thread.
  host->running = true;
  CHECK(!quick([&] { return session.next_event(10s); }));
  expect_blocking_refusal(session.last_error(), runtime::blocking_in_thread_reason);
  expect_blocking_refusal(quick([&] { return session.join(); }).error, runtime::blocking_in_thread_reason);
  host->running = false;
  // Allowed again, the waits are the waits they were.
  CHECK(await<realtime::Ready>(session));
  session.cancel();
  const auto outcome = session.join();
  CHECK(outcome.error && outcome.error->kind == ErrorKind::Cancelled);

  // From an I/O thread, with no executor at all: the callback that renews the lease runs there.
  realtime::Options callback;
  realtime::Session* self = nullptr;
  bool pulled = true;
  std::optional<Error> pull_error, join_error;
  std::atomic<bool> called{false};
  callback.renew = [&](const Usage&) {
    pulled = self->next_event(10s).has_value();
    pull_error = self->last_error();
    join_error = self->join().error;
    called = true;
    return true;
  };
  auto inside = opened(transport, d, callback);
  self = &inside;
  CHECK(inside.send(realtime::Respond{}));
  CHECK(await<realtime::TurnCommitted>(inside));
  for (const auto until = std::chrono::steady_clock::now() + 5s; !called && std::chrono::steady_clock::now() < until;) std::this_thread::sleep_for(5ms);
  CHECK(called && !pulled);
  expect_blocking_refusal(pull_error, runtime::blocking_in_thread_reason);
  expect_blocking_refusal(join_error, runtime::blocking_in_thread_reason);
  inside.close();
  CHECK(!inside.join().error);
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
    deadline_counts_from_open(transport, d);
    provider_close(transport);
    tool_turn_settles_at_the_end(transport);
    control_preempts_full_ring(transport, d);
    blocking_calls_refuse(transport, d);
    audio_formats(transport, d);
    media_ring(transport, d);
    media_ring_fence(transport, d);
    ends_by_cause(transport, d);
    refused_setup(transport, d);
    g_peer = nullptr;
    std::cout << "realtime session properties passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "realtime session test failed: " << error.what() << '\n';
    return 1;
  }
}
