#include "transport/realtime_session.h"
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
// Why a session that did not close cleanly ended, by cause: a cancellation and a refused handshake are not network
// faults, and a host must not retry them as such.
Error ended_early(const transport::WsResult& result, const configuration::RuntimePolicy& policy) {
  if (result.transport.status == transport::Status::Cancelled) return session_error(ErrorKind::Cancelled, "runtime.realtime.cancelled");
  if (result.transport.status == transport::Status::DeadlineExceeded) return session_error(ErrorKind::DeadlineExceeded, "runtime.realtime.closed");
  // The server answered the handshake with something other than 101: an HTTP answer, classified by the runtime error
  // policy as for a call.
  if (result.http_status != 0 && result.http_status != 101) {
    transport::Result answer;
    answer.status = transport::Status::Completed;
    answer.http_status = result.http_status;
    answer.attempt.response_head_seen = true;
    const auto now = std::chrono::steady_clock::now();
    const auto info = runtime::detail::inspect_response(policy, {}, result.http_status, {}, {}, now, std::chrono::system_clock::now());
    auto error = runtime::detail::classify_failure(answer, info, std::nullopt, false, 1, now);
    error.reason = "runtime.realtime.refused";
    return error;
  }
  return session_error(ErrorKind::Transport, "runtime.realtime.closed");
}
}  // namespace

struct SessionState {
  std::mutex mutex;
  std::condition_variable changed;
  std::unique_ptr<Codec> codec;
  transport::WsSession socket;
  Options options;
  std::deque<Event> events;
  std::size_t inbound_media = 0;
  // The outbound audio ring: audio, and the turn markers queued behind it.
  struct Queued { Command command; std::size_t bytes = 0; };
  std::deque<Queued> ring;
  std::size_t ring_bytes = 0;
  std::chrono::steady_clock::time_point media_end{};
  std::uint64_t dropped = 0;
  bool done = false, closing = false;
  std::optional<Error> error, last_error;
  transport::WsResult result;

  bool write(const std::vector<Frame>& frames) {
    for (const auto& frame : frames)
      if (!(frame.binary ? socket.send_binary(frame.data) : socket.send_text(frame.data))) return false;
    return true;
  }
  // Moves queued audio to the socket while it stays within `lead` of real time; a marker goes once the audio before
  // it went.
  void pump_locked() {
    const auto now = std::chrono::steady_clock::now();
    const double bytes_per_second = 2.0 * codec->input_rate();
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
      auto event = std::move(s.events.front());
      s.events.pop_front();
      if (const auto* audio = std::get_if<AudioDelta>(&event)) s.inbound_media -= std::min(s.inbound_media, audio->pcm.size());
      return event;
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
  auto opening = state->codec->open(setup);
  if (auto* error = std::get_if<Error>(&opening)) return *error;
  transport::WsRequest request;
  request.url = std::get<Connection>(connection).url;
  for (auto& [name, value] : std::get<Connection>(connection).headers) request.headers.push_back({std::move(name), std::move(value)});
  request.deadline = state->options.deadline;
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
        for (auto& event : std::get<std::vector<Event>>(decoded)) {
          if (const auto* audio = std::get_if<AudioDelta>(&event)) s->inbound_media += audio->pcm.size();
          if (const auto* turn = std::get_if<TurnCommitted>(&event)) turns.push_back(turn->turn.usage);
          s->events.push_back(std::move(event));
        }
        if (s->inbound_media > s->options.inbound_media_bytes) {
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
    {
      std::lock_guard lock(s->mutex);
      s->done = true;
      s->result = result;
      if (!s->error && !result.clean && !s->closing) s->error = ended_early(result, *s->options.runtime_policy);
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
