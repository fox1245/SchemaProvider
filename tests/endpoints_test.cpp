// Embeddings, rerank and moderation endpoints (D6): the request each provider shape takes and the typed result of a
// response.
#include "codecs/embeddings.h"
#include "codecs/generations.h"
#include "codecs/moderation.h"
#include "codecs/rerank.h"
#include "codecs/speech.h"
#include "codecs/transcription.h"
#include "descriptor/descriptor.h"
#include "json/json.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
using namespace sp;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)
descriptor::ValidatedDescriptor at(std::string_view origin) {
  const std::string family = origin == "https://generativelanguage.googleapis.com" ? "google.generate" : "openai.chat";
  const std::string path = family == "google.generate" ? "/v1beta/models/{model}:generateContent"
      : origin == "https://openrouter.ai" ? "/api/v1/chat/completions" : "/v1/chat/completions";
  const std::string stream = family == "google.generate" ? "/v1beta/models/{model}:streamGenerateContent?alt=sse" : path;
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"endpoints-fixture","family":)" + json::quote(family) +
                                 R"(,"connection":{"base_url":)" + json::quote(origin) + R"(,"paths":{"buffered":)" + json::quote(path) +
                                 R"(,"streaming":)" + json::quote(stream) + "}}}");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
std::shared_ptr<const json::Document> document(std::string_view text) {
  auto parsed = json::parse(text);
  CHECK(std::holds_alternative<json::Document>(parsed));
  return std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
}
template <class Encoded, class Request, class Encode> Encoded encoded(Encode encode, const descriptor::ValidatedDescriptor& d, const Request& r) {
  auto result = encode(d, r);
  if (const auto* error = std::get_if<Error>(&result)) throw std::runtime_error("encode refused: " + error->safe_message);
  return std::get<Encoded>(std::move(result));
}
template <class Request, class Encode> ErrorKind refused(Encode encode, const descriptor::ValidatedDescriptor& d, const Request& r) {
  auto result = encode(d, r);
  CHECK(std::holds_alternative<Error>(result));
  return std::get<Error>(result).kind;
}
template <class Codec, class Typed> auto decoded(Typed typed, const descriptor::ValidatedDescriptor& d, std::string_view body, std::string model) {
  Accumulator accumulator;
  Codec codec(d, accumulator, std::move(model));
  codec.buffered(body, {});
  CHECK(accumulator.outcome());
  return typed(*accumulator.outcome());
}
const char png[] = "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAEklEQVR4nGO4ckUEK2IYlRIBAPzqLqpk9vWsAAAAAElFTkSuQmCC";

void embedding_endpoints() {
  const auto openai = at("https://api.openai.com"), router = at("https://openrouter.ai"), google = at("https://generativelanguage.googleapis.com");
  embeddings::Request request{"text-embedding-3-small", {{"a red fox"}, {"a blue whale"}}};
  request.dimensions = 256;
  const auto direct = encoded<embeddings::Encoded>(embeddings::encode, openai, request);
  CHECK(direct.path == "/v1/embeddings" && direct.charge_on_accept);
  {
    const auto body = document(direct.body);
    const auto root = body->root();
    CHECK(root.get("input").size() == 2 && root.get("input").at(1).as_string() == "a blue whale" && root.get("dimensions").as_uint() == 256 &&
          root.get("encoding_format").as_string() == "float");
  }
  // Google names the model in the path and embeds each input as its own request, media inline.
  auto multimodal = request; multimodal.model = "gemini-embedding-2"; multimodal.task = "RETRIEVAL_DOCUMENT";
  multimodal.inputs[1].media = {Media::image("image/png", std::make_shared<const std::string>(png))};
  const auto via_google = encoded<embeddings::Encoded>(embeddings::encode, google, multimodal);
  CHECK(via_google.path == "/v1beta/models/gemini-embedding-2:batchEmbedContents" && via_google.auth == "x-goog-api-key");
  {
    const auto body = document(via_google.body);
    const auto requests = body->root().get("requests");
    CHECK(requests.size() == 2 && requests.at(0).get("model").as_string() == "models/gemini-embedding-2" &&
          requests.at(1).get("content").get("parts").size() == 2 && requests.at(1).get("content").get("parts").at(1).get("inline_data").get("mime_type").as_string() == "image/png" &&
          requests.at(0).get("taskType").as_string() == "RETRIEVAL_DOCUMENT" && requests.at(0).get("outputDimensionality").as_uint() == 256);
  }
  // OpenRouter takes media as content lists, and the task as `input_type`.
  auto routed = multimodal; routed.model = "openai/text-embedding-3-small"; routed.task = "query";
  {
    const auto body = document(encoded<embeddings::Encoded>(embeddings::encode, router, routed).body);
    const auto input = body->root().get("input");
    CHECK(input.at(0).get("content").at(0).get("type").as_string() == "text" && input.at(1).get("content").at(1).get("image_url").get("url").as_string().starts_with("data:image/png;base64,") &&
          body->root().get("input_type").as_string() == "query");
  }
  CHECK(refused(embeddings::encode, openai, multimodal) == ErrorKind::Unsupported);
  auto tasked = request; tasked.task = "query";
  CHECK(refused(embeddings::encode, openai, tasked) == ErrorKind::Unsupported);
  auto slashed = multimodal; slashed.model = "models/gemini-embedding-2";
  CHECK(refused(embeddings::encode, google, slashed) == ErrorKind::InvalidRequest);
  auto empty = request; empty.inputs.push_back({});
  CHECK(refused(embeddings::encode, openai, empty) == ErrorKind::InvalidRequest);
  CHECK(refused(embeddings::encode, at("http://127.0.0.1:18080"), request) == ErrorKind::Unsupported);
  // Vectors come back in input order, whatever order the provider listed them in.
  const auto result = decoded<embeddings::Codec>(embeddings::typed, openai,
      R"({"object":"list","model":"text-embedding-3-small","data":[{"object":"embedding","index":1,"embedding":[0.5,-0.25]},{"object":"embedding","index":0,"embedding":[0.125,1]}],"usage":{"prompt_tokens":6,"total_tokens":6}})",
      "text-embedding-3-small");
  const auto& vectors = std::get<embeddings::Result>(result);
  CHECK(vectors.vectors.size() == 2 && vectors.vectors[0] == (std::vector<float>{0.125f, 1.f}) && vectors.vectors[1][1] == -0.25f &&
        vectors.usage.input_total->value == 6 && vectors.model.relation == ServedRelation::Exact);
  const auto from_google = decoded<embeddings::Codec>(embeddings::typed, google, R"({"embeddings":[{"values":[1,2,3]},{"values":[4,5,6]}]})", "gemini-embedding-2");
  CHECK(std::get<embeddings::Result>(from_google).vectors.size() == 2 && std::get<embeddings::Result>(from_google).vectors[1][2] == 6.f);
  for (const auto* body : {R"({"data":[{"index":0,"embedding":"AAAA"}]})", R"({"data":[{"index":0,"embedding":[]}]})", R"({"model":"x"})"}) {
    const auto failed = decoded<embeddings::Codec>(embeddings::typed, openai, body, "m");
    CHECK(std::holds_alternative<Failure>(failed) && std::get<Failure>(failed).error.kind == ErrorKind::ProtocolCorrupt);
  }
}

void rerank_endpoint() {
  const auto router = at("https://openrouter.ai");
  rerank::Request request{"cohere/rerank-v3.5", "Which animal lives in the sea?", {"A fox lives in the forest.", "A whale lives in the sea."}};
  request.top = 1;
  request.extensions = document(R"({"provider":{"order":["cohere"]}})");
  const auto wire = encoded<rerank::Encoded>(rerank::encode, router, request);
  CHECK(wire.path == "/api/v1/rerank");
  {
    const auto body = document(wire.body);
    CHECK(body->root().get("documents").size() == 2 && body->root().get("top_n").as_uint() == 1 && body->root().get("provider").get("order").at(0).as_string() == "cohere");
  }
  CHECK(refused(rerank::encode, at("https://api.openai.com"), request) == ErrorKind::Unsupported);
  auto none = request; none.documents.clear();
  CHECK(refused(rerank::encode, router, none) == ErrorKind::InvalidRequest);
  const auto result = decoded<rerank::Codec>(rerank::typed, router,
      R"({"id":"gen-rr-1","model":"cohere/rerank-v3.5","provider":"Cohere","results":[{"index":1,"relevance_score":0.97,"document":{"text":"A whale lives in the sea."}}],"usage":{"cost":0.002,"search_units":1}})",
      "cohere/rerank-v3.5");
  const auto& ranked = std::get<rerank::Result>(result);
  CHECK(ranked.ranked.size() == 1 && ranked.ranked[0].index == 1 && ranked.ranked[0].relevance == 0.97 && ranked.provider == "Cohere" &&
        ranked.usage.extra.at("search_units").value == 1 && ranked.usage.provider_cost.total->nano_usd == 2'000'000u);
  const auto failed = decoded<rerank::Codec>(rerank::typed, router, R"({"results":[{"index":"1"}]})", "m");
  CHECK(std::holds_alternative<Failure>(failed));
}

void moderation_endpoint() {
  const auto openai = at("https://api.openai.com");
  moderation::Request request{"omni-moderation-latest", {{"I will hurt you."}, {"Nice weather today."}}};
  const auto wire = encoded<moderation::Encoded>(moderation::encode, openai, request);
  CHECK(wire.path == "/v1/moderations" && !wire.charge_on_accept);
  CHECK(document(wire.body)->root().get("input").at(1).as_string() == "Nice weather today.");
  auto pictured = request; pictured.inputs[1].media = {Media::image("image/png", std::make_shared<const std::string>(png))};
  {
    const auto body = document(encoded<moderation::Encoded>(moderation::encode, openai, pictured).body);
    const auto input = body->root().get("input");
    CHECK(input.size() == 3 && input.at(0).get("type").as_string() == "text" && input.at(2).get("type").as_string() == "image_url");
  }
  CHECK(refused(moderation::encode, at("https://openrouter.ai"), request) == ErrorKind::Unsupported);
  const auto result = decoded<moderation::Codec>(moderation::typed, openai,
      R"({"id":"modr-1","model":"omni-moderation-2024-09-26","results":[{"flagged":true,"categories":{"violence":true,"harassment":false},"category_scores":{"violence":0.91,"harassment":0.02}},{"flagged":false,"categories":{"violence":false},"category_scores":{"violence":0.001}}]})",
      "omni-moderation-latest");
  const auto& verdicts = std::get<moderation::Result>(result);
  CHECK(verdicts.verdicts.size() == 2 && verdicts.verdicts[0].flagged && verdicts.verdicts[0].categories[0] == (std::pair<std::string, bool>{"violence", true}) &&
        verdicts.verdicts[0].scores[0].second == 0.91 && !verdicts.verdicts[1].flagged && verdicts.id == "modr-1");
  const auto failed = decoded<moderation::Codec>(moderation::typed, openai, R"({"results":[{"categories":{}}]})", "m");
  CHECK(std::holds_alternative<Failure>(failed));
}
template <class Codec, class Typed> auto decoded_with(Typed typed, const descriptor::ValidatedDescriptor& d, std::string_view body,
                                                     std::vector<std::pair<std::string, std::string>> headers, std::string model) {
  Accumulator accumulator;
  Codec codec(d, accumulator, std::move(model));
  codec.buffered(body, {true, ErrorKind::Truncated, std::move(headers)});
  CHECK(accumulator.outcome());
  return typed(*accumulator.outcome());
}
void speech_endpoint() {
  const auto openai = at("https://api.openai.com"), router = at("https://openrouter.ai");
  speech::Request request{"gpt-4o-mini-tts", "Hello there.", "alloy"};
  request.instructions = "Speak slowly."; request.format = "mp3"; request.speed = 1.25;
  const auto wire = encoded<speech::Encoded>(speech::encode, openai, request);
  CHECK(wire.path == "/v1/audio/speech" && wire.accept == "audio/*" && wire.content_type == "application/json");
  {
    const auto body = document(wire.body);
    CHECK(body->root().get("voice").as_string() == "alloy" && body->root().get("response_format").as_string() == "mp3" && body->root().get("speed").as_double() == 1.25);
  }
  CHECK(encoded<speech::Encoded>(speech::encode, router, request).path == "/api/v1/audio/speech");
  auto slow = request; slow.speed = 0.0; CHECK(refused(speech::encode, openai, slow) == ErrorKind::InvalidRequest);
  // The body is the audio: its type comes from the response header, and the raw record keeps its size and digest.
  const std::string mp3("ID3\x03\x00\x00\x00\x00\x00\x00 synthetic frames", 26);
  const auto spoken = decoded_with<speech::Codec>(speech::typed, router, mp3, {{"content-type", "audio/mpeg"}, {"x-generation-id", "gen-tts-1"}}, "openai/gpt-4o-mini-tts");
  const auto& audio = std::get<speech::Result>(spoken);
  std::string bytes;
  CHECK(audio.audio.kind == MediaKind::Audio && audio.audio.mime == "audio/mpeg" && base64_decode(*audio.audio.data, bytes) && bytes == mp3 && audio.id == "gen-tts-1");
  CHECK(audio.raw_events.size() == 1 && audio.raw_events[0].payload->root().get("bytes").as_uint() == mp3.size() &&
        audio.raw_events[0].payload->root().get("sha256").as_string().size() == 64);
  for (const auto& [body, type] : std::initializer_list<std::pair<std::string, std::string>>{{R"({"text":"no"})", "application/json"}, {"", "audio/mpeg"}}) {
    const auto failed = decoded_with<speech::Codec>(speech::typed, openai, body, {{"content-type", type}}, "m");
    CHECK(std::holds_alternative<Failure>(failed) && std::get<Failure>(failed).error.kind == ErrorKind::ProtocolCorrupt);
  }
}
void transcription_endpoint() {
  const auto openai = at("https://api.openai.com"), router = at("https://openrouter.ai");
  const auto wav = std::make_shared<const std::string>(base64_encode(std::string("RIFF\x24\x00\x00\x00WAVEfmt synthetic", 24)));
  transcription::Request request{"gpt-4o-mini-transcribe", Media::audio("audio/wav", wav)};
  request.language = "en"; request.prompt = "A greeting.";
  request.extensions = document(R"({"temperature":0,"timestamp_granularities":["word"]})");
  // OpenAI takes a multipart upload: the fields, then the audio's bytes under `file`.
  const auto wire = encoded<transcription::Encoded>(transcription::encode, openai, request);
  CHECK(wire.path == "/v1/audio/transcriptions" && wire.content_type.starts_with("multipart/form-data; boundary=sp-boundary-"));
  const auto boundary = wire.content_type.substr(wire.content_type.find('=') + 1);
  CHECK(wire.body.find("--" + boundary + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\ngpt-4o-mini-transcribe\r\n") == 0);
  CHECK(wire.body.find("name=\"language\"\r\n\r\nen\r\n") != std::string::npos && wire.body.find("name=\"timestamp_granularities[]\"\r\n\r\nword\r\n") != std::string::npos &&
        wire.body.find("filename=\"audio.wav\"\r\nContent-Type: audio/wav\r\n\r\nRIFF") != std::string::npos && wire.body.ends_with("--" + boundary + "--\r\n"));
  // OpenRouter takes JSON with the audio inline or by URL, and no prompt.
  auto routed = request; routed.model = "openai/gpt-4o-mini-transcribe"; routed.prompt.reset(); routed.extensions.reset();
  {
    const auto body = document(encoded<transcription::Encoded>(transcription::encode, router, routed).body);
    CHECK(body->root().get("input_audio").get("data").as_string() == *wav && body->root().get("input_audio").get("format").as_string() == "wav" && body->root().get("language").as_string() == "en");
  }
  auto linked = routed; linked.audio = Media::url(MediaKind::Audio, "https://example.test/a.mp3", "audio/mpeg");
  CHECK(document(encoded<transcription::Encoded>(transcription::encode, router, linked).body)->root().get("input_audio").get("url").as_string() == "https://example.test/a.mp3");
  CHECK(refused(transcription::encode, openai, [&] { auto r = linked; r.model = "gpt-4o-mini-transcribe"; return r; }()) == ErrorKind::Unsupported);
  CHECK(refused(transcription::encode, router, [&] { auto r = routed; r.prompt = "x"; return r; }()) == ErrorKind::Unsupported);
  const auto result = decoded<transcription::Codec>(transcription::typed, openai,
      R"({"text":"Hello there.","usage":{"type":"tokens","input_tokens":14,"output_tokens":4,"total_tokens":18}})", "gpt-4o-mini-transcribe");
  CHECK(std::get<transcription::Result>(result).text == "Hello there." && std::get<transcription::Result>(result).usage.total->value == 18);
  const auto routed_result = decoded<transcription::Codec>(transcription::typed, router,
      R"({"text":"Hello there.","language":"en","duration":1.4,"usage":{"cost":0.0001,"seconds":1.4}})", "openai/gpt-4o-mini-transcribe");
  CHECK(std::get<transcription::Result>(routed_result).seconds == 1.4 && std::get<transcription::Result>(routed_result).language == "en" &&
        std::get<transcription::Result>(routed_result).usage.provider_cost.total->nano_usd == 100'000u);
  CHECK(std::holds_alternative<Failure>(decoded<transcription::Codec>(transcription::typed, openai, R"({"segments":[]})", "m")));
  // A translation into English: OpenAI only, and without a language.
  auto translate = request; translate.model = "whisper-1"; translate.language.reset(); translate.extensions.reset(); translate.translate = true;
  CHECK(encoded<transcription::Encoded>(transcription::encode, openai, translate).path == "/v1/audio/translations");
  CHECK(refused(transcription::encode, openai, [&] { auto r = translate; r.language = "ko"; return r; }()) == ErrorKind::InvalidRequest);
  CHECK(refused(transcription::encode, router, [&] { auto r = routed; r.translate = true; r.language.reset(); return r; }()) == ErrorKind::Unsupported);
}

void generation_endpoint() {
  const auto router = at("https://openrouter.ai");
  const auto wire = encoded<generations::Encoded>(generations::encode, router, generations::Get{"gen-1792420896-swTWG0mrnUerfipmnSCE"});
  CHECK(wire.path == "/api/v1/generation?id=gen-1792420896-swTWG0mrnUerfipmnSCE" && wire.method == "GET" && wire.body.empty());
  CHECK(refused(generations::encode, router, generations::Get{"gen-1&x=2"}) == ErrorKind::InvalidRequest);
  CHECK(refused(generations::encode, at("https://api.openai.com"), generations::Get{"gen-1"}) == ErrorKind::Unsupported);
  // The statistics of a live generation, 2026-10-19.
  Accumulator accumulator;
  generations::Codec codec(router, accumulator);
  codec.buffered(R"({"data":{"created_at":"2026-10-19T14:41:35.908Z","model":"google/gemini-3.1-flash-lite-20260507","streamed":true,"cancelled":false,"latency":781,"generation_time":807,"tokens_prompt":2,"tokens_completion":1,"native_tokens_prompt":3,"native_tokens_completion":1,"native_tokens_reasoning":0,"native_tokens_cached":0,"is_byok":false,"finish_reason":"stop","native_finish_reason":"STOP","usage":2.25e-06,"id":"gen-1792420896-swTWG0mrnUerfipmnSCE","upstream_id":"ICzWavyyDJqYvdAPl_yHqQQ","total_cost":2.25e-06,"upstream_inference_cost":0,"provider_name":"Google"}})", {});
  CHECK(accumulator.outcome());
  const auto stats = std::get<generations::Result>(generations::typed(*accumulator.outcome()));
  CHECK(stats.id == "gen-1792420896-swTWG0mrnUerfipmnSCE" && stats.model == "google/gemini-3.1-flash-lite-20260507" && stats.provider == "Google" &&
        stats.usage.input_total->value == 3 && stats.usage.output_total->value == 1 && stats.tokens_prompt == 2u && stats.latency_ms == 781u &&
        stats.generation_ms == 807u && stats.finish_reason == "stop" && stats.native_finish_reason == "STOP" && stats.streamed && !stats.cancelled &&
        stats.usage.provider_cost.total->nano_usd == 2250u && stats.usage.provider_cost.is_byok == false && stats.created_at == 1792420895u && stats.raw);
  Accumulator broken;
  generations::Codec bad_codec(router, broken);
  bad_codec.buffered(R"({"data":{"id":"gen-1","native_tokens_prompt":"3"}})", {});
  CHECK(broken.outcome() && std::holds_alternative<Failure>(*broken.outcome()));
}
}  // namespace

// A MIME type may have whitespace around its parameters. A data URL is written without it, a JSON member keeps it as
// given (escaped), and a multipart part header keeps it as the header syntax allows.
void mime_parameters_with_whitespace() {
  const auto openai = at("https://api.openai.com"), router = at("https://openrouter.ai"), google = at("https://generativelanguage.googleapis.com");
  const std::string compact = std::string("data:image/png;q=1;base64,") + png;
  for (const char* mime : {"image/png; q=1", "image/png ;q=1", "image/png;\tq=1", "image/png \t;\t q=1"}) {
    const auto image = Media::image(mime, std::make_shared<const std::string>(png));
    embeddings::Request routed{"openai/text-embedding-3-small", {{"a red fox"}, {"a blue whale"}}};
    routed.inputs[1].media = {image};
    CHECK(document(encoded<embeddings::Encoded>(embeddings::encode, router, routed).body)->root().get("input").at(1).get("content").at(1).get("image_url").get("url").as_string() == compact);
    embeddings::Request multimodal{"gemini-embedding-2", {{"a red fox"}, {"a blue whale"}}};
    multimodal.inputs[1].media = {image, Media::file(MediaKind::Image, "files/abc", mime)};
    {
      const auto body = document(encoded<embeddings::Encoded>(embeddings::encode, google, multimodal).body);
      const auto parts = body->root().get("requests").at(1).get("content").get("parts");
      CHECK(parts.at(1).get("inline_data").get("mime_type").as_string() == mime && parts.at(2).get("file_data").get("mime_type").as_string() == mime);
    }
    moderation::Request pictured{"omni-moderation-latest", {{"Nice weather today."}}};
    pictured.inputs[0].media = {image};
    CHECK(document(encoded<moderation::Encoded>(moderation::encode, openai, pictured).body)->root().get("input").at(1).get("image_url").get("url").as_string() == compact);
  }
  for (const char* mime : {"image/png;q=#", "image/png;q=%41"}) {
    embeddings::Request routed{"openai/text-embedding-3-small", {{"a red fox"}}};
    routed.inputs[0].media = {Media::image(mime, std::make_shared<const std::string>(png))};
    CHECK(refused(embeddings::encode, router, routed) == ErrorKind::InvalidRequest);
    moderation::Request pictured{"omni-moderation-latest", {{"Nice weather today."}}};
    pictured.inputs[0].media = routed.inputs[0].media;
    CHECK(refused(moderation::encode, openai, pictured) == ErrorKind::InvalidRequest);
  }
  const auto wav = std::make_shared<const std::string>(base64_encode(std::string("RIFF\x24\x00\x00\x00WAVEfmt synthetic", 24)));
  for (const char* mime : {"audio/wav; rate=16000", "audio/wav;\trate=16000"}) {
    const auto wire = encoded<transcription::Encoded>(transcription::encode, openai, transcription::Request{"gpt-4o-mini-transcribe", Media::audio(mime, wav)});
    CHECK(wire.body.find(std::string("filename=\"audio.wav\"\r\nContent-Type: ") + mime + "\r\n\r\nRIFF") != std::string::npos);
  }
}

int main() {
  try { embedding_endpoints(); rerank_endpoint(); moderation_endpoint(); speech_endpoint(); transcription_endpoint(); generation_endpoint(); mime_parameters_with_whitespace(); std::cout << "endpoint properties passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << "endpoint test failed: " << error.what() << '\n'; return 1; }
}
