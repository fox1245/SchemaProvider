#pragma once
#include "codecs/accumulator.h"
#include "core/request_controls.h"
#include <initializer_list>
#include <map>
#include <set>
#include <utility>

namespace sp::descriptor { class ValidatedDescriptor; }
namespace sp::json { class Value; }
namespace sp::chat {
struct InputMessage {
  Role role = Role::User;
  std::string text;
  std::vector<sp::ToolCall> tool_calls{};
  std::string tool_call_id{};
  // Typed Chat content is canonical: media in vector order, then nonempty text.
  // Generic Message-based APIs instead retain the caller's exact Part order.
  std::vector<sp::Media> media{};
};
struct ToolDefinition { std::string name, description; std::shared_ptr<const json::Document> parameters; };
struct ReasoningOptions {
  std::optional<std::string> effort;
  std::optional<uint64_t> max_tokens;
  std::optional<bool> exclude, enabled;
};
enum class OutputModality { Text, Audio, Image };
// OpenAI audio output parameters. `voice` is a preset name or a custom `voice_...` id; `format`
// is wav, aac, mp3, flac, opus or pcm16.
struct AudioOutput { std::string voice, format; };
// OpenRouter image_config. Every present field is sent as given.
using ImageOutput = sp::OpenRouterImageConfig;
struct Request {
  std::string model;
  std::vector<InputMessage> messages;
  std::vector<ToolDefinition> tools;
  std::optional<double> temperature, top_p;
  std::optional<uint64_t> max_output_tokens;
  std::optional<std::string> reasoning_effort{};
  std::optional<std::string> service_tier;
  // Generic conversation path preserves ordered Text/Media parts. Exactly one
  // of messages/canonical_messages is populated; unsupported native parts reject.
  std::vector<sp::Message> canonical_messages{};
  std::optional<sp::OpenRouterRouting> provider{};
  std::optional<sp::ResponseFormat> response_format{};
  std::optional<ReasoningOptions> reasoning{};
  std::optional<bool> include_reasoning{}, usage_include{};
  std::vector<std::string> models{};
  std::vector<sp::OpenRouterPlugin> plugins{};
  // Top-level members the policy admits for this origin (#32), sent as given after the typed ones.
  std::shared_ptr<const json::Document> extensions{};
  // Generated media. Audio output needs the Audio modality together with `audio`; image output
  // (modalities and image_config) is an OpenRouter extension.
  std::vector<OutputModality> modalities{};
  std::optional<AudioOutput> audio{};
  std::optional<ImageOutput> image_config{};

  static Request simple(std::string model_name, std::string user_prompt,
                        std::optional<uint64_t> max_tokens = std::nullopt,
                        std::string system_prompt = {}) {
    Request request;
    request.model = std::move(model_name);
    if (!system_prompt.empty()) {
      request.messages.push_back(InputMessage{Role::System, std::move(system_prompt)});
    }
    request.messages.push_back(InputMessage{Role::User, std::move(user_prompt)});
    request.max_output_tokens = max_tokens;
    return request;
  }
  static Request simple(std::string model_name, std::vector<sp::Message> history,
                        std::optional<uint64_t> max_tokens = std::nullopt,
                        std::string system_prompt = {}) {
    Request request;
    request.model = std::move(model_name);
    if (!system_prompt.empty()) {
      request.canonical_messages.push_back(sp::Message::system_text(std::move(system_prompt)));
    }
    for (auto& msg : history) request.canonical_messages.push_back(std::move(msg));
    request.max_output_tokens = max_tokens;
    return request;
  }
};
struct EncodedRequest {
  std::string method, path;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::shared_ptr<const NativeContext> context{};
  std::optional<std::uint64_t> max_output_tokens{};
  std::optional<std::uint64_t> model_invocation_limit{1};
  // What the encoder did with the cache marks of the text parts (canonical_messages).
  CacheMarkReport cache_marks{};
};
using EncodeResult = std::variant<EncodedRequest, Error>;
EncodeResult encode(const descriptor::ValidatedDescriptor&, const Request&, bool streaming);
enum class Mode { Buffered, Sse };
struct Close { bool normal = true; ErrorKind error = ErrorKind::Truncated; };
struct Diagnostics { size_t unknown_properties = 0; };
class Codec {
 public:
  // The validated descriptor and accumulator must outlive this codec.
  Codec(const descriptor::ValidatedDescriptor&, Mode, Accumulator&, SemanticLimits = {},
        std::shared_ptr<const NativeContext> = {});
  // Decodes the complete buffered body and finalizes using the observed close.
  bool buffered(std::string_view body, Close close);
  bool frame(std::string_view event, std::string_view data);
  void finish(Close close = {});
  const Diagnostics& diagnostics() const { return diagnostics_; }
 private:
  struct ToolBinding { LocalId local; std::string id, name; uint64_t order; };
  struct ReasoningBinding {
    json::Value original;
    std::map<std::string_view, json::Value> fields;
    std::string_view payload_key;
    std::string payload;
  };
  bool document(std::string_view, std::string_view event, bool streaming);
  bool item(json::Value, bool streaming);
  bool tool(json::Value, bool streaming, uint64_t position);
  bool usage(json::Value);
  bool emit(const Event&);
  bool fail(ErrorKind, std::string);
  void unknown(json::Value, std::initializer_list<std::string_view>);
  bool text(json::Value, PartKind, bool streaming);
  bool content(json::Value, bool streaming);
  bool structured_text(std::string_view, bool streaming);
  bool end_structured_text();
  bool annotations(json::Value);
  // URL citations of the answer's text, attached when the text part is sealed.
  std::vector<Citation> citations_;
  std::size_t citation_bytes_ = 0;
  bool reasoning_details(json::Value, bool streaming);
  bool emit_reasoning_details(json::Value);
  bool emit_reasoning_part(std::shared_ptr<const json::Document>);
  bool finish_reasoning_details();
  bool audio(json::Value, bool streaming);
  bool audio_chunk(std::string_view, bool streaming);
  bool images(json::Value);
  bool image(json::Value, bool indexed);
  bool finish_audio();
  uint64_t media_order();
  const descriptor::ValidatedDescriptor& descriptor_;
  Mode mode_;
  Accumulator& accumulator_;
  std::shared_ptr<const NativeContext> context_;
  SemanticLimits limits_;
  Diagnostics diagnostics_;
  bool begun_ = false, stopped_ = false, done_ = false, closed_ = false, body_seen_ = false;
  bool audio_usage_tail_ = false;
  std::optional<Error> buffered_failure_;
  std::string generation_, model_, finish_reason_;
  // The upstream a gateway named for this response (empty until it names one).
  std::string upstream_;
  uint64_t created_ = 0;  // Latest chunk created value; nondecreasing within a stream.
  std::optional<bool> indexed_tools_;
  uint32_t next_part_ = 1;
  std::optional<LocalId> text_, refusal_, thinking_;
  bool structured_content_ = false;
  // The open Text part of a streamed content array: consecutive text elements continue it.
  std::optional<LocalId> structured_run_;
  // A streamed reply's reasoning_details merge into these entries; their values alias the raw frames the accumulator keeps.
  bool reasoning_streamed_ = false;
  std::vector<ReasoningBinding> reasoning_bindings_;
  std::map<uint64_t, size_t> reasoning_indices_;
  std::map<uint64_t, size_t> indices_;
  std::vector<ToolBinding> tools_;
  // A generated audio reply accumulates here: streamed chunks are base64 of independent byte runs.
  struct AudioReply {
    bool seen = false, sealed = false;
    std::string id, transcript, payload, pending;
    std::optional<uint64_t> expires_at;
    uint64_t order = 0;
  } audio_;
  std::size_t media_parts_ = 0;
  // URL views alias immutable raw JSON documents already retained by the accumulator.
  std::map<uint64_t, std::string_view> image_indices_;
  size_t input_bytes_ = 0;
};
} // namespace sp::chat
