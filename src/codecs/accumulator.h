#pragma once
#include "core/value.h"
#include <functional>
#include <map>

namespace sp {
class Accumulator {
 public:
  // Events and delta/snapshot views are borrowed for this invocation only.
  using Sink = std::function<void(const Event&)>;
  explicit Accumulator(SemanticLimits limits = {}, Sink sink = {});
  // A known transport failure takes precedence over errors in deferred decoding.
  bool accept(const Event& event, const Error* failure_override = nullptr);
  const std::optional<Outcome>& outcome() const { return outcome_; }
  bool terminal() const { return outcome_.has_value(); }
 private:
  enum class State { Created, Receiving, Draining, Terminal };
  struct Cursor {
    PartKind kind;
    PartHeader header;
    std::string bytes;
    std::optional<std::string> signature;
    bool sealed = false;
    std::optional<Part> value;
  };
  struct MessageCursor {
    Message message;
    bool sealed = false;
    std::shared_ptr<const NativeContext> native_context;
    // IDs, not cursor pointers, keep this index valid across accumulator copies/moves.
    std::map<uint64_t, uint32_t> parts_by_order;
  };
  bool apply(const Begin&); bool apply(const MessageBegin&); bool apply(const PartBegin&);
  bool apply(const PartDelta&); bool apply(const PartSeal&); bool apply(const MessageSeal&);
  bool apply(const UsageUpdate&); bool apply(const Stop&); bool apply(const Commit&); bool apply(const Fail&);
  bool reject(ErrorKind kind, std::string message);
  std::vector<Message> take_messages(bool partial);
  Part seal_value(Cursor&, bool partial);
  SemanticLimits limits_;
  Sink sink_;
  State state_ = State::Created;
  std::map<uint32_t, MessageCursor> messages_;
  std::map<uint32_t, Cursor> parts_;
  size_t content_bytes_ = 0;
  Usage usage_;
  std::optional<StopReason> stop_;
  const Error* failure_override_ = nullptr;
  std::optional<Outcome> outcome_;
};
} // namespace sp
