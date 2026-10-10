#pragma once
#include "core/value.h"
#include <functional>
#include <map>
#include <utility>

namespace sp {
class Accumulator {
 public:
  // Events and delta/snapshot views are borrowed for this invocation only.
  using Sink = std::function<void(const Event&)>;
  explicit Accumulator(SemanticLimits limits = {}, Sink sink = {},
                       std::size_t source_bytes_limit = config_defaults::defaults_max_response_bytes);
  // A known transport failure takes precedence over errors in deferred decoding.
  // The sink observes normalized semantic Stops, including invalid client-call
  // intent; specific vendor terminal reasons and server-executed calls survive.
  bool accept(const Event& event, const Error* failure_override = nullptr);
  const std::optional<Outcome>& outcome() const { return outcome_; }
  // Terminal ownership transfer; discard the accumulator after taking its outcome.
  std::optional<Outcome> take_outcome() { return std::move(outcome_); }
  bool terminal() const { return outcome_.has_value(); }
 private:
  enum class State { Created, Receiving, Draining, Terminal };
  struct CursorHeader {
    std::string wire_id, name;
    ToolCallKind tool_kind = ToolCallKind::ClientExecuted;
    std::string wire_type;
    std::shared_ptr<const json::Document> wire_metadata;
  };
  struct Cursor {
    PartKind kind;
    CursorHeader header;
    std::string bytes;
    std::optional<std::string> signature;
    bool google_part = false;
    bool sealed = false;
    // Media holds its owned description here while open; only sealed values carry payload.
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
  bool apply(const RawWire&); bool apply(const ResponseEnvelope&);
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
  std::shared_ptr<const json::Document> wire_envelope_;
  std::vector<RawWire> raw_events_;
  std::size_t raw_bytes_ = 0, raw_bytes_limit_ = 0;
  const Error* failure_override_ = nullptr;
  std::optional<Outcome> outcome_;
};
} // namespace sp
