#pragma once
#include "codecs/accumulator.h"
#include "json/json.h"
#include <initializer_list>
#include <map>
#include <set>

namespace sp::descriptor { class ValidatedDescriptor; }
namespace sp::responses {
enum class Mode { Buffered, Sse };
struct Close { bool normal = true; ErrorKind error = ErrorKind::Truncated; };
struct Diagnostics { size_t unknown_properties = 0; };
class Codec {
 public:
  // The descriptor and accumulator outlive the codec; encode supplies context.
  Codec(const descriptor::ValidatedDescriptor&, Mode, Accumulator&,
        std::shared_ptr<const NativeContext>, SemanticLimits = {});
  bool buffered(std::string_view, Close);
  bool frame(std::string_view event, std::string_view data);
  void finish(Close = {});
  const Diagnostics& diagnostics() const { return diagnostics_; }
 private:
  struct TextState {
    LocalId local{};
    PartKind kind = PartKind::Text;
    std::string type, bytes;
    bool text_done = false, part_done = false;
    std::shared_ptr<const json::Document> added, done;
    std::map<uint64_t, std::shared_ptr<const json::Document>> annotations;
    std::vector<std::shared_ptr<const json::Document>> logprob_deltas;
    std::shared_ptr<const json::Document> logprobs_done;
  };
  struct Item {
    std::string id, type, call_id, name;
    LocalId local{};
    uint64_t index = 0;
    size_t summary_emitted = 0;
    bool closed = false, arguments_done = false;
    std::string arguments;
    std::shared_ptr<const json::Document> added, done;
    std::map<uint64_t, TextState> content, summary;
  };
  bool document(std::string_view, std::string_view, bool);
  bool response(json::Value, std::string_view, bool);
  bool identity(json::Value);
  bool add_item(json::Value, uint64_t, bool);
  bool done_item(json::Value, uint64_t, bool);
  bool item_fields(json::Value, bool);
  bool part(Item&, uint64_t, json::Value, bool, bool);
  bool text_event(Item&, json::Value, std::string_view);
  bool emit_summary(Item&);
  bool logprobs(TextState&, json::Value, bool);
  bool reconcile(TextState&, std::string_view);
  bool reconcile_parts(Item&, json::Value, bool, bool);
  bool initial_metadata(json::Value, json::Value, std::initializer_list<std::string_view>);
  bool begin_part(LocalId&, PartKind, PartHeader, uint64_t);
  Item* event_item(json::Value, uint64_t&);
  bool index(json::Value, std::string_view, uint64_t&);
  json::Value usage_at(json::Value) const;
  bool usage(json::Value);
  bool usage_leaves(json::Value, const std::string&);
  bool counter(json::Value, std::optional<Count>&, std::string_view);
  void conflict(std::string_view, std::string_view);
  bool emit(const Event&);
  bool fail(ErrorKind, std::string);
  void unknown(json::Value, std::initializer_list<std::string_view>);
  std::shared_ptr<const json::Document> own(json::Value);
  std::shared_ptr<const json::Document> own_completed_output();
  const descriptor::ValidatedDescriptor& descriptor_;
  Mode mode_;
  Accumulator& accumulator_;
  std::shared_ptr<const NativeContext> context_;
  const bool context_valid_;
  SemanticLimits limits_;
  Diagnostics diagnostics_;
  bool begun_ = false, terminal_ = false, closed_ = false, body_seen_ = false;
  bool function_seen_ = false, refusal_seen_ = false;
  std::optional<Error> buffered_failure_;
  std::optional<uint64_t> sequence_;
  std::string generation_, model_, status_;
  double created_at_ = 0;
  std::map<uint64_t, Item> items_;
  std::set<std::string, std::less<>> ids_, call_ids_;
  uint32_t next_part_ = 1;
  size_t input_bytes_ = 0, retained_bytes_ = 0;
  Usage usage_;
};
} // namespace sp::responses
