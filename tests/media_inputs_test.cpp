#include "codecs/chat.h"
#include "codecs/gemini.h"
#include "codecs/gemini_request.h"
#include "codecs/interactions.h"
#include "codecs/interactions_request.h"
#include "codecs/messages.h"
#include "codecs/messages_request.h"
#include "codecs/responses.h"
#include "codecs/responses_request.h"
#include "support/vision_fixture.h"
#include "core/media.h"
#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <array>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace sp;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)
#define CHECK_AT(condition, context) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition " [" + std::string(context) + "]"); } while (false)

// ---------------------------------------------------------------------------------------------
// Independent byte/base64 helpers. The suite never relies on the SDK's own base64 to build input.
// ---------------------------------------------------------------------------------------------
constexpr std::string_view b64_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string b64(std::string_view bytes) {
  std::string out;
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    const unsigned a = static_cast<unsigned char>(bytes[i]);
    const unsigned b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0U;
    const unsigned c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0U;
    const unsigned n = (a << 16) | (b << 8) | c;
    out.push_back(b64_alphabet[(n >> 18) & 63]);
    out.push_back(b64_alphabet[(n >> 12) & 63]);
    out.push_back(i + 1 < bytes.size() ? b64_alphabet[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < bytes.size() ? b64_alphabet[n & 63] : '=');
  }
  return out;
}
std::string bytes_of(std::initializer_list<int> values) {
  std::string out;
  for (const int v : values) out.push_back(static_cast<char>(v));
  return out;
}
std::string be32(std::uint32_t v) { return bytes_of({int(v >> 24), int((v >> 16) & 255), int((v >> 8) & 255), int(v & 255)}); }
// ISO base media file: ftyp box with a major brand and compatible brands.
std::string ftyp(std::string_view major, std::initializer_list<std::string_view> compatible = {}) {
  std::string body = "ftyp" + std::string(major) + std::string(4, '\0');
  for (const auto brand : compatible) body += std::string(brand);
  return be32(static_cast<std::uint32_t>(body.size() + 4)) + body + std::string(8, '\0');
}
std::string lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
  return out;
}
// Structural fixtures, not pixel/sample decoders. Vision fixtures already provide real PNGs.
std::string sample_bytes(std::string_view mime) {
  if (mime_equal(mime, "image/png")) return bytes_of({0x89, 'P', 'N', 'G', 13, 10, 26, 10});
  if (mime_equal(mime, "image/heic")) return ftyp("heic", {"mif1"});
  if (mime_equal(mime, "image/heif")) return ftyp("mif1", {"heix"});
  if (mime_equal(mime, "audio/wav")) return "RIFF" + be32(36) + "WAVEfmt ";
  if (mime_equal(mime, "audio/mpeg")) return "ID3" + std::string(7, '\0');
  if (mime_equal(mime, "audio/m4a")) return ftyp("M4A ", {"mp42", "isom"});
  if (mime_equal(mime, "video/mp4")) return ftyp("iso6", {"mp42"});
  if (mime_equal(mime, "application/pdf")) return "%PDF-1.7\n";
  if (mime_equal(mime, "application/vnd.openxmlformats-officedocument.wordprocessingml.document"))
    return "PK" + bytes_of({3, 4, 20, 0, 0, 0});
  return "plain text\n";
}
std::shared_ptr<const std::string> shared(std::string text) { return std::make_shared<const std::string>(std::move(text)); }
MediaKind kind_for(std::string_view mime) {
  const auto top = lower(mime.substr(0, mime.find('/')));
  if (top == "image") return MediaKind::Image;
  if (top == "audio") return MediaKind::Audio;
  if (top == "video") return MediaKind::Video;
  return MediaKind::Document;
}
Media inline_media(MediaKind kind, std::string mime, std::string raw = {}) {
  if (raw.empty()) raw = sample_bytes(mime);
  return Media::inline_data(kind, std::move(mime), shared(b64(raw)));
}
Media inline_media(std::string mime) { const auto kind = kind_for(mime); return inline_media(kind, std::move(mime)); }

// ---------------------------------------------------------------------------------------------
// Families, origins and descriptors
// ---------------------------------------------------------------------------------------------
enum class Fam { Chat, ChatCanonical, Responses, Messages, Gemini, Interactions };
enum class Origin { Direct, OpenRouter };
constexpr std::array<Fam, 6> all_families{Fam::Chat, Fam::ChatCanonical, Fam::Responses, Fam::Messages, Fam::Gemini, Fam::Interactions};
const char* fam_name(Fam f) {
  switch (f) {
    case Fam::Chat: return "chat";
    case Fam::ChatCanonical: return "chat-canonical";
    case Fam::Responses: return "responses";
    case Fam::Messages: return "messages";
    case Fam::Gemini: return "gemini";
    case Fam::Interactions: return "interactions";
  }
  return "?";
}
std::string descriptor_text(Fam f, std::string_view base_url) {
  const std::string head = R"({"descriptor_version":1,"revision":1,"id":"media-inputs","family":")";
  auto connection = [&](std::string_view buffered, std::string_view streaming) {
    return std::string(R"(","connection":{"base_url":")") + std::string(base_url) + R"(","paths":{"buffered":")" + std::string(buffered) +
           R"(","streaming":")" + std::string(streaming) + R"("}})";
  };
  const bool gateway = base_url.find("openrouter") != std::string_view::npos;
  switch (f) {
    case Fam::Chat:
    case Fam::ChatCanonical:
      return head + "openai.chat" + connection(gateway ? "/api/v1/chat/completions" : "/v1/chat/completions", gateway ? "/api/v1/chat/completions" : "/v1/chat/completions") + "}";
    case Fam::Responses:
      return head + "openai.responses" + connection(gateway ? "/api/v1/responses" : "/v1/responses", gateway ? "/api/v1/responses" : "/v1/responses") +
             R"(,"bindings":{"model":"model","messages":"input","stream":"stream","max_output_tokens":"max_output_tokens","usage":["usage"]}})";
    case Fam::Messages:
      return head + "anthropic.messages" + connection(gateway ? "/api/v1/messages" : "/v1/messages", gateway ? "/api/v1/messages" : "/v1/messages") + "}";
    case Fam::Gemini:
      return head + "google.generate" + connection("/v1beta/models/fixture-model:generateContent", "/v1beta/models/fixture-model:streamGenerateContent?alt=sse") +
             R"(,"bindings":{"model":"model","messages":"contents","stream":"stream","max_output_tokens":"maxOutputTokens","usage":["usageMetadata"]}})";
    case Fam::Interactions:
      return head + "google.interactions" + connection("/v1beta/interactions", "/v1beta/interactions") +
             R"(,"bindings":{"model":"model","messages":"input","stream":"stream","max_output_tokens":"max_output_tokens","usage":["usage"]}})";
  }
  return {};
}
std::string_view default_origin(Fam f, Origin o) {
  if (o == Origin::OpenRouter) return "https://openrouter.ai";
  switch (f) {
    case Fam::Chat:
    case Fam::ChatCanonical:
    case Fam::Responses: return "https://api.openai.com";
    case Fam::Messages: return "https://api.anthropic.com";
    case Fam::Gemini:
    case Fam::Interactions: return "https://generativelanguage.googleapis.com";
  }
  return {};
}
const descriptor::ValidatedDescriptor& load_descriptor(Fam f, std::string_view base_url, const descriptor::PolicySnapshot& policy) {
  // Cached for the process: a descriptor owns its policy snapshot.
  static std::map<std::string, std::unique_ptr<descriptor::ValidatedDescriptor>> cache;
  const std::string key = std::string(fam_name(f)) + "|" + std::string(base_url) + "|" + (policy ? std::string(policy->identity()) : std::string());
  auto found = cache.find(key);
  if (found != cache.end()) return *found->second;
  auto loaded = policy ? descriptor::load(descriptor_text(f, base_url), policy) : descriptor::load(descriptor_text(f, base_url));
  if (!std::holds_alternative<descriptor::ValidatedDescriptor>(loaded)) throw std::runtime_error("descriptor did not load: " + key);
  return *cache.emplace(key, std::make_unique<descriptor::ValidatedDescriptor>(std::get<descriptor::ValidatedDescriptor>(std::move(loaded)))).first->second;
}
const descriptor::ValidatedDescriptor& load_descriptor(Fam f, std::string_view base_url) {
  static const descriptor::PolicySnapshot embedded;
  return load_descriptor(f, base_url, embedded);
}
const descriptor::ValidatedDescriptor& desc(Fam f, Origin o = Origin::Direct) { return load_descriptor(f, default_origin(f, o)); }

// ---------------------------------------------------------------------------------------------
// One uniform encode entry point
// ---------------------------------------------------------------------------------------------
struct Wire {
  std::string body, path;
  std::vector<std::pair<std::string, std::string>> headers;
  std::shared_ptr<json::Document> doc;
  json::Value root() const { return doc->root(); }
};
using Outcome = std::variant<Wire, Error>;
std::shared_ptr<json::Document> parse_json(std::string_view text) {
  auto parsed = json::parse(text, {96U << 20, 64});
  if (!std::holds_alternative<json::Document>(parsed)) throw std::runtime_error("encoder produced invalid JSON: " + std::string(text.substr(0, 200)));
  return std::make_shared<json::Document>(std::get<json::Document>(std::move(parsed)));
}
template <class Encoded> Outcome finish(std::variant<Encoded, Error> result) {
  if (auto* error = std::get_if<Error>(&result)) return std::move(*error);
  auto& encoded = std::get<Encoded>(result);
  Wire wire{std::move(encoded.body), std::move(encoded.path), std::move(encoded.headers), nullptr};
  wire.doc = parse_json(wire.body);
  return wire;
}
Outcome run_descriptor(Fam f, const descriptor::ValidatedDescriptor& d, std::vector<Part> parts, bool streaming, Role role = Role::User) {
  switch (f) {
    case Fam::Chat: {
      chat::Request r; r.model = "fixture-model";
      chat::InputMessage m; m.role = role;
      for (auto& part : parts) {
        if (auto* media = std::get_if<Media>(&part)) m.media.push_back(std::move(*media));
        else if (auto* text = std::get_if<Text>(&part)) { if (!m.text.empty()) throw std::runtime_error("legacy Chat carries one text"); m.text = text->value; }
      }
      r.messages.push_back(std::move(m));
      return finish(chat::encode(d, r, streaming));
    }
    case Fam::ChatCanonical: {
      chat::Request r; r.model = "fixture-model";
      r.canonical_messages.push_back(Message{{}, role, std::move(parts)});
      return finish(chat::encode(d, r, streaming));
    }
    case Fam::Responses: {
      responses::Request r; r.model = "fixture-model"; r.account_scope = "scope";
      r.messages.push_back(Message{{}, role, std::move(parts)});
      return finish(responses::encode(d, r, streaming));
    }
    case Fam::Messages: {
      messages::Request r; r.model = "fixture-model"; r.account_scope = "scope";
      r.messages.push_back(Message{{}, role, std::move(parts)});
      return finish(messages::encode(d, r, streaming));
    }
    case Fam::Gemini: {
      gemini::Request r; r.model = "fixture-model"; r.account_scope = "scope";
      r.messages.push_back(Message{{}, role, std::move(parts)});
      return finish(gemini::encode(d, r, streaming));
    }
    case Fam::Interactions: {
      interactions::Request r; r.model = "fixture-model"; r.account_scope = "scope";
      r.messages.push_back(Message{{}, role, std::move(parts)});
      return finish(interactions::encode(d, r, streaming));
    }
  }
  throw std::runtime_error("unreachable");
}
Outcome run(Fam f, Origin o, std::vector<Part> parts, bool streaming, Role role = Role::User) {
  return run_descriptor(f, desc(f, o), std::move(parts), streaming, role);
}
Outcome run1(Fam f, Origin o, const Media& media, bool streaming) { return run(f, o, {media}, streaming); }
bool is_wire(const Outcome& outcome) { return std::holds_alternative<Wire>(outcome); }
const Wire& wire_of(const Outcome& outcome) { return std::get<Wire>(outcome); }

// The ordered content parts of the first message, in each family's wire shape.
json::Value content_of(Fam f, json::Value root) {
  switch (f) {
    case Fam::Chat:
    case Fam::ChatCanonical: return root.get("messages").at(0).get("content");
    case Fam::Responses: return root.get("input").at(0).get("content");
    case Fam::Messages: return root.get("messages").at(0).get("content");
    case Fam::Gemini: return root.get("contents").at(0).get("parts");
    case Fam::Interactions: return root.get("input").at(0).get("content");
  }
  return {};
}
// Compare exact official content shapes structurally; object-member ordering is not material.
bool json_matches(json::Value actual, std::string expected) {
  auto doc = json::parse(expected, {16U << 20, 64});
  if (!std::holds_alternative<json::Document>(doc)) throw std::runtime_error("bad expectation JSON: " + expected.substr(0, 300));
  return json::equal(actual, std::get<json::Document>(doc).root());
}

void expect_error(Outcome result, ErrorKind kind, int line = __builtin_LINE()) {
  if (!std::holds_alternative<Error>(result) || std::get<Error>(result).kind != kind)
    throw std::runtime_error("line " + std::to_string(line) + ": expected error " + std::to_string(static_cast<int>(kind)) +
        (std::holds_alternative<Error>(result) ? ", got " + std::to_string(static_cast<int>(std::get<Error>(result).kind)) +
         ": " + std::get<Error>(result).safe_message : ", got encoded request"));
}
void expect_part(Fam family, Origin origin, const Media& media, std::string expected) {
  for (const bool streaming : {false, true}) {
    const auto result = run1(family, origin, media, streaming);
    CHECK_AT(is_wire(result), fam_name(family));
    const auto content = content_of(family, wire_of(result).root());
    CHECK(content.is_array() && content.size() == 1);
    CHECK_AT(json_matches(content.at(0), expected), fam_name(family));
  }
}
std::string data_url(const Media& media) { return "data:" + media.mime + ";base64," + *media.data; }
std::string google_part(Fam family, const Media& media) {
  if (family == Fam::Gemini) {
    if (media.source == MediaSource::Inline)
      return "{\"inlineData\":{\"mimeType\":" + json::quote(media.mime) + ",\"data\":" + json::quote(*media.data) + "}}";
    return "{\"fileData\":{" + (media.mime.empty() ? std::string() : "\"mimeType\":" + json::quote(media.mime) + ",") +
        "\"fileUri\":" + json::quote(media.reference) + "}}";
  }
  return "{\"type\":" + json::quote(media_kind_name(media.kind)) +
      (media.mime.empty() ? std::string() : ",\"mime_type\":" + json::quote(lower(mime_essence(media.mime)))) +
      (media.source == MediaSource::Inline ? ",\"data\":" + json::quote(*media.data) : ",\"uri\":" + json::quote(media.reference)) + "}";
}

// Official image guides admit both HEIC and HEIF; Nokia's HEIF technical specification
// identifies mif1/msf1 structural brands and HEVC profile brands:
// https://ai.google.dev/gemini-api/docs/generate-content/image-understanding
// https://ai.google.dev/api/interactions-api
// https://nokiatech.github.io/heif/technical.html
// https://mp4ra.org/registered-types/brands (mif2 structural CICP/alpha/depth brand)
void heif_admission_and_brand_boundaries() {
  std::string late_body = "isom" + std::string(4, '\0');
  for (int i = 0; i < 20; ++i) late_body += "iso6";
  late_body += "mif1";
  const auto late = be32(static_cast<std::uint32_t>(late_body.size() + 8)) + "ftyp" + late_body;
  const auto extended = be32(1) + "ftyp" + be32(0) + be32(28) + "heix" + be32(0) + "mif1";
  const std::array<std::string, 6> containers{
      ftyp("heic", {"mif1"}), ftyp("heix", {"mif1"}), ftyp("mif1", {"heic"}), ftyp("mif2"), late, extended};
  for (const auto family : {Fam::Gemini, Fam::Interactions}) {
    for (const auto mime : {"image/heic", "image/heif"}) {
      for (const auto& bytes : containers) {
        const auto media = inline_media(MediaKind::Image, mime, bytes);
        expect_part(family, Origin::Direct, media, google_part(family, media));
      }
      // Neither an unrelated MP4 brand nor a truncated/undersized ftyp is an image.
      for (const auto& bytes : {ftyp("isom", {"mp42"}), be32(12) + "ftypheic", be32(128) + "ftypheic" + be32(0)}) {
        const auto media = inline_media(MediaKind::Image, mime, bytes);
        expect_error(run1(family, Origin::Direct, media, false), ErrorKind::InvalidRequest);
      }
    }
  }
}

void representative_inline_wire_and_order() {
  const auto wav = inline_media("audio/wav");
  for (const auto family : {Fam::Chat, Fam::ChatCanonical})
    expect_part(family, Origin::Direct, wav,
        "{\"type\":\"input_audio\",\"input_audio\":{\"data\":" + json::quote(*wav.data) + ",\"format\":\"wav\"}}");
  // Responses file inputs cover binary documents beyond PDF.
  auto docx = inline_media("application/vnd.openxmlformats-officedocument.wordprocessingml.document");
  docx.name = "résumé.docx";
  expect_part(Fam::Responses, Origin::Direct, docx,
      "{\"type\":\"input_file\",\"filename\":\"résumé.docx\",\"file_data\":" + json::quote(data_url(docx)) + "}");
  auto text = inline_media(MediaKind::Document, "text/plain", "hello \"world\"\n한글");
  text.name = "notes";
  expect_part(Fam::Messages, Origin::Direct, text,
      R"({"type":"document","source":{"type":"text","media_type":"text/plain","data":"hello \"world\"\n한글"},"title":"notes"})");
  auto invalid_utf8 = inline_media(MediaKind::Document, "text/plain", bytes_of({0xc0, 0xaf}));
  expect_error(run1(Fam::Messages, Origin::Direct, invalid_utf8, false), ErrorKind::InvalidRequest);
  for (const auto family : {Fam::Gemini, Fam::Interactions}) {
    for (const auto& media : {inline_media("audio/m4a"), inline_media("video/mp4"), inline_media("application/pdf"),
                             inline_media("text/csv")})
      expect_part(family, Origin::Direct, media, google_part(family, media));
    for (const bool streaming : {false, true}) {
      const auto image = vision_test::scene_a().image();
      const auto audio = inline_media("audio/wav");
      const auto result = run(family, Origin::Direct, {Text{"before"}, image, Text{"between"}, audio, Text{"after"}}, streaming);
      CHECK(is_wire(result));
      const auto content = content_of(family, wire_of(result).root());
      CHECK(content.size() == 5);
      CHECK(content.at(0).get("text").as_string() == "before");
      CHECK(json_matches(content.at(1), google_part(family, image)));
      CHECK(content.at(2).get("text").as_string() == "between");
      CHECK(json_matches(content.at(3), google_part(family, audio)));
      CHECK(content.at(4).get("text").as_string() == "after");
    }
  }
  // ISO-BMFF audio/video files may contain a leading free box. A four-byte magic
  // probe is not a decoder and must not prohibit valid provider-admitted containers.
  const auto free_box = be32(8) + "free";
  for (const auto family : {Fam::Gemini, Fam::Interactions}) {
    for (const auto& media : {inline_media(MediaKind::Audio, "audio/m4a", free_box + ftyp("M4A ", {"iso6"})),
                             inline_media(MediaKind::Video, "video/mp4", free_box + ftyp("iso6", {"mp42"}))})
      expect_part(family, Origin::Direct, media, google_part(family, media));
  }
}

void url_and_provider_file_forms() {
  for (const auto family : {Fam::Gemini, Fam::Interactions}) {
    for (const auto kind : {MediaKind::Image, MediaKind::Audio, MediaKind::Video, MediaKind::Document}) {
      const std::string mime = kind == MediaKind::Image ? "image/heif" : kind == MediaKind::Audio ? "audio/wav" :
          kind == MediaKind::Video ? "video/mp4" : "application/pdf";
      for (const auto& media : {Media::url(kind, "https://example.test/media?version=2", mime),
                               Media::file(kind, "https://generativelanguage.googleapis.com/v1beta/files/fixture", mime)})
        expect_part(family, Origin::Direct, media, google_part(family, media));
    }
    const auto youtube = Media::url(MediaKind::Video, "https://www.youtube.com/watch?v=fixture");
    expect_part(family, Origin::Direct, youtube, google_part(family, youtube));
  }
  for (const auto family : {Fam::Chat, Fam::ChatCanonical})
    expect_part(family, Origin::Direct, Media::file(MediaKind::Document, "file-pdf"),
        R"({"type":"file","file":{"file_id":"file-pdf"}})");
  expect_part(Fam::Responses, Origin::Direct, Media::url(MediaKind::Document, "https://example.test/report.pdf"),
      R"({"type":"input_file","file_url":"https://example.test/report.pdf"})");
  expect_part(Fam::Responses, Origin::Direct, Media::file(MediaKind::Image, "file-image"),
      R"({"type":"input_image","file_id":"file-image","detail":"auto"})");
  expect_part(Fam::Responses, Origin::Direct, Media::file(MediaKind::Document, "file-doc"),
      R"({"type":"input_file","file_id":"file-doc"})");
  expect_part(Fam::Messages, Origin::Direct, Media::url(MediaKind::Document, "https://example.test/report.pdf"),
      R"({"type":"document","source":{"type":"url","url":"https://example.test/report.pdf"}})");
  expect_part(Fam::Messages, Origin::Direct, Media::file(MediaKind::Image, "file-image"),
      R"({"type":"image","source":{"type":"file","file_id":"file-image"}})");
  expect_part(Fam::Messages, Origin::Direct, Media::file(MediaKind::Document, "file-doc", "text/plain"),
      R"({"type":"document","source":{"type":"file","file_id":"file-doc"}})");
  expect_error(run1(Fam::Messages, Origin::Direct, Media::url(MediaKind::Document, "https://example.test/text", "text/plain"), false),
      ErrorKind::Unsupported);
}

// Gateway support comes from the descriptor's declared origin, not its model name,
// path spelling, host substring, or the mere existence of an OpenRouter wire shape.
void declared_origin_and_unsupported_precedence() {
  const auto video = inline_media("video/mp4");
  const auto flac = inline_media(MediaKind::Audio, "audio/flac", "fLaC" + std::string(38, '\0'));
  for (const auto family : {Fam::Chat, Fam::ChatCanonical}) {
    expect_error(run1(family, Origin::Direct, video, false), ErrorKind::Unsupported);
    expect_error(run1(family, Origin::Direct, flac, false), ErrorKind::InvalidRequest);
    expect_part(family, Origin::OpenRouter, flac,
        "{\"type\":\"input_audio\",\"input_audio\":{\"data\":" + json::quote(*flac.data) + ",\"format\":\"flac\"}}");
    expect_part(family, Origin::OpenRouter, video,
        "{\"type\":\"video_url\",\"video_url\":{\"url\":" + json::quote(data_url(video)) + "}}");
    expect_part(family, Origin::OpenRouter, Media::url(MediaKind::Document, "https://example.test/report.pdf", "application/pdf"),
        R"({"type":"file","file":{"filename":"document.pdf","file_data":"https://example.test/report.pdf"}})");
  }
  expect_error(run1(Fam::Responses, Origin::Direct, inline_media("audio/wav"), false), ErrorKind::Unsupported);
  expect_error(run1(Fam::Responses, Origin::Direct, video, false), ErrorKind::Unsupported);
  const auto wav = inline_media("audio/wav");
  expect_part(Fam::Responses, Origin::OpenRouter, wav,
      "{\"type\":\"input_audio\",\"input_audio\":{\"data\":" + json::quote(*wav.data) + ",\"format\":\"wav\"}}");
  expect_part(Fam::Responses, Origin::OpenRouter, video,
      "{\"type\":\"input_video\",\"video_url\":" + json::quote(data_url(video)) + "}");
  expect_error(run1(Fam::Responses, Origin::OpenRouter, flac, false), ErrorKind::InvalidRequest);
  for (const auto family : {Fam::Chat, Fam::ChatCanonical, Fam::Responses}) {
    for (const auto url : {"https://openrouter.ai.example.test", "https://proxy.openrouter.ai"}) {
      expect_error(run_descriptor(family, load_descriptor(family, url), {video}, false), ErrorKind::Unsupported);
    }
  }
  for (const auto family : {Fam::Chat, Fam::ChatCanonical, Fam::Responses, Fam::Messages}) {
    const auto origin = family == Fam::Responses ? Origin::OpenRouter : Origin::Direct;
    // Bad payload/source fields cannot make an unrepresentable source look like invalid input.
    auto audio_url = Media::url(MediaKind::Audio, "not-a-url", "audio/wav");
    audio_url.data = shared("%%%=");
    expect_error(run1(family, origin, audio_url, false), ErrorKind::Unsupported);
  }
  for (const auto family : {Fam::Chat, Fam::ChatCanonical}) {
    expect_error(run1(family, Origin::Direct, Media::file(MediaKind::Image, ""), false), ErrorKind::Unsupported);
    expect_error(run1(family, Origin::Direct, Media::url(MediaKind::Document, "bad", "not/a-mime"), false), ErrorKind::Unsupported);
  }
  for (const auto family : {Fam::Responses, Fam::Messages}) {
    auto malformed = video; malformed.mime = "bad"; malformed.data = shared("%%%=");
    expect_error(run1(family, Origin::Direct, malformed, false), ErrorKind::Unsupported);
  }
  for (const auto family : {Fam::Chat, Fam::ChatCanonical, Fam::Responses, Fam::Messages})
    expect_error(run1(family, Origin::Direct, inline_media("image/heic"), false), ErrorKind::InvalidRequest);
}

void mime_and_malformed_payloads() {
  const auto image = vision_test::scene_a().image();
  for (const auto family : all_families) {
    auto mixed_case = image; mixed_case.mime = "IMAGE/PNG;profile=standard";
    const auto result = run1(family, Origin::Direct, mixed_case, false);
    CHECK_AT(is_wire(result), fam_name(family));
    const auto part = content_of(family, wire_of(result).root()).at(0);
    if (family == Fam::Messages)
      CHECK(part.get("source").get("media_type").as_string() == "image/png");
    else if (family == Fam::Gemini)
      CHECK(part.get("inlineData").get("mimeType").as_string() == mixed_case.mime);
    else if (family == Fam::Interactions)
      CHECK(part.get("mime_type").as_string() == "image/png");
    else if (family == Fam::Responses)
      CHECK(part.get("image_url").as_string() == data_url(mixed_case));
    else CHECK(part.get("image_url").get("url").as_string() == data_url(mixed_case));
    for (const auto payload : {"", "AAAAA", "AAA?", "AA=A", "AB==", "AAB=", "AAAA\n", "data:image/png;base64,AAAA"}) {
      auto malformed = image; malformed.data = shared(payload);
      expect_error(run1(family, Origin::Direct, malformed, false), ErrorKind::InvalidRequest);
    }
    for (const auto mime : {"", "image/png\"", "image/png;profile", "image//png", "image/png;profile=x,y", "image/svg+xml"}) {
      auto malformed = image; malformed.mime = mime;
      expect_error(run1(family, Origin::Direct, malformed, false), ErrorKind::InvalidRequest);
    }
    auto malformed = image; malformed.data.reset();
    expect_error(run1(family, Origin::Direct, malformed, false), ErrorKind::InvalidRequest);
    malformed = image; malformed.reference = "https://example.test/image";
    expect_error(run1(family, Origin::Direct, malformed, false), ErrorKind::InvalidRequest);
    malformed = image; malformed.kind = static_cast<MediaKind>(255);
    expect_error(run1(family, Origin::Direct, malformed, false), ErrorKind::InvalidRequest);
    malformed = image; malformed.source = static_cast<MediaSource>(255);
    expect_error(run1(family, Origin::Direct, malformed, false), ErrorKind::InvalidRequest);
    malformed = image; malformed.mime = "image/jpeg";
    expect_error(run1(family, Origin::Direct, malformed, false), ErrorKind::InvalidRequest);
    malformed = image; malformed.data = shared(b64("not a PNG"));
    expect_error(run1(family, Origin::Direct, malformed, false), ErrorKind::InvalidRequest);
    auto bad_url = Media::url(MediaKind::Image, "http://example.test/image");
    expect_error(run1(family, Origin::Direct, bad_url, false), ErrorKind::InvalidRequest);
  }
  auto mismatch = image; mismatch.kind = MediaKind::Audio;
  expect_error(run1(Fam::Gemini, Origin::Direct, mismatch, false), ErrorKind::InvalidRequest);
  mismatch.kind = MediaKind::Video;
  // A disallowed family kind outranks its invalid MIME-kind combination.
  expect_error(run1(Fam::Chat, Origin::Direct, mismatch, false), ErrorKind::Unsupported);
  for (const auto family : {Fam::Gemini, Fam::Interactions}) {
    auto invalid_file = Media::file(MediaKind::Document, "file id", "application/pdf");
    expect_error(run1(family, Origin::Direct, invalid_file, false), ErrorKind::InvalidRequest);
    auto wrong_source = Media::url(MediaKind::Image, "https://example.test/image");
    wrong_source.data = image.data;
    expect_error(run1(family, Origin::Direct, wrong_source, false), ErrorKind::InvalidRequest);
  }
  auto unknown_audio = inline_media(MediaKind::Audio, "audio/unknown", "samples");
  expect_error(run1(Fam::Gemini, Origin::Direct, unknown_audio, false), ErrorKind::InvalidRequest);
}

std::string replace_member(json::Value object, std::string_view key, std::string_view replacement) {
  std::string result = "{";
  for (const auto member : object.members()) {
    if (result.size() > 1) result += ',';
    result += json::quote(member.key) + ':';
    result += member.key == key ? std::string(replacement) : member.value.dump();
  }
  return result + '}';
}
descriptor::PolicySnapshot bounded_policy(std::size_t request_bytes) {
  const auto defaults = parse_json(config_defaults::codec_defaults_json);
  auto resources = parse_json(replace_member(defaults->root().get("resources"), "request_bytes", std::to_string(request_bytes)));
  for (const auto member : {"image_decoded_bytes", "audio_decoded_bytes", "video_decoded_bytes", "document_decoded_bytes"})
    resources = parse_json(replace_member(resources->root(), member, "192"));
  auto loaded = descriptor::load_policy(config_defaults::descriptor_policy_json,
      replace_member(defaults->root(), "resources", resources->root().dump()));
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(loaded));
  return std::get<descriptor::PolicySnapshot>(std::move(loaded));
}
void policy_cannot_create_unrepresentable_sources() {
  const auto defaults = parse_json(config_defaults::descriptor_policy_json);
  std::string families = "[";
  for (const auto family : defaults->root().get("families").elements()) {
    if (families.size() > 1) families += ',';
    const auto name = family.get("family").as_string();
    if (name == "openai.chat" || name == "openai.responses" || name == "anthropic.messages") {
      const auto media = replace_member(family.get("media"), "audio",
          R"({"mime":["audio/wav"],"sources":["inline","url","file"]})");
      families += replace_member(family, "media", media);
    } else families += family.dump();
  }
  families += ']';
  auto loaded = descriptor::load_policy(replace_member(defaults->root(), "families", families),
      config_defaults::codec_defaults_json);
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(loaded));
  const auto policy = std::get<descriptor::PolicySnapshot>(std::move(loaded));
  for (const auto family : {Fam::Chat, Fam::ChatCanonical, Fam::Responses, Fam::Messages}) {
    const auto& d = load_descriptor(family, default_origin(family, Origin::Direct), policy);
    for (const auto& media : {Media::url(MediaKind::Audio, "https://example.test/sound", "audio/wav"),
                             Media::file(MediaKind::Audio, "file-audio", "audio/wav")})
      expect_error(run_descriptor(family, d, {media}, false), ErrorKind::Unsupported);
  }
}
void size_and_aggregate_error_contract() {
  const auto per_kind = bounded_policy(4096);
  const auto aggregate = bounded_policy(900);
  for (const auto family : all_families) {
    const auto& d = load_descriptor(family, default_origin(family, Origin::Direct), per_kind);
    const auto& small = load_descriptor(family, default_origin(family, Origin::Direct), aggregate);
    for (const auto mime : {"image/png", "application/pdf"}) {
      auto raw = sample_bytes(mime); raw.resize(192, '\0');
      const auto at_limit = inline_media(kind_for(mime), mime, raw);
      const auto too_big = inline_media(kind_for(mime), mime, raw + '\0');
      for (const bool streaming : {false, true}) {
        CHECK(is_wire(run_descriptor(family, d, {at_limit}, streaming)));
        expect_error(run_descriptor(family, d, {too_big}, streaming), ErrorKind::ResourceLimit);
        CHECK(is_wire(run_descriptor(family, small, {at_limit}, streaming)));
        expect_error(run_descriptor(family, small, {at_limit, at_limit, at_limit, at_limit}, streaming), ErrorKind::ResourceLimit);
      }
      auto invalid = too_big;
      auto broken = *invalid.data; broken[broken.size() / 2] = '?'; invalid.data = shared(std::move(broken));
      expect_error(run_descriptor(family, d, {invalid}, false), ErrorKind::InvalidRequest);
    }
  }
  for (const auto family : {Fam::Gemini, Fam::Interactions}) {
    const auto& d = load_descriptor(family, default_origin(family, Origin::Direct), per_kind);
    for (const auto mime : {"audio/wav", "video/mp4"}) {
      auto raw = sample_bytes(mime); raw.resize(192, '\0');
      CHECK(is_wire(run_descriptor(family, d, {inline_media(kind_for(mime), mime, raw)}, false)));
      expect_error(run_descriptor(family, d, {inline_media(kind_for(mime), mime, raw + '\0')}, false), ErrorKind::ResourceLimit);
    }
  }
}

// Blob.mimeType and Blob/FileData.displayName:
// https://ai.google.dev/api/generate-content.md.txt
// These are protocol aliases, not a general relaxation of MIME syntax. Interactions' closed
// AudioContent/DocumentContent MIME enums do not contain them:
// https://ai.google.dev/static/api/interactions.openapi.json
void generate_alias_inputs_and_safety() {
  for (const auto mime : {"video/audio/s16le", "video/audio/wav", "video/text/timestamp"}) {
    const auto kind = std::string_view(mime) == "video/text/timestamp" ? MediaKind::Document : MediaKind::Audio;
    const auto media = inline_media(kind, mime, "fixture bytes");
    CHECK(mime_syntax(mime) && mime_is_generate_alias(mime));
    CHECK(mime_kind(mime) == kind);
    CHECK(check_media(media, 4096) == MediaFault::None);
    for (const auto& input : {media, Media::url(kind, "https://example.test/alias", mime),
                             Media::file(kind, "https://generativelanguage.googleapis.com/v1beta/files/alias", mime)})
      expect_part(Fam::Gemini, Origin::Direct, input, google_part(Fam::Gemini, input));
    for (const auto family : {Fam::Chat, Fam::ChatCanonical, Fam::Responses, Fam::Messages, Fam::Interactions}) {
      const auto error = kind == MediaKind::Audio && (family == Fam::Responses || family == Fam::Messages)
          ? ErrorKind::Unsupported : ErrorKind::InvalidRequest;
      expect_error(run1(family, Origin::Direct, media, false), error);
    }
    auto wrong_kind = media; wrong_kind.kind = MediaKind::Video;
    CHECK(check_media(wrong_kind, 4096) == MediaFault::Kind);
    expect_error(run1(Fam::Gemini, Origin::Direct, wrong_kind, false), ErrorKind::InvalidRequest);
    for (const auto payload : {"data:video/audio/wav;base64,QQ==", "QR==", "QQ==\n"}) {
      auto invalid = media; invalid.data = shared(payload);
      expect_error(run1(Fam::Gemini, Origin::Direct, invalid, false), ErrorKind::InvalidRequest);
    }
  }
  auto parameterized = inline_media(MediaKind::Audio, "VIDEO/AUDIO/S16LE;rate=16000", "fixture bytes");
  expect_part(Fam::Gemini, Origin::Direct, parameterized, google_part(Fam::Gemini, parameterized));
  for (const auto mime : {"video/audio/unknown", "video/audio/wav/extra", "video//audio/wav",
                          "audio/video/wav", "video/text/timestamps", "video/text/timestamp/",
                          "video/audio/wav\"", "video/audio/wav\\", "video/audio/wav;rate",
                          "video/audio/wav;rate=x,y", "video/audio/wav;rate=\"16000\"",
                          "video/audio/wav;rate=16000\n", "video/audio/wav;base64,QQ=="}) {
    CHECK(!mime_syntax(mime));
    const auto media = inline_media(MediaKind::Audio, mime, "fixture bytes");
    CHECK(check_media(media, 4096) == MediaFault::Mime);
    expect_error(run1(Fam::Gemini, Origin::Direct, media, false), ErrorKind::InvalidRequest);
  }
}

descriptor::PolicyResult policy_with_alias(std::string_view family_name, std::string_view kind,
                                         std::string_view mime, bool gateway = false) {
  const auto defaults = parse_json(config_defaults::descriptor_policy_json);
  std::string families = "[";
  for (const auto family : defaults->root().get("families").elements()) {
    if (families.size() > 1) families += ',';
    if (family.get("family").as_string() == family_name) {
      const auto member = gateway ? "openrouter_media" : "media";
      const auto media = replace_member(family.get(member), kind,
          "{\"mime\":[" + json::quote(mime) + "],\"sources\":[\"inline\"]}");
      families += replace_member(family, member, media);
    } else families += family.dump();
  }
  families += ']';
  return descriptor::load_policy(replace_member(defaults->root(), "families", families),
                                  config_defaults::codec_defaults_json);
}
void generate_alias_policy_is_closed() {
  for (const auto mime : {"video/audio/s16le", "video/audio/wav", "video/text/timestamp"}) {
    const auto kind = std::string_view(mime) == "video/text/timestamp" ? "document" : "audio";
    auto admitted = policy_with_alias("google.generate", kind, mime);
    CHECK(std::holds_alternative<descriptor::PolicySnapshot>(admitted));
    const auto& d = load_descriptor(Fam::Gemini, default_origin(Fam::Gemini, Origin::Direct),
                                    std::get<descriptor::PolicySnapshot>(std::move(admitted)));
    const auto media = inline_media(std::string_view(kind) == "audio" ? MediaKind::Audio : MediaKind::Document,
                                    mime, "fixture bytes");
    CHECK(is_wire(run_descriptor(Fam::Gemini, d, {media}, false)));
    for (const auto family : {"google.interactions", "openai.chat", "openai.responses", "anthropic.messages"})
      CHECK(std::holds_alternative<descriptor::ConfigError>(policy_with_alias(family, kind, mime)));
    CHECK(std::holds_alternative<descriptor::ConfigError>(policy_with_alias("openai.chat", kind, mime, true)));
    CHECK(std::holds_alternative<descriptor::ConfigError>(policy_with_alias("google.generate", "video", mime)));
  }
  for (const auto mime : {"video/audio/new", "video/audio/wav/extra", "video/text/timestamp/",
                          "VIDEO/AUDIO/WAV", "video/audio/wav;rate=16000", "video/audio/wav\"",
                          "video/audio/wav\\", "video/audio/wav,other"})
    CHECK(std::holds_alternative<descriptor::ConfigError>(policy_with_alias("google.generate", "audio", mime)));
}

void generate_alias_output_semantics() {
  const auto& d = desc(Fam::Gemini);
  gemini::Request request; request.model = "fixture-model";
  request.messages.push_back(Message{"", Role::User, {Text{"generate"}}});
  const std::array<std::string_view, 3> aliases{"video/audio/s16le", "video/audio/wav", "video/text/timestamp"};
  const auto decode = [&](std::string parts, bool streaming) -> sp::Outcome {
    auto encoded = gemini::encode(d, request, streaming);
    CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
    Accumulator accumulator;
    gemini::Codec codec(d, streaming ? gemini::Mode::Sse : gemini::Mode::Buffered, accumulator,
                         std::get<gemini::EncodedRequest>(encoded).context);
    const auto response = "{\"responseId\":\"alias-output\",\"modelVersion\":\"fixture-model\","
        "\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":" + parts + "},\"finishReason\":\"STOP\"}]}";
    if (streaming) { codec.frame({}, response); codec.finish(); }
    else codec.buffered(response, {});
    CHECK(accumulator.outcome());
    return *accumulator.outcome();
  };
  std::string parts = "[";
  for (const auto mime : aliases) {
    if (parts.size() > 1) parts += ',';
    parts += "{\"inlineData\":{\"mimeType\":" + json::quote(mime) + ",\"data\":\"QQ==\"}},"
        "{\"fileData\":{\"mimeType\":" + json::quote(mime) +
        ",\"fileUri\":\"https://generativelanguage.googleapis.com/v1beta/files/output\"}}";
  }
  parts += ']';
  for (const bool streaming : {false, true}) {
    const auto outcome = decode(parts, streaming);
    CHECK(std::holds_alternative<Completion>(outcome));
    const auto& messages = std::get<Completion>(outcome).messages;
    CHECK(messages.size() == 1 && messages[0].parts.size() == 6);
    for (std::size_t i = 0; i < aliases.size(); ++i) {
      for (std::size_t j = 0; j < 2; ++j) {
        const auto& media = std::get<Media>(messages[0].parts[2 * i + j]);
        CHECK(media.kind == (i == 2 ? MediaKind::Document : MediaKind::Audio));
        CHECK(media.mime == aliases[i]);
        CHECK(media.source == (j == 0 ? MediaSource::Inline : MediaSource::File));
        if (j == 0) CHECK(media.data && *media.data == "QQ==");
        else CHECK(media.reference == "https://generativelanguage.googleapis.com/v1beta/files/output");
      }
    }
    for (const auto mime : {"video/audio/unknown", "video/text/timestamp/extra", "video/audio/wav;rate=x,y"}) {
      const auto rejected = decode("[{\"inlineData\":{\"mimeType\":" + json::quote(mime) + ",\"data\":\"QQ==\"}}]", streaming);
      CHECK(std::holds_alternative<Failure>(rejected));
      CHECK(std::get<Failure>(rejected).error.kind == ErrorKind::ProtocolCorrupt);
    }
  }
}

// The schema gives all Generate Blob/FileData forms displayName, but only Interactions
// VideoContent has name. Other Interactions media properties have no naming field.
void google_named_reference_wire() {
  auto invoice_a = inline_media("application/pdf"); invoice_a.name = "invoice_A.pdf";
  auto invoice_b = Media::file(MediaKind::Document,
      "https://generativelanguage.googleapis.com/v1beta/files/invoice-b", "application/pdf");
  invoice_b.name = "invoice_B.pdf";
  auto opening = Media::url(MediaKind::Video, "https://example.test/opening.mp4", "video/mp4");
  opening.name = "opening_clip";
  auto closing = Media::file(MediaKind::Video,
      "https://generativelanguage.googleapis.com/v1beta/files/closing", "video/mp4");
  closing.name = "closing_clip";
  for (const bool streaming : {false, true}) {
    const auto docs = run(Fam::Gemini, Origin::Direct,
        {Text{"Compare invoice_A.pdf against invoice_B.pdf."}, invoice_a, invoice_b}, streaming);
    CHECK(is_wire(docs));
    const auto content = content_of(Fam::Gemini, wire_of(docs).root());
    CHECK(content.size() == 3);
    CHECK(json_matches(content.at(1), "{\"inlineData\":{\"mimeType\":\"application/pdf\",\"data\":" +
        json::quote(*invoice_a.data) + ",\"displayName\":\"invoice_A.pdf\"}}"));
    CHECK(json_matches(content.at(2), "{\"fileData\":{\"mimeType\":\"application/pdf\",\"fileUri\":" +
        json::quote(invoice_b.reference) + ",\"displayName\":\"invoice_B.pdf\"}}"));
    for (const auto family : {Fam::Gemini, Fam::Interactions}) {
      const auto videos = run(family, Origin::Direct,
          {Text{"Compare opening_clip against closing_clip."}, opening, closing}, streaming);
      CHECK(is_wire(videos));
      const auto clips = content_of(family, wire_of(videos).root());
      CHECK(clips.size() == 3);
      for (std::size_t i = 0; i < 2; ++i) {
        const auto& media = i == 0 ? opening : closing;
        const auto expected = family == Fam::Gemini
            ? "{\"fileData\":{\"mimeType\":\"video/mp4\",\"fileUri\":" + json::quote(media.reference) +
                ",\"displayName\":" + json::quote(media.name) + "}}"
            : "{\"type\":\"video\",\"mime_type\":\"video/mp4\",\"uri\":" + json::quote(media.reference) +
                ",\"name\":" + json::quote(media.name) + "}";
        CHECK(json_matches(clips.at(i + 1), expected));
      }
    }
  }
  // Named inline videos and JSON-sensitive names are also represented, not raw interpolated.
  auto video = inline_media("video/mp4"); video.name = "clip \"A\"\\résumé";
  expect_part(Fam::Gemini, Origin::Direct, video,
      "{\"inlineData\":{\"mimeType\":\"video/mp4\",\"data\":" + json::quote(*video.data) +
      ",\"displayName\":" + json::quote(video.name) + "}}");
  expect_part(Fam::Interactions, Origin::Direct, video,
      "{\"type\":\"video\",\"mime_type\":\"video/mp4\",\"data\":" + json::quote(*video.data) +
      ",\"name\":" + json::quote(video.name) + "}");
  for (const auto mime : {"image/png", "audio/wav", "application/pdf"}) {
    auto media = inline_media(mime); media.name = "reference_name";
    expect_part(Fam::Gemini, Origin::Direct, media,
        "{\"inlineData\":{\"mimeType\":" + json::quote(media.mime) + ",\"data\":" + json::quote(*media.data) +
        ",\"displayName\":\"reference_name\"}}");
    for (const auto& named : {media,
         [&] { auto value = Media::url(media.kind, "https://example.test/named", media.mime); value.name = media.name; return value; }(),
         [&] { auto value = Media::file(media.kind, "https://generativelanguage.googleapis.com/v1beta/files/named", media.mime); value.name = media.name; return value; }()}) {
      for (const bool streaming : {false, true})
        expect_error(run1(Fam::Interactions, Origin::Direct, named, streaming), ErrorKind::Unsupported);
    }
  }
}

template<class Request> Request tool_request(std::vector<Message> history) {
  Request request; request.model="fixture-model"; request.account_scope="scope";
  request.messages=std::move(history);
  request.tools.push_back({"lookup","",parse_json(R"({"type":"object","properties":{}})")});
  return request;
}
template<class Encoded> Outcome tool_encoding(std::variant<Encoded,Error> result,
                                             std::shared_ptr<const NativeContext>* context) {
  if(context && std::holds_alternative<Encoded>(result)) *context=std::get<Encoded>(result).context;
  return finish(std::move(result));
}
Outcome tool_history(Fam family,const descriptor::ValidatedDescriptor& d,std::vector<Message> history,
                     bool streaming=false,std::shared_ptr<const NativeContext>* context=nullptr) {
  switch(family) {
    case Fam::Responses:return tool_encoding(responses::encode(d,tool_request<responses::Request>(std::move(history)),streaming),context);
    case Fam::Messages:return tool_encoding(messages::encode(d,tool_request<messages::Request>(std::move(history)),streaming),context);
    case Fam::Gemini:return tool_encoding(gemini::encode(d,tool_request<gemini::Request>(std::move(history)),streaming),context);
    case Fam::Interactions:return tool_encoding(interactions::encode(d,tool_request<interactions::Request>(std::move(history)),streaming),context);
    default:throw std::runtime_error("unexpected tool family");
  }
}
Message captured_tool_reply(Fam family,const descriptor::ValidatedDescriptor& d,
                            const std::vector<Message>& history,bool calls,bool streaming=false) {
  std::shared_ptr<const NativeContext> context;
  CHECK(is_wire(tool_history(family,d,history,streaming,&context)));
  Accumulator accumulator;
  switch(family) {
    case Fam::Responses:{
      const std::string output=calls ?
          R"([{"type":"function_call","id":"fc1","call_id":"call1","name":"lookup","arguments":"{}","status":"completed"},{"type":"function_call","id":"fc2","call_id":"call2","name":"lookup","arguments":"{}","status":"completed"}])" :
          R"([{"type":"message","id":"done","role":"assistant","status":"completed","content":[{"type":"output_text","text":"done","annotations":[]}]}])";
      responses::Codec codec(d,responses::Mode::Buffered,accumulator,context);
      codec.buffered(R"({"id":"response_tool","object":"response","created_at":1,"model":"fixture-model","status":"completed","output":)"+output+
          R"(,"usage":{"input_tokens":1,"output_tokens":1,"total_tokens":2},"incomplete_details":null,"error":null})",{});
      codec.finish();break;
    }
    case Fam::Messages:{
      const std::string content=calls ?
          R"([{"type":"tool_use","id":"call1","name":"lookup","input":{}},{"type":"tool_use","id":"call2","name":"lookup","input":{}}])" :
          R"([{"type":"text","text":"done"}])";
      messages::Codec codec(d,messages::Mode::Buffered,accumulator,context);
      codec.buffered(R"({"id":"message_tool","type":"message","role":"assistant","model":"fixture-model","content":)"+content+
          R"(,"stop_reason":)"+json::quote(calls ? "tool_use" : "end_turn")+
          R"(,"stop_sequence":null,"usage":{"input_tokens":1,"output_tokens":1}})",{});
      codec.finish();break;
    }
    case Fam::Gemini:{
      const std::string parts=calls ?
          R"([{"functionCall":{"id":"call1","name":"lookup","args":{}},"thoughtSignature":"CALL1"},{"functionCall":{"id":"call2","name":"lookup","args":{}},"thoughtSignature":"CALL2"}])" :
          R"([{"text":"done"}])";
      gemini::Codec codec(d,gemini::Mode::Buffered,accumulator,context);
      codec.buffered(R"({"modelVersion":"fixture-model","responseId":"generation_tool","candidates":[{"index":0,"content":{"role":"model","parts":)"+parts+
          R"(},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":1,"candidatesTokenCount":1,"totalTokenCount":2}})",{});
      codec.finish();break;
    }
    case Fam::Interactions:{
      const std::string steps=calls ?
          R"([{"type":"function_call","id":"call1","name":"lookup","arguments":{}},{"type":"function_call","id":"call2","name":"lookup","arguments":{}}])" :
          R"([{"type":"model_output","content":[{"type":"text","text":"done"}]}])";
      interactions::Codec codec(d,interactions::Mode::Buffered,accumulator,context);
      codec.buffered(R"({"id":"interaction_tool","model":"fixture-model","status":)"+json::quote(calls ? "requires_action" : "completed")+
          R"(,"steps":)"+steps+R"(,"usage":{"total_input_tokens":1,"total_output_tokens":1,"total_tokens":2}})",{});
      codec.finish();break;
    }
    default:throw std::runtime_error("unexpected capture family");
  }
  CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));
  auto message=std::get<Completion>(*accumulator.outcome()).messages.at(0);
  CHECK(message.native);return message;
}
std::vector<Message> tool_prefix(Fam family,const descriptor::ValidatedDescriptor& d,bool streaming=false) {
  std::vector<Message> history{Message{{},Role::User,{Text{"lookup"}}}};
  history.push_back(captured_tool_reply(family,d,history,true,streaming));return history;
}
std::vector<Message> with_tool_results(Fam family,std::vector<Message> history,ToolResult result) {
  // Legacy Gemini results are one JSON object each; other families carry free text.
  ToolResult second{"call2",family==Fam::Gemini ? R"({"result":"second"})" : "second"};
  history.push_back(Message{{},family==Fam::Messages ? Role::User : Role::Tool,{std::move(result),std::move(second)}});
  return history;
}
json::Value first_tool_result(Fam family,json::Value root) {
  switch(family) {
    case Fam::Responses:return root.get("input").at(3);
    case Fam::Messages:return root.get("messages").at(2).get("content").at(0);
    case Fam::Gemini:return root.get("contents").at(2).get("parts").at(0).get("functionResponse");
    case Fam::Interactions:return root.get("input").at(3);
    default:return {};
  }
}
void nested_tool_media_wire_and_binding() {
  for(const auto family:{Fam::Responses,Fam::Messages,Fam::Gemini,Fam::Interactions}) {
    const auto& d=desc(family);
    const auto prefix=tool_prefix(family,d);
    const auto image=inline_media("image/png");
    auto other=image; other.data=shared(b64(sample_bytes("image/png")+"different"));
    ToolResult result{"call1",""};
    result.content_parts={Text{"before"},image,Text{"between"},other,Text{"after"}};
    const auto history=with_tool_results(family,prefix,result);
    for(const bool streaming:{false,true}) {
      const auto encoded=tool_history(family,d,
          streaming ? with_tool_results(family,tool_prefix(family,d,true),result) : history,streaming);
      CHECK(is_wire(encoded));
      const auto wire=first_tool_result(family,wire_of(encoded).root());
      if(family==Fam::Gemini) {
        // generateContent has two ordered lanes, not a text/media interleaving union.
        CHECK(json_matches(wire.get("response"),R"({"output":["before","between","after"]})"));
        CHECK(json_matches(wire.get("parts"),"["+google_part(family,image)+","+google_part(family,other)+"]"));
      } else {
        const auto content=wire.get(family==Fam::Responses ? "output" : family==Fam::Messages ? "content" : "result");
        CHECK(content.size()==5 && content.at(0).get("text").as_string()=="before" &&
              content.at(2).get("text").as_string()=="between" && content.at(4).get("text").as_string()=="after");
        CHECK(content.at(1).get("type").as_string()==(family==Fam::Responses ? "input_image" : "image"));
        const auto first=family==Fam::Responses ? content.at(1).get("image_url") :
            family==Fam::Messages ? content.at(1).get("source").get("data") : content.at(1).get("data");
        const auto last=family==Fam::Responses ? content.at(3).get("image_url") :
            family==Fam::Messages ? content.at(3).get("source").get("data") : content.at(3).get("data");
        CHECK(first.as_string()==(family==Fam::Responses ? data_url(image) : *image.data));
        CHECK(last.as_string()==(family==Fam::Responses ? data_url(other) : *other.data));
      }
    }
    auto sealed=history;
    sealed.push_back(captured_tool_reply(family,d,sealed,false));
    sealed.push_back(Message{{},Role::User,{Text{"continue"}}});
    CHECK(is_wire(tool_history(family,d,sealed)));
    for(unsigned mutation=0;mutation<11;++mutation) {
      auto edited=sealed;auto& tool=std::get<ToolResult>(edited[2].parts[0]);
      auto& media=std::get<Media>(tool.content_parts[1]);
      switch(mutation) {
        case 0:media.kind=MediaKind::Audio;break;
        case 1:media.source=MediaSource::Url;break;
        case 2:media.mime="image/jpeg";break;
        case 3:media.data=other.data;break;
        case 4:media.reference="https://example.test/edited";break;
        case 5:media.name="edited.png";break;
        case 6:media.detail=ImageDetail::High;break;
        case 7:media.id="edited";break;
        case 8:media.transcript="edited";break;
        case 9:std::get<Text>(tool.content_parts[0]).value="edited";break;
        case 10:std::swap(tool.content_parts[0],tool.content_parts[1]);break;
      }
      expect_error(tool_history(family,d,std::move(edited)),ErrorKind::ReplayIneligible);
    }
    auto exclusive=result;exclusive.content="legacy";
    expect_error(tool_history(family,d,with_tool_results(family,prefix,exclusive)),ErrorKind::InvalidRequest);
    auto errored=result;errored.is_error=true;
    const auto encoded=tool_history(family,d,with_tool_results(family,prefix,errored));
    if(family==Fam::Responses)expect_error(encoded,ErrorKind::Unsupported);
    else {
      CHECK(is_wire(encoded));const auto wire=first_tool_result(family,wire_of(encoded).root());
      if(family==Fam::Gemini)CHECK(wire.get("response").get("error").size()==3);
      else CHECK(wire.get("is_error").as_bool());
    }
    auto wrong=result;wrong.tool_use_id="missing";
    expect_error(tool_history(family,d,with_tool_results(family,prefix,wrong)),ErrorKind::InvalidRequest);
    auto duplicate=history;std::get<ToolResult>(duplicate[2].parts[1]).tool_use_id="call1";
    expect_error(tool_history(family,d,duplicate),ErrorKind::InvalidRequest);
    auto missing=history;missing[2].parts.pop_back();
    expect_error(tool_history(family,d,missing),ErrorKind::InvalidRequest);
    auto reverse=history;std::swap(reverse[2].parts[0],reverse[2].parts[1]);
    CHECK(is_wire(tool_history(family,d,reverse)));
    auto imported=history;imported[1].native.reset();
    expect_error(tool_history(family,d,imported),ErrorKind::ReplayIneligible);
    auto mixed=history;mixed[2].parts.insert(mixed[2].parts.begin(),Text{"outside"});
    expect_error(tool_history(family,d,mixed),ErrorKind::InvalidRequest);
    auto malformed=result;std::get<Media>(malformed.content_parts[1]).kind=static_cast<MediaKind>(255);
    expect_error(tool_history(family,d,with_tool_results(family,prefix,malformed)),ErrorKind::InvalidRequest);
    malformed=result;std::get<Media>(malformed.content_parts[1]).source=static_cast<MediaSource>(255);
    expect_error(tool_history(family,d,with_tool_results(family,prefix,malformed)),ErrorKind::InvalidRequest);
  }
}
void nested_tool_family_sources_and_limits() {
  const auto policy=bounded_policy(4096);
  const auto small_policy=bounded_policy(900);
  for(const auto family:{Fam::Responses,Fam::Messages,Fam::Gemini,Fam::Interactions}) {
    const auto& d=load_descriptor(family,default_origin(family,Origin::Direct),policy);
    const auto prefix=tool_prefix(family,d);
    for(const auto mime:{"image/png","application/pdf","audio/wav","video/mp4"}) {
      ToolResult result{"call1",""};result.content_parts={Text{"caption"},inline_media(mime)};
      const auto encoded=tool_history(family,d,with_tool_results(family,prefix,result));
      const bool supported=family==Fam::Gemini || (std::string_view(mime)=="image/png") ||
          (family!=Fam::Interactions && std::string_view(mime)=="application/pdf");
      if(supported)CHECK(is_wire(encoded));else expect_error(encoded,ErrorKind::Unsupported);
      if(is_wire(encoded) && family==Fam::Messages && std::string_view(mime)=="application/pdf")
        CHECK(first_tool_result(family,wire_of(encoded).root()).get("content").at(1).get("type").as_string()=="document");
    }
    for(const auto source:{MediaSource::Url,MediaSource::File}) {
      ToolResult result{"call1",""};
      const auto image=source==MediaSource::Url ?
          Media::url(MediaKind::Image,"https://example.test/image.png","image/png") :
          Media::file(MediaKind::Image,family==Fam::Interactions ? "https://generativelanguage.googleapis.com/v1beta/files/image" : "file-image","image/png");
      result.content_parts={image};
      const auto encoded=tool_history(family,d,with_tool_results(family,prefix,result));
      if(family==Fam::Gemini)expect_error(encoded,ErrorKind::Unsupported);else CHECK(is_wire(encoded));
    }
    if(family==Fam::Responses || family==Fam::Messages) {
      for(const auto source:{MediaSource::Inline,MediaSource::Url,MediaSource::File}) {
        ToolResult document_result{"call1",""};
        auto document=source==MediaSource::Inline ? inline_media("application/pdf") :
            source==MediaSource::Url ? Media::url(MediaKind::Document,"https://example.test/invoice.pdf","application/pdf") :
                                     Media::file(MediaKind::Document,"file-document","application/pdf");
        document_result.content_parts={Text{"invoice"},document};
        const auto encoded=tool_history(family,d,with_tool_results(family,prefix,document_result));CHECK(is_wire(encoded));
        const auto leaf=first_tool_result(family,wire_of(encoded).root()).get(family==Fam::Responses ? "output" : "content").at(1);
        CHECK(leaf.get("type").as_string()==(family==Fam::Responses ? "input_file" : "document"));
        if(source==MediaSource::Inline && family==Fam::Responses)CHECK(leaf.get("file_data").as_string()==data_url(document));
        if(source==MediaSource::Url && family==Fam::Responses)CHECK(leaf.get("file_url").as_string()==document.reference);
        if(source==MediaSource::File && family==Fam::Responses)CHECK(leaf.get("file_id").as_string()==document.reference);
        if(family==Fam::Messages)CHECK(leaf.get("source").get("type").as_string()==
            (source==MediaSource::Inline ? "base64" : source==MediaSource::Url ? "url" : "file"));
      }
    }
    ToolResult result{"call1",""};
    auto raw=sample_bytes("image/png");raw.resize(193,'x');
    result.content_parts={inline_media(MediaKind::Image,"image/png",raw)};
    expect_error(tool_history(family,d,with_tool_results(family,prefix,result)),ErrorKind::ResourceLimit);
    raw.resize(192);result.content_parts={inline_media(MediaKind::Image,"image/png",raw)};
    CHECK(is_wire(tool_history(family,d,with_tool_results(family,prefix,result))));
    result.content_parts.assign(8,std::get<Media>(result.content_parts.front()));
    const auto& small=load_descriptor(family,default_origin(family,Origin::Direct),small_policy);
    expect_error(tool_history(family,small,with_tool_results(family,tool_prefix(family,small),result)),ErrorKind::ResourceLimit);
    // Content strings do not import vendor media arrays or authorize sibling media.
    const std::string literal=R"([{"type":"image","data":"not-media"}])";
    if(family==Fam::Gemini) {
      // Legacy Gemini content is exactly one JSON object; arrays are not accepted, nor converted.
      expect_error(tool_history(family,d,with_tool_results(family,prefix,ToolResult{"call1",literal})),ErrorKind::InvalidRequest);
      const std::string object=R"({"type":"image","data":"not-media"})";
      const auto legacy=tool_history(family,d,with_tool_results(family,prefix,ToolResult{"call1",object}));
      CHECK(is_wire(legacy));const auto legacy_result=first_tool_result(family,wire_of(legacy).root());
      CHECK(!legacy_result.get("parts").valid() && json_matches(legacy_result.get("response"),object));
    } else {
      const auto legacy=tool_history(family,d,with_tool_results(family,prefix,ToolResult{"call1",literal}));
      CHECK(is_wire(legacy));const auto legacy_result=first_tool_result(family,wire_of(legacy).root());
      CHECK(legacy_result.get(family==Fam::Responses ? "output" : family==Fam::Messages ? "content" : "result").as_string()==literal);
    }
    const auto defaults=parse_json(config_defaults::codec_defaults_json);
    const auto counts=replace_member(defaults->root().get("resources"),"request_parts","4");
    const auto loaded=descriptor::load_policy(config_defaults::descriptor_policy_json,replace_member(defaults->root(),"resources",counts));
    CHECK(std::holds_alternative<descriptor::PolicySnapshot>(loaded));
    const auto& count_limited=load_descriptor(family,default_origin(family,Origin::Direct),std::get<descriptor::PolicySnapshot>(loaded));
    result.content_parts.assign(5,Text{"bounded"});
    expect_error(tool_history(family,count_limited,with_tool_results(family,tool_prefix(family,count_limited),result)),ErrorKind::InvalidRequest);
  }
  const auto& d=desc(Fam::Gemini);const auto prefix=tool_prefix(Fam::Gemini,d);
  // Legacy text results stay one JSON object; text and media use the typed parts lane instead.
  for(const auto content:{"plain result",R"([1,2])"})
    expect_error(tool_history(Fam::Gemini,d,with_tool_results(Fam::Gemini,prefix,ToolResult{"call1",content})),ErrorKind::InvalidRequest);
  const auto object=tool_history(Fam::Gemini,d,with_tool_results(Fam::Gemini,prefix,ToolResult{"call1",R"({"result":7})"}));
  CHECK(is_wire(object));
  CHECK(json_matches(first_tool_result(Fam::Gemini,wire_of(object).root()).get("response"),R"({"result":7})"));
  ToolResult text_document{"call1",""};text_document.content_parts={inline_media("text/plain")};
  expect_error(tool_history(Fam::Gemini,d,with_tool_results(Fam::Gemini,prefix,text_document)),ErrorKind::Unsupported);
}
void messages_decoded_document_cache_budget() {
  const auto policy=bounded_policy(900);
  const auto& d=load_descriptor(Fam::Messages,default_origin(Fam::Messages,Origin::Direct),policy);
  const auto document=inline_media(MediaKind::Document,"text/plain",std::string(192,'x'));
  std::vector<Message> history;
  for(unsigned i=0;i<5;++i)history.push_back(Message{{},Role::User,{document}});
  expect_error(tool_history(Fam::Messages,d,history),ErrorKind::ResourceLimit);
  const auto prefix=tool_prefix(Fam::Messages,d);
  ToolResult result{"call1",""};result.content_parts={document,document,document,document,document};
  expect_error(tool_history(Fam::Messages,d,with_tool_results(Fam::Messages,prefix,result)),ErrorKind::ResourceLimit);
  const auto& large=desc(Fam::Messages);
  result.content_parts={Text{"first"},inline_media(MediaKind::Document,"text/plain","nested\n\"text"),Text{"last"}};
  auto nested_history=with_tool_results(Fam::Messages,tool_prefix(Fam::Messages,large),result);
  nested_history[2].parts.emplace_back(inline_media(MediaKind::Document,"text/plain","outside"));
  for(const bool streaming:{false,true}) {
    const auto encoded=tool_history(Fam::Messages,large,nested_history,streaming);CHECK(is_wire(encoded));
    const auto content=wire_of(encoded).root().get("messages").at(2).get("content");
    CHECK(content.at(0).get("content").at(1).get("source").get("data").as_string()=="nested\n\"text");
    CHECK(content.at(2).get("source").get("data").as_string()=="outside");
  }
  auto invalid=document;invalid.data=shared("!!!!");
  expect_error(tool_history(Fam::Messages,d,{Message{{},Role::User,{invalid}}}),ErrorKind::InvalidRequest);
  auto invalid_utf8=inline_media(MediaKind::Document,"text/plain",bytes_of({255}));
  expect_error(tool_history(Fam::Messages,d,{Message{{},Role::User,{invalid_utf8}}}),ErrorKind::InvalidRequest);
  // Aggregate cache refusal cannot conceal an edited native prefix.
  std::vector<Message> sealed{Message{{},Role::User,{Text{"small"}}}};
  sealed.push_back(captured_tool_reply(Fam::Messages,d,sealed,false));
  std::get<Text>(sealed[0].parts[0]).value="edited";
  for(unsigned i=0;i<5;++i)sealed.push_back(Message{{},Role::User,{document}});
  expect_error(tool_history(Fam::Messages,d,sealed),ErrorKind::ReplayIneligible);
}
} // namespace
int main() {
  try {
    heif_admission_and_brand_boundaries();
    representative_inline_wire_and_order();
    url_and_provider_file_forms();
    declared_origin_and_unsupported_precedence();
    mime_and_malformed_payloads();
    size_and_aggregate_error_contract();
    policy_cannot_create_unrepresentable_sources();
    generate_alias_inputs_and_safety();
    generate_alias_policy_is_closed();
    generate_alias_output_semantics();
    google_named_reference_wire();
    nested_tool_media_wire_and_binding();
    nested_tool_family_sources_and_limits();
    messages_decoded_document_cache_budget();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
