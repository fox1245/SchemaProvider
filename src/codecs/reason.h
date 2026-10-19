#pragma once
#include <span>
#include <string_view>

namespace sp::reason {
// Stable codes for the fixed failure strings the decoders raise (Error::reason). Each decoder and the
// accumulator has one table from its string to a code of lower-case dotted segments, "<source>.<subject>.<problem>".
// The code is picked by the string the decoder source contains, never by anything from the response.
enum class Source {
  Chat, Messages, Responses, Gemini, Interactions, Accumulator,
  // Request encoders, and the checks they share (policy choices, request controls, media, native replay).
  ChatRequest, MessagesRequest, ResponsesRequest, GeminiRequest, InteractionsRequest, SharedRequest,
  // Non-chat endpoints (D6): each decoder, then its request encoder.
  Images, ImagesRequest, Decisions, DecisionsRequest,
};
struct Entry { std::string_view message, code; };
std::span<const Entry> entries(Source) noexcept;
// The code for one of the source's failure strings; empty for a string without one (an abnormal close
// belongs to the transport, whose errors keep kind, http_status and vendor_code).
std::string_view code(Source, std::string_view message) noexcept;
// The code of an encoder refusal: the family encoder's table, then the shared table.
std::string_view request_code(Source family_encoder, std::string_view message) noexcept;
}  // namespace sp::reason
