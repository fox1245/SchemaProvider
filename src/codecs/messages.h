#pragma once
#include "codecs/accumulator.h"
#include <initializer_list>
#include <map>
#include <set>

namespace sp::descriptor { class ValidatedDescriptor; }
namespace sp::json { class Value; }
namespace sp::messages {
enum class Mode { Buffered, Sse };
struct Close { bool normal = true; ErrorKind error = ErrorKind::Truncated; };
struct Diagnostics { size_t unknown_properties = 0; };
class Codec {
 public:
  // Descriptor and accumulator must outlive this codec. Context comes from encode.
  Codec(const descriptor::ValidatedDescriptor&, Mode, Accumulator&,
        std::shared_ptr<const NativeContext>, SemanticLimits = {});
  bool buffered(std::string_view body, Close close);
  bool frame(std::string_view event, std::string_view data);
  void finish(Close close = {});
  const Diagnostics& diagnostics() const { return diagnostics_; }
 private:
  // A member of a streamed kept block, as raw JSON or, for a string, its text so far.
  struct KeptMember { std::string key, raw; std::optional<std::string> text; };
  struct Block {
    LocalId local;
    PartKind kind;
    bool closed = false, argument_bytes = false, initial_input = false, signature_started = false;
    std::vector<Citation> citations{};
    // A streamed kept block (`compaction`), whose `<type>_delta` events set its members.
    std::string kept_type{};
    std::shared_ptr<const json::Document> kept_start{};
    std::vector<KeptMember> kept{};
    bool kept_changed = false;
  };
  struct Call { std::string name; ToolCallKind kind; uint64_t index; };
  bool document(std::string_view, std::string_view event, bool streaming);
  bool message(json::Value, bool streaming);
  bool identity(json::Value, bool required);
  bool metadata(json::Value);
  bool block(json::Value, uint64_t, bool streaming);
  bool delta(json::Value, uint64_t);
  bool end_block(uint64_t);
  bool terminal(json::Value, bool initial = false);
  // `initial` is a message object (message_start or a buffered body); `placeholder_output` marks the streaming message_start.
  bool usage(json::Value, bool initial, bool placeholder_output = false);
  bool usage_leaves(json::Value, const std::string& prefix);
  json::Value usage_at(json::Value) const;
  bool counter(json::Value, std::optional<Count>&, std::string_view, bool nullable = false);
  void conflict(std::string_view, std::string_view);
  bool caller(json::Value);
  bool server_content(std::string_view, json::Value);
  bool all_blocks_closed();
  bool emit(const Event&);
  bool fail(ErrorKind, std::string);
  void unknown(json::Value, std::initializer_list<std::string_view>);
  std::shared_ptr<const json::Document> own(json::Value);
  const descriptor::ValidatedDescriptor& descriptor_;
  Mode mode_;
  Accumulator& accumulator_;
  std::shared_ptr<const NativeContext> context_;
  const bool context_valid_;
  SemanticLimits limits_;
  Diagnostics diagnostics_;
  bool begun_ = false, stopped_ = false, done_ = false, closed_ = false, body_seen_ = false, message_delta_ = false, sentinel_ = false;
  std::optional<Error> buffered_failure_;
  std::optional<StopReason> stop_;
  std::string generation_, model_;
  std::map<uint64_t, Block> blocks_;
  std::map<std::string, Call, std::less<>> calls_;
  std::set<std::string, std::less<>> results_;
  Usage usage_;
  // The output count of message_start: a placeholder, kept only to detect a later cumulative count below it.
  std::optional<uint64_t> started_output_;
  size_t input_bytes_ = 0;
};
} // namespace sp::messages
