#pragma once
// A realtime session (D8): a provider codec (`<codecs/realtime.h>`) over a WebSocket session of the transport.
//
//  * Inbound is pulled: `next_event` returns the server's events in order, blocking up to a timeout. Queued model
//    audio is bounded, and so are the other queued events (text, transcripts, tool calls, finished turns, messages
//    the codec does not model); past either bound the session ends (`runtime.realtime.inbound_overflow`) rather than
//    drop the model's output.
//  * Outbound has two lanes. Commands other than audio go at once. Audio waits in a bounded ring and reaches the
//    socket at most `lead` ahead of real time, so a tool result or a cancel never waits behind seconds of audio.
//    When the ring is full the oldest audio is dropped or `send` refuses the new audio, by `media_policy`. The end of
//    the user's audio is a turn marker: it follows the audio queued before it, and a drop never crosses it. Audio
//    ahead of a marker still queued is never dropped, so when only such audio would be left to drop, `send` refuses
//    the new audio under either policy.
//    Queued audio moves on every call to `send` and `next_event`.
//  * Money: after each committed turn the session calls `renew` with the turn's usage. When it returns false the
//    session closes (`realtime.lease.refused`); the SDK prices nothing.
//  * The session never reconnects. Resumption handles and drain notices are events for the host to journal; a new
//    session is a new `open`.
#include "codecs/realtime.h"
#include "configuration/runtime_policy.h"
#include "runtime/host.h"
#include "transport/ws_transport.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace sp::realtime {
enum class MediaPolicy { DropOldest, Refuse };
struct Options {
  // The end of the whole session, from the handshake to the close. Unset: `max_duration` after the call to `open`.
  std::optional<std::chrono::steady_clock::time_point> deadline{};
  std::chrono::steady_clock::duration max_duration = std::chrono::minutes(15);
  // The outbound audio ring, in bytes (about 2 s at 16 kHz), and how far ahead of real time audio reaches the socket.
  std::size_t media_bytes = 64000;
  MediaPolicy media_policy = MediaPolicy::DropOldest;
  std::chrono::milliseconds lead{200};
  // Queued inbound model audio, in bytes.
  std::size_t inbound_media_bytes = std::size_t{32} << 20;
  // Queued inbound events other than model audio, in bytes of what they hold (a finished turn holds its audio again, in
  // base64, which is why the default is twice the audio bound).
  std::size_t inbound_event_bytes = std::size_t{64} << 20;
  // Called after each committed turn with its usage; false ends the session. It runs on an I/O thread: no blocking.
  std::function<bool(const Usage&)> renew{};
  // An extra trust anchor (tests, private CAs).
  std::string ca_file{};
  // Classifies the answer to a refused handshake, as it classifies the answer to a call: by its status table.
  configuration::PolicySnapshot runtime_policy = configuration::builtin_runtime_policy();
  SemanticLimits limits{};
  // The executor of the host that calls this session, when it has one that is pumped by the thread making the call
  // (`Executor::host_pumped`, runtime/host.h). `next_event` and `join` wait, so with such an executor, and when called
  // from an I/O thread or from a task of it, they refuse at once instead (see Session). Empty: only the I/O-thread
  // check applies.
  std::shared_ptr<runtime::Executor> executor{};
};
struct Outcome {
  // Why the session ended early: a refused setup, a lease refusal, an inbound overflow, a protocol or transport
  // failure, a `cancel` (`Cancelled`), a refused handshake (the kind and retry class `runtime_policy` gives its
  // status, with `http_status`), or a close the provider started with a code its fact reports as a failure, such as a
  // wrong key (`runtime.realtime.provider_closed`, the kind and retry class `runtime_policy` gives the fact's status,
  // `vendor_code` the close code). `clean` is true for such a close, as both Close frames were exchanged. Empty after
  // any other clean close.
  std::optional<Error> error{};
  std::uint16_t close_code = 0;
  std::string close_reason{};
  bool clean = false;
  // Outbound audio the ring dropped, in bytes.
  std::uint64_t media_dropped = 0;
};

struct SessionState;
class Session {
 public:
  Session() = default;
  Session(Session&&) noexcept;
  Session& operator=(Session&&) noexcept;
  ~Session();
  bool valid() const noexcept { return state_ != nullptr; }
  // Queues a command. False when the session has ended, the codec refused the command (`last_error`), or the
  // audio ring has no room for the audio: it is full under `MediaPolicy::Refuse`, or what it holds is fenced by a
  // turn marker.
  bool send(Command);
  // The next event, waiting up to `timeout`; std::nullopt on a timeout or once the session ended (`ended`).
  // A caller that must not wait gets std::nullopt at once, without waiting, and `last_error` holds the refusal: kind
  // `Misuse`, reason `runtime.blocking.host_pumped` when `Options::executor` is host-pumped, or
  // `runtime.blocking.in_thread` when called from an I/O thread (a `renew` callback, for one) or a task of that
  // executor. The session is untouched: the host reads events from a thread it may block, or not at all.
  std::optional<Event> next_event(std::chrono::milliseconds timeout);
  bool ended() const;
  std::optional<Error> last_error() const;
  // Closes with a closing handshake; `cancel` ends at once.
  void close();
  void cancel() noexcept;
  // Waits for the end and returns how the session ended. A caller that must not wait (see `next_event`) gets an
  // Outcome at once whose `error` is the refusal and whose `clean` is false; the session goes on.
  Outcome join();
 private:
  explicit Session(std::shared_ptr<SessionState> state) : state_(std::move(state)) {}
  std::shared_ptr<SessionState> state_;
  friend std::variant<Session, Error> open(transport::Transport&, const descriptor::ValidatedDescriptor&, std::string_view, Setup, Options);
};
// Checks the setup against the origin's realtime fact with no I/O, then connects and sends the setup.
std::variant<Session, Error> open(transport::Transport&, const descriptor::ValidatedDescriptor&, std::string_view api_key, Setup, Options = {});
}  // namespace sp::realtime
