#pragma once
#include "codecs/accumulator.h"
#include <initializer_list>
#include <map>
#include <set>

namespace sp::descriptor { class ValidatedDescriptor; }
namespace sp::json { class Value; }
namespace sp::interactions {
enum class Mode { Buffered, Sse };
struct Close { bool normal = true; ErrorKind error = ErrorKind::Truncated; };
struct Diagnostics { size_t unknown_properties = 0; };
class Codec {
 public:
  Codec(const descriptor::ValidatedDescriptor&, Mode, Accumulator&,
        std::shared_ptr<const NativeContext>, SemanticLimits = {});
  bool buffered(std::string_view, Close);
  bool frame(std::string_view event, std::string_view data);
  void finish(Close = {});
  const Diagnostics& diagnostics() const { return diagnostics_; }
 private:
  struct Step {
    std::shared_ptr<const json::Document> original;
    std::string type, arguments;
    std::vector<std::string> texts;
    std::vector<std::shared_ptr<const json::Document>> summary;
    std::optional<std::string> signature;
    std::vector<LocalId> parts;
    bool closed = false, signature_delta = false, argument_delta = false;
  };
  bool document(std::string_view, std::string_view, bool);
  bool identity(json::Value, bool);
  bool start(json::Value, uint64_t);
  bool delta(json::Value, uint64_t);
  bool stop(uint64_t);
  bool terminal(json::Value, bool);
  bool reconcile(Step&, json::Value);
  bool seal(Step&, json::Value);
  bool usage(json::Value, bool);
  bool leaves(json::Value, std::string);
  bool count(json::Value, std::optional<Count>&, std::string_view);
  void conflict(std::string_view, std::string_view);
  bool emit(const Event&);
  bool fail(ErrorKind, std::string);
  void unknown(json::Value, std::initializer_list<std::string_view>);
  std::shared_ptr<const json::Document> own(json::Value);
  std::shared_ptr<const json::Document> assembled(const Step&);
  const descriptor::ValidatedDescriptor& descriptor_;
  Mode mode_;
  Accumulator& accumulator_;
  std::shared_ptr<const NativeContext> context_;
  SemanticLimits limits_;
  Diagnostics diagnostics_;
  std::map<uint64_t, Step> steps_;
  std::set<std::string, std::less<>> calls_;
  std::string generation_;
  Usage usage_;
  std::optional<Error> buffered_failure_;
  bool begun_ = false, terminal_ = false, closed_ = false, body_seen_ = false, done_ = false;
  uint32_t next_part_ = 0;
  size_t input_bytes_ = 0;
};
} // namespace sp::interactions
