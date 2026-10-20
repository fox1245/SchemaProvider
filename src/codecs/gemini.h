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
  bool document(std::string_view, std::string_view event = {});
  bool identity(json::Value);
  bool candidate(json::Value);
  bool part(json::Value);
  bool usage(json::Value);
  bool emit(const Event&);
  // `vendor_code` is kept only when it has the shape of Google's UPPER_SNAKE reasons.
  bool fail(ErrorKind, std::string, std::string_view vendor_code = {});
  void unknown(json::Value, std::initializer_list<std::string_view>);
  // The id of a call or result the service sent without one: "gemini-local-<token>-<part>". The token is random and
  // drawn per response, so an id saved by one process is not minted again by another, nor by a forked worker.
  bool local_id(std::string&, LocalId);
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
  // The open run: consecutive wire parts that continue one another are one typed part and one retained wire part. A run
  // is a streamed answer, thought or transcript, which grows by each chunk's text, or the inline audio frames of one
  // concatenable MIME (raw PCM, MP3), each a delta of an append-only canonical base64 stream. The wire part has a slot
  // in `wire_parts_` from the first chunk, and is filled when the run closes: a different part, the finish reason, or the
  // end of the stream.
  struct Run {
    bool open = false;
    PartKind kind = PartKind::Text;
    LocalId part{0};
    std::size_t slot = 0, chunks = 0;
    std::shared_ptr<const json::Document> head;
    std::string outer, inner, data, carry;
  };
  Run run_;
  bool close_run();
  std::vector<LocalId> pending_seals_;
  std::set<std::string, std::less<>> calls_;
  // Service-run calls whose result has not arrived: the result's data member, then the call id.
  std::vector<std::pair<std::string, std::string>> open_server_calls_;
  Usage usage_;
  size_t input_bytes_ = 0;
  // The random part of this response's local call ids, drawn on first use.
  std::string ownership_token_;
};
} // namespace sp::gemini
