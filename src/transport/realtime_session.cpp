#include "transport/realtime_session.h"
#include "descriptor/policy.h"
#include "runtime/policy.h"
#include <condition_variable>
#include <deque>
#include <mutex>

namespace sp::realtime {
namespace {
Error session_error(ErrorKind kind, std::string reason) {
  Error error{kind, std::string(runtime::detail::safe_message(kind))};
  error.reason = std::move(reason);
  return error;
}
// The kind and retry class the runtime error policy gives an HTTP status, as for a call.
Error classified(int status, const configuration::RuntimePolicy& policy) {
  transport::Result answer;
  answer.status = transport::Status::Completed;
  answer.http_status = status;
  answer.attempt.response_head_seen = true;
  const auto now = std::chrono::steady_clock::now();
  const auto info = runtime::detail::inspect_response(policy, {}, status, {}, {}, now, std::chrono::system_clock::now());
  return runtime::detail::classify_failure(answer, info, std::nullopt, false, 1, now);
}
// Why a session that did not close cleanly ended, by cause: a cancellation and a refused handshake are not network
// faults, and a host must not retry them as such.
Error ended_early(const transport::WsResult& result, const configuration::RuntimePolicy& policy) {
  if (result.transport.status == transport::Status::Cancelled) return session_error(ErrorKind::Cancelled, "runtime.realtime.cancelled");
  if (result.transport.status == transport::Status::DeadlineExceeded) return session_error(ErrorKind::DeadlineExceeded, "runtime.realtime.closed");
  // The server answered the handshake with something other than 101: an HTTP answer, classified by the runtime error
  // policy as for a call.
  if (result.http_status != 0 && result.http_status != 101) {
    auto error = classified(result.http_status, policy);
    error.reason = "runtime.realtime.refused";
    return error;
  }
  return session_error(ErrorKind::Transport, "runtime.realtime.closed");
}
// A provider ends a session that failed after the handshake with a close code of its own (a wrong key: Google 1007,
// OpenAI 3000). The endpoint's fact names those codes and the HTTP status each stands for; the first entry that
// matches decides, and the runtime error policy classifies the status.
std::optional<Error> ended_by_provider(const transport::WsResult& result, const std::vector<descriptor::CloseCodeFact>& closes,
                                       const configuration::RuntimePolicy& policy) {
  if (!result.closed_by_peer) return std::nullopt;
  for (const auto& close : closes) {
    if (close.code != result.close_code || result.close_reason.find(close.reason) == std::string::npos) continue;
    auto error = classified(close.status, policy);
    error.http_status = 0;
    error.vendor_code = std::to_string(result.close_code);
    error.reason = "runtime.realtime.provider_closed";
    return error;
  }
  return std::nullopt;
}
// What a queued event holds. Audio counts apart from the rest, and every event costs the space of the event itself, so
// a queue of events that carry nothing is bounded too.
struct Held { std::size_t media = 0, other = sizeof(Event); };
std::size_t completion_bytes(const Completion& completion) {
  std::size_t bytes = 0;
  for (const auto& message : completion.messages)
    for (const auto& part : message.parts) {
      if (const auto* text = std::get_if<sp::Text>(&part)) bytes += text->value.size();
      else if (const auto* media = std::get_if<Media>(&part)) bytes += (media->data ? media->data->size() : 0) + media->transcript.size();
      else if (const auto* call = std::get_if<ToolCall>(&part)) bytes += call->id.size() + call->name.size() + (call->input ? call->input->retained_bytes() : 0);
    }
  return bytes;
}
Held held_by(const Event& event) {
  Held held;
  if (const auto* audio = std::get_if<AudioDelta>(&event)) held.media = audio->pcm.size();
  else if (const auto* text = std::get_if<TextDelta>(&event)) held.other += text->text.size();
  else if (const auto* transcript = std::get_if<Transcript>(&event)) held.other += transcript->text.size();
  else if (const auto* call = std::get_if<ToolCallEvent>(&event)) held.other += call->id.size() + call->name.size() + (call->arguments ? call->arguments->retained_bytes() : 0);
  else if (const auto* cancelled = std::get_if<ToolCallsCancelled>(&event)) { for (const auto& id : cancelled->ids) held.other += id.size(); }
  else if (const auto* turn = std::get_if<TurnCommitted>(&event)) held.other += completion_bytes(turn->turn);
  else if (const auto* resumption = std::get_if<ResumptionUpdate>(&event)) held.other += resumption->handle.size();
  else if (const auto* failure = std::get_if<ProviderError>(&event)) held.other += failure->error.safe_message.size() + failure->error.vendor_code.size();
  else if (const auto* raw = std::get_if<Unmodelled>(&event)) held.other += raw->raw ? raw->raw->retained_bytes() : 0;
  return held;
}
}  // namespace

struct SessionState {
  std::mutex mutex;
  std::condition_variable changed;
  std::unique_ptr<Codec> codec;
  transport::WsSession socket;
  Options options;
  struct Arrived { Event event; Held held; };
  std::deque<Arrived> events;
  Held inbound{0, 0};
  // The close codes of the endpoint's fact that report a failure.
  std::vector<descriptor::CloseCodeFact> closes;
  // The outbound audio ring: audio, and the turn markers queued behind it.
  struct Queued { Command command; std::size_t bytes = 0; };
  std::deque<Queued> ring;
  std::size_t ring_bytes = 0;
  std::chrono::steady_clock::time_point media_end{};
  std::uint64_t dropped = 0;
  bool done = false, closing = false;
  std::optional<Error> error, last_error;
  transport::WsResult result;

  // Queues the events a codec decoded, and collects the usage of the turns among them.
  void take_locked(std::vector<Event>& decoded, std::vector<Usage>& turns) {
    for (auto& event : decoded) {
      const auto held = held_by(event);
      inbound.media += held.media;
      inbound.other += held.other;
      if (const auto* turn = std::get_if<TurnCommitted>(&event)) turns.push_back(turn->turn.usage);
      events.push_back({std::move(event), held});
    }
  }
  bool write(const std::vector<Frame>& frames) {
    for (const auto& frame : frames)
      if (!(frame.binary ? socket.send_binary(frame.data) : socket.send_text(frame.data))) return false;
    return true;
  }
  // Moves queued audio to the socket while it stays within `lead` of real time; a marker goes once the audio before
  // it went.
  void pump_locked() {
    const auto now = std::chrono::steady_clock::now();
    const double bytes_per_second = codec->input_bytes_per_second();
    while (!ring.empty() && !done) {
      auto& front = ring.front();
      if (const auto* audio = std::get_if<Audio>(&front.command)) {
        if (media_end > now + options.lead) return;
        auto frames = codec->encode(*audio);
        if (auto* failure = std::get_if<Error>(&frames)) { last_error = *failure; }
        else write(std::get<std::vector<Frame>>(frames));
        media_end = std::max(media_end, now) + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(static_cast<double>(audio->pcm.size()) / bytes_per_second));
        ring_bytes -= front.bytes;
      } else {
        auto frames = codec->encode(front.command);
        if (auto* failure = std::get_if<Error>(&frames)) last_error = *failure;
        else write(std::get<std::vector<Frame>>(frames));
      }
      ring.pop_front();
    }
  }
};

Session::Session(Session&&) noexcept = default;
Session& Session::operator=(Session&&) noexcept = default;
Session::~Session() {
  if (state_) state_->socket.cancel();
}

bool Session::send(Command command) {
  if (!state_) return false;
  std::unique_lock lock(state_->mutex);
  auto& s = *state_;
  if (s.done || s.closing) return false;
  if (control(command)) {
    // Control goes ahead of queued audio.
    auto frames = s.codec->encode(command);
    if (auto* failure = std::get_if<Error>(&frames)) { s.last_error = *failure; return false; }
    return s.write(std::get<std::vector<Frame>>(frames));
  }
  if (const auto* audio = std::get_if<Audio>(&command)) {
    const auto bytes = audio->pcm.size();
    if (bytes > s.options.media_bytes) { s.last_error = session_error(ErrorKind::InvalidRequest, "runtime.realtime.audio_too_large"); return false; }
    if (s.ring_bytes + bytes > s.options.media_bytes) {
      if (s.options.media_policy == MediaPolicy::Refuse) return false;
      // Only the audio queued after the last turn marker, the current turn's, can go: the audio before a marker is
      // what the marker commits. When dropping all of it would still leave no room, the new audio is refused.
      const auto fence = std::find_if(s.ring.rbegin(), s.ring.rend(), [](const SessionState::Queued& q) { return turn_marker(q.command); }).base() - s.ring.begin();
      std::size_t fenced = 0;
      for (auto it = s.ring.begin(); it != s.ring.begin() + fence; ++it) fenced += it->bytes;
      if (fenced + bytes > s.options.media_bytes) return false;
      while (s.ring_bytes + bytes > s.options.media_bytes) {
        const auto oldest = std::find_if(s.ring.begin() + fence, s.ring.end(), [](const SessionState::Queued& q) { return std::holds_alternative<Audio>(q.command); });
        s.ring_bytes -= oldest->bytes;
        s.dropped += oldest->bytes;
        s.ring.erase(oldest);
      }
    }
    s.ring.push_back({std::move(command), bytes});
    s.ring_bytes += bytes;
  } else {
    s.ring.push_back({std::move(command), 0});
  }
  s.pump_locked();
  return true;
}

std::optional<Event> Session::next_event(std::chrono::milliseconds timeout) {
  if (!state_) return std::nullopt;
  auto& s = *state_;
  const auto until = std::chrono::steady_clock::now() + timeout;
  std::unique_lock lock(s.mutex);
  for (;;) {
    s.pump_locked();
    if (!s.events.empty()) {
      auto arrived = std::move(s.events.front());
      s.events.pop_front();
      s.inbound.media -= std::min(s.inbound.media, arrived.held.media);
      s.inbound.other -= std::min(s.inbound.other, arrived.held.other);
      return std::move(arrived.event);
    }
    if (s.done) return std::nullopt;
    const auto now = std::chrono::steady_clock::now();
    if (now >= until) return std::nullopt;
    // Wake at least every 20 ms while audio is queued, to keep it moving.
    const auto wait = s.ring.empty() ? until - now : std::min<std::chrono::steady_clock::duration>(until - now, std::chrono::milliseconds(20));
    s.changed.wait_for(lock, wait);
  }
}

bool Session::ended() const {
  if (!state_) return true;
  std::lock_guard lock(state_->mutex);
  return state_->done && state_->events.empty();
}
std::optional<Error> Session::last_error() const {
  if (!state_) return std::nullopt;
  std::lock_guard lock(state_->mutex);
  return state_->last_error;
}
void Session::close() {
  if (!state_) return;
  {
    std::lock_guard lock(state_->mutex);
    state_->closing = true;
  }
  state_->socket.close();
}
void Session::cancel() noexcept {
  if (state_) state_->socket.cancel();
}
Outcome Session::join() {
  if (!state_) return {session_error(ErrorKind::Misuse, "runtime.realtime.invalid")};
  state_->socket.join();
  std::unique_lock lock(state_->mutex);
  state_->changed.wait(lock, [&] { return state_->done; });
  Outcome outcome;
  outcome.error = state_->error;
  outcome.close_code = state_->result.close_code;
  outcome.close_reason = state_->result.close_reason;
  outcome.clean = state_->result.clean;
  outcome.media_dropped = state_->dropped;
  return outcome;
}

std::variant<Session, Error> open(transport::Transport& transport, const descriptor::ValidatedDescriptor& descriptor, std::string_view api_key,
                                  Setup setup, Options options) {
  auto connection = connect(descriptor, setup, api_key);
  if (auto* error = std::get_if<Error>(&connection)) return *error;
  auto state = std::make_shared<SessionState>();
  state->codec = make_codec(descriptor, options.limits);
  state->options = std::move(options);
  if (!state->options.runtime_policy) state->options.runtime_policy = configuration::builtin_runtime_policy();
  if (const auto* fact = descriptor.policy()->endpoint("realtime", descriptor.base_url())) state->closes = fact->close_codes;
  auto opening = state->codec->open(setup);
  if (auto* error = std::get_if<Error>(&opening)) return *error;
  transport::WsRequest request;
  request.url = std::get<Connection>(connection).url;
  for (auto& [name, value] : std::get<Connection>(connection).headers) request.headers.push_back({std::move(name), std::move(value)});
  if (state->options.deadline) request.deadline = *state->options.deadline;
  else {
    const auto now = std::chrono::steady_clock::now();
    request.deadline = state->options.max_duration >= std::chrono::steady_clock::time_point::max() - now
                           ? std::chrono::steady_clock::time_point::max() : now + state->options.max_duration;
  }
  request.ca_file = state->options.ca_file;
  std::weak_ptr<SessionState> weak = state;
  transport::WsCallbacks callbacks;
  callbacks.on_message = [weak](transport::WsMessageKind kind, std::string_view data) {
    auto s = weak.lock();
    if (!s) return;
    std::vector<Usage> turns;
    bool overflow = false, failed = false;
    {
      std::lock_guard lock(s->mutex);
      auto decoded = s->codec->decode(kind == transport::WsMessageKind::Binary, data);
      if (auto* error = std::get_if<Error>(&decoded)) {
        s->error = *error;
        failed = true;
      } else {
        s->take_locked(std::get<std::vector<Event>>(decoded), turns);
        if (s->inbound.media > s->options.inbound_media_bytes || s->inbound.other > s->options.inbound_event_bytes) {
          s->error = session_error(ErrorKind::ResourceLimit, "runtime.realtime.inbound_overflow");
          overflow = true;
        }
      }
    }
    s->changed.notify_all();
    if (failed || overflow) { s->socket.close(failed ? transport::ws_close::ProtocolError : transport::ws_close::TooBig); return; }
    // The host's budget renews the lease after each turn, outside the session's lock.
    if (s->options.renew)
      for (const auto& usage : turns)
        if (!s->options.renew(usage)) {
          {
            std::lock_guard lock(s->mutex);
            if (!s->error) s->error = session_error(ErrorKind::ResourceLimit, "runtime.realtime.lease_refused");
            s->closing = true;
          }
          s->socket.close(transport::ws_close::Normal, "budget");
          return;
        }
  };
  callbacks.on_done = [weak](const transport::WsResult& result) {
    auto s = weak.lock();
    if (!s) return;
    // A turn that waited for a message the session did not get settles now, and the host's budget sees its usage.
    std::vector<Usage> turns;
    {
      std::lock_guard lock(s->mutex);
      if (!s->error)
        if (auto tail = s->codec->finish(); std::holds_alternative<std::vector<Event>>(tail)) s->take_locked(std::get<std::vector<Event>>(tail), turns);
    }
    if (s->options.renew)
      for (const auto& usage : turns)
        if (!s->options.renew(usage)) {
          std::lock_guard lock(s->mutex);
          if (!s->error) s->error = session_error(ErrorKind::ResourceLimit, "runtime.realtime.lease_refused");
          break;
        }
    {
      std::lock_guard lock(s->mutex);
      s->done = true;
      s->result = result;
      if (!s->error && !s->closing) {
        if (!result.clean) s->error = ended_early(result, *s->options.runtime_policy);
        else s->error = ended_by_provider(result, s->closes, *s->options.runtime_policy);
      }
    }
    s->changed.notify_all();
  };
  state->socket = transport::open_websocket(transport, std::move(request), std::move(callbacks));
  // Messages sent before the handshake completes are queued by the socket and written after it.
  {
    std::lock_guard lock(state->mutex);
    state->write(std::get<std::vector<Frame>>(opening));
  }
  return Session(std::move(state));
}
}  // namespace sp::realtime
