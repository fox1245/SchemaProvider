#pragma once
#include "sp/config_defaults.h"

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace sp::transport {

// Views are valid only during the sink invocation; copy values to retain them.
// An absent/empty event field is delivered as "message". The id persists until
// another non-NUL id field replaces it (an empty id field resets it).
struct SseFrame {
  std::string_view event;
  std::string_view data;
  std::string_view id;
};

struct SseLimits {
  // Decoded line bytes, excluding its CR/LF terminator and the leading BOM.
  std::size_t max_line_bytes = config_defaults::defaults_sse_max_line_bytes;
  // Aggregate retained data (including joining LFs), event type and persistent
  // id bytes. Replacements reclaim the old field's allowance. The implicit
  // "message" type is a constant, not retained input. The line buffer has its
  // own independent limit above.
  std::size_t max_event_bytes = config_defaults::defaults_sse_max_event_bytes;
  // Wire bytes consumed, including BOM, comments, ignored fields and CR/LF.
  std::size_t max_total_bytes = config_defaults::defaults_sse_max_total_bytes;
};

enum class SseError { None, ResourceLimit, InvalidUtf8, Misuse };

// Pure, single-stream framing; no provider semantics or reconnect behavior.
// UTF-8 is fail-closed rather than browser replacement: malformed/overlong
// sequences, surrogates, out-of-range scalars and incomplete encoding at EOF
// fail with InvalidUtf8. Earlier complete events remain delivered. One leading
// BOM is stripped. Chunk partitioning never changes frames or the final error.
// Not thread-safe. Do not destroy/move the framer from its sink. Recursive feed
// or finish, and an empty sink, terminate the stream with Misuse. Exceptions
// from a sink propagate after permanently stopping further delivery.
class SseFramer {
 public:
  using Sink = std::function<bool(const SseFrame&)>;

  explicit SseFramer(SseLimits limits = {});

  // True means all bytes were accepted and consumption remains open. False
  // means permanently stopped: sink returned false, an error, or already
  // finished. A false sink is termination, never pause/retry, and leaves error
  // None. Unconsumed bytes after termination are not validated or counted.
  bool feed(std::string_view bytes, const Sink& sink);

  // Discards pending data without synthesizing a delimiter/event. Idempotent;
  // feed after finish returns false without changing the existing error.
  void finish();

  SseError error() const { return error_; }
  // True after finish, a false/throwing sink, or any error.
  bool stopped() const { return stopped_; }

 private:
  bool fail(SseError error);
  bool consume_byte(unsigned char byte, const Sink& sink);
  bool consume_scalar(std::string_view scalar, const Sink& sink);
  bool process_line(const Sink& sink);
  bool fits_event(std::size_t data, std::size_t type, std::size_t id) const;

  SseLimits limits_;
  SseError error_ = SseError::None;
  std::size_t total_bytes_ = 0;
  std::string line_;
  std::string data_;
  std::string event_;
  std::string id_;
  std::array<char, 4> scalar_{};
  unsigned scalar_size_ = 0;
  unsigned remaining_ = 0;
  unsigned char continuation_min_ = 0x80;
  unsigned char continuation_max_ = 0xbf;
  bool at_start_ = true;
  bool after_cr_ = false;
  bool has_data_ = false;
  bool stopped_ = false;
  bool feeding_ = false;
};

}  // namespace sp::transport
