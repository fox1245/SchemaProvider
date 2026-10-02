#pragma once
#include "codecs/accumulator.h"
#include <initializer_list>
#include <set>

namespace sp::descriptor { class ValidatedDescriptor; }
namespace sp::json { class Value; }
namespace sp::gemini {
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
  bool document(std::string_view);
  bool identity(json::Value);
  bool candidate(json::Value);
  bool part(json::Value);
  bool usage(json::Value);
  bool emit(const Event&);
  bool fail(ErrorKind, std::string);
  void unknown(json::Value, std::initializer_list<std::string_view>);
  std::shared_ptr<const json::Document> own(json::Value);
  const descriptor::ValidatedDescriptor& descriptor_;
  Mode mode_;
  Accumulator& accumulator_;
  std::shared_ptr<const NativeContext> context_;
  SemanticLimits limits_;
  Diagnostics diagnostics_;
  bool begun_ = false, stopped_ = false, closed_ = false, body_seen_ = false, has_call_ = false;
  std::optional<Error> buffered_failure_;
  std::string generation_, model_;
  std::optional<StopReason> stop_;
  std::vector<std::shared_ptr<const json::Document>> wire_parts_;
  std::vector<LocalId> pending_seals_;
  std::set<std::string, std::less<>> calls_;
  Usage usage_;
  size_t input_bytes_ = 0;
  uint64_t ownership_generation_ = 0;
};
} // namespace sp::gemini
