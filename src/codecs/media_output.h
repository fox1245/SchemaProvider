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
