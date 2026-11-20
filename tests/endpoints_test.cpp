// Embeddings, rerank and moderation endpoints (D6): the request each provider shape takes and the typed result of a
// response.
#include "codecs/batches.h"
#include "codecs/decisions.h"
#include "codecs/embeddings.h"
#include "codecs/files.h"
#include "codecs/generations.h"
#include "codecs/images.h"
#include "codecs/jobs.h"
#include "codecs/moderation.h"
#include "codecs/provider_cost.h"
#include "codecs/realtime.h"
#include "codecs/rerank.h"
#include "codecs/speech.h"
#include "codecs/transcription.h"
#include "codecs/videos.h"
#include "descriptor/descriptor.h"
#include "descriptor/owned_keys.h"
#include "descriptor/policy.h"
#include "descriptor/presets.h"
#include "json/json.h"
#include "sp/config_defaults.h"
#include "support/endpoint_fakes.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
using namespace sp;
using namespace sp::test;
using namespace std::chrono_literals;
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

// The results are checked against the call: one vector per input, each index once and in range.
void embedding_counts() {
  const auto openai = at("https://api.openai.com"), google = at("https://generativelanguage.googleapis.com");
  embeddings::Request request{"text-embedding-3-small", {{"a red fox"}, {"a blue whale"}}};
  const auto wire = encoded<embeddings::Encoded>(embeddings::encode, openai, request);
  CHECK(wire.context.inputs == 2 && wire.context.results == 0 && wire.context.secret.empty());
  const auto run = [&](const descriptor::ValidatedDescriptor& d, std::string_view body, std::uint64_t inputs) {
    Accumulator accumulator;
    embeddings::Codec codec(d, accumulator, "m", {}, inputs);
    codec.buffered(body, {});
    CHECK(accumulator.outcome());
    return embeddings::typed(*accumulator.outcome());
  };
  const auto reason = [](const embeddings::Outcome& outcome) {
    CHECK(std::holds_alternative<Failure>(outcome) && std::get<Failure>(outcome).error.kind == ErrorKind::ProtocolCorrupt);
    return std::get<Failure>(outcome).error.reason;
  };
  CHECK(std::get<embeddings::Result>(run(openai, R"({"data":[{"index":1,"embedding":[2]},{"index":0,"embedding":[1]}]})", 2)).vectors ==
        (std::vector<std::vector<float>>{{1.f}, {2.f}}));
  CHECK(std::get<embeddings::Result>(run(openai, R"({"data":[{"embedding":[1]},{"embedding":[2]}]})", 2)).vectors.size() == 2);   // by position
  // Fewer vectors than inputs, or more, is not a result.
  CHECK(reason(run(openai, R"({"data":[{"index":0,"embedding":[1]}]})", 2)) == "embeddings.vectors.count_mismatch");
  CHECK(reason(run(openai, R"({"data":[{"embedding":[1]},{"embedding":[2]},{"embedding":[3]}]})", 2)) == "embeddings.vectors.count_mismatch");
  CHECK(reason(run(openai, R"({"data":[]})", 2)) == "embeddings.vectors.count_mismatch");
  CHECK(reason(run(google, R"({"embeddings":[{"values":[1]}]})", 2)) == "embeddings.vectors.count_mismatch");
  CHECK(std::get<embeddings::Result>(run(google, R"({"embeddings":[{"values":[1]},{"values":[2]}]})", 2)).vectors.size() == 2);
  // A repeated index, one out of range, or only some items naming theirs.
  CHECK(reason(run(openai, R"({"data":[{"index":0,"embedding":[1]},{"index":0,"embedding":[2]}]})", 2)) == "embeddings.vector.index_invalid");
  CHECK(reason(run(openai, R"({"data":[{"index":0,"embedding":[1]},{"index":7,"embedding":[2]}]})", 2)) == "embeddings.vector.index_invalid");
  CHECK(reason(run(openai, R"({"data":[{"index":2,"embedding":[1]},{"index":1,"embedding":[2]}]})", 2)) == "embeddings.vector.index_invalid");
  CHECK(reason(run(openai, R"({"data":[{"index":7,"embedding":[1]},{"embedding":[2]}]})", 2)) == "embeddings.vector.index_invalid");
  CHECK(reason(run(openai, R"({"data":[{"index":"1","embedding":[1]},{"index":0,"embedding":[2]}]})", 2)) == "embeddings.vector.index_invalid");
  // A decoder run without the call's input count checks no count.
  CHECK(std::get<embeddings::Result>(run(openai, R"({"data":[{"index":0,"embedding":[1]}]})", 0)).vectors.size() == 1);
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
  CHECK(wire.context.inputs == 2 && wire.context.results == 1);
}

// The results are checked against the call, and listed most relevant first whatever order the provider sent them in.
void rerank_results() {
  const auto router = at("https://openrouter.ai");
  rerank::Request request{"cohere/rerank-v3.5", "Which animal lives in the sea?", {"A fox.", "A whale.", "A crab."}};
  const auto all = encoded<rerank::Encoded>(rerank::encode, router, request);
  CHECK(all.context.inputs == 3 && all.context.results == 0);
  const auto run = [&](std::string_view body, std::uint64_t documents, std::uint64_t top) {
    Accumulator accumulator;
    rerank::Codec codec(router, accumulator, "m", {}, documents, top);
    codec.buffered(body, {});
    CHECK(accumulator.outcome());
    return rerank::typed(*accumulator.outcome());
  };
  const auto reason = [](const rerank::Outcome& outcome) {
    CHECK(std::holds_alternative<Failure>(outcome) && std::get<Failure>(outcome).error.kind == ErrorKind::ProtocolCorrupt);
    return std::get<Failure>(outcome).error.reason;
  };
  // Unordered scores are sorted; equal scores keep the provider's order; the raw document keeps the provider's order.
  const auto sorted = std::get<rerank::Result>(run(R"({"results":[{"index":0,"relevance_score":0.1},{"index":2,"relevance_score":0.9},{"index":1,"relevance_score":0.9}]})", 3, 0));
  CHECK(sorted.ranked.size() == 3 && sorted.ranked[0].index == 2 && sorted.ranked[1].index == 1 && sorted.ranked[2].index == 0);
  CHECK(sorted.raw_events[0].payload->root().get("results").at(0).get("index").as_uint() == 0);
  // The top results the call asked for (all of them when it asked for none, and no more than there are documents).
  CHECK(std::get<rerank::Result>(run(R"({"results":[{"index":1,"relevance_score":0.9},{"index":0,"relevance_score":0.5}]})", 3, 2)).ranked.size() == 2);
  CHECK(std::get<rerank::Result>(run(R"({"results":[{"index":1,"relevance_score":0.9},{"index":0,"relevance_score":0.5},{"index":2,"relevance_score":0.1}]})", 3, 10)).ranked.size() == 3);
  CHECK(reason(run(R"({"results":[{"index":1,"relevance_score":0.9}]})", 3, 0)) == "rerank.results.count_mismatch");
  CHECK(reason(run(R"({"results":[{"index":1,"relevance_score":0.9},{"index":0,"relevance_score":0.5},{"index":2,"relevance_score":0.1}]})", 3, 2)) == "rerank.results.count_mismatch");
  CHECK(reason(run(R"({"results":[]})", 3, 0)) == "rerank.results.count_mismatch");
  // A repeated index, or one that names no document.
  CHECK(reason(run(R"({"results":[{"index":1,"relevance_score":0.9},{"index":1,"relevance_score":0.5},{"index":2,"relevance_score":0.1}]})", 3, 0)) == "rerank.result.index_invalid");
  CHECK(reason(run(R"({"results":[{"index":1,"relevance_score":0.9},{"index":0,"relevance_score":0.5},{"index":3,"relevance_score":0.1}]})", 3, 0)) == "rerank.result.index_invalid");
  CHECK(reason(run(R"({"results":[{"index":1,"relevance_score":0.9},{"index":1,"relevance_score":0.5}]})", 0, 0)) == "rerank.result.index_invalid");
  // A decoder run without the call's counts checks only that an index is not repeated.
  CHECK(std::get<rerank::Result>(run(R"({"results":[{"index":9,"relevance_score":0.9}]})", 0, 0)).ranked.size() == 1);
}

// Through the runtime: the counts of the call reach its decoder.
void runtime_checks() {
  auto executor = std::make_shared<QueueExecutor>();
  auto wire = std::make_shared<ScriptTransport>();
  runtime::Options options;
  options.api_key = "synthetic-key";
  auto client = runtime::detail::ClientAccess::make(at("https://openrouter.ai"), options, executor, wire);
  runtime::RunOptions run;
  run.retry = runtime::RetryPolicy{true, false, 3, 0ms, 0ms};
  const auto run_for = [&] { auto value = run; value.deadline = executor->now() + 10s; return value; };
  const auto complete = [&](auto request) { auto call = client.start(client.prepare(std::move(request), run_for())); executor->drain(); return call.join(); };
  wire->script = [](Exchange& e) { e.reply(200, "application/json", R"({"data":[{"index":0,"embedding":[1]}]})"); };
  auto vectors = complete(embeddings::Request{"openai/text-embedding-3-small", {{"a"}, {"b"}}});
  CHECK(std::holds_alternative<Failure>(vectors) && std::get<Failure>(vectors).error.reason == "embeddings.vectors.count_mismatch");
  wire->script = [](Exchange& e) { e.reply(200, "application/json", R"({"data":[{"index":1,"embedding":[2]},{"index":0,"embedding":[1]}]})"); };
  vectors = complete(embeddings::Request{"openai/text-embedding-3-small", {{"a"}, {"b"}}});
  CHECK(std::holds_alternative<embeddings::Result>(vectors) && std::get<embeddings::Result>(vectors).vectors.size() == 2);
  wire->script = [](Exchange& e) { e.reply(200, "application/json", R"({"results":[{"index":0,"relevance_score":0.2},{"index":1,"relevance_score":0.8}]})"); };
  rerank::Request ranking{"cohere/rerank-v3.5", "q", {"a", "b"}};
  auto ranked = complete(ranking);
  CHECK(std::holds_alternative<rerank::Result>(ranked) && std::get<rerank::Result>(ranked).ranked[0].index == 1);
  ranking.top = 1;
  ranked = complete(ranking);
  CHECK(std::holds_alternative<Failure>(ranked) && std::get<Failure>(ranked).error.reason == "rerank.results.count_mismatch");
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
  // The usage records the reported length in whole seconds, rounded up: `duration`, else `usage.seconds`.
  const auto seconds_of = [&](std::string_view body) -> std::optional<std::uint64_t> {
    const auto decoded_result = decoded<transcription::Codec>(transcription::typed, router, body, "m");
    const auto& extra = std::get<transcription::Result>(decoded_result).usage.extra;
    const auto found = extra.find("seconds");
    if (found == extra.end()) return std::nullopt;
    if (found->second.evidence != Evidence::Derived) throw std::runtime_error("the seconds are derived from the reported length");
    return found->second.value;
  };
  CHECK(std::get<transcription::Result>(routed_result).usage.extra.at("seconds").value == 2);
  CHECK(seconds_of(R"({"text":"a","duration":2.0})") == 2u && seconds_of(R"({"text":"a","duration":0})") == 0u &&
        seconds_of(R"({"text":"a","duration":0.0001})") == 1u && seconds_of(R"({"text":"a","duration":7})") == 7u &&
        seconds_of(R"({"text":"a","duration":59.999999999})") == 60u && seconds_of(R"({"text":"a","duration":1e3})") == 1000u &&
        seconds_of(R"({"text":"a","duration":12.5,"usage":{"seconds":30}})") == 30u && seconds_of(R"({"text":"a","usage":{"seconds":30.2}})") == 31u &&
        seconds_of(R"({"text":"a","usage":{"seconds":4,"input_tokens":3}})") == 4u);
  // The provider cost reader and the length reader read a reported number the same way, from its shortest decimal, and
  // round up: whole seconds are the cost's nano-USD divided by 1e9, rounded up, on the same number.
  const std::pair<const char*, std::optional<std::uint64_t>> numbers[]{
      {"1", 1},     {"1.0", 1},      {"1.000001", 2}, {"0.999999", 1}, {"59.5", 60},
      {"1e2", 100}, {"12345.678901234567", 12346},    {"0", 0},        {"-1", std::nullopt}};
  for (const auto& [text, seconds] : numbers) {
    const auto read = seconds_of(R"({"text":"a","duration":)" + std::string(text) + "}");
    CHECK(read == seconds);
    std::optional<UsdAmount> cost;
    const auto status = codecs::usd_amount(document(text)->root(), true, cost);
    CHECK(seconds ? status == CostStatus::Available && cost && cost->nano_usd / 1'000'000'000 + (cost->nano_usd % 1'000'000'000 ? 1 : 0) == *seconds
                  : status == CostStatus::Malformed && !cost);
  }
  // A number that is not finite is no JSON number, so neither reader sees one.
  CHECK(std::holds_alternative<json::ParseError>(json::parse("1e400")) &&
        std::holds_alternative<Failure>(decoded<transcription::Codec>(transcription::typed, router, R"({"text":"a","duration":1e400})", "m")));
  // A result that reports no length, or one no count can hold, records none.
  CHECK(!seconds_of(R"({"text":"a"})") && !seconds_of(R"({"text":"a","usage":{"input_tokens":3}})") &&
        !seconds_of(R"({"text":"a","usage":{"seconds":-1}})") && !seconds_of(R"({"text":"a","usage":{"seconds":1e30}})") &&
        !seconds_of(R"({"text":"a","usage":{"seconds":"30"}})"));
  // A `duration` no count can hold is no length, so `usage.seconds` gives it. A number at or above 2^53 that is not an
  // integer is no exact count of seconds: past that a double's spacing is above one second, so it would round down.
  CHECK(seconds_of(R"({"text":"a","duration":-1,"usage":{"seconds":90}})") == 90u &&
        seconds_of(R"({"text":"a","duration":-0.5,"usage":{"seconds":30.2}})") == 31u &&
        seconds_of(R"({"text":"a","duration":1e30,"usage":{"seconds":30}})") == 30u &&
        seconds_of(R"({"text":"a","duration":1e16,"usage":{"seconds":30}})") == 30u);
  CHECK(!seconds_of(R"({"text":"a","duration":9007199254740993.2})") && !seconds_of(R"({"text":"a","duration":9007199254740992.0})") &&
        seconds_of(R"({"text":"a","duration":9007199254740991.0})") == 9'007'199'254'740'991u &&
        seconds_of(R"({"text":"a","duration":9007199254740993})") == 9'007'199'254'740'993u);
  // Both reports are read, and two that disagree give the larger: the smaller must never price the call.
  CHECK(seconds_of(R"({"text":"a","duration":0,"usage":{"seconds":12.4}})") == 13u &&
        seconds_of(R"({"text":"a","duration":30,"usage":{"seconds":12.4}})") == 30u &&
        seconds_of(R"({"text":"a","duration":-3,"usage":{"seconds":12.4}})") == 13u &&
        seconds_of(R"({"text":"a","duration":12.4,"usage":{"seconds":0}})") == 13u);
  // The result's own length is not read from a negative `duration` either, and is the larger report as well.
  const auto length_of = [&](std::string_view body) { return std::get<transcription::Result>(decoded<transcription::Codec>(transcription::typed, router, body, "m")).seconds; };
  CHECK(!length_of(R"({"text":"a","duration":-1})") && length_of(R"({"text":"a","duration":-1,"usage":{"seconds":90}})") == 90.0 &&
        length_of(R"({"text":"a","duration":0})") == 0.0 && length_of(R"({"text":"a","duration":0,"usage":{"seconds":12.4}})") == 12.4 &&
        length_of(R"({"text":"a","duration":30,"usage":{"seconds":12.4}})") == 30.0);
  CHECK(std::holds_alternative<Failure>(decoded<transcription::Codec>(transcription::typed, openai, R"({"segments":[]})", "m")));
  // A translation into English: OpenAI only, and without a language.
  auto translate = request; translate.model = "whisper-1"; translate.language.reset(); translate.extensions.reset(); translate.translate = true;
  CHECK(encoded<transcription::Encoded>(transcription::encode, openai, translate).path == "/v1/audio/translations");
  CHECK(refused(transcription::encode, openai, [&] { auto r = translate; r.language = "ko"; return r; }()) == ErrorKind::InvalidRequest);
  CHECK(refused(transcription::encode, router, [&] { auto r = routed; r.translate = true; r.language.reset(); return r; }()) == ErrorKind::Unsupported);
}

// The members an endpoint's encoder owns are the members its requests carry. Requests that set every typed member, at each
// provider shape, are encoded; the top-level members of each body (the form fields of a multipart one, the members of a
// session's setup message) are the members the endpoint writes. The table holds each of them as written, and writes none of
// its other members.
std::set<std::string> json_members(const std::string& body) {
  // The document owns the bytes the member names view, so it outlives the loop; each name is copied into the result.
  const auto parsed = document(body);
  std::set<std::string> out;
  for (auto member : parsed->root().members()) out.insert(std::string(member.key));
  return out;
}
// The field names of a multipart body, a bracketed name (`expires_after[seconds]`) by its first part.
std::set<std::string> form_fields(const std::string& body) {
  std::set<std::string> out;
  const std::string mark = "Content-Disposition: form-data; name=\"";
  for (auto at = body.find(mark); at != std::string::npos; at = body.find(mark, at + mark.size())) {
    const auto begin = at + mark.size();
    const auto name = body.substr(begin, body.find('"', begin) - begin);
    out.insert(name.substr(0, name.find('[')));
  }
  return out;
}
void endpoint_members_cover_encoders() {
  const auto openai = at("https://api.openai.com"), router = at("https://openrouter.ai"), google = at("https://generativelanguage.googleapis.com");
  const auto picture = Media::image("image/png", std::make_shared<const std::string>(png));
  const auto wav = std::make_shared<const std::string>(base64_encode(std::string("RIFF\x24\x00\x00\x00WAVEfmt synthetic", 24)));
  std::map<std::string, std::set<std::string>> written;
  const auto add = [&](const std::string& endpoint, std::set<std::string> members) { written[endpoint].insert(members.begin(), members.end()); };

  images::Request image{"m", "a fox"};
  image.count = 2; image.size = "1024x1024"; image.quality = "high"; image.background = "transparent"; image.output_format = "webp"; image.output_compression = 80;
  add("images", json_members(encoded<images::Encoded>(images::encode, openai, image).body));
  image.references = {picture};
  add("images", json_members(encoded<images::Encoded>(images::encode, openai, image).body));
  add("images", json_members(encoded<images::Encoded>(images::encode, router, image).body));

  decisions::Request question;
  question.model = "m";
  question.input = document(R"("a ticket")");
  question.questions.push_back({"urgent", decisions::Kind::Predicate, document(R"("is it urgent?")")});
  add("decisions", json_members(encoded<decisions::Encoded>(decisions::encode, openai, question).body));
  add("decisions", json_members(encoded<decisions::Encoded>(decisions::encode, router, question).body));
  question.assets = {{"picture", Media::image("image/png", std::make_shared<const std::string>(png))}};
  add("decisions", json_members(encoded<decisions::Encoded>(decisions::encode, router, question).body));

  embeddings::Request vectors{"text-embedding-3-small", {{"a red fox"}, {"a blue whale"}}};
  vectors.dimensions = 256;
  add("embeddings", json_members(encoded<embeddings::Encoded>(embeddings::encode, openai, vectors).body));
  vectors.task = "query";
  add("embeddings", json_members(encoded<embeddings::Encoded>(embeddings::encode, router, vectors).body));
  vectors.task = "RETRIEVAL_DOCUMENT";
  add("embeddings", json_members(encoded<embeddings::Encoded>(embeddings::encode, google, vectors).body));

  rerank::Request ranked{"m", "q", {"a", "b"}};
  ranked.top = 1;
  add("rerank", json_members(encoded<rerank::Encoded>(rerank::encode, router, ranked).body));
  add("moderation", json_members(encoded<moderation::Encoded>(moderation::encode, openai, moderation::Request{"m", {{"a"}}}).body));

  speech::Request spoken{"m", "Hello there.", "alloy"};
  spoken.instructions = "Speak slowly."; spoken.format = "mp3"; spoken.speed = 1.25;
  add("speech", json_members(encoded<speech::Encoded>(speech::encode, openai, spoken).body));

  transcription::Request heard{"m", Media::audio("audio/wav", wav)};
  heard.language = "en"; heard.prompt = "A greeting.";
  add("transcription", form_fields(encoded<transcription::Encoded>(transcription::encode, openai, heard).body));
  heard.prompt.reset();
  add("transcription", json_members(encoded<transcription::Encoded>(transcription::encode, router, heard).body));

  videos::Request clip{"m", "a fox"};
  clip.references = {picture}; clip.duration = 4; clip.aspect_ratio = "16:9"; clip.resolution = "720p"; clip.size = "1280x720"; clip.audio = true;
  add("videos", json_members(encoded<videos::Encoded>(videos::encode, router, clip).body));

  files::Upload upload{{std::make_shared<const std::string>("note")}, "note.txt", "text/plain", "user_data", 3600};
  add("files", form_fields(encoded<files::Encoded>(static_cast<std::variant<files::Encoded, Error> (*)(const descriptor::ValidatedDescriptor&, const files::Upload&)>(files::encode), openai, upload).body));

  // The items of an Anthropic or Google batch are requests of that family, so their descriptors are the family's own.
  using BatchEncode = std::variant<batches::Encoded, Error> (*)(const descriptor::ValidatedDescriptor&, const batches::Submit&);
  add("batches", json_members(encoded<batches::Encoded>(static_cast<BatchEncode>(batches::encode), openai, batches::Submit{{}, "file-9", "/v1/responses"}).body));
  add("batches", json_members(encoded<batches::Encoded>(static_cast<BatchEncode>(batches::encode), descriptor::presets::anthropic_messages(),
      batches::Submit{{{"a-1", messages::Request::simple("claude-haiku-4-5", "Say: alpha", 16)}}}).body));
  add("batches", json_members(encoded<batches::Encoded>(static_cast<BatchEncode>(batches::encode), descriptor::presets::google_generate(),
      batches::Submit{{{"g-1", gemini::Request::simple("gemini-3.5-flash-lite", "x")}}}).body));

  // A realtime session sets itself up with one message: Google's `setup` object, and OpenAI's `session` object.
  const auto setup_members = [&](const descriptor::ValidatedDescriptor& d, const realtime::Setup& setup, std::string_view wrapper) {
    auto codec = realtime::make_codec(d);
    CHECK(codec);
    auto opened = codec->open(setup);
    CHECK(std::holds_alternative<std::vector<realtime::Frame>>(opened));
    const auto& frames = std::get<std::vector<realtime::Frame>>(opened);
    CHECK(frames.size() == 1);
    const auto message = document(frames[0].data);
    std::set<std::string> out;
    for (auto member : message->root().get(wrapper).members()) out.insert(std::string(member.key));
    return out;
  };
  realtime::Setup session;
  session.model = "m"; session.instructions = "Be brief."; session.voice = "alloy"; session.input_transcription = true; session.transcription_model = "t";
  session.output_transcription = true; session.resume = std::string();
  session.tools = {realtime::Tool{"lookup", "d", document(R"({"type":"object"})")}};
  add("realtime", setup_members(google, session, "setup"));
  session.resume.reset(); session.output_transcription = false;
  add("realtime", setup_members(openai, session, "session"));

  // The table holds every member written as written, and no member it holds as held or reserved is written. The OpenAI
  // transcription encoder also writes `response_format`, which the policy's options admit; the table owns the typed members only.
  written["transcription"].erase("response_format");
  for (const auto endpoint : {"images", "decisions", "embeddings", "rerank", "moderation", "speech", "transcription", "videos", "files", "realtime", "batches"}) {
    std::set<std::string> table;
    for (const auto& member : descriptor::owned_members(endpoint)) if (member.role == descriptor::MemberRole::Written) table.insert(std::string(member.name));
    CHECK(!table.empty());
    if (written[endpoint] != table) {
      for (const auto& name : written[endpoint]) if (!table.contains(name)) std::cerr << endpoint << " writes " << name << ", which the table does not hold as written\n";
      for (const auto& name : table) if (!written[endpoint].contains(name)) std::cerr << endpoint << ": the table holds " << name << " as written, and no request writes it\n";
    }
    CHECK(written[endpoint] == table);
  }
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

// The policy's `headers` of an endpoint fact go with every call to it.
descriptor::PolicySnapshot policy_with_headers() {
  auto parsed = json::parse(config_defaults::descriptor_policy_json);
  CHECK(std::holds_alternative<json::Document>(parsed));
  std::string text = "{";
  for (auto member : std::get<json::Document>(parsed).root().members()) {
    if (text.size() > 1) text += ',';
    if (member.key != "endpoints") { text += json::quote(member.key) + ":" + member.value.dump(); continue; }
    text += "\"endpoints\":[";
    bool first = true;
    for (auto fact : member.value.elements()) {
      if (!std::exchange(first, false)) text += ',';
      // Every fact gets a header of its own, beside the ones it carries (Anthropic's version).
      text += R"({"headers":{"x-sp-test":"1")";
      for (auto header : fact.get("headers").members()) text += "," + json::quote(header.key) + ":" + header.value.dump();
      text += "}";
      for (auto field : fact.members()) if (field.key != "headers") text += "," + json::quote(field.key) + ":" + field.value.dump();
      text += "}";
    }
    text += "]";
  }
  auto policy = descriptor::load_policy(text + "}", config_defaults::codec_defaults_json);
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(policy));
  return std::get<descriptor::PolicySnapshot>(std::move(policy));
}
descriptor::ValidatedDescriptor at_with(std::string_view origin, const descriptor::PolicySnapshot& policy) {
  const std::string family = origin == "https://generativelanguage.googleapis.com" ? "google.generate" : origin == "https://api.anthropic.com" ? "anthropic.messages" : "openai.chat";
  const std::string path = family == "google.generate" ? "/v1beta/models/{model}:generateContent" : family == "anthropic.messages" ? "/v1/messages"
      : origin == "https://openrouter.ai" ? "/api/v1/chat/completions" : "/v1/chat/completions";
  const std::string stream = family == "google.generate" ? "/v1beta/models/{model}:streamGenerateContent?alt=sse" : path;
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"headers-fixture","family":)" + json::quote(family) +
                                 R"(,"connection":{"base_url":)" + json::quote(origin) + R"(,"paths":{"buffered":)" + json::quote(path) +
                                 R"(,"streaming":)" + json::quote(stream) + "}}}", policy);
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
void fact_headers() {
  const auto policy = policy_with_headers();
  const auto openai = at_with("https://api.openai.com", policy), router = at_with("https://openrouter.ai", policy),
             google = at_with("https://generativelanguage.googleapis.com", policy), anthropic = at_with("https://api.anthropic.com", policy);
  const auto sends = [](const endpoints::Encoded& wire) {
    return std::count(wire.headers.begin(), wire.headers.end(), std::pair<std::string, std::string>{"x-sp-test", "1"}) == 1;
  };
  const auto png_image = Media::image("image/png", std::make_shared<const std::string>(png));
  images::Request image{"m", "a fox"};
  CHECK(sends(encoded<images::Encoded>(images::encode, openai, image)));
  image.references = {png_image};
  CHECK(sends(encoded<images::Encoded>(images::encode, openai, image)));
  CHECK(sends(encoded<images::Encoded>(images::encode, router, image)));
  decisions::Request question;
  question.model = "m";
  question.input = document(R"("a ticket")");
  question.questions.push_back({"urgent", decisions::Kind::Predicate, document(R"("is it urgent?")")});
  CHECK(sends(encoded<decisions::Encoded>(decisions::encode, openai, question)));
  CHECK(sends(encoded<decisions::Encoded>(decisions::encode, router, question)));
  embeddings::Request vectors{"m", {{"a"}}};
  CHECK(sends(encoded<embeddings::Encoded>(embeddings::encode, openai, vectors)));
  CHECK(sends(encoded<embeddings::Encoded>(embeddings::encode, google, vectors)));
  CHECK(sends(encoded<rerank::Encoded>(rerank::encode, router, rerank::Request{"m", "q", {"a", "b"}})));
  CHECK(sends(encoded<moderation::Encoded>(moderation::encode, openai, moderation::Request{"m", {{"a"}}})));
  CHECK(sends(encoded<speech::Encoded>(speech::encode, openai, speech::Request{"m", "hello", "alloy"})));
  CHECK(sends(encoded<speech::Encoded>(speech::encode, router, speech::Request{"m", "hello", "alloy"})));
  const auto wav = std::make_shared<const std::string>(base64_encode(std::string("RIFF\x24\x00\x00\x00WAVEfmt synthetic", 24)));
  transcription::Request heard{"m", Media::audio("audio/wav", wav)};
  CHECK(sends(encoded<transcription::Encoded>(transcription::encode, openai, heard)));
  CHECK(sends(encoded<transcription::Encoded>(transcription::encode, router, heard)));
  CHECK(sends(encoded<videos::Encoded>(videos::encode, router, videos::Request{"m", "a fox"})));
  const jobs::Handle handle{"videos", "https://openrouter.ai", "job-1", "m"};
  using PollEncode = std::variant<jobs::Encoded, Error> (*)(const descriptor::ValidatedDescriptor&, const jobs::Poll&);
  using DownloadEncode = std::variant<jobs::Encoded, Error> (*)(const descriptor::ValidatedDescriptor&, const jobs::Download&);
  CHECK(sends(encoded<jobs::Encoded>(static_cast<PollEncode>(jobs::encode), router, jobs::Poll{handle})));
  CHECK(sends(encoded<jobs::Encoded>(static_cast<DownloadEncode>(jobs::encode), router, jobs::Download{handle, 0})));
  // The endpoints that already did, and a fact's own headers beside the added one.
  CHECK(sends(encoded<files::Encoded>(static_cast<std::variant<files::Encoded, Error> (*)(const descriptor::ValidatedDescriptor&, const files::Get&)>(files::encode), openai, files::Get{"file-a"})));
  const auto version = encoded<files::Encoded>(static_cast<std::variant<files::Encoded, Error> (*)(const descriptor::ValidatedDescriptor&, const files::Get&)>(files::encode), anthropic, files::Get{"file_a"});
  CHECK(sends(version) && std::count(version.headers.begin(), version.headers.end(), std::pair<std::string, std::string>{"anthropic-version", "2023-06-01"}) == 1);
  CHECK(sends(encoded<batches::Encoded>(static_cast<std::variant<batches::Encoded, Error> (*)(const descriptor::ValidatedDescriptor&, const batches::Get&)>(batches::encode), openai, batches::Get{"batch_1"})));
  CHECK(sends(encoded<generations::Encoded>(generations::encode, router, generations::Get{"gen-1"})));

  // Through the runtime the header is on the wire.
  auto executor = std::make_shared<QueueExecutor>();
  auto wire = std::make_shared<ScriptTransport>();
  runtime::Options options;
  options.api_key = "synthetic-key";
  auto client = runtime::detail::ClientAccess::make(router, options, executor, wire);
  runtime::RunOptions run;
  run.deadline = executor->now() + 10s;
  wire->script = [](Exchange& e) { e.reply(200, "application/json", R"({"results":[{"index":0,"relevance_score":0.5},{"index":1,"relevance_score":0.2}]})"); };
  auto call = client.start(client.prepare(rerank::Request{"m", "q", {"a", "b"}}, run));
  executor->drain();
  CHECK(std::holds_alternative<rerank::Result>(call.join()));
  CHECK(header(wire->exchanges.back()->request, "x-sp-test") == "1");
}

// The built-in policy with the `max_response_bytes` of its images facts set to `bound`, or left out (zero).
descriptor::PolicySnapshot policy_with_image_bound(std::uint64_t bound) {
  auto parsed = json::parse(config_defaults::descriptor_policy_json);
  CHECK(std::holds_alternative<json::Document>(parsed));
  std::string text = "{";
  for (auto member : std::get<json::Document>(parsed).root().members()) {
    if (text.size() > 1) text += ',';
    if (member.key != "endpoints") { text += json::quote(member.key) + ":" + member.value.dump(); continue; }
    text += "\"endpoints\":[";
    bool first = true;
    for (auto fact : member.value.elements()) {
      if (!std::exchange(first, false)) text += ',';
      if (fact.get("endpoint").as_string() != "images") { text += fact.dump(); continue; }
      text += "{\"endpoint\":\"images\"";
      for (auto field : fact.members()) if (field.key != "endpoint" && field.key != "max_response_bytes") text += "," + json::quote(field.key) + ":" + field.value.dump();
      if (bound) text += ",\"max_response_bytes\":" + std::to_string(bound);
      text += "}";
    }
    text += "]";
  }
  auto policy = descriptor::load_policy(text + "}", config_defaults::codec_defaults_json);
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(policy));
  return std::get<descriptor::PolicySnapshot>(std::move(policy));
}
// A charged result over the response bound fails with a reason, and an endpoint fact raises the bound for its own calls.
void response_bounds() {
  std::string bytes;
  CHECK(base64_decode(png, bytes));
  bytes.append(6000, '\0');
  const auto body = std::string(R"({"created":1,"output_format":"png","data":[{"b64_json":")") + base64_encode(bytes) + R"("}]})";
  CHECK(body.size() > 8000 && body.size() < 9000);
  const auto image = [&](std::uint64_t fact_bound, std::size_t client_bound) {
    const auto policy = policy_with_image_bound(fact_bound);
    auto executor = std::make_shared<QueueExecutor>();
    auto wire = std::make_shared<ScriptTransport>();
    runtime::Options options;
    options.api_key = "synthetic-key";
    options.limits.max_response_bytes = client_bound;
    auto client = runtime::detail::ClientAccess::make(at_with("https://api.openai.com", policy), options, executor, wire);
    wire->script = [&](Exchange& e) { e.reply(200, "application/json", body); };
    runtime::RunOptions run;
    run.deadline = executor->now() + 10s;
    auto call = client.start(client.prepare(images::Request{"gpt-image-2", "a fox"}, run));
    executor->drain();
    return call.join();
  };
  const auto over = [](const images::Outcome& outcome) {
    CHECK(std::holds_alternative<Failure>(outcome));
    const auto& error = std::get<Failure>(outcome).error;
    return error.kind == ErrorKind::ResourceLimit && error.reason == "runtime.response.limit";
  };
  CHECK(over(image(0, 4096)));                                          // no fact bound: the client's
  CHECK(over(image(6000, 4096)));                                       // the fact's bound is still below the response
  CHECK(std::holds_alternative<images::Result>(image(16384, 4096)));    // the fact's bound is above the client's
  CHECK(std::holds_alternative<images::Result>(image(4096, 32768)));    // a fact never lowers the client's bound
  // The bound of a response that is an error is the error bound.
  {
    auto executor = std::make_shared<QueueExecutor>();
    auto wire = std::make_shared<ScriptTransport>();
    runtime::Options options;
    options.api_key = "synthetic-key";
    auto client = runtime::detail::ClientAccess::make(at("https://api.openai.com"), options, executor, wire);
    wire->script = [](Exchange& e) { e.reply(500, "text/plain", std::string(config_defaults::defaults_max_error_bytes + 1, 'x')); };
    runtime::RunOptions run;
    run.deadline = executor->now() + 10s;
    auto call = client.start(client.prepare(images::Request{"gpt-image-2", "a fox"}, run));
    executor->drain();
    const auto outcome = call.join();
    CHECK(std::holds_alternative<Failure>(outcome) && std::get<Failure>(outcome).error.reason == "runtime.error_body.limit");
  }
}

// RFC 3339 times name real days: February has 28 or 29, and the short months 30.
void calendar_dates() {
  const auto at = [](std::string_view text) { return endpoints::unix_seconds(text); };
  CHECK(at("2024-02-29T00:00:00Z") == 1709164800u && at("2000-02-29T00:00:00Z") && at("2026-02-28T23:59:59Z") && at("2026-04-30T00:00:00Z"));
  for (const char* text : {"2026-02-31T00:00:00Z", "2026-02-30T00:00:00Z", "2026-02-29T00:00:00Z", "2100-02-29T00:00:00Z", "2026-04-31T00:00:00Z",
                           "2026-06-31T00:00:00Z", "2026-09-31T00:00:00Z", "2026-11-31T00:00:00Z", "2026-01-32T00:00:00Z", "2026-01-00T00:00:00Z"})
    CHECK(!at(text));
  CHECK(at("2026-01-31T00:00:00Z") && at("2026-12-31T00:00:00+01:00"));
}

// A transcription asks for the response form its options name; timestamps come only in `verbose_json`.
void transcription_forms() {
  const auto openai = at("https://api.openai.com");
  const auto wav = std::make_shared<const std::string>(base64_encode(std::string("RIFF\x24\x00\x00\x00WAVEfmt synthetic", 24)));
  transcription::Request request{"whisper-1", Media::audio("audio/wav", wav)};
  const auto form = [&](std::string_view options) {
    auto r = request;
    if (!options.empty()) r.extensions = document(options);
    return encoded<transcription::Encoded>(transcription::encode, openai, r).body;
  };
  const auto fields = [](const std::string& body, std::string_view name) {
    std::size_t count = 0, at = 0;
    const std::string needle = "name=\"" + std::string(name) + "\"\r\n\r\n";
    while ((at = body.find(needle, at)) != std::string::npos) { ++count; at += needle.size(); }
    return count;
  };
  const auto format = [](const std::string& body) {
    const std::string needle = "name=\"response_format\"\r\n\r\n";
    const auto at = body.find(needle) + needle.size();
    return body.substr(at, body.find("\r\n", at) - at);
  };
  CHECK(format(form("")) == "json" && fields(form(""), "response_format") == 1);
  CHECK(format(form(R"({"temperature":0})")) == "json");
  // Timestamps ask for the verbose form, which the request may also name itself.
  CHECK(format(form(R"({"timestamp_granularities":["word","segment"]})")) == "verbose_json");
  CHECK(format(form(R"({"response_format":"verbose_json"})")) == "verbose_json" && fields(form(R"({"response_format":"verbose_json"})"), "response_format") == 1);
  const auto verbose = form(R"({"response_format":"verbose_json","timestamp_granularities":["word"]})");
  CHECK(format(verbose) == "verbose_json" && fields(verbose, "response_format") == 1 && fields(verbose, "timestamp_granularities[]") == 1);
  CHECK(format(form(R"({"response_format":"json"})")) == "json");
  // Timestamps cannot come in the plain form, and the policy admits no other form.
  auto plain = request; plain.extensions = document(R"({"response_format":"json","timestamp_granularities":["word"]})");
  CHECK(refused(transcription::encode, openai, plain) == ErrorKind::InvalidRequest);
  auto other = request; other.extensions = document(R"({"response_format":"srt"})");
  CHECK(refused(transcription::encode, openai, other) == ErrorKind::InvalidRequest);
  // The verbose form reports the language and the audio's length, which the result carries.
  const auto spoken = decoded<transcription::Codec>(transcription::typed, openai,
      R"({"task":"transcribe","language":"english","duration":1.4,"text":"Hello there.","words":[{"word":"Hello","start":0.0,"end":0.4}]})", "whisper-1");
  const auto& result = std::get<transcription::Result>(spoken);
  CHECK(result.text == "Hello there." && result.language == "english" && result.seconds == 1.4);
  CHECK(result.raw_events[0].payload->root().get("words").size() == 1);
}

// A transcription fact lists the audio it takes: OpenAI's types and 25 MiB, beside the family's chat audio.
void transcription_audio() {
  const auto openai = at("https://api.openai.com"), router = at("https://openrouter.ai");
  const auto clip = [](std::size_t size) { return std::make_shared<const std::string>(base64_encode(std::string(size, 'a'))); };
  const auto tiny = clip(24);
  for (const auto& [mime, extension] : std::initializer_list<std::pair<const char*, const char*>>{
           {"audio/webm", "webm"}, {"audio/flac", "flac"}, {"audio/x-flac", "flac"}, {"audio/mp4", "mp4"}, {"audio/ogg", "ogg"}, {"audio/m4a", "m4a"},
           {"audio/x-m4a", "m4a"}, {"audio/mpeg", "mp3"}, {"audio/mp3", "mp3"}, {"audio/mpga", "mpga"}, {"audio/wav", "wav"}, {"audio/x-wav", "wav"},
           {"AUDIO/WEBM; codecs=opus", "webm"}}) {
    const auto wire = encoded<transcription::Encoded>(transcription::encode, openai, transcription::Request{"gpt-4o-mini-transcribe", Media::audio(mime, tiny)});
    CHECK(wire.body.find(std::string("filename=\"audio.") + extension + "\"\r\nContent-Type: " + mime + "\r\n\r\n") != std::string::npos);
  }
  // A type the fact does not list, or one that is not audio.
  CHECK(refused(transcription::encode, openai, transcription::Request{"m", Media::audio("audio/aac", tiny)}) == ErrorKind::InvalidRequest);
  CHECK(refused(transcription::encode, openai, transcription::Request{"m", Media::audio("audio/opus", tiny)}) == ErrorKind::InvalidRequest);
  // The fact's size limit, not the chat family's 10 MiB, and not the request limit of 16 MiB around the audio.
  constexpr std::size_t limit = 26214400;
  CHECK(encoded<transcription::Encoded>(transcription::encode, openai, transcription::Request{"m", Media::audio("audio/mpeg", clip(12u << 20))}).body.size() > (12u << 20));
  const auto largest = encoded<transcription::Encoded>(transcription::encode, openai, transcription::Request{"m", Media::audio("audio/mpeg", clip(limit))});
  CHECK(largest.body.size() > limit && largest.body.size() < limit + 4096);
  CHECK(refused(transcription::encode, openai, transcription::Request{"m", Media::audio("audio/mpeg", clip(limit + 1))}) == ErrorKind::ResourceLimit);
  // What surrounds the audio is still held to the request limit.
  auto prompted = transcription::Request{"m", Media::audio("audio/mpeg", clip(12u << 20))};
  prompted.prompt = std::string(17u << 20, 'p');
  CHECK(refused(transcription::encode, openai, prompted) == ErrorKind::ResourceLimit);
  // OpenRouter lists its own types and states no size, so the descriptor's audio limit applies.
  const auto routed = encoded<transcription::Encoded>(transcription::encode, router, transcription::Request{"m", Media::audio("audio/flac", tiny)});
  CHECK(document(routed.body)->root().get("input_audio").get("format").as_string() == "flac");
  CHECK(document(encoded<transcription::Encoded>(transcription::encode, router, transcription::Request{"m", Media::audio("audio/l16; rate=16000", tiny)}).body)->root().get("input_audio").get("format").as_string() == "pcm16");
  CHECK(refused(transcription::encode, router, transcription::Request{"m", Media::audio("audio/webm", tiny)}) == ErrorKind::InvalidRequest);
  CHECK(refused(transcription::encode, router, transcription::Request{"m", Media::audio("audio/wav", clip(12u << 20))}) == ErrorKind::ResourceLimit);
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
  try { embedding_endpoints(); embedding_counts(); rerank_endpoint(); rerank_results(); runtime_checks(); fact_headers(); moderation_endpoint(); speech_endpoint(); transcription_endpoint(); generation_endpoint(); mime_parameters_with_whitespace(); response_bounds(); calendar_dates(); transcription_forms(); transcription_audio(); endpoint_members_cover_encoders(); std::cout << "endpoint properties passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << "endpoint test failed: " << error.what() << '\n'; return 1; }
}
