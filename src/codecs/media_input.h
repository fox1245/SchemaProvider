#pragma once
#include "core/value.h"
#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <optional>
#include <string>
#include <string_view>

// Shared by the five request encoders: policy and structural admission of user media, plus the
// few wire spellings that more than one family uses.
namespace sp::media_input {
// Decoded-byte ceiling configured for the kind.
std::size_t decoded_limit(const descriptor::CodecResources&, MediaKind) noexcept;
// Whether the descriptor's family and origin accept this media, and whether it is well formed.
// A kind or source form the family cannot express is Unsupported; a MIME type outside the admitted
// list or a malformed payload is InvalidRequest. Roles and ordering are the encoder's concern.
std::optional<Error> admit(const descriptor::ValidatedDescriptor&, const Media&);
// OpenAI-style `input_audio.format` short name for an admitted audio MIME type.
std::optional<std::string_view> audio_format(std::string_view mime) noexcept;
// The caller's name, or a MIME-derived one for providers that read a file extension.
std::string document_filename(const Media&);
// Writes `"data:<mime>;base64,<payload>"` including quotes; MIME and payload are validated ASCII.
void data_uri(json::BoundedWriter&, const Media&);
// A data URI for inline media, otherwise the quoted https URL (for `url` sources).
void uri_or_url(json::BoundedWriter&, const Media&);
bool valid_utf8(std::string_view) noexcept;
} // namespace sp::media_input
