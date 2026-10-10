#pragma once
#include "codecs/accumulator.h"
#include "json/json.h"
#include <initializer_list>
#include <map>
#include <memory>
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
  // One content item of a model_output step: text, or image, audio, video or document media.
  // Payload is canonical base64, including concatenated independently padded audio chunks.
  struct Content {
    std::string type, text, mime, uri;
    std::map<std::string_view, json::Value> fields;
    // Annotation values alias retained raw observations, never consumer-visible prose.
    std::vector<json::Value> annotations;
    bool annotations_present = false;
    // Views alias immutable raw wire retained by the accumulator; only multi-chunk audio owns a run.
    std::string_view payload;
    std::unique_ptr<std::string> audio_run;
  };
  struct Step {
    std::shared_ptr<const json::Document> original;
    std::string type, arguments;
    std::vector<Content> content;
    std::vector<std::shared_ptr<const json::Document>> summary;
    std::optional<std::string> signature;
    std::vector<LocalId> parts;
    bool closed = false, signature_delta = false, argument_delta = false;
  };
  bool begin(Step&, uint64_t, PartKind, PartHeader, uint64_t);
  bool media_item(Step&, uint64_t, json::Value, bool);
  bool document(std::string_view, std::string_view, bool);
  bool identity(json::Value, bool);
  bool start(json::Value, uint64_t);
  bool delta(json::Value, uint64_t);
  bool thought_content(Step&, json::Value);
  bool annotations(json::Value, std::string_view);
  bool annotate(Content&, json::Value);
  bool stop(uint64_t);
  bool terminal(json::Value, bool);
  bool reconcile(Step&, json::Value);
  bool seal(Step&, json::Value, uint64_t);
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
