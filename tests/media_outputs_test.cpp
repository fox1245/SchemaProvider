// Response-side generated media: every decoder (Chat, Responses, Gemini generateContent, Gemini
// Interactions), buffered and SSE. Wire shapes follow the vendors' documented formats.
#include "codecs/chat.h"
#include "codecs/gemini.h"
#include "codecs/gemini_request.h"
#include "codecs/interactions.h"
#include "codecs/interactions_request.h"
#include "codecs/responses.h"
#include "codecs/responses_request.h"
#include "core/native.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace sp;
using Frame = std::pair<std::string, std::string>;

[[noreturn]] void fail_check(int line, const char* what) {
  throw std::runtime_error("line " + std::to_string(line) + ": " + what);
}
#define CHECK(condition) do { if (!(condition)) fail_check(__LINE__, #condition); } while (false)
#define CHECK_AT(line, condition) do { if (!(condition)) fail_check(line, #condition); } while (false)

// Independent of the SDK's own codec so payload equality is a real check.
std::string b64(std::string_view bytes) {
  static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    const std::size_t left = bytes.size() - i;
    unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
    if (left > 1) v |= static_cast<unsigned char>(bytes[i + 1]) << 8;
    if (left > 2) v |= static_cast<unsigned char>(bytes[i + 2]);
    out += alphabet[(v >> 18) & 63];
    out += alphabet[(v >> 12) & 63];
    out += left > 1 ? alphabet[(v >> 6) & 63] : '=';
    out += left > 2 ? alphabet[v & 63] : '=';
  }
  return out;
}
// Deterministic binary content that includes NUL, 0xFF and every residue mod 3.
std::string bytes_of(std::size_t size, unsigned seed = 1) {
  std::string out(size, '\0');
  for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<char>((i * 37 + seed * 101 + 11) & 0xff);
  if (size > 2) { out[0] = '\0'; out[size - 1] = static_cast<char>(0xff); }
  return out;
}

const Completion& completed(const Outcome& outcome, int line = __builtin_LINE()) {
  CHECK_AT(line, std::holds_alternative<Completion>(outcome));
  return std::get<Completion>(outcome);
}
const Failure& failed(const Outcome& outcome, ErrorKind kind, int line = __builtin_LINE()) {
  CHECK_AT(line, std::holds_alternative<Failure>(outcome));
  const auto& failure = std::get<Failure>(outcome);
  CHECK_AT(line, failure.error.kind == kind);
  return failure;
}
std::vector<const Media*> media_of(const std::vector<Message>& messages) {
  std::vector<const Media*> out;
  for (const auto& message : messages) for (const auto& part : message.parts) if (const auto* m = std::get_if<Media>(&part)) out.push_back(m);
  return out;
}
const std::vector<Part>& parts_of(const Completion& c) { CHECK(c.messages.size() == 1); return c.messages[0].parts; }

struct Expect {
  MediaKind kind;
  std::string mime;
  MediaSource source;
  std::string payload;  // canonical base64 for Inline; ignored otherwise
  std::string reference, name, id, transcript;
  bool check_mime;
  Expect(MediaKind k, std::string m, MediaSource s = MediaSource::Inline, std::string p = {}, std::string r = {}, std::string n = {},
         std::string i = {}, std::string t = {}, bool c = true)
      : kind(k), mime(std::move(m)), source(s), payload(std::move(p)), reference(std::move(r)), name(std::move(n)),
        id(std::move(i)), transcript(std::move(t)), check_mime(c) {}
};
void expect_media(const Part& part, const Expect& e, int line = __builtin_LINE()) {
  const auto* m = std::get_if<Media>(&part);
  CHECK_AT(line, m);
  CHECK_AT(line, m->kind == e.kind && m->source == e.source);
  if (e.check_mime) CHECK_AT(line, m->mime == e.mime);
  CHECK_AT(line, m->reference == e.reference && m->name == e.name && m->id == e.id && m->transcript == e.transcript);
  CHECK_AT(line, m->detail == ImageDetail::Auto);
  if (e.source == MediaSource::Inline) {
    CHECK_AT(line, m->data && *m->data == e.payload);
    CHECK_AT(line, canonical_base64(*m->data));
  } else CHECK_AT(line, !m->data);
}
bool is_text(const Part& part, std::string_view text) {
  const auto* t = std::get_if<Text>(&part);
  return t && t->value == text;
}
std::shared_ptr<const json::Document> parse_doc(std::string_view text) {
  auto p = json::parse(text);
  CHECK(std::holds_alternative<json::Document>(p));
  return std::make_shared<const json::Document>(std::move(std::get<json::Document>(p)));
}
descriptor::ValidatedDescriptor load_descriptor(std::string_view json_text) {
  auto loaded = descriptor::load(json_text);
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
std::string descriptor_text(std::string_view id, std::string_view family, std::string_view base, std::string_view buffered, std::string_view streaming, std::string_view bindings = {}) {
  return "{\"descriptor_version\":1,\"revision\":1,\"id\":" + json::quote(id) + ",\"family\":" + json::quote(family) +
         ",\"connection\":{\"base_url\":" + json::quote(base) + ",\"paths\":{\"buffered\":" + json::quote(buffered) +
         ",\"streaming\":" + json::quote(streaming) + "}}" + std::string(bindings) + "}";
}

SemanticLimits tight_parts(std::size_t parts) { SemanticLimits limits; limits.max_parts = parts; return limits; }
SemanticLimits tight_bytes(std::size_t bytes) { SemanticLimits limits; limits.max_content_bytes = bytes; return limits; }

// ============================================================================= Chat
namespace chat_fixture {
const descriptor::ValidatedDescriptor& desc() {
  static const auto d = load_descriptor(descriptor_text("media-chat", "openai.chat", "http://127.0.0.1:18080", "/v1/chat/completions", "/v1/chat/completions"));
  return d;
}
const descriptor::ValidatedDescriptor& router() {
  static const auto d = load_descriptor(descriptor_text("media-router", "openai.chat", "https://openrouter.ai", "/api/v1/chat/completions", "/api/v1/chat/completions"));
  return d;
}
// The reply's audio format is the one the request asked for; it reaches the decoder through the context.
std::shared_ptr<const NativeContext> context(const descriptor::ValidatedDescriptor& d, std::string_view format, bool streaming) {
  chat::Request request;
  request.model = "fixture-model";
  request.messages.push_back({Role::User, "speak"});
  if (!format.empty()) {
    request.modalities = {chat::OutputModality::Text, chat::OutputModality::Audio};
    request.audio = chat::AudioOutput{"alloy", std::string(format)};
  }
  auto encoded = chat::encode(d, request, streaming);
  CHECK(std::holds_alternative<chat::EncodedRequest>(encoded));
  return std::get<chat::EncodedRequest>(encoded).context;
}
std::string wrap(std::string_view choices_and_more, bool streaming) {
  return std::string("{\"id\":\"gen-1\",\"model\":\"fixture-model\",\"created\":7,\"object\":\"") +
         (streaming ? "chat.completion.chunk" : "chat.completion") + "\"," + std::string(choices_and_more) + "}";
}
std::string body(std::string_view message, std::string_view finish = "\"stop\"") {
  return wrap("\"choices\":[{\"index\":0,\"message\":" + std::string(message) + ",\"finish_reason\":" + std::string(finish) + "}]", false);
}
std::string chunk(std::string_view delta, std::string_view finish = "null") {
  return wrap("\"choices\":[{\"index\":0,\"delta\":" + std::string(delta) + ",\"finish_reason\":" + std::string(finish) + "}]", true);
}
std::string audio_delta(std::string_view fields, bool first = false) {
  return std::string("{") + (first ? "\"role\":\"assistant\"," : "") + "\"audio\":{" + std::string(fields) + "}}";
}
Outcome buffered(std::string_view wire, std::shared_ptr<const NativeContext> ctx = {}, SemanticLimits limits = {}, const descriptor::ValidatedDescriptor& d = desc()) {
  Accumulator accumulator(limits);
  chat::Codec codec(d, chat::Mode::Buffered, accumulator, limits, std::move(ctx));
  codec.buffered(wire, {});
  CHECK(accumulator.outcome());
  return *accumulator.outcome();
}
Outcome stream(const std::vector<std::string>& wires, std::shared_ptr<const NativeContext> ctx = {}, chat::Close close = {}, SemanticLimits limits = {},
               const descriptor::ValidatedDescriptor& d = desc(), bool done = true) {
  Accumulator accumulator(limits);
  chat::Codec codec(d, chat::Mode::Sse, accumulator, limits, std::move(ctx));
  bool ok = true;
  for (const auto& wire : wires) if (ok && !codec.frame("message", wire)) ok = false;
  if (ok && done) codec.frame("message", "[DONE]");
  codec.finish(close);
  CHECK(accumulator.outcome());
  return *accumulator.outcome();
}
// A complete stream: the given deltas, one finish chunk, [DONE].
std::vector<std::string> deltas_then_stop(const std::vector<std::string>& deltas) {
  std::vector<std::string> wires;
  for (const auto& delta : deltas) wires.push_back(chunk(delta));
  wires.push_back(chunk("{}", "\"stop\""));
  return wires;
}

void audio_formats_buffered() {
  struct Case { const char* format; std::vector<const char*> mimes; };
  // pcm16 and mp3 are spelled more than one way on the wire; the essence decides.
  const std::vector<Case> cases{{"wav", {"audio/wav"}}, {"mp3", {"audio/mpeg", "audio/mp3"}}, {"flac", {"audio/flac"}},
                                {"opus", {"audio/opus"}}, {"aac", {"audio/aac"}}, {"pcm16", {"audio/l16"}}};
  const auto payload = b64(bytes_of(50, 3));
  for (const auto& c : cases) {
    const auto wire = body("{\"role\":\"assistant\",\"content\":null,\"audio\":{\"id\":\"audio_abc\",\"data\":\"" + payload +
                           "\",\"expires_at\":1730000000,\"transcript\":\"Hello there.\"}}");
    const auto outcome = buffered(wire, context(desc(), c.format, false));
    const auto& parts = parts_of(completed(outcome));
    CHECK(parts.size() == 1);  // null content never fabricates an empty text part
    Expect e{MediaKind::Audio, {}, MediaSource::Inline, payload, {}, {}, "audio_abc", "Hello there.", false};
    expect_media(parts[0], e);
    const auto& mime = std::get<Media>(parts[0]).mime;
    bool known = false;
    for (const auto* candidate : c.mimes) known = known || mime_equal(mime, candidate);
    CHECK(known);
    CHECK(mime_syntax(mime) && mime_kind(mime) == MediaKind::Audio);
    // The same reply streamed as one chunk is the same media.
    const auto streamed = stream(deltas_then_stop({audio_delta("\"id\":\"audio_abc\",\"data\":\"" + payload + "\",\"transcript\":\"Hello there.\"", true)}),
                                 context(desc(), c.format, true));
    const auto& s = parts_of(completed(streamed));
    CHECK(s.size() == 1);
    expect_media(s[0], Expect{MediaKind::Audio, mime, MediaSource::Inline, payload, {}, {}, "audio_abc", "Hello there."});
  }
  // The MIME follows the request format, not the payload: two formats, two MIME types.
  const auto wire = body("{\"role\":\"assistant\",\"content\":null,\"audio\":{\"id\":\"a\",\"data\":\"" + payload + "\"}}");
  const auto wav = std::get<Media>(parts_of(completed(buffered(wire, context(desc(), "wav", false))))[0]).mime;
  const auto flac = std::get<Media>(parts_of(completed(buffered(wire, context(desc(), "flac", false))))[0]).mime;
  CHECK(!mime_equal(wav, flac));
  // Without an audio request the reply is still audio; it is not dropped or mislabelled as another kind.
  const auto bare = completed(buffered(wire));
  CHECK(parts_of(bare).size() == 1 && std::get<Media>(parts_of(bare)[0]).kind == MediaKind::Audio);
  CHECK(*std::get<Media>(parts_of(bare)[0]).data == payload);
  CHECK(std::get<Media>(parts_of(bare)[0]).mime.empty());
}
void audio_and_text_order() {
  const auto payload = b64(bytes_of(9));
  const auto ctx = context(desc(), "wav", false);
  const auto outcome = buffered(body("{\"role\":\"assistant\",\"content\":\"spoken text\",\"audio\":{\"id\":\"a1\",\"data\":\"" + payload + "\"}}"), ctx);
  const auto& parts = parts_of(completed(outcome));
  CHECK(parts.size() == 2 && is_text(parts[0], "spoken text"));
  expect_media(parts[1], Expect{MediaKind::Audio, "audio/wav", MediaSource::Inline, payload, {}, {}, "a1", {}});
  const auto streamed = stream(deltas_then_stop({R"({"role":"assistant","content":"spoken "})", audio_delta("\"id\":\"a1\",\"data\":\"" + payload + "\""), R"({"content":"text"})"}),
                               context(desc(), "wav", true));
  const auto& s = parts_of(completed(streamed));
  CHECK(s.size() == 2 && is_text(s[0], "spoken text"));
  expect_media(s[1], Expect{MediaKind::Audio, "audio/wav", MediaSource::Inline, payload, {}, {}, "a1", {}});
}
void audio_stream_chunks() {
  // Each chunk is the base64 of an independent byte run: paddings fall mid-stream.
  const std::vector<std::string> runs{bytes_of(1, 1), bytes_of(2, 2), bytes_of(3, 3), bytes_of(7, 4), bytes_of(1000, 5), bytes_of(4, 6)};
  std::string all;
  std::vector<std::string> wires;
  const std::vector<std::string> transcripts{"Hel", "lo ", "", "wor", "ld", "!"};
  for (std::size_t i = 0; i < runs.size(); ++i) {
    all += runs[i];
    std::string fields = "\"data\":\"" + b64(runs[i]) + "\"";
    if (i == 0 || i == 2 || i == 5) fields = "\"id\":\"audio_s\"," + fields;  // repeated identical id is fine
    if (!transcripts[i].empty()) fields += ",\"transcript\":" + json::quote(transcripts[i]);
    wires.push_back(chunk(audio_delta(fields, i == 0)));
  }
  wires.push_back(chunk(audio_delta("\"expires_at\":1730000123")));  // expiry-only update before the finish
  wires.push_back(chunk("{}", "\"stop\""));
  const auto outcome = stream(wires, context(desc(), "wav", true));
  const auto& parts = parts_of(completed(outcome));
  CHECK(parts.size() == 1);
  expect_media(parts[0], Expect{MediaKind::Audio, "audio/wav", MediaSource::Inline, b64(all), {}, {}, "audio_s", "Hello world!"});

  // The final audio update after the finish reason carries only its expiry.
  auto tail_wires = deltas_then_stop({audio_delta("\"id\":\"audio_s\",\"data\":\"" + b64(runs[0]) + "\"", true)});
  const std::string tail_usage = "\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":2,\"total_tokens\":3}";
  tail_wires.push_back(wrap("\"choices\":[{\"index\":0,\"delta\":{\"audio\":{\"expires_at\":1730000999}},\"finish_reason\":\"stop\"}]," + tail_usage, true));
  const auto tail = completed(stream(tail_wires, context(desc(), "wav", true)));
  CHECK(parts_of(tail).size() == 1 && *std::get<Media>(parts_of(tail)[0]).data == b64(runs[0]));
  CHECK(tail.usage.total && tail.usage.total->value == 3);
  // Anything beyond the expiry after the finish reason is corruption, and it never alters the sealed payload.
  for (const char* late : {"\"data\":\"QUJD\"", "\"transcript\":\"late\"", "\"id\":\"audio_s\""}) {
    auto bad = deltas_then_stop({audio_delta("\"id\":\"audio_s\",\"data\":\"" + b64(runs[0]) + "\"", true)});
    bad.push_back(wrap("\"choices\":[{\"index\":0,\"delta\":{\"audio\":{" + std::string(late) + "}},\"finish_reason\":\"stop\"}]," + tail_usage, true));
    failed(stream(bad, context(desc(), "wav", true)), ErrorKind::ProtocolCorrupt);
  }
  for (const char* expiry : {"null", "-1", "\"soon\"", "1.5", "true"}) {
    auto bad = deltas_then_stop({audio_delta("\"data\":\"QQ==\"", true)});
    bad.push_back(wrap("\"choices\":[{\"index\":0,\"delta\":{\"audio\":{\"expires_at\":" + std::string(expiry) +
                       "}},\"finish_reason\":\"stop\"}]," + tail_usage, true));
    failed(stream(bad, context(desc(), "wav", true)), ErrorKind::ProtocolCorrupt);
  }
  // OpenRouter documents joining encoded strings, with no quartet alignment guarantee.
  const auto encoded = b64(bytes_of(67));
  for (std::size_t width : {1u, 2u, 5u, 9u}) {
    std::vector<std::string> fragments;
    for (std::size_t offset = 0; offset < encoded.size(); offset += width)
      fragments.push_back(audio_delta("\"data\":" + json::quote(std::string_view(encoded).substr(offset, width)), offset == 0));
    const auto outcome = stream(deltas_then_stop(fragments), context(router(), "wav", true), {}, {}, router());
    expect_media(parts_of(completed(outcome))[0], Expect{MediaKind::Audio, "audio/wav", MediaSource::Inline, encoded});
  }
  const auto padded = stream(deltas_then_stop({audio_delta("\"data\":\"Q\"", true), audio_delta("\"data\":\"Q==Qg\""), audio_delta("\"data\":\"==\"")}),
                             context(router(), "wav", true), {}, {}, router());
  expect_media(parts_of(completed(padded))[0], Expect{MediaKind::Audio, "audio/wav", MediaSource::Inline, "QUI="});
  failed(stream(deltas_then_stop({audio_delta("\"data\":\"AQ\"", true)}), context(router(), "wav", true), {}, {}, router()), ErrorKind::ProtocolCorrupt);
  auto no_usage_tail = deltas_then_stop({audio_delta("\"data\":\"QQ==\"", true)});
  no_usage_tail.push_back(chunk(audio_delta("\"expires_at\":1"), "\"stop\""));
  CHECK(media_of(completed(stream(no_usage_tail, context(desc(), "wav", true))).messages).size() == 1);
}
void metadata_heartbeats() {
  const std::vector<std::string> heartbeats{
    wrap(R"("usage":null,"obfuscation":"padding","service_tier":"default","system_fingerprint":null)", true),
    wrap(R"("choices":[],"usage":null,"obfuscation":"padding")", true),
    wrap(R"("obfuscation":"padding")", true),
    wrap(R"("choices":[])", true)};
  for (const auto& heartbeat : heartbeats) {
    const auto outcome = stream({heartbeat, chunk(R"({"role":"assistant","content":"before"})"), heartbeat,
                                 chunk(R"({"content":" after"})", "\"stop\""), heartbeat});
    const auto& result = completed(outcome);
    CHECK(parts_of(result).size() == 1 && is_text(parts_of(result)[0], "before after"));
    CHECK(result.raw_events.size() == 5);
    failed(stream({heartbeat}, {}, {}, {}, desc(), false), ErrorKind::Truncated);
    failed(stream({heartbeat}), ErrorKind::ProtocolCorrupt);
    failed(buffered(wrap(R"("choices":[],"usage":null)", false)), ErrorKind::ProtocolCorrupt);
  }
  // With neither choices nor a heartbeat marker, a bare response envelope remains corruption.
  failed(stream({R"({"id":"gen-1","model":"fixture-model","created":7,"object":"chat.completion.chunk"})"}),
         ErrorKind::ProtocolCorrupt);
  // Missing choices does not authorize a misplaced semantic channel, or relax its type.
  for (const char* extra : {R"("content":"late")", R"("audio":{"data":"QUJD"})", R"("delta":{})",
                           R"("tool_calls":[])", R"("refusal":null)", R"("finish_reason":"stop")",
                           R"("choices":null)", R"("choices":{})"}) {
    const auto malformed = wrap(std::string(R"("usage":null,"obfuscation":"padding",)") + extra, true);
    failed(stream({chunk(R"({"content":"kept"})"), malformed}), ErrorKind::ProtocolCorrupt);
  }
  auto changed_identity = heartbeats.front();
  changed_identity.replace(changed_identity.find("gen-1"), std::string("gen-1").size(), "gen-2");
  failed(stream({chunk(R"({"content":"kept"})"), changed_identity}), ErrorKind::ProtocolCorrupt);
}
void audio_usage_expiry_terminal() {
  // A bounded synthetic version of the audio wire lifecycle: no frame has a finish reason.
  const auto metadata = [](std::string wire, bool null_usage = true) {
    wire.pop_back();
    return wire + (null_usage ? ",\"usage\":null" : "") +
        R"(,"service_tier":"default","system_fingerprint":null,"obfuscation":"padding"})";
  };
  const auto heartbeat = wrap(R"("usage":null,"service_tier":"default","system_fingerprint":null,"obfuscation":"padding")", true);
  const auto first_bytes = bytes_of(6, 2), last_bytes = bytes_of(10, 3);
  const auto usage = metadata(wrap(R"("choices":[],"usage":{"prompt_tokens":3,"completion_tokens":5,"total_tokens":8,"completion_tokens_details":{"audio_tokens":4}})", true), false);
  const std::vector<std::string> wires{
    metadata(chunk(R"({"role":"assistant","refusal":null})")),
    metadata(chunk(R"({"content":null,"audio":{"id":"audio_fixture","transcript":"Hel"}})")),
    metadata(chunk(audio_delta(R"("transcript":"lo")"))),
    heartbeat, heartbeat,
    metadata(chunk(R"({"role":"assistant","content":null,"refusal":null,"audio":{"id":"audio_fixture","data":)" +
                   json::quote(b64(first_bytes)) + "}}")),
    heartbeat,
    metadata(chunk(audio_delta("\"data\":" + json::quote(b64(last_bytes))))),
    heartbeat, usage,
    metadata(wrap(R"("choices":[{"index":0,"delta":{"audio":{"expires_at":9}}}])", true))};
  std::size_t stops = 0, commits = 0, media_parts = 0;
  Accumulator accumulator({}, [&](const Event& event) {
    if (std::holds_alternative<Stop>(event)) ++stops;
    if (std::holds_alternative<Commit>(event)) ++commits;
    if (const auto* part = std::get_if<PartBegin>(&event); part && part->kind == PartKind::Media) ++media_parts;
  });
  chat::Codec codec(desc(), chat::Mode::Sse, accumulator, {}, context(desc(), "pcm16", true));
  for (const auto& wire : wires) CHECK(codec.frame("message", wire));
  CHECK(stops == 0 && commits == 0 && media_parts == 0 && !accumulator.outcome());
  CHECK(codec.frame("message", "[DONE]"));
  CHECK(stops == 1 && commits == 0 && media_parts == 1 && !accumulator.outcome());
  codec.finish();
  CHECK(stops == 1 && commits == 1 && media_parts == 1);
  const auto& result = completed(*accumulator.outcome());
  CHECK(parts_of(result).size() == 1);
  expect_media(parts_of(result)[0], Expect{MediaKind::Audio, "audio/l16", MediaSource::Inline,
                                         b64(first_bytes + last_bytes), {}, {}, "audio_fixture", "Hello"});
  CHECK(result.stop.kind == StopKind::EndTurn && result.stop.raw == "derived:audio_usage+audio.expires_at+DONE");
  CHECK(result.usage.stage == UsageStage::Final && result.usage.total && result.usage.total->value == 8);
  CHECK(result.usage.extra.at("audio_tokens").value == 4);
  CHECK(result.messages[0].native && result.messages[0].native->complete() && !result.messages[0].wire_output);
  CHECK(result.raw_events.size() == wires.size());
  for (std::size_t i = 0; i < wires.size(); ++i)
    CHECK(json::equal(result.raw_events[i].payload->root(), parse_doc(wires[i])->root()));
  const auto expiry = result.raw_events.back().payload->root().get("choices").at(0);
  CHECK(!expiry.get("finish_reason").valid() && expiry.get("delta").get("audio").get("expires_at").as_uint() == 9);

  // Consume the source seal on the same route and request configuration, not as re-uploaded audio.
  chat::Request next;
  next.model = "fixture-model";
  next.modalities = {chat::OutputModality::Text, chat::OutputModality::Audio};
  next.audio = chat::AudioOutput{"alloy", "pcm16"};
  next.canonical_messages = {Message{"", Role::User, {Text{"speak"}}}, result.messages[0],
                             Message{"", Role::User, {Text{"continue"}}}};
  const auto encoded = chat::encode(desc(), next, true);
  CHECK(std::holds_alternative<chat::EncodedRequest>(encoded));
  const auto carried_document = parse_doc(std::get<chat::EncodedRequest>(encoded).body);
  const auto carried = carried_document->root().get("messages").at(1);
  CHECK(carried.get("audio").size() == 1 && carried.get("audio").get("id").as_string() == "audio_fixture");
  CHECK(carried.get("content").is_null());
  for (int field = 0; field != 5; ++field) {
    auto changed = next;
    auto& audio = std::get<Media>(changed.canonical_messages[1].parts[0]);
    if (field == 0) audio.data = std::make_shared<const std::string>("QUJD");
    if (field == 1) audio.id = "different_audio";
    if (field == 2) audio.transcript = "changed";
    if (field == 3) audio.mime = "audio/wav";
    if (field == 4) std::get<Text>(changed.canonical_messages[0].parts[0]).value = "changed prompt";
    const auto refused = chat::encode(desc(), changed, true);
    CHECK(std::holds_alternative<Error>(refused) && std::get<Error>(refused).kind == ErrorKind::ReplayIneligible);
  }
  for (std::size_t count = 1; count <= wires.size(); ++count) {
    const std::vector<std::string> prefix(wires.begin(), wires.begin() + static_cast<std::ptrdiff_t>(count));
    const auto cut = stream(prefix, context(desc(), "pcm16", true), {}, {}, desc(), false);
    const auto& partial = failed(cut, ErrorKind::Truncated).partial;
    CHECK(partial.messages.size() == 1 && partial.messages[0].native && !partial.messages[0].native->complete());
    CHECK(media_of(partial.messages).empty());
    auto unsealed = next;
    unsealed.canonical_messages[1] = partial.messages[0];
    const auto refused = chat::encode(desc(), unsealed, true);
    CHECK(std::holds_alternative<Error>(refused) && std::get<Error>(refused).kind == ErrorKind::ReplayIneligible);
  }
  const auto abnormal = stream(wires, context(desc(), "pcm16", true), {false, ErrorKind::Cancelled});
  const auto& cancelled = failed(abnormal, ErrorKind::Cancelled).partial;
  CHECK(cancelled.messages[0].native && !cancelled.messages[0].native->complete());
  CHECK(cancelled.usage.stage == UsageStage::Partial);

  // Transcript is optional for audio-only output; expiry may precede the final usage.
  chat::Request audio_only;
  audio_only.model = "fixture-model";
  audio_only.messages.push_back({Role::User, "speak"});
  audio_only.modalities = {chat::OutputModality::Audio};
  audio_only.audio = chat::AudioOutput{"alloy", "pcm16"};
  const auto audio_request = chat::encode(desc(), audio_only, true);
  CHECK(std::holds_alternative<chat::EncodedRequest>(audio_request));
  const auto silent_transcript = stream({chunk(audio_delta(R"("id":"audio_fixture","data":"QUJD")", true)),
                                        chunk(audio_delta(R"("expires_at":0)")), usage},
                                       std::get<chat::EncodedRequest>(audio_request).context);
  expect_media(parts_of(completed(silent_transcript))[0], Expect{MediaKind::Audio, "audio/l16", MediaSource::Inline,
                                                              "QUJD", {}, {}, "audio_fixture", {}});
  // A null-reason expiry also remains valid metadata after a real finish reason and usage.
  const auto explicit_stop = stream({chunk(audio_delta(R"("id":"audio_fixture","data":"QUJD")", true)),
                                    chunk("{}", "\"stop\""), usage, chunk(audio_delta(R"("expires_at":9)"))},
                                   context(desc(), "pcm16", true));
  CHECK(completed(explicit_stop).stop.raw == "stop" && parts_of(completed(explicit_stop)).size() == 1);
  const auto null_reason_tail = stream({chunk(audio_delta(R"("id":"audio_fixture","data":"QUJD")", true)),
                                       usage, chunk(audio_delta(R"("expires_at":9)"))},
                                      context(desc(), "pcm16", true));
  CHECK(completed(null_reason_tail).stop.kind == StopKind::EndTurn && parts_of(completed(null_reason_tail)).size() == 1);
}
void audio_usage_terminal_failures() {
  const auto initial = chunk(audio_delta(R"("id":"audio_fixture","data":"QUJD","transcript":"words")", true));
  const auto usage = wrap(R"("choices":[],"usage":{"prompt_tokens":1,"completion_tokens":2,"total_tokens":3})", true);
  const auto expiry = chunk(audio_delta(R"("expires_at":9)"));
  const auto ctx = [] { return context(desc(), "pcm16", true); };
  const auto missing_reason = [](std::string_view delta) {
    return wrap("\"choices\":[{\"index\":0,\"delta\":" + std::string(delta) + "}]", true);
  };
  // Absent finish_reason is accepted only for the complete usage-terminated audio expiry.
  for (const auto& wires : std::vector<std::vector<std::string>>{
      {missing_reason(R"({"content":"text"})")},
      {missing_reason(audio_delta(R"("id":"audio_fixture","data":"QUJD")", true))},
      {initial, missing_reason(audio_delta(R"("expires_at":9)"))},
      {initial, usage, missing_reason("{}")},
      {initial, usage, missing_reason(R"({"content":"late"})")},
      {initial, usage, missing_reason(audio_delta(R"("expires_at":9,"data":"REVG")"))},
      {initial, usage, missing_reason(audio_delta(R"("id":"audio_fixture")"))},
      {chunk(audio_delta(R"("id":"audio_fixture","data":"AQ")", true)), usage,
       missing_reason(audio_delta(R"("expires_at":9)"))}}) {
    const auto outcome = stream(wires, ctx());
    const auto& partial = failed(outcome, ErrorKind::ProtocolCorrupt).partial;
    CHECK(media_of(partial.messages).empty());
  }
  for (const auto& incomplete : std::vector<std::vector<std::string>>{
      {initial, usage}, {initial, expiry}, {chunk(R"({"role":"assistant","content":"text"})"), usage},
      {chunk(audio_delta(R"("id":"audio_fixture","transcript":"words")", true)), usage, expiry},
      {chunk(audio_delta(R"("id":"audio_fixture","data":"AQ")", true)), usage, expiry}}) {
    const auto outcome = stream(incomplete, ctx());
    const auto& partial = failed(outcome, ErrorKind::ProtocolCorrupt).partial;
    CHECK(media_of(partial.messages).empty());
    CHECK(!partial.messages.empty() && partial.messages[0].native && !partial.messages[0].native->complete());
  }
  // Final audio usage is a semantic barrier even before the expiry arrives.
  for (const char* late : {R"({"audio":{"data":"REVG"}})", R"({"audio":{"id":"audio_fixture"}})",
                          R"({"audio":{"id":"different_audio"}})", R"({"audio":{"transcript":"late"}})",
                          R"({"content":"late"})", R"({"reasoning":"late"})", R"({"refusal":"late"})",
                          R"({"tool_calls":[]})"}) {
    for (bool after_expiry : {false, true}) {
      std::vector<std::string> wires{initial, usage};
      if (after_expiry) wires.push_back(expiry);
      wires.push_back(chunk(late));
      const auto outcome = stream(wires, ctx());
      const auto& partial = failed(outcome, ErrorKind::ProtocolCorrupt).partial;
      CHECK(media_of(partial.messages).empty());
      CHECK(partial.messages[0].native && !partial.messages[0].native->complete());
    }
  }
  for (const char* invalid_expiry : {"null", "-1", "1.5", "true", "\"soon\""}) {
    failed(stream({initial, usage, chunk(audio_delta("\"expires_at\":" + std::string(invalid_expiry)))}, ctx()),
           ErrorKind::ProtocolCorrupt);
  }
  failed(stream({initial, chunk(audio_delta(R"("id":"different_audio")")), usage, expiry}, ctx()), ErrorKind::ProtocolCorrupt);
  failed(stream({initial, usage, chunk(audio_delta(R"("expires_at":9,"data":"REVG")"))}, ctx()), ErrorKind::ProtocolCorrupt);
  failed(stream({initial, usage, expiry, "[DONE]", "[DONE]"}, ctx(), {}, {}, desc(), false), ErrorKind::ProtocolCorrupt);
  failed(stream({initial, usage, expiry, "[DONE]", wrap(R"("usage":null,"obfuscation":"padding")", true)},
                ctx(), {}, {}, desc(), false), ErrorKind::ProtocolCorrupt);
  // Audio metadata cannot replace the required terminal reason for a tool invocation.
  failed(stream({initial, chunk(R"({"tool_calls":[{"index":0,"id":"call_fixture","type":"function","function":{"name":"f","arguments":"{}"}}]})"),
                 usage, expiry}, ctx()), ErrorKind::ProtocolCorrupt);
}
void audio_malformed() {
  const auto ctx = [] { return context(desc(), "wav", false); };
  const auto sctx = [] { return context(desc(), "wav", true); };
  const auto message = [](std::string_view audio) { return body("{\"role\":\"assistant\",\"content\":null,\"audio\":" + std::string(audio) + "}"); };
  for (const char* data : {"QQ", "QR==", "QUJD\\nREVG", "QUJD REVG", "QU-_", "====", "QUJDRA=", "QUJ$"})
    failed(buffered(message("{\"id\":\"a\",\"data\":\"" + std::string(data) + "\"}"), ctx()), ErrorKind::ProtocolCorrupt);
  // An audio object that never carries bytes is not an audio reply.
  failed(buffered(message(R"({"id":"a","transcript":"words only"})"), ctx()), ErrorKind::ProtocolCorrupt);
  failed(buffered(message(R"({"id":"a","data":""})"), ctx()), ErrorKind::ProtocolCorrupt);
  failed(buffered(message(R"({"expires_at":1})"), ctx()), ErrorKind::ProtocolCorrupt);
  // Wrong field types.
  for (const char* audio : {R"({"id":"a","data":12})", R"({"id":7,"data":"QUJD"})", R"({"id":"a","data":"QUJD","transcript":5})",
                            R"({"id":"a","data":"QUJD","expires_at":"soon"})", R"({"id":"a","data":"QUJD","expires_at":-1})",
                            R"({"id":"a","data":["QUJD"]})", R"("QUJD")", R"([])", "true"})
    failed(buffered(message(audio), ctx()), ErrorKind::ProtocolCorrupt);
  // Null audio is simply no audio.
  const auto none = completed(buffered(message("null"), ctx()));
  CHECK(media_of(none.messages).empty());
  // A changed identity inside one reply is a different reply glued together.
  failed(stream(deltas_then_stop({audio_delta("\"id\":\"a1\",\"data\":\"QUJD\"", true), audio_delta("\"id\":\"a2\",\"data\":\"REVG\"")}), sctx()), ErrorKind::ProtocolCorrupt);
  // A corrupt chunk fails the stream; the partial result carries no payload from it.
  const auto outcome = stream({chunk(audio_delta("\"id\":\"a1\",\"data\":\"QUJD\"", true)), chunk(audio_delta("\"data\":\"QR==\""))}, sctx());
  const auto& failure = failed(outcome, ErrorKind::ProtocolCorrupt);
  for (const auto* m : media_of(failure.partial.messages)) CHECK(!m->data);
  // A stream that ends before the finish reason never exposes the audio bytes it saw as media.
  const auto cut = stream({chunk(audio_delta("\"id\":\"a1\",\"data\":\"QUJD\"", true))}, sctx(), {false, ErrorKind::Cancelled}, {}, desc(), false);
  const auto& cancelled = failed(cut, ErrorKind::Cancelled);
  for (const auto* m : media_of(cancelled.partial.messages)) CHECK(!m->data);
  // Unknown additive fields are tolerated and only counted.
  Accumulator accumulator;
  chat::Codec codec(desc(), chat::Mode::Buffered, accumulator, {}, ctx());
  CHECK(codec.buffered(message(R"({"id":"a","data":"QUJD","expires_at":3,"voice":"alloy","future":{"x":1}})"), {}));
  CHECK(completed(*accumulator.outcome()).messages.size() == 1 && codec.diagnostics().unknown_properties >= 2);
}

void images_buffered_and_streamed() {
  const auto png = b64(bytes_of(40, 7)), jpeg = b64(bytes_of(41, 8));
  const std::string url = "https://cdn.example.com/images/gen-9.png?sig=a%20b&x=1";
  const auto message = "{\"role\":\"assistant\",\"content\":\"Here you go\",\"images\":[{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64," + png +
                       "\"},\"index\":0},{\"type\":\"image_url\",\"image_url\":{\"url\":" + json::quote(url) + "},\"index\":1},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64," + jpeg + "\"},\"index\":2}]}";
  for (const auto* d : {&router(), &desc()}) {
    const auto outcome = buffered(body(message), {}, {}, *d);
    const auto& parts = parts_of(completed(outcome));
    CHECK(parts.size() == 4 && is_text(parts[0], "Here you go"));
    expect_media(parts[1], Expect{MediaKind::Image, "image/png", MediaSource::Inline, png});
    expect_media(parts[2], Expect{MediaKind::Image, {}, MediaSource::Url, {}, url, {}, {}, {}, false});
    CHECK(std::get<Media>(parts[2]).mime.empty() && url_syntax(std::get<Media>(parts[2]).reference));
    expect_media(parts[3], Expect{MediaKind::Image, "image/jpeg", MediaSource::Inline, jpeg});
  }
  // Streaming: OpenRouter repeats an indexed image in later deltas; it is still one image.
  const auto image = [&](unsigned index, const std::string& data_url) {
    return std::string("{\"images\":[{\"type\":\"image_url\",\"image_url\":{\"url\":") + json::quote(data_url) + "},\"index\":" + std::to_string(index) + "}]}";
  };
  const auto first = "data:image/png;base64," + png, second = "data:image/jpeg;base64," + jpeg;
  const auto outcome = stream(deltas_then_stop({R"({"role":"assistant","content":"Here "})", image(0, first), R"({"content":"you go"})", image(0, first), image(1, second), image(1, second), image(0, first)}),
                              {}, {}, {}, router());
  const auto& parts = parts_of(completed(outcome));
  CHECK(parts.size() == 3 && is_text(parts[0], "Here you go"));
  expect_media(parts[1], Expect{MediaKind::Image, "image/png", MediaSource::Inline, png});
  expect_media(parts[2], Expect{MediaKind::Image, "image/jpeg", MediaSource::Inline, jpeg});
  // Both entries in one delta array also keep their order.
  const auto both = stream(deltas_then_stop({R"({"role":"assistant","images":[{"type":"image_url","image_url":{"url":")" + second + R"("},"index":0},{"type":"image_url","image_url":{"url":")" + first + R"("},"index":1}]})"}),
                           {}, {}, {}, router());
  const auto& ordered = parts_of(completed(both));
  CHECK(ordered.size() == 2);
  expect_media(ordered[0], Expect{MediaKind::Image, "image/jpeg", MediaSource::Inline, jpeg});
  expect_media(ordered[1], Expect{MediaKind::Image, "image/png", MediaSource::Inline, png});
  // Entries without an index carry no identity to dedupe on: each one is an image.
  const auto unindexed = "{\"images\":[{\"type\":\"image_url\",\"image_url\":{\"url\":\"" + first + "\"}}]}";
  const auto plain = completed(stream(deltas_then_stop({unindexed, unindexed}), {}, {}, {}, router()));
  CHECK(media_of(plain.messages).size() == 2);
  // The same data URL under different indices is two images.
  const auto twice = completed(buffered(body("{\"role\":\"assistant\",\"content\":null,\"images\":[{\"type\":\"image_url\",\"image_url\":{\"url\":\"" + first + "\"},\"index\":0},{\"type\":\"image_url\",\"image_url\":{\"url\":\"" + first + "\"},\"index\":1}]}"), {}, {}, router()));
  CHECK(media_of(twice.messages).size() == 2);
  failed(stream(deltas_then_stop({image(0, first), image(0, second)}), {}, {}, {}, router()), ErrorKind::ProtocolCorrupt);
  // Audio keeps its first-observed position even though it seals at normal close.
  const auto mixed = stream(deltas_then_stop({audio_delta("\"data\":\"QQ==\"", true), image(0, first)}),
                            context(router(), "wav", true), {}, {}, router());
  const auto& m = parts_of(completed(mixed));
  CHECK(m.size() == 2 && std::get<Media>(m[0]).kind == MediaKind::Audio && std::get<Media>(m[1]).kind == MediaKind::Image);
}
void images_malformed() {
  const auto png = b64(bytes_of(12, 2));
  const auto reply = [](std::string_view images) { return body("{\"role\":\"assistant\",\"content\":null,\"images\":" + std::string(images) + "}"); };
  const auto entry = [](std::string_view url) { return "[{\"type\":\"image_url\",\"image_url\":{\"url\":" + json::quote(url) + "},\"index\":0}]"; };
  // Payload and location problems are corruption.
  const std::vector<std::string> urls{"data:image/png;base64,QQ", "data:image/png;base64,", "data:image/png;base64,QR==", "data:image/png;base64,QUJD REVG",
                                "data:;base64," + png, "data:image/png," + png, "image/png;base64," + png, "ftp://example.com/x.png", "file:///etc/passwd",
                                "https://example.com/a b.png", "https://example.com/a\"b.png", "https://example.com/a\\b.png", "https://", "/relative/path.png"};
  for (const auto& url : urls)
    failed(buffered(reply(entry(url)), {}, {}, router()), ErrorKind::ProtocolCorrupt);
  // Shape problems.
  for (const char* images : {R"([{"type":"image_url","index":0}])", R"([{"type":"image_url","image_url":{"url":""},"index":0}])", R"([{"type":"image_url","image_url":{"url":5},"index":0}])",
                             R"([{"type":"image_url","image_url":"https://example.com/x.png","index":0}])", R"([{"type":"image_url","image_url":{"url":"https://example.com/x.png"},"index":-1}])",
                             R"([{"type":"image_url","image_url":{"url":"https://example.com/x.png"},"index":"0"}])", R"(["https://example.com/x.png"])", R"({"type":"image_url"})", R"("x")"})
    failed(buffered(reply(images), {}, {}, router()), ErrorKind::ProtocolCorrupt);
  // A generated entry of another type is not an image this decoder understands.
  failed(buffered(reply(R"([{"type":"video_url","image_url":{"url":"https://example.com/x.mp4"},"index":0}])"), {}, {}, router()), ErrorKind::Unsupported);
  failed(buffered(reply(R"([{"type":7,"image_url":{"url":"https://example.com/x.png"}}])"), {}, {}, router()), ErrorKind::Unsupported);
  // null and empty arrays are no images.
  CHECK(media_of(completed(buffered(reply("null"), {}, {}, router())).messages).empty());
  CHECK(media_of(completed(buffered(reply("[]"), {}, {}, router())).messages).empty());
  // A bad entry after a good one fails the whole reply and the partial result carries no payload from the bad one.
  const auto bad = stream(deltas_then_stop({R"({"role":"assistant","images":[{"type":"image_url","image_url":{"url":"data:image/png;base64,QR=="},"index":0}]})"}), {}, {}, {}, router());
  for (const auto* m : media_of(failed(bad, ErrorKind::ProtocolCorrupt).partial.messages)) CHECK(!m->data);
  // Unknown fields tolerated.
  Accumulator accumulator;
  chat::Codec codec(router(), chat::Mode::Buffered, accumulator, {}, nullptr);
  CHECK(codec.buffered(reply("[{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64," + png + "\",\"detail\":\"high\"},\"index\":0,\"revised_prompt\":\"x\"}]"), {}));
  CHECK(media_of(completed(*accumulator.outcome()).messages).size() == 1 && codec.diagnostics().unknown_properties >= 1);
}
void content_arrays_and_image_history() {
  // A replayed image is admitted structurally like user input, so it carries a real PNG header.
  const std::string png_bytes = std::string("\x89PNG\r\n\x1a\n", 8) + "generated";
  const std::string png = b64(png_bytes);
  const std::string image = R"({"type":"image_url","image_url":{"url":"data:image/png;base64,)" + png + R"("}})";
  const std::string content = R"([{"type":"text","text":"before"},)" + image + R"(,{"type":"text","text":"after"}])";
  const std::string array_message = R"({"role":"assistant","content":)" + content + "}";
  const std::string images_message = R"({"role":"assistant","content":"before","images":[)" + image + "]}";
  // The documented array is ordered, including text on either side of an image.
  for (const auto& outcome : {buffered(body(array_message), {}, {}, router()),
                             stream(deltas_then_stop({array_message}), {}, {}, {}, router())}) {
    const auto& parts = parts_of(completed(outcome));
    CHECK(parts.size() == 3 && is_text(parts[0], "before") && is_text(parts[2], "after"));
    expect_media(parts[1], Expect{MediaKind::Image, "image/png", MediaSource::Inline, png});
    CHECK(completed(outcome).raw_events.front().payload);
  }
  // Arrays, the separate audio channel, and images[] share arrival ordering.
  const std::string mixed = R"({"role":"assistant","content":)" + content +
      R"(,"audio":{"id":"a","data":"Qg==","transcript":"spoken"},"images":[)" + image + "]}";
  for (const auto& outcome : {buffered(body(mixed), context(router(), "wav", false), {}, router()),
                             stream(deltas_then_stop({mixed}), context(router(), "wav", true), {}, {}, router())}) {
    const auto& parts = parts_of(completed(outcome));
    CHECK(parts.size() == 5 && is_text(parts[0], "before") && is_text(parts[2], "after"));
    expect_media(parts[3], Expect{MediaKind::Audio, "audio/wav", MediaSource::Inline, "Qg==", {}, {}, "a", "spoken"});
    expect_media(parts[4], Expect{MediaKind::Image, "image/png", MediaSource::Inline, png});
    chat::Request next;
    next.model = "fixture-model";
    next.modalities = {chat::OutputModality::Text, chat::OutputModality::Audio};
    next.audio = chat::AudioOutput{"alloy", "wav"};
    next.canonical_messages = {Message{"", Role::User, {Text{"speak"}}}, completed(outcome).messages[0],
                               Message{"", Role::User, {Text{"edit the image"}}}};
    const auto encoded = chat::encode(router(), next, false);
    CHECK(std::holds_alternative<chat::EncodedRequest>(encoded));
    const auto request = parse_doc(std::get<chat::EncodedRequest>(encoded).body);
    const auto assistant = request->root().get("messages").at(1);
    CHECK(assistant.get("audio").get("id").as_string() == "a" && !assistant.get("audio").get("data").valid());
    CHECK(assistant.get("content").size() == 4);
    for (auto part : assistant.get("content").elements()) CHECK(part.get("type").as_string() != "input_audio");
  }
  const auto audio_first = stream(deltas_then_stop({audio_delta("\"data\":\"Qg==\"", true), array_message}),
                                  context(router(), "wav", true), {}, {}, router());
  CHECK(std::get<Media>(parts_of(completed(audio_first))[0]).kind == MediaKind::Audio);
  const auto scalar_tail = stream(deltas_then_stop({array_message, R"({"content":"tail"})"}), {}, {}, {}, router());
  CHECK(is_text(parts_of(completed(scalar_tail))[3], "tail"));
  const auto high = buffered(body(R"({"role":"assistant","content":[{"type":"image_url","image_url":{"url":"https://example.com/image.png","detail":"high"}}]})"), {}, {}, router());
  CHECK(std::get<Media>(parts_of(completed(high))[0]).detail == ImageDetail::High);
  failed(buffered(body(array_message), {}, {}, desc()), ErrorKind::Unsupported);
  for (const auto* bad : {R"([{"type":"future","payload":"x"}])", R"([{"type":"input_audio","input_audio":{"data":"QQ==","format":"wav"}}])"}) {
    failed(buffered(body(R"({"role":"assistant","content":)" + std::string(bad) + "}"), {}, {}, router()), ErrorKind::Unsupported);
    failed(stream(deltas_then_stop({R"({"content":)" + std::string(bad) + "}"}), {}, {}, {}, router()), ErrorKind::Unsupported);
  }
  for (const auto* bad : {R"([{"type":"text","text":7}])", R"([{"text":"x"}])", R"([{"type":"image_url","image_url":{}}])", R"([7])"})
    failed(buffered(body(R"({"role":"assistant","content":)" + std::string(bad) + "}"), {}, {}, router()), ErrorKind::ProtocolCorrupt);
  failed(buffered(body(array_message), {}, tight_parts(2), router()), ErrorKind::ResourceLimit);
  for (const auto& message : {array_message, images_message}) for (bool sse : {false, true}) {
    chat::Request initial;
    initial.model = "fixture-model";
    initial.canonical_messages.push_back(Message{"", Role::User, {Text{"draw"}}});
    const auto encoded = chat::encode(router(), initial, sse);
    CHECK(std::holds_alternative<chat::EncodedRequest>(encoded));
    const auto ctx = std::get<chat::EncodedRequest>(encoded).context;
    const auto outcome = sse ? stream(deltas_then_stop({message}), ctx, {}, {}, router()) :
                               buffered(body(message), ctx, {}, router());
    const auto& assistant = completed(outcome).messages[0];
    CHECK(assistant.native && assistant.native->complete());
    auto next = initial;
    next.canonical_messages.push_back(assistant);
    next.canonical_messages.push_back(Message{"", Role::User, {Text{"make it blue"}}});
    const auto edit = chat::encode(router(), next, sse);
    CHECK(std::holds_alternative<chat::EncodedRequest>(edit));
    const auto wire = parse_doc(std::get<chat::EncodedRequest>(edit).body);
    const auto replay = wire->root().get("messages").at(1).get("content");
    CHECK(replay.is_array() && replay.at(1).get("type").as_string() == "image_url");
    CHECK(replay.at(1).get("image_url").get("url").as_string() == "data:image/png;base64," + png);
    CHECK(wire->root().get("messages").at(2).get("content").at(0).get("text").as_string() == "make it blue");
    auto changed = next;
    std::get<Text>(changed.canonical_messages[0].parts[0]).value = "different original prompt";
    const auto refused = chat::encode(router(), changed, sse);
    CHECK(std::holds_alternative<Error>(refused) && std::get<Error>(refused).kind == ErrorKind::ReplayIneligible);
    changed = next;
    std::get<Media>(changed.canonical_messages[1].parts[1]).data = std::make_shared<const std::string>("Qg==");
    const auto tampered = chat::encode(router(), changed, sse);
    CHECK(std::holds_alternative<Error>(tampered) && std::get<Error>(tampered).kind == ErrorKind::ReplayIneligible);
    // Portable history admits the same image, but never manufactures a seal.
    next.canonical_messages[1].native.reset();
    const auto portable = chat::encode(router(), next, sse);
    CHECK(std::holds_alternative<chat::EncodedRequest>(portable));
    CHECK(!next.canonical_messages[1].native && !next.canonical_messages[1].wire_output);
    const auto direct = chat::encode(desc(), next, sse);
    CHECK(std::holds_alternative<Error>(direct) && std::get<Error>(direct).kind == ErrorKind::Unsupported);
  }
  chat::Request convenience;
  convenience.model = "fixture-model";
  convenience.messages.push_back({Role::User, "draw"});
  chat::InputMessage assistant{Role::Assistant, "before"};
  assistant.media.push_back(Media::image("image/png", std::make_shared<const std::string>(png)));
  convenience.messages.push_back(assistant);
  convenience.messages.push_back({Role::User, "make it blue"});
  const auto encoded = chat::encode(router(), convenience, false);
  CHECK(std::holds_alternative<chat::EncodedRequest>(encoded));
  const auto wire = parse_doc(std::get<chat::EncodedRequest>(encoded).body);
  CHECK(wire->root().get("messages").at(1).get("content").at(0).get("type").as_string() == "image_url");
  const auto direct = chat::encode(desc(), convenience, false);
  CHECK(std::holds_alternative<Error>(direct) && std::get<Error>(direct).kind == ErrorKind::Unsupported);
  convenience.messages[1].media[0].data = std::make_shared<const std::string>("QR==");
  const auto malformed = chat::encode(router(), convenience, false);
  CHECK(std::holds_alternative<Error>(malformed) && std::get<Error>(malformed).kind == ErrorKind::InvalidRequest);
}
void pdf_and_citation_annotations() {
  // https://openrouter.ai/docs/guides/overview/multimodal/pdfs.md
  const std::string annotations = R"([{"type":"file","file":{"hash":"abc","name":"document.pdf","content":[{"type":"text","text":"Parsed text"},{"type":"image_url","image_url":{"url":"data:image/png;base64,QQ=="}}]}},{"type":"url_citation","url_citation":{"url":"https://example.com/source","title":"Source","content":"Excerpt","start_index":0,"end_index":7}}])";
  const std::string message = R"({"role":"assistant","content":"Summary","annotations":)" + annotations + "}";
  chat::Request request;
  request.model = "fixture-model";
  chat::InputMessage input{Role::User, "Summarize this PDF"};
  input.media.push_back(Media::inline_data(MediaKind::Document, "application/pdf", std::make_shared<const std::string>("JVBERi0=")));
  request.messages.push_back(input);
  for (bool sse : {false, true}) {
    const auto encoded = chat::encode(router(), request, sse);
    CHECK(std::holds_alternative<chat::EncodedRequest>(encoded));
    const auto ctx = std::get<chat::EncodedRequest>(encoded).context;
    const auto outcome = sse ? stream(deltas_then_stop({message}), ctx, {}, {}, router()) :
                               buffered(body(message), ctx, {}, router());
    const auto& result = completed(outcome);
    CHECK(parts_of(result).size() == 1 && is_text(parts_of(result)[0], "Summary"));
    const auto retained = result.raw_events.front().payload->root().get("choices").at(0).get(sse ? "delta" : "message").get("annotations");
    CHECK(json::equal(retained, parse_doc(annotations)->root()));
  }
  // Filename is optional in the primary FileAnnotation schema.
  CHECK(std::holds_alternative<Completion>(buffered(body(R"({"role":"assistant","content":"Summary","annotations":[{"type":"file","file":{"hash":"abc","content":[]}}]})"), {}, {}, router())));
  failed(buffered(body(message), {}, {}, desc()), ErrorKind::Unsupported);
  const auto citation = R"({"role":"assistant","content":"Summary","annotations":[{"type":"url_citation","url_citation":{"url":"https://example.com","title":"Source","start_index":0,"end_index":7}}]})";
  CHECK(std::holds_alternative<Completion>(buffered(body(citation))));
  failed(buffered(body(message), {}, tight_parts(1), router()), ErrorKind::ResourceLimit);
  failed(buffered(body(message), {}, tight_bytes(256), router()), ErrorKind::ResourceLimit);
  for (const auto* bad : {R"([{"type":"file","file":{"hash":7,"content":[]}}])", R"([{"type":"file","file":{"hash":"x","name":7,"content":[]}}])",
                         R"([{"type":"file","file":{"hash":"x","content":[{"type":"text","text":7}]}}])",
                         R"([{"type":"url_citation","url_citation":{"url":"https://e.com","title":"x","start_index":4,"end_index":2}}])"}) {
    const auto malformed = R"({"role":"assistant","content":"Summary","annotations":)" + std::string(bad) + "}";
    failed(buffered(body(malformed), {}, {}, router()), ErrorKind::ProtocolCorrupt);
    failed(stream(deltas_then_stop({malformed}), {}, {}, {}, router()), ErrorKind::ProtocolCorrupt);
  }
  failed(buffered(body(R"({"role":"assistant","content":"Summary","annotations":[{"type":"future","value":"x"}]})"), {}, {}, router()), ErrorKind::Unsupported);
}
void typed_tool_media_is_not_chat_content() {
  chat::Request request;
  request.model = "fixture-model";
  request.canonical_messages.push_back(Message{"", Role::User, {Text{"look"}}});
  ToolCall call{"call", "look", ToolCallKind::ClientExecuted, parse_doc("{}")};
  request.canonical_messages.push_back(Message{"", Role::Assistant, {call}});
  ToolResult result{"call", ""};
  result.content_parts.emplace_back(Media::image("image/png", std::make_shared<const std::string>("QQ==")));
  request.canonical_messages.push_back(Message{"", Role::Tool, {result}});
  const auto unsupported = chat::encode(router(), request, false);
  CHECK(std::holds_alternative<Error>(unsupported) && std::get<Error>(unsupported).kind == ErrorKind::Unsupported);
  std::get<ToolResult>(request.canonical_messages.back().parts[0]).content_parts.clear();
  std::get<ToolResult>(request.canonical_messages.back().parts[0]).content = "legacy result";
  const auto legacy = chat::encode(router(), request, false);
  CHECK(std::holds_alternative<chat::EncodedRequest>(legacy));
  const auto wire = parse_doc(std::get<chat::EncodedRequest>(legacy).body);
  CHECK(wire->root().get("messages").at(2).get("content").as_string() == "legacy result");
}
void chat_limits() {
  const auto big = b64(bytes_of(3000, 2)), small = b64(bytes_of(9, 2));
  const auto reply = [](std::string_view audio) { return body("{\"role\":\"assistant\",\"content\":null,\"audio\":{\"id\":\"a\",\"data\":\"" + std::string(audio) + "\"}}"); };
  CHECK(std::holds_alternative<Completion>(buffered(reply(small), context(desc(), "wav", false), tight_bytes(4096))));
  failed(buffered(reply(big), context(desc(), "wav", false), tight_bytes(4096)), ErrorKind::ResourceLimit);
  // Many small chunks whose total outgrows the limit.
  std::vector<std::string> wires;
  for (int i = 0; i < 40; ++i) wires.push_back(chunk(audio_delta("\"data\":\"" + b64(bytes_of(90, i)) + "\"", i == 0)));
  wires.push_back(chunk("{}", "\"stop\""));
  failed(stream(wires, context(desc(), "wav", true), {}, tight_bytes(2048)), ErrorKind::ResourceLimit);
  // Part count: three images into a two-part budget.
  const auto images = "{\"role\":\"assistant\",\"content\":null,\"images\":[{\"type\":\"image_url\",\"image_url\":{\"url\":\"https://e.com/1.png\"}},{\"type\":\"image_url\",\"image_url\":{\"url\":\"https://e.com/2.png\"}},{\"type\":\"image_url\",\"image_url\":{\"url\":\"https://e.com/3.png\"}}]}";
  failed(buffered(body(images), {}, tight_parts(2), router()), ErrorKind::ResourceLimit);
  CHECK(media_of(completed(buffered(body(images), {}, tight_parts(3), router())).messages).size() == 3);
}
} // namespace chat_fixture

namespace responses_fixture {
const descriptor::ValidatedDescriptor& desc() {
  static const auto d = load_descriptor(descriptor_text("media-responses", "openai.responses", "http://127.0.0.1:18080", "/v1/responses", "/v1/responses"));
  return d;
}
responses::Request request() {
  responses::Request r; r.model = "fixture-model"; r.messages.push_back(Message{"", Role::User, {Text{"draw"}}});
  r.hosted_tools.push_back(responses::ImageGenerationTool{}); return r;
}
std::shared_ptr<const NativeContext> context() {
  auto e = responses::encode(desc(), request(), true); CHECK(std::holds_alternative<responses::EncodedRequest>(e));
  return std::get<responses::EncodedRequest>(e).context;
}
std::string body(std::string_view output, std::string_view status = "completed") {
  return "{\"id\":\"r1\",\"object\":\"response\",\"created_at\":1,\"model\":\"fixture-model\",\"status\":" + json::quote(status) +
      ",\"output\":" + std::string(output) + ",\"usage\":null,\"error\":null}";
}
Frame frame(std::string type, std::string fields) { return {type, "{\"type\":" + json::quote(type) + fields + "}"}; }
Outcome buffered(std::string_view wire, SemanticLimits limits = {}) {
  Accumulator a(limits); responses::Codec c(desc(), responses::Mode::Buffered, a, context(), limits);
  c.buffered(wire, {}); CHECK(a.outcome()); return *a.outcome();
}
Outcome stream(std::string_view item, std::string_view final_item, bool preview = true, responses::Close close = {},
               std::string_view initial_item = R"({"id":"ig1","type":"image_generation_call","status":"in_progress","result":null})") {
  Accumulator a; responses::Codec c(desc(), responses::Mode::Sse, a, context());
  std::vector<Frame> frames{
    frame("response.created", ",\"response\":" + body("[]", "in_progress")),
    frame("response.output_item.added", ",\"output_index\":0,\"item\":" + std::string(initial_item))};
  if (preview) frames.push_back(frame("response.image_generation_call.partial_image", R"(,"output_index":0,"item_id":"ig1","partial_image_index":0,"partial_image_b64":"QR==")"));
  frames.push_back(frame("response.output_item.done", ",\"output_index\":0,\"item\":" + std::string(item)));
  frames.push_back(frame("response.completed", ",\"response\":" + body("[" + std::string(final_item) + "]")));
  for (const auto& [name, bytes] : frames) if (!c.frame(name, bytes)) break;
  c.finish(close); CHECK(a.outcome()); return *a.outcome();
}
void generated_images() {
  const auto payload = b64(bytes_of(61));
  for (const auto& [format, mime] : std::vector<std::pair<std::string, std::string>>{{"png", "image/png"}, {"jpeg", "image/jpeg"}, {"webp", "image/webp"}, {"future", ""}, {"", ""}}) {
    const auto item = R"({"id":"ig1","type":"image_generation_call","status":"completed","result":)" + json::quote(payload) +
        (format.empty() ? "" : ",\"output_format\":" + json::quote(format)) + "}";
    for (auto outcome : {buffered(body("[" + item + "]")), stream(item, item)}) {
      const auto& completion = completed(outcome); const auto& parts = parts_of(completion);
      CHECK(parts.size() == 1);
      expect_media(parts[0], Expect{MediaKind::Image, mime, MediaSource::Inline, payload, {}, {}, "ig1"});
      CHECK(completion.messages[0].native && completion.messages[0].native->complete());
      CHECK(json::equal(completion.messages[0].wire_output->root().at(0), parse_doc(item)->root()));
      CHECK(!completion.raw_events.empty());
      auto next = request(); next.messages.push_back(completion.messages[0]);
      auto replay = responses::encode(desc(), next, false); CHECK(std::holds_alternative<responses::EncodedRequest>(replay));
      CHECK(json::equal(parse_doc(std::get<responses::EncodedRequest>(replay).body)->root().get("input").at(1), parse_doc(item)->root()));
    }
  }
  for (const char* status : {"failed", "incomplete", "generating"}) {
    const auto item = R"({"id":"ig1","type":"image_generation_call","status":)" + json::quote(status) + R"(,"result":"QR==","output_format":"png"})";
    const auto outcome = buffered(body("[" + item + "]"));
    const auto& parts = parts_of(completed(outcome));
    CHECK(parts.size() == 1 && std::holds_alternative<Opaque>(parts[0]));
    CHECK(json::equal(std::get<Opaque>(parts[0]).wire_metadata->root(), parse_doc(item)->root()));
  }
  const std::string good = R"({"id":"ig1","type":"image_generation_call","status":"completed","result":"QQ==","output_format":"png"})";
  const std::string changed = R"({"id":"ig1","type":"image_generation_call","status":"completed","result":"Qg==","output_format":"png"})";
  failed(stream(good, changed), ErrorKind::ProtocolCorrupt);
  failed(stream(changed, changed, false, {}, good), ErrorKind::ProtocolCorrupt);
  const auto missing_status = buffered(body(R"([{"id":"ig1","type":"image_generation_call","result":"QQ=="}])"));
  CHECK(std::holds_alternative<Opaque>(parts_of(completed(missing_status))[0]));
  failed(buffered(body("[" + good + "]"), tight_parts(0)), ErrorKind::ResourceLimit);
  const auto interrupted = stream(good, good, true, {false, ErrorKind::Cancelled});
  failed(interrupted, ErrorKind::Cancelled);
}
} // namespace responses_fixture

namespace google_fixture {
const descriptor::ValidatedDescriptor& gemini_desc() {
  static const auto d = load_descriptor(descriptor_text("media-gemini", "google.generate", "http://127.0.0.1:18080",
      "/v1beta/models/fixture-model:generateContent", "/v1beta/models/fixture-model:streamGenerateContent?alt=sse"));
  return d;
}
const descriptor::ValidatedDescriptor& interactions_desc() {
  static const auto d = load_descriptor(descriptor_text("media-interactions", "google.interactions", "http://127.0.0.1:18080", "/v1beta/interactions", "/v1beta/interactions"));
  return d;
}
gemini::Request gemini_request() {
  gemini::Request r; r.model = "fixture-model"; r.messages.push_back(Message{"", Role::User, {Text{"draw"}}}); return r;
}
interactions::Request interactions_request() {
  interactions::Request r; r.model = "fixture-model"; r.messages.push_back(Message{"", Role::User, {Text{"draw"}}}); return r;
}
std::string gemini_body(std::string_view parts, bool final = true) {
  return "{\"responseId\":\"g1\",\"modelVersion\":\"fixture-model\",\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":" +
      std::string(parts) + "}" + (final ? ",\"finishReason\":\"STOP\"" : "") + "}]}";
}
Outcome gemini_decode(std::string_view wire, bool streamed, SemanticLimits limits = {}) {
  auto encoded = gemini::encode(gemini_desc(), gemini_request(), streamed); CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  Accumulator a(limits); gemini::Codec c(gemini_desc(), streamed ? gemini::Mode::Sse : gemini::Mode::Buffered, a,
                                        std::get<gemini::EncodedRequest>(encoded).context, limits);
  if (streamed) { c.frame({}, wire); c.finish(); } else c.buffered(wire, {});
  CHECK(a.outcome()); return *a.outcome();
}
std::string resource(std::string_view steps) {
  return "{\"id\":\"i1\",\"model\":\"fixture-model\",\"status\":\"completed\",\"steps\":" + std::string(steps) + "}";
}
Frame interaction_frame(std::string name, std::string fields) { return {name, "{\"event_type\":" + json::quote(name) + fields + "}"}; }
Outcome interactions_decode(std::string_view steps, const std::vector<Frame>& deltas = {}, bool streamed = false, SemanticLimits limits = {}) {
  auto encoded = interactions::encode(interactions_desc(), interactions_request(), streamed);
  CHECK(std::holds_alternative<interactions::EncodedRequest>(encoded));
  Accumulator a(limits); interactions::Codec c(interactions_desc(), streamed ? interactions::Mode::Sse : interactions::Mode::Buffered,
      a, std::get<interactions::EncodedRequest>(encoded).context, limits);
  if (!streamed) c.buffered(resource(steps), {});
  else {
    std::vector<Frame> frames{interaction_frame("interaction.created", R"(,"interaction":{"id":"i1","model":"fixture-model","status":"in_progress"})")};
    frames.insert(frames.end(), deltas.begin(), deltas.end());
    const auto final_resource = steps.empty() ? R"({"id":"i1","model":"fixture-model","status":"completed"})" : resource(steps);
    frames.push_back(interaction_frame("interaction.completed", ",\"interaction\":" + final_resource));
    for (const auto& [name, bytes] : frames) if (!c.frame(name, bytes)) break;
    c.finish();
  }
  CHECK(a.outcome()); return *a.outcome();
}
void gemini_generated() {
  const auto payload = b64(bytes_of(43));
  const auto native = R"([{"text":"before"},{"inlineData":{"mimeType":"image/png","data":)" + json::quote(payload) +
      R"(},"thought":true,"thoughtSignature":"opaque"},{"inlineData":{"mimeType":"audio/L16;rate=24000","data":)" + json::quote(payload) +
      R"(}},{"text":"after"},{"fileData":{"fileUri":"https://cdn.example/x.mp4","mimeType":"video/mp4"}}])";
  for (bool sse : {false, true}) {
    const auto outcome = gemini_decode(gemini_body(native), sse);
    const auto& completion = completed(outcome); const auto& parts = parts_of(completion);
    CHECK(parts.size() == 5 && is_text(parts[0], "before") && std::holds_alternative<Opaque>(parts[1]) && is_text(parts[3], "after"));
    expect_media(parts[2], Expect{MediaKind::Audio, "audio/L16;rate=24000", MediaSource::Inline, payload});
    expect_media(parts[4], Expect{MediaKind::Video, "video/mp4", MediaSource::File, {}, "https://cdn.example/x.mp4"});
    CHECK(json::equal(completion.messages[0].wire_output->root(), parse_doc(native)->root()));
    auto next = gemini_request(); next.messages.push_back(completion.messages[0]);
    auto replay = gemini::encode(gemini_desc(), next, sse); CHECK(std::holds_alternative<gemini::EncodedRequest>(replay));
    CHECK(json::equal(parse_doc(std::get<gemini::EncodedRequest>(replay).body)->root().get("contents").at(1).get("parts"), parse_doc(native)->root()));
  }
  failed(gemini_decode(gemini_body(R"([{"inlineData":{"mimeType":"image/png","data":"QR=="}}])"), false), ErrorKind::ProtocolCorrupt);
  failed(gemini_decode(gemini_body(native), true, tight_parts(2)), ErrorKind::ResourceLimit);
}
void interactions_generated() {
  const std::string image = R"({"type":"image","mime_type":"image/png","data":"QQ==","resolution":"high"})";
  const std::string thought = R"({"type":"thought","signature":"sig","summary":[{"type":"text","text":"consider"},)" + image + "]}";
  const std::string model = R"({"type":"model_output","content":[{"type":"text","text":"before"},{"type":"audio","mime_type":"audio/wav","data":"QUJDREU="},{"type":"text","text":"after"},)" + image + "]}";
  const auto steps = "[" + thought + "," + model + "]";
  const std::vector<Frame> deltas{
    interaction_frame("step.start", R"(,"index":0,"step":{"type":"thought","summary":[]})"),
    interaction_frame("step.delta", R"(,"index":0,"delta":{"type":"thought_summary","content":{"type":"text","text":"consider"}})"),
    interaction_frame("step.delta", R"(,"index":0,"delta":{"type":"thought_summary","content":)" + image + "}"),
    interaction_frame("step.stop", R"(,"index":0)"),
    interaction_frame("step.start", R"(,"index":1,"step":{"type":"model_output","content":[]})"),
    interaction_frame("step.delta", R"(,"index":1,"delta":{"type":"text","text":"before"})"),
    interaction_frame("step.delta", R"(,"index":1,"delta":{"type":"audio","data":"QQ=="})"),
    interaction_frame("step.delta", R"(,"index":1,"delta":{"type":"audio","data":"QkM="})"),
    interaction_frame("step.delta", R"(,"index":1,"delta":{"type":"audio","data":"REU="})"),
    interaction_frame("step.delta", R"(,"index":1,"delta":{"type":"text","text":"after"})"),
    interaction_frame("step.delta", R"(,"index":1,"delta":)" + image),
    interaction_frame("step.stop", R"(,"index":1)")};
  for (auto outcome : {interactions_decode(steps), interactions_decode(steps, deltas, true)}) {
    const auto& completion = completed(outcome); const auto& parts = parts_of(completion);
    CHECK(parts.size() == 5 && std::get<Thought>(parts[0]).summary == std::vector<std::string>{"consider"});
    CHECK(is_text(parts[1], "before") && is_text(parts[3], "after"));
    expect_media(parts[2], Expect{MediaKind::Audio, "audio/wav", MediaSource::Inline, "QUJDREU="});
    expect_media(parts[4], Expect{MediaKind::Image, "image/png", MediaSource::Inline, "QQ=="});
    CHECK(json::equal(completion.messages[0].wire_output->root(), parse_doc(steps)->root()));
    auto next = interactions_request(); next.messages.push_back(completion.messages[0]);
    auto replay = interactions::encode(interactions_desc(), next, false); CHECK(std::holds_alternative<interactions::EncodedRequest>(replay));
    const auto input = parse_doc(std::get<interactions::EncodedRequest>(replay).body);
    CHECK(json::equal(input->root().get("input").at(1), parse_doc(thought)->root()));
  }
  const auto changed = "[" + thought + R"(,{"type":"model_output","content":[{"type":"text","text":"before"},{"type":"audio","mime_type":"audio/wav","data":"QUJDREY="},{"type":"text","text":"after"},)" + image + "]}]";
  failed(interactions_decode(changed, deltas, true), ErrorKind::ProtocolCorrupt);
  failed(interactions_decode(steps, deltas, true, tight_parts(3)), ErrorKind::ResourceLimit);
  const auto no_mime = interactions_decode(R"([{"type":"model_output","content":[{"type":"audio","data":"QQ=="}]}])");
  expect_media(parts_of(completed(no_mime))[0], Expect{MediaKind::Audio, "", MediaSource::Inline, "QQ=="});
  const auto altered_description = "[" + thought +
      R"(,{"type":"model_output","content":[{"type":"text","text":"before"},{"type":"audio","mime_type":"audio/wav","data":"QUJDREU="},{"type":"text","text":"after"},{"type":"image","mime_type":"image/png","data":"QQ==","resolution":"low"}]}])";
  failed(interactions_decode(altered_description, deltas, true), ErrorKind::ProtocolCorrupt);
  const auto altered_thought = R"([{"type":"thought","signature":"sig","summary":[{"type":"text","text":"consider"},{"type":"image","mime_type":"image/png","data":"Qg==","resolution":"high"}]},)" + model + "]";
  failed(interactions_decode(altered_thought, deltas, true), ErrorKind::ProtocolCorrupt);
}
void interactions_transcription_annotations() {
  const std::string first = R"({"type":"word_info","text":"Héllo","start_index":0,"end_index":6,"start_offset":"0.100s","end_offset":"0.450s","speaker":"spk_1","future":{"exact":true}})";
  const std::string second = R"({"type":"word_info","text":"world","start_index":7,"end_index":12,"start_offset":"0.500s","end_offset":"0.900s","speaker":"spk_2"})";
  const std::string citation = R"({"type":"url_citation","start_index":0,"end_index":12,"url":"https://example.invalid/source","title":"source"})";
  const std::string annotations = "[" + first + "," + second + "," + citation + "]";
  const std::string steps = R"([{"type":"model_output","content":[{"type":"text","text":"Héllo world","annotations":)" +
      annotations + R"(},{"type":"image","mime_type":"image/png","data":"QQ=="},{"type":"text","text":"after","annotations":[{"type":"speech_metadata","speaker":"spk_2","style":"calm","start_index":0,"end_index":5}]}]}])";
  const auto delta = [](std::string value, int index = 0) {
    return interaction_frame("step.delta", ",\"index\":" + std::to_string(index) + ",\"delta\":" + value);
  };
  const std::vector<Frame> frames{
    interaction_frame("step.start", R"(,"index":0,"step":{"type":"model_output","content":[]})"),
    delta(R"({"type":"text","text":"Héllo"})"),
    delta(R"({"type":"text_annotation_delta","annotations":[)" + first + "]}"),
    delta(R"({"type":"text","text":" world"})"),
    delta(R"({"type":"text_annotation_delta","annotations":[)" + second + "," + citation + "]}"),
    delta(R"({"type":"image","mime_type":"image/png","data":"QQ=="})"),
    delta(R"({"type":"text","text":"after"})"),
    delta(R"({"type":"text_annotation_delta","annotations":[{"type":"speech_metadata","speaker":"spk_2","style":"calm","start_index":0,"end_index":5}]})"),
    interaction_frame("step.stop", R"(,"index":0)")};
  for (auto outcome : {interactions_decode(steps), interactions_decode(steps, frames, true), interactions_decode("", frames, true)}) {
    const auto& completion = completed(outcome);
    const auto& parts = parts_of(completion);
    CHECK(parts.size() == 3 && is_text(parts[0], "Héllo world") && is_text(parts[2], "after"));
    expect_media(parts[1], Expect{MediaKind::Image, "image/png", MediaSource::Inline, "QQ=="});
    CHECK(completion.messages[0].native);
    CHECK(json::equal(completion.messages[0].wire_output->root(), parse_doc(steps)->root()));
    auto next = interactions_request(); next.messages.push_back(completion.messages[0]);
    const auto encoded = interactions::encode(interactions_desc(), next, false);
    CHECK(std::holds_alternative<interactions::EncodedRequest>(encoded));
    CHECK(json::equal(parse_doc(std::get<interactions::EncodedRequest>(encoded).body)->root().get("input").at(1),
                     parse_doc(steps)->root().at(0)));
    if (completion.raw_events.size() > 1) {
      CHECK(json::equal(completion.raw_events[3].payload->root().get("delta").get("annotations").at(0), parse_doc(first)->root()));
    }
    next.messages.back().wire_output = parse_doc(R"([{"type":"model_output","content":[{"type":"text","text":"Héllo world","annotations":[]}]}])");
    const auto tampered = interactions::encode(interactions_desc(), next, false);
    CHECK(std::holds_alternative<Error>(tampered) && std::get<Error>(tampered).kind == ErrorKind::ReplayIneligible);
  }
  const auto changed = [&](std::string_view from, std::string_view to) {
    auto value = steps; const auto at = value.find(from); CHECK(at != std::string::npos);
    value.replace(at, from.size(), to); return value;
  };
  failed(interactions_decode(changed("\"speaker\":\"spk_1\"", "\"speaker\":\"spk_3\""), frames, true), ErrorKind::ProtocolCorrupt);
  failed(interactions_decode(changed("\"0.450s\"", "\"0.451s\""), frames, true), ErrorKind::ProtocolCorrupt);
  failed(interactions_decode(changed("\"end_index\":6", "\"end_index\":5"), frames, true), ErrorKind::ProtocolCorrupt);
  failed(interactions_decode(changed(annotations, "[" + second + "," + first + "," + citation + "]"), frames, true), ErrorKind::ProtocolCorrupt);
  failed(interactions_decode(changed(annotations, "[" + first + "]"), frames, true), ErrorKind::ProtocolCorrupt);
  failed(interactions_decode(changed(",\"annotations\":" + annotations, ""), frames, true), ErrorKind::ProtocolCorrupt);
  const auto extended = changed(annotations, "[" + first + "," + second + "," + citation +
      R"(,{"type":"file_citation","start_index":0,"end_index":6,"document_uri":"files/source","custom_metadata":{"origin":"exact"}},{"type":"place_citation","start_index":7,"end_index":12,"name":"World"}])");
  const auto extension_outcome = interactions_decode(extended, frames, true);
  CHECK(json::equal(completed(extension_outcome).messages[0].wire_output->root(), parse_doc(extended)->root()));
  auto optional_frames = frames;
  optional_frames[2] = delta(R"({"type":"text_annotation_delta","annotations":[{"type":"word_info","text":"Héllo","start_offset":"0.100s","end_offset":"0.450s","speaker":"spk_1"}]})");
  const auto optional_outcome = interactions_decode("", optional_frames, true);
  CHECK(completed(optional_outcome).messages[0].wire_output->root().at(0).get("content").at(0).get("annotations").at(0).get("speaker").as_string() == "spk_1");
  optional_frames[2] = delta(R"({"type":"text_annotation_delta"})");
  completed(interactions_decode("", optional_frames, true));
  auto malformed = frames;
  for (const auto& [value, kind] : std::vector<std::pair<std::string, ErrorKind>>{
      {R"({"type":"text_annotation_delta","annotations":{}})", ErrorKind::ProtocolCorrupt},
      {R"({"type":"text_annotation_delta","annotations":null})", ErrorKind::ProtocolCorrupt},
      {R"({"type":"text_annotation_delta","annotations":[{"type":"word_info","speaker":1}]})", ErrorKind::ProtocolCorrupt},
      {R"({"type":"text_annotation_delta","annotations":[{"type":"word_info","text":"other"}]})", ErrorKind::ProtocolCorrupt},
      {R"({"type":"text_annotation_delta","annotations":[{"type":"word_info","start_index":-1}]})", ErrorKind::ProtocolCorrupt},
      {R"({"type":"text_annotation_delta","annotations":[{"text":"Héllo"}]})", ErrorKind::ProtocolCorrupt},
      {R"({"type":"text_annotation_delta","annotations":[{"type":"word_info","start_index":0,"end_index":99}]})", ErrorKind::ProtocolCorrupt},
      {R"({"type":"text_annotation_delta","annotations":[{"type":"word_info","text":"other","start_index":0,"end_index":6}]})", ErrorKind::ProtocolCorrupt},
      {R"({"type":"text_annotation_delta","annotations":[{"type":"future_critical"}]})", ErrorKind::Unsupported},
      {R"({"type":"text_annotation","annotations":[]})", ErrorKind::Unsupported},
      {R"({"type":"future_critical_delta"})", ErrorKind::Unsupported}}) {
    malformed[2] = delta(value);
    failed(interactions_decode("", malformed, true), kind);
  }
  malformed = frames; malformed[2] = delta(R"({"type":"text_annotation_delta","annotations":[]})", 1);
  failed(interactions_decode("", malformed, true), ErrorKind::ProtocolCorrupt);
  malformed = frames; malformed[1] = delta(R"({"type":"text_annotation_delta","annotations":[]})");
  failed(interactions_decode("", malformed, true), ErrorKind::ProtocolCorrupt);
  malformed = frames; malformed[6] = delta(R"({"type":"text_annotation_delta","annotations":[]})");
  failed(interactions_decode("", malformed, true), ErrorKind::ProtocolCorrupt);
  malformed = frames; malformed.insert(malformed.end(), delta(R"({"type":"text_annotation_delta","annotations":[]})"));
  failed(interactions_decode("", malformed, true), ErrorKind::ProtocolCorrupt);
  failed(interactions_decode(steps, frames, true, tight_parts(2)), ErrorKind::ResourceLimit);
  failed(interactions_decode(steps, frames, true, tight_bytes(128)), ErrorKind::ResourceLimit);
}
void media_kinds_and_unknown_formats() {
  struct Case { const char* type; const char* mime; MediaKind kind; };
  const std::vector<Case> cases{{"image", "image/webp", MediaKind::Image}, {"audio", "audio/L16;rate=22050", MediaKind::Audio},
      {"video", "video/mp4", MediaKind::Video}, {"document", "application/x-provider-document", MediaKind::Document}};
  for (const auto& item : cases) {
    const auto gparts = R"([{"inlineData":{"mimeType":)" + json::quote(item.mime) + R"(,"data":"QUJD"}}])";
    const auto block = "{\"type\":" + json::quote(item.type) + ",\"mime_type\":" + json::quote(item.mime) + R"(,"data":"QUJD"})";
    const auto steps = R"([{"type":"model_output","content":[)" + block + "]}]";
    const std::vector<Frame> frames{
      interaction_frame("step.start", R"(,"index":0,"step":{"type":"model_output","content":[]})"),
      interaction_frame("step.delta", ",\"index\":0,\"delta\":" + block),
      interaction_frame("step.stop", R"(,"index":0)")};
    for (bool sse : {false, true}) {
      const auto g = gemini_decode(gemini_body(gparts), sse);
      expect_media(parts_of(completed(g))[0], Expect{item.kind, item.mime, MediaSource::Inline, "QUJD"});
      const auto i = interactions_decode(steps, frames, sse);
      expect_media(parts_of(completed(i))[0], Expect{item.kind, item.mime, MediaSource::Inline, "QUJD"});
    }
  }
  const auto file = gemini_decode(gemini_body(R"([{"fileData":{"fileUri":"https://example.com/blob"}}])"), false);
  expect_media(parts_of(completed(file))[0], Expect{MediaKind::Document, "", MediaSource::File, {}, "https://example.com/blob"});
}
} // namespace google_fixture

void unsealed_media_never_promotes() {
  Accumulator a;
  CHECK(a.accept(Begin{"g"}));
  CHECK(a.accept(MessageBegin{{0}, {}, Role::Assistant, {}}));
  PartHeader header; header.media.kind = MediaKind::Image; header.media.mime = "image/png";
  CHECK(a.accept(PartBegin{{0}, {0}, PartKind::Media, header, 0}));
  CHECK(a.accept(PartDelta{{0}, {PartKind::Media, "QQ=="}}));
  CHECK(a.accept(Fail{{ErrorKind::Truncated, "cut"}}));
  const auto& failure = failed(*a.outcome(), ErrorKind::Truncated);
  CHECK(media_of(failure.partial.messages).size() == 1 && !media_of(failure.partial.messages)[0]->data);
  Accumulator smuggled;
  CHECK(smuggled.accept(Begin{"g"}) && smuggled.accept(MessageBegin{{0}, {}, Role::Assistant, {}}));
  header.media.source = MediaSource::Url; header.media.reference = "https://example.com/image";
  header.media.data = std::make_shared<const std::string>("QQ==");
  CHECK(!smuggled.accept(PartBegin{{0}, {0}, PartKind::Media, header, 0}));
  failed(*smuggled.outcome(), ErrorKind::ProtocolCorrupt);
}
void open_media_cursor_copy_move_isolated() {
  Accumulator original;
  CHECK(original.accept(Begin{"g"}));
  CHECK(original.accept(MessageBegin{{0}, {}, Role::Assistant, {}}));
  PartHeader header;
  header.media.kind = MediaKind::Audio;
  header.media.mime = "audio/wav";
  header.media.name = "owned.wav";
  header.media.id = "audio_owned";
  header.media.transcript = "seven four two one";
  CHECK(original.accept(PartBegin{{0}, {0}, PartKind::Media, header, 0}));
  CHECK(original.accept(PartDelta{{0}, {PartKind::Media, "AQI"}}));
  Accumulator copied = original;
  Accumulator moved = std::move(original);
  header.media.mime = "changed/type";
  header.media.name = "changed";
  header.media.id = "changed";
  header.media.transcript = "changed";

  CHECK(copied.accept(PartDelta{{0}, {PartKind::Media, "DBA=="}}));
  CHECK(copied.accept(PartSeal{{0}, {}}));
  CHECK(copied.accept(MessageSeal{{0}, {}}));
  CHECK(copied.accept(Stop{{StopKind::EndTurn, "stop"}}));
  CHECK(copied.accept(Commit{"normal close"}));
  expect_media(parts_of(completed(*copied.outcome()))[0],
               Expect{MediaKind::Audio, "audio/wav", MediaSource::Inline, "AQIDBA==",
                      {}, "owned.wav", "audio_owned", "seven four two one"});

  CHECK(moved.accept(Fail{{ErrorKind::Truncated, "cut"}}));
  const auto& partial = failed(*moved.outcome(), ErrorKind::Truncated).partial;
  CHECK(partial.messages.size() == 1 && partial.messages[0].parts.size() == 1);
  const auto& description = std::get<Media>(partial.messages[0].parts[0]);
  CHECK(description.kind == MediaKind::Audio && description.mime == "audio/wav");
  CHECK(description.name == "owned.wav" && description.id == "audio_owned");
  CHECK(description.transcript == "seven four two one" && !description.data);
}
} // namespace

int main() {
  try {
    chat_fixture::audio_formats_buffered();
    chat_fixture::audio_and_text_order();
    chat_fixture::audio_stream_chunks();
    chat_fixture::metadata_heartbeats();
    chat_fixture::audio_usage_expiry_terminal();
    chat_fixture::audio_usage_terminal_failures();
    chat_fixture::audio_malformed();
    chat_fixture::images_buffered_and_streamed();
    chat_fixture::images_malformed();
    chat_fixture::content_arrays_and_image_history();
    chat_fixture::pdf_and_citation_annotations();
    chat_fixture::typed_tool_media_is_not_chat_content();
    chat_fixture::chat_limits();
    responses_fixture::generated_images();
    google_fixture::gemini_generated();
    google_fixture::interactions_generated();
    google_fixture::interactions_transcription_annotations();
    google_fixture::media_kinds_and_unknown_formats();
    unsealed_media_never_promotes();
    open_media_cursor_copy_move_isolated();
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
  std::cout << "media output tests passed\n";
  return 0;
}
