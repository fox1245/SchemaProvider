#include "codecs/media_input.h"

namespace sp::media_input {
namespace {
bool lower_equal(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    auto x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + ('a' - 'A'));
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + ('a' - 'A'));
    if (x != y) return false;
  }
  return true;
}
Error rejection(ErrorKind kind, std::string message) {
  Error error;
  error.kind = kind;
  error.safe_message = std::move(message);
  return error;
}
} // namespace

std::size_t decoded_limit(const descriptor::CodecResources& resources, MediaKind kind) noexcept {
  switch (kind) {
    case MediaKind::Image: return resources.image_decoded_bytes;
    case MediaKind::Audio: return resources.audio_decoded_bytes;
    case MediaKind::Video: return resources.video_decoded_bytes;
    case MediaKind::Document: return resources.document_decoded_bytes;
  }
  return 0;
}

std::optional<Error> admit(const descriptor::ValidatedDescriptor& descriptor, const Media& media) {
  if (static_cast<unsigned>(media.kind) >= media_kind_count || static_cast<unsigned>(media.source) > 2)
    return rejection(ErrorKind::InvalidRequest, "invalid media kind or source");
  // Policy may narrow provider support, but cannot create a source union the
  // encoder has no wire representation for (or make inline_audio dereference a URL).
  const auto family = descriptor.family();
  const bool unrepresentable =
      (family == "openai.chat" &&
       ((media.kind == MediaKind::Audio && media.source != MediaSource::Inline) ||
        ((media.kind == MediaKind::Image || media.kind == MediaKind::Video) && media.source == MediaSource::File))) ||
      (family == "openai.responses" &&
       ((media.kind == MediaKind::Audio && media.source != MediaSource::Inline) ||
        (media.kind == MediaKind::Video && media.source == MediaSource::File))) ||
      (family == "anthropic.messages" && (media.kind == MediaKind::Audio || media.kind == MediaKind::Video));
  if (unrepresentable) return rejection(ErrorKind::Unsupported, "this family cannot represent this media kind or source");
  const auto& policy = descriptor::media_policy(descriptor).kinds[static_cast<std::size_t>(media.kind)];
  const std::string kind(media_kind_name(media.kind));
  if (!policy.sources) return rejection(ErrorKind::Unsupported, "this family does not accept " + kind + " input");
  if (!(policy.sources & (1U << static_cast<unsigned>(media.source))))
    return rejection(ErrorKind::Unsupported, "this family does not accept " + kind + " input as " +
                     std::string(media_source_name(media.source)));
  if (!media.mime.empty()) {
    const auto essence = mime_essence(media.mime);
    bool listed = false;
    for (const auto& admitted : policy.mime) if (lower_equal(admitted, essence)) { listed = true; break; }
    if (!listed) return rejection(ErrorKind::InvalidRequest, "media MIME type is not admitted for this family");
  }
  const auto fault = check_media(media, decoded_limit(descriptor.policy()->resources(), media.kind));
  if (fault != MediaFault::None)
    return rejection(fault == MediaFault::Size ? ErrorKind::ResourceLimit : ErrorKind::InvalidRequest,
                     media_fault_name(fault));
  return std::nullopt;
}

std::optional<std::string_view> audio_format(std::string_view mime) noexcept {
  struct Format { std::string_view mime, name; };
  static constexpr Format formats[]{
    {"audio/wav", "wav"}, {"audio/mp3", "mp3"}, {"audio/mpeg", "mp3"}, {"audio/aiff", "aiff"},
    {"audio/aac", "aac"}, {"audio/ogg", "ogg"}, {"audio/flac", "flac"}, {"audio/m4a", "m4a"},
    {"audio/l16", "pcm16"}, {"audio/l24", "pcm24"}};
  for (const auto& format : formats) if (lower_equal(format.mime, mime_essence(mime))) return format.name;
  return std::nullopt;
}

std::string document_filename(const Media& media) {
  if (!media.name.empty()) return media.name;
  struct Extension { std::string_view mime, suffix; };
  static constexpr Extension extensions[]{
    {"application/pdf", ".pdf"}, {"text/plain", ".txt"}, {"text/markdown", ".md"}, {"text/html", ".html"},
    {"text/csv", ".csv"}, {"text/tsv", ".tsv"}, {"text/xml", ".xml"}, {"text/css", ".css"},
    {"text/javascript", ".js"}, {"application/javascript", ".js"}, {"application/json", ".json"},
    {"application/rtf", ".rtf"}, {"text/rtf", ".rtf"}, {"application/msword", ".doc"},
    {"application/vnd.openxmlformats-officedocument.wordprocessingml.document", ".docx"},
    {"application/vnd.ms-excel", ".xls"},
    {"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", ".xlsx"},
    {"application/vnd.ms-powerpoint", ".ppt"},
    {"application/vnd.openxmlformats-officedocument.presentationml.presentation", ".pptx"},
    {"application/vnd.oasis.opendocument.text", ".odt"}, {"text/x-python", ".py"}, {"text/yaml", ".yaml"},
    {"text/x-yaml", ".yaml"}, {"application/yaml", ".yaml"}};
  for (const auto& extension : extensions)
    if (lower_equal(extension.mime, mime_essence(media.mime))) return "document" + std::string(extension.suffix);
  return "document";
}

void data_uri(json::BoundedWriter& writer, const Media& media) {
  writer.raw("\"data:").raw(media.mime).raw(";base64,").raw(*media.data).raw("\"");
}
void uri_or_url(json::BoundedWriter& writer, const Media& media) {
  if (media.source == MediaSource::Inline) data_uri(writer, media);
  else writer.quoted(media.reference);
}

bool valid_utf8(std::string_view text) noexcept {
  std::size_t i = 0;
  while (i < text.size()) {
    const auto c = static_cast<unsigned char>(text[i]);
    std::size_t length = 0;
    std::uint32_t point = 0;
    if (c < 0x80) { ++i; continue; }
    if ((c & 0xE0) == 0xC0) { length = 2; point = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { length = 3; point = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { length = 4; point = c & 0x07; }
    else return false;
    if (i + length > text.size()) return false;
    for (std::size_t k = 1; k < length; ++k) {
      const auto next = static_cast<unsigned char>(text[i + k]);
      if ((next & 0xC0) != 0x80) return false;
      point = (point << 6) | (next & 0x3F);
    }
    if ((length == 2 && point < 0x80) || (length == 3 && point < 0x800) || (length == 4 && point < 0x10000) ||
        point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF)) return false;
    i += length;
  }
  return true;
}
} // namespace sp::media_input
