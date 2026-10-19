#pragma once
#include "core/value.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// Shared by the response decoders: how model-generated media enters the accumulator. A generated
// part is begun with a header-only Media description and sealed with its canonical base64 payload
// as the snapshot, so the accumulator validates and owns the bytes exactly once.
namespace sp::media_output {
// `data:<mime>[;parameter=value]*;base64,<payload>`. Both views alias `uri`; the MIME keeps its
// parameters and the payload is not validated here.
bool parse_data_uri(std::string_view uri, std::string_view& mime, std::string_view& payload) noexcept;
// image/*, audio/* and video/* by top-level type; everything else, including an empty type, is a
// document. Generated output is described, not admitted, so no MIME list applies.
MediaKind kind_of(std::string_view mime) noexcept;
// A payload-free description of generated media.
Media describe(std::string_view mime, MediaSource source, std::string_view reference = {});
// Concatenates independently padded audio chunks without allocating a decoded payload.
// Both arguments must be canonical base64 (or `joined` may be empty).
void append_audio(std::string& joined, std::string_view chunk);
// An append-only canonical stream of media deltas. `chunk` is canonical base64; `carry` keeps the 0 to 2
// decoded bytes that do not fill a 3-byte group yet. The return value encodes every complete group, so the
// concatenation of the returned strings and finish_stream(carry) is the canonical base64 of all the bytes.
std::string stream_chunk(std::string& carry, std::string_view chunk);
std::string finish_stream(std::string& carry);

// Emits one complete generated media part through the decoder's event function. `payload` is the
// canonical base64 of an inline media and must be empty for url or file sources.
template <class Emit>
bool emit_part(Emit&& emit, LocalId message, LocalId part, std::uint64_t order, Media media, std::string_view payload) {
  PartHeader header;
  header.wire_type = "media";
  header.media = std::move(media);
  if (!emit(PartBegin{message, part, PartKind::Media, std::move(header), order})) return false;
  return emit(PartSeal{part, payload.empty() ? std::optional<std::string_view>{} : std::optional<std::string_view>{payload}, {}});
}
} // namespace sp::media_output
