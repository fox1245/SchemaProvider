#pragma once
#include "sp/config_defaults.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace sp {
enum class ImageDetail { Auto, Low, High, Original };
enum class MediaKind : std::uint8_t { Image, Audio, Video, Document };
enum class MediaSource : std::uint8_t { Inline, Url, File };
inline constexpr std::size_t media_kind_count = 4;

// One typed media content part: a user input or a model-generated output.
// Inline media owns canonical base64 behind shared immutable storage. Url media is an https
// location the provider fetches; File media is a provider-held file id or Files-API URI. The SDK
// never reads files, fetches URLs or decodes pixels/samples: admission is structural.
struct Media {
  MediaKind kind = MediaKind::Image;
  // type/subtype[;parameter=value], or an exact generateContent protocol alias. May be empty
  // only for Url/File sources whose provider derives the type itself (e.g. a YouTube location).
  std::string mime;
  MediaSource source = MediaSource::Inline;
  std::shared_ptr<const std::string> data;  // Inline: canonical base64
  std::string reference;                    // Url: https location; File: provider file id or URI
  std::string name;                         // input: file name or title; output: display name
  ImageDetail detail = ImageDetail::Auto;   // image inputs only
  std::string id;                           // output: provider-held identity (audio id, call id)
  std::string transcript;                   // output audio: the spoken text, when reported

  static Media inline_data(MediaKind kind, std::string mime, std::shared_ptr<const std::string> base64) {
    Media media; media.kind = kind; media.mime = std::move(mime); media.data = std::move(base64); return media;
  }
  static Media image(std::string mime, std::shared_ptr<const std::string> base64, ImageDetail detail = ImageDetail::Auto) {
    Media media = inline_data(MediaKind::Image, std::move(mime), std::move(base64)); media.detail = detail; return media;
  }
  static Media audio(std::string mime, std::shared_ptr<const std::string> base64) {
    return inline_data(MediaKind::Audio, std::move(mime), std::move(base64));
  }
  static Media video(std::string mime, std::shared_ptr<const std::string> base64) {
    return inline_data(MediaKind::Video, std::move(mime), std::move(base64));
  }
  static Media document(std::string mime, std::shared_ptr<const std::string> base64, std::string name = {}) {
    Media media = inline_data(MediaKind::Document, std::move(mime), std::move(base64)); media.name = std::move(name); return media;
  }
  static Media url(MediaKind kind, std::string location, std::string mime = {}) {
    Media media; media.kind = kind; media.mime = std::move(mime); media.source = MediaSource::Url;
    media.reference = std::move(location); return media;
  }
  static Media file(MediaKind kind, std::string provider_id, std::string mime = {}) {
    Media media; media.kind = kind; media.mime = std::move(mime); media.source = MediaSource::File;
    media.reference = std::move(provider_id); return media;
  }
};

std::string_view image_detail_name(ImageDetail) noexcept;
std::string_view media_kind_name(MediaKind) noexcept;
std::string_view media_source_name(MediaSource) noexcept;

// MIME text is `type/subtype` followed by optional `;name=value` token parameters. The only
// multi-slash exceptions are generateContent's documented video/audio/{s16le,wav} and
// video/text/timestamp aliases; family policy must explicitly admit them. Neither a quote,
// backslash, comma nor whitespace is admitted, so valid values are safe in JSON and `data:` URIs.
bool mime_syntax(std::string_view mime) noexcept;
// The `type/subtype` part before any parameter.
std::string_view mime_essence(std::string_view mime) noexcept;
// ASCII case-insensitive comparison of the essences only.
bool mime_equal(std::string_view a, std::string_view b) noexcept;
// True only for the three documented generateContent alias essences (case-insensitive).
bool mime_is_generate_alias(std::string_view mime) noexcept;
// Classifies by top-level type except generateContent's audio and timestamp aliases.
std::optional<MediaKind> mime_kind(std::string_view mime) noexcept;
// https location without whitespace, control characters, quotes or backslashes.
bool url_syntax(std::string_view url) noexcept;
// Provider-held file id or Files-API URI of printable ASCII without quotes or backslashes.
bool reference_syntax(std::string_view reference) noexcept;

// Canonical padded RFC 4648 base64 with zero trailing bits; the empty string is not valid.
bool canonical_base64(std::string_view) noexcept;
std::size_t base64_decoded_size(std::string_view canonical) noexcept;
std::string base64_encode(std::string_view bytes);
// Decodes canonical base64 into `out` (replacing its contents); false leaves `out` unspecified.
bool base64_decode(std::string_view canonical, std::string& out);

enum class MediaFault { None, Kind, Mime, Source, Base64, Size, Signature, Detail, Text };
// Structure of one Media independent of family policy: enumerations, MIME/kind consistency,
// source fields, canonical base64 and the decoded-size ceiling. Registered image signatures must
// agree with the declared MIME; audio, video and document admission does not pretend to decode
// containers or reject their legitimate alternative encodings.
MediaFault check_media(const Media&, std::size_t decoded_limit) noexcept;
const char* media_fault_name(MediaFault) noexcept;
// True when the essence has a registered signature and the payload matches it, or when no
// signature is registered. `canonical` must already satisfy canonical_base64.
bool media_signature_matches(std::string_view essence, std::string_view canonical) noexcept;
} // namespace sp
