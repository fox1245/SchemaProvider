#pragma once
#include "codecs/accumulator.h"
#include <initializer_list>
#include <map>
#include <utility>

namespace sp::descriptor { class ValidatedDescriptor; }
namespace sp::json { class Value; }
namespace sp::chat {
struct InputMessage {
  Role role = Role::User;
  std::string text;
  std::vector<sp::ToolCall> tool_calls{};
  std::string tool_call_id{};
};
struct ToolDefinition { std::string name, description; std::shared_ptr<const json::Document> parameters; };
struct Request {
  std::string model;
  std::vector<InputMessage> messages;
  std::vector<ToolDefinition> tools;
  std::optional<double> temperature, top_p;
  std::optional<uint64_t> max_output_tokens;
};
struct EncodedRequest { std::string method, path; std::vector<std::pair<std::string, std::string>> headers; std::string body; };
using EncodeResult = std::variant<EncodedRequest, Error>;
EncodeResult encode(const descriptor::ValidatedDescriptor&, const Request&, bool streaming);
enum class Mode { Buffered, Sse };
struct Close { bool normal = true; ErrorKind error = ErrorKind::Truncated; };
struct Diagnostics { size_t unknown_properties = 0; };
class Codec {
 public:
  // The validated descriptor and accumulator must outlive this codec.
  Codec(const descriptor::ValidatedDescriptor&, Mode, Accumulator&, SemanticLimits = {});
  // Decodes the complete buffered body and finalizes using the observed close.
  bool buffered(std::string_view body, Close close);
  bool frame(std::string_view event, std::string_view data);
  void finish(Close close = {});
  const Diagnostics& diagnostics() const { return diagnostics_; }
 private:
  struct ToolBinding { LocalId local; std::string id, name; uint64_t order; };
  bool document(std::string_view, bool streaming);
  bool item(json::Value, bool streaming);
  bool tool(json::Value, bool streaming, uint64_t position);
  bool usage(json::Value);
  bool emit(const Event&);
  bool fail(ErrorKind, std::string);
  void unknown(json::Value, std::initializer_list<std::string_view>);
  bool text(json::Value, PartKind, bool streaming);
  const descriptor::ValidatedDescriptor& descriptor_;
  Mode mode_;
  Accumulator& accumulator_;
  SemanticLimits limits_;
  Diagnostics diagnostics_;
  bool begun_ = false, stopped_ = false, done_ = false, closed_ = false, body_seen_ = false;
  std::optional<Error> buffered_failure_;
  std::string generation_, model_;
  uint64_t created_ = 0;
  std::optional<bool> indexed_tools_;
  uint32_t next_part_ = 1;
  std::optional<LocalId> text_, refusal_;
  std::map<uint64_t, size_t> indices_;
  std::vector<ToolBinding> tools_;
  size_t input_bytes_ = 0;
};
} // namespace sp::chat
