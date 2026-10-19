#include "codecs/chat.h"
#include "codecs/gemini.h"
#include "codecs/gemini_request.h"
#include "codecs/interactions.h"
#include "codecs/interactions_request.h"
#include "codecs/responses.h"
#include "codecs/responses_request.h"
#include "core/native.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include "sp/config_defaults.h"
#include <iostream>
#include <stdexcept>

namespace {
using namespace sp;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (false)
json::Document parse(std::string_view bytes) {
  auto parsed = json::parse(bytes); CHECK(std::holds_alternative<json::Document>(parsed));
  return std::get<json::Document>(std::move(parsed));
}
std::shared_ptr<const json::Document> own(std::string_view bytes) { return std::make_shared<const json::Document>(parse(bytes)); }
descriptor::ValidatedDescriptor descriptor_for(std::string_view family, std::string_view model = "fixture-model") {
  const bool gemini = family == "google.generate";
  const std::string path = gemini ? "/v1beta/models/" + std::string(model) + ":generateContent" : family == "google.interactions" ? "/v1beta/interactions" : family == "openai.chat" ? "/v1/chat/completions" : "/v1/responses";
  const std::string stream = gemini ? "/v1beta/models/" + std::string(model) + ":streamGenerateContent?alt=sse" : path;
  auto loaded = descriptor::load("{\"descriptor_version\":1,\"revision\":1,\"id\":\"media-controls\",\"family\":" + json::quote(family) + ",\"connection\":{\"base_url\":\"http://127.0.0.1:18080\",\"paths\":{\"buffered\":" + json::quote(path) + ",\"streaming\":" + json::quote(stream) + "}}}");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}
template<class Request> Request request() {
  Request value; value.model = "fixture-model"; value.messages.push_back(Message{"", Role::User, {Text{"question"}}}); return value;
}
template<class Result> void invalid(const Result& result) { CHECK(std::holds_alternative<Error>(result)); CHECK(std::get<Error>(result).kind == ErrorKind::InvalidRequest); }
template<class Result> void replay_rejected(const Result& result) { CHECK(std::holds_alternative<Error>(result)); CHECK(std::get<Error>(result).kind == ErrorKind::ReplayIneligible); }
Message gemini_reply(const descriptor::ValidatedDescriptor& descriptor, const gemini::EncodedRequest& encoded) {
  Accumulator accumulator; gemini::Codec codec(descriptor, gemini::Mode::Buffered, accumulator, encoded.context);
  codec.buffered(R"({"modelVersion":"fixture-model","responseId":"g1","candidates":[{"index":0,"content":{"role":"model","parts":[{"text":"answer"}]},"finishReason":"STOP"}]})", {});
  CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));
  return std::get<Completion>(*accumulator.outcome()).messages.front();
}
Message interaction_reply(const descriptor::ValidatedDescriptor& descriptor, const interactions::EncodedRequest& encoded, bool image_summary = false) {
  Accumulator accumulator; interactions::Codec codec(descriptor, interactions::Mode::Buffered, accumulator, encoded.context);
  const std::string thought = image_summary ? R"({"type":"thought","signature":"sig","summary":[{"type":"image","mime_type":"image/jpeg","data":"AQID"},{"type":"text","text":"brief"}]},)" : "";
  codec.buffered("{\"id\":\"i1\",\"model\":\"fixture-model\",\"status\":\"completed\",\"steps\":[" + thought + R"({"type":"model_output","content":[{"type":"text","text":"answer"}]}]})", {});
  CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));
  return std::get<Completion>(*accumulator.outcome()).messages.front();
}
void gemini_nonthinking_media_request() {
  const auto descriptor = descriptor_for("google.generate", "gemini-2.5-flash-preview-tts");
  auto value = request<gemini::Request>();
  value.model = "gemini-2.5-flash-preview-tts";
  value.response_modalities = {gemini::ResponseModality::Audio};
  value.speech_config = gemini::SpeechConfig{gemini::VoiceConfig{std::string("Kore"), {}}, {}, {}};
  for (const bool streaming : {false, true}) {
    auto encoded = gemini::encode(descriptor, value, streaming);
    CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
    auto wire = parse(std::get<gemini::EncodedRequest>(encoded).body);
    // Non-thinking media models must not receive an unsolicited thinking directive.
    CHECK(!wire.root().get("generationConfig").get("thinkingConfig").valid());
    auto explicit_control = value; explicit_control.include_thoughts = true;
    encoded = gemini::encode(descriptor, explicit_control, streaming);
    CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
    wire = parse(std::get<gemini::EncodedRequest>(encoded).body);
    CHECK(wire.root().get("generationConfig").get("thinkingConfig").get("includeThoughts").as_bool());
  }
}
void gemini_boundaries_and_binding() {
  const auto descriptor = descriptor_for("google.generate");
  auto value = request<gemini::Request>();
  value.response_modalities = {gemini::ResponseModality::Audio};
  value.speech_config = gemini::SpeechConfig{gemini::VoiceConfig{std::string("Kore"), {}}, {}, std::string("en-US")};
  auto encoded = gemini::encode(descriptor, value, false); CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  auto wire = parse(std::get<gemini::EncodedRequest>(encoded).body);
  CHECK(wire.root().get("generationConfig").get("responseModalities").at(0).as_string() == "AUDIO");
  CHECK(wire.root().get("generationConfig").get("speechConfig").get("voiceConfig").get("prebuiltVoiceConfig").get("voiceName").as_string() == "Kore");
  auto next = value; next.messages.push_back(gemini_reply(descriptor, std::get<gemini::EncodedRequest>(encoded))); next.messages.push_back(Message{"", Role::User, {Text{"again"}}});
  CHECK(std::holds_alternative<gemini::EncodedRequest>(gemini::encode(descriptor, next, false)));
  auto changed = next; changed.speech_config->language_code = "fr-FR"; replay_rejected(gemini::encode(descriptor, changed, false));
  changed = next; changed.speech_config->voice_config->prebuilt_voice_name = "Puck"; replay_rejected(gemini::encode(descriptor, changed, false));
  changed = next; changed.response_modalities = {gemini::ResponseModality::Text, gemini::ResponseModality::Audio}; replay_rejected(gemini::encode(descriptor, changed, false));
  auto bad = value; bad.speech_config->voice_config->voice = "voice_custom"; invalid(gemini::encode(descriptor, bad, false));
  bad = value; bad.speech_config->speaker_voice_configs.push_back({"Joe", {std::string("Puck"), {}}}); invalid(gemini::encode(descriptor, bad, false));
  bad = value; bad.response_modalities.push_back(gemini::ResponseModality::Audio); invalid(gemini::encode(descriptor, bad, false));
  value = request<gemini::Request>(); value.response_modalities = {gemini::ResponseModality::Image}; value.image_config = gemini::ImageConfig{std::string("16:9"), std::string("2K")};
  encoded = gemini::encode(descriptor, value, false); CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  next = value; next.messages.push_back(gemini_reply(descriptor, std::get<gemini::EncodedRequest>(encoded))); next.messages.push_back(Message{"", Role::User, {Text{"again"}}});
  changed = next; changed.image_config->image_size = "4K"; replay_rejected(gemini::encode(descriptor, changed, false));
  value = request<gemini::Request>(); value.audio_transcription_config = gemini::AudioTranscriptionConfig{{"en-US"}, {}, true, true, gemini::TranscriptionMode::Verbatim};
  encoded = gemini::encode(descriptor, value, false); CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  wire = parse(std::get<gemini::EncodedRequest>(encoded).body); CHECK(wire.root().get("generationConfig").get("audioTranscriptionConfig").get("wordTimestamp").as_bool());
  next = value; next.messages.push_back(gemini_reply(descriptor, std::get<gemini::EncodedRequest>(encoded))); next.messages.push_back(Message{"", Role::User, {Text{"again"}}});
  changed = next; changed.audio_transcription_config->diarization = false; replay_rejected(gemini::encode(descriptor, changed, false));
  bad = value; bad.audio_transcription_config->mode = gemini::TranscriptionMode::Smart; invalid(gemini::encode(descriptor, bad, false));
  bad = value; bad.audio_transcription_config->custom_vocabulary = {"NeoGraph"}; invalid(gemini::encode(descriptor, bad, false));
  bad = value; bad.audio_transcription_config->word_timestamp = false; bad.audio_transcription_config->diarization = false; bad.audio_transcription_config->mode = gemini::TranscriptionMode::Smart;
  CHECK(std::holds_alternative<gemini::EncodedRequest>(gemini::encode(descriptor, bad, false)));
}
void gemini_builtin_tools_and_audio_format() {
  const auto descriptor = descriptor_for("google.generate");
  auto value = request<gemini::Request>();
  value.builtin_tools = {gemini::GoogleSearchTool{}, gemini::UrlContextTool{}, gemini::CodeExecutionTool{}, gemini::GoogleMapsTool{}};
  auto encoded = gemini::encode(descriptor, value, false); CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  auto wire = parse(std::get<gemini::EncodedRequest>(encoded).body);
  auto tools = wire.root().get("tools");
  CHECK(tools.size() == 4 && tools.at(0).get("googleSearch").size() == 0 && tools.at(1).get("urlContext").is_object() &&
        tools.at(2).get("codeExecution").is_object() && tools.at(3).get("googleMaps").size() == 0);
  CHECK(!wire.root().get("toolConfig").valid());
  // Next to a client function the service requires its own invocations in the answer: on by default, and an
  // explicit choice is kept beside the function calling mode.
  auto combined = value; combined.builtin_tools = {gemini::GoogleSearchTool{}};
  combined.tools = {gemini::ToolDefinition{"lookup", "Looks up a record", own(R"({"type":"object","properties":{}})")}};
  encoded = gemini::encode(descriptor, combined, false); CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  wire = parse(std::get<gemini::EncodedRequest>(encoded).body);
  tools = wire.root().get("tools");
  CHECK(tools.size() == 2 && tools.at(0).get("functionDeclarations").size() == 1 && tools.at(1).get("googleSearch").is_object());
  CHECK(wire.root().get("toolConfig").get("includeServerSideToolInvocations").as_bool());
  auto chosen = combined; chosen.tool_choice = gemini::ToolChoice{gemini::ToolChoiceMode::Any, {"lookup"}};
  encoded = gemini::encode(descriptor, chosen, false); CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  wire = parse(std::get<gemini::EncodedRequest>(encoded).body);
  CHECK(wire.root().get("toolConfig").get("functionCallingConfig").get("mode").as_string() == "ANY" &&
        wire.root().get("toolConfig").get("includeServerSideToolInvocations").as_bool());
  auto off = combined; off.server_tool_invocations = false;
  encoded = gemini::encode(descriptor, off, false); CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  wire = parse(std::get<gemini::EncodedRequest>(encoded).body);
  CHECK(!wire.root().get("toolConfig").get("includeServerSideToolInvocations").as_bool());
  auto alone = value; alone.server_tool_invocations = true;
  encoded = gemini::encode(descriptor, alone, false); CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  wire = parse(std::get<gemini::EncodedRequest>(encoded).body);
  CHECK(wire.root().get("toolConfig").get("includeServerSideToolInvocations").as_bool() && !wire.root().get("toolConfig").get("functionCallingConfig").valid());
  auto bad = request<gemini::Request>(); bad.server_tool_invocations = true; invalid(gemini::encode(descriptor, bad, false));
  bad = value; bad.builtin_tools.push_back(gemini::GoogleSearchTool{}); invalid(gemini::encode(descriptor, bad, false));

  // The service's calls and results are typed parts, its candidate metadata stays in the stop details, and the next
  // turn sends the parts back as received.
  encoded = gemini::encode(descriptor, value, false); CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  Accumulator accumulator; gemini::Codec codec(descriptor, gemini::Mode::Buffered, accumulator, std::get<gemini::EncodedRequest>(encoded).context);
  codec.buffered(R"re({"modelVersion":"fixture-model","responseId":"g2","candidates":[{"index":0,"content":{"role":"model","parts":[)re"
                 R"re({"executableCode":{"language":"PYTHON","code":"print(1+1)"},"thoughtSignature":"c2ln"},{"codeExecutionResult":{"outcome":"OUTCOME_OK","output":"2\n"}},)re"
                 R"re({"toolCall":{"id":"s1","toolType":"GOOGLE_SEARCH_WEB","args":{"queries":["q"]}}},{"toolResponse":{"id":"s1","toolType":"GOOGLE_SEARCH_WEB","response":{}}},{"text":"2"}]},)re"
                 R"re("finishReason":"STOP","groundingMetadata":{"webSearchQueries":["q"]},"urlContextMetadata":{"urlMetadata":[]}}]})re", {});
  CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));
  const auto& completion = std::get<Completion>(*accumulator.outcome());
  const auto& parts = completion.messages.front().parts;
  CHECK(parts.size() == 5 && std::get<ToolCall>(parts[0]).wire_type == "executableCode" && std::get<ServerToolResult>(parts[1]).wire_type == "codeExecutionResult" &&
        std::get<ToolCall>(parts[2]).wire_type == "toolCall" && std::get<ServerToolResult>(parts[3]).wire_type == "toolResponse" && std::get<Text>(parts[4]).value == "2");
  CHECK(std::get<ToolCall>(parts[2]).name == "google_search_web" && std::get<ServerToolResult>(parts[3]).tool_use_id == "s1");
  CHECK(completion.stop.details && completion.stop.details->root().get("groundingMetadata").get("webSearchQueries").size() == 1 &&
        completion.stop.details->root().get("urlContextMetadata").is_object());
  auto next = value; next.messages.push_back(completion.messages.front()); next.messages.push_back(Message{"", Role::User, {Text{"again"}}});
  const auto replay = gemini::encode(descriptor, next, false); CHECK(std::holds_alternative<gemini::EncodedRequest>(replay));
  const auto replayed = parse(std::get<gemini::EncodedRequest>(replay).body);
  const auto sent = replayed.root().get("contents").at(1).get("parts");
  CHECK(sent.size() == 5 && sent.at(0).get("executableCode").get("code").as_string() == "print(1+1)" && sent.at(0).get("thoughtSignature").as_string() == "c2ln" &&
        sent.at(1).get("codeExecutionResult").is_object() && sent.at(2).get("toolCall").get("id").as_string() == "s1" && sent.at(3).get("toolResponse").is_object());
  // The tools are configuration. Showing the invocations and the retrieval location are per-turn choices.
  auto changed = next; changed.builtin_tools.pop_back(); replay_rejected(gemini::encode(descriptor, changed, false));
  changed = next; std::get<gemini::GoogleSearchTool>(changed.builtin_tools[0]).image_search = true; replay_rejected(gemini::encode(descriptor, changed, false));
  changed = next; changed.server_tool_invocations = true; changed.retrieval_config = gemini::RetrievalConfig{37.5, 127.0, std::string("ko-KR")};
  CHECK(std::holds_alternative<gemini::EncodedRequest>(gemini::encode(descriptor, changed, false)));

  value = request<gemini::Request>(); value.response_modalities = {gemini::ResponseModality::Audio};
  value.audio_format = gemini::AudioFormat{std::string("AUDIO_L16"), 24000u};
  encoded = gemini::encode(descriptor, value, false); CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  wire = parse(std::get<gemini::EncodedRequest>(encoded).body);
  auto audio = wire.root().get("generationConfig").get("responseFormat").get("audio");
  CHECK(audio.get("mimeType").as_string() == "AUDIO_L16" && audio.get("sampleRate").as_uint() == 24000u);
  next = value; next.messages.push_back(gemini_reply(descriptor, std::get<gemini::EncodedRequest>(encoded))); next.messages.push_back(Message{"", Role::User, {Text{"again"}}});
  CHECK(std::holds_alternative<gemini::EncodedRequest>(gemini::encode(descriptor, next, false)));
  changed = next; changed.audio_format->sample_rate = 16000u; replay_rejected(gemini::encode(descriptor, changed, false));
  changed = next; changed.audio_format->mime_type = "AUDIO_MP3"; replay_rejected(gemini::encode(descriptor, changed, false));
  auto rate_only = value; rate_only.audio_format = gemini::AudioFormat{std::nullopt, 16000u};
  encoded = gemini::encode(descriptor, rate_only, false); CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  wire = parse(std::get<gemini::EncodedRequest>(encoded).body);
  audio = wire.root().get("generationConfig").get("responseFormat").get("audio");
  CHECK(!audio.get("mimeType").valid() && audio.get("sampleRate").as_uint() == 16000u);
  // An empty format sends nothing and changes nothing.
  auto empty = value; empty.audio_format = gemini::AudioFormat{};
  encoded = gemini::encode(descriptor, empty, false); CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  CHECK(!parse(std::get<gemini::EncodedRequest>(encoded).body).root().get("generationConfig").get("responseFormat").valid());
  for (const auto* token : {"", "audio/l16", "AUDIO L16"}) { bad = value; bad.audio_format->mime_type = token; invalid(gemini::encode(descriptor, bad, false)); }
  bad = value; bad.audio_format->sample_rate = 0u; invalid(gemini::encode(descriptor, bad, false));
}
gemini::EncodedRequest encoded_ok(const descriptor::ValidatedDescriptor& descriptor, const gemini::Request& value) {
  auto encoded = gemini::encode(descriptor, value, false); CHECK(std::holds_alternative<gemini::EncodedRequest>(encoded));
  return std::get<gemini::EncodedRequest>(std::move(encoded));
}
void gemini_service_tool_options() {
  const auto descriptor = descriptor_for("google.generate");
  auto value = request<gemini::Request>();
  gemini::GoogleSearchTool search; search.start_time = "2026-01-01T00:00:00Z"; search.end_time = "2026-10-01T00:00:00Z"; search.web_search = search.image_search = true;
  gemini::ComputerUseTool computer; computer.excluded_functions = {"drag_and_drop"}; computer.disabled_safety_policies = {"SOME_POLICY"}; computer.prompt_injection_detection = true;
  gemini::FileSearchTool files; files.store_names = {"fileSearchStores/notes-1"}; files.metadata_filter = "kind = \"memo\""; files.top_k = 3;
  value.builtin_tools = {search, gemini::GoogleMapsTool{true}, computer, files};
  gemini::McpServer server; server.name = "docs"; server.url = "https://mcp.example.test/mcp"; server.headers = {{"Authorization", "Bearer one"}};
  server.timeout = "30s"; server.sse_read_timeout = "1.5s"; server.terminate_on_close = true;
  value.mcp_servers = {server};
  value.retrieval_config = gemini::RetrievalConfig{37.5665, 126.978, std::string("ko-KR")};
  value.audio_format = gemini::AudioFormat{std::string("AUDIO_MP3"), 24000u, 64000u, std::string("INLINE")};
  auto encoded = encoded_ok(descriptor, value);
  auto wire = parse(encoded.body);
  auto tools = wire.root().get("tools");
  CHECK(tools.size() == 5);
  auto google = tools.at(0).get("googleSearch");
  CHECK(google.get("timeRangeFilter").get("startTime").as_string() == "2026-01-01T00:00:00Z" && google.get("timeRangeFilter").get("endTime").as_string() == "2026-10-01T00:00:00Z" &&
        google.get("searchTypes").get("webSearch").is_object() && google.get("searchTypes").get("imageSearch").is_object());
  CHECK(tools.at(1).get("googleMaps").get("enableWidget").as_bool());
  auto use = tools.at(2).get("computerUse");
  CHECK(use.get("environment").as_string() == "ENVIRONMENT_BROWSER" && use.get("excludedPredefinedFunctions").at(0).as_string() == "drag_and_drop" &&
        use.get("disabledSafetyPolicies").at(0).as_string() == "SOME_POLICY" && use.get("enablePromptInjectionDetection").as_bool());
  auto search_files = tools.at(3).get("fileSearch");
  CHECK(search_files.get("fileSearchStoreNames").at(0).as_string() == "fileSearchStores/notes-1" && search_files.get("metadataFilter").as_string() == "kind = \"memo\"" &&
        search_files.get("topK").as_uint() == 3u);
  auto transport = tools.at(4).get("mcpServers").at(0);
  CHECK(transport.get("name").as_string() == "docs");
  transport = transport.get("streamableHttpTransport");
  CHECK(transport.get("url").as_string() == "https://mcp.example.test/mcp" && transport.get("headers").get("Authorization").as_string() == "Bearer one" &&
        transport.get("timeout").as_string() == "30s" && transport.get("sseReadTimeout").as_string() == "1.5s" && transport.get("terminateOnClose").as_bool());
  auto config = wire.root().get("toolConfig");
  CHECK(!config.get("includeServerSideToolInvocations").valid() && config.get("retrievalConfig").get("latLng").get("latitude").as_double() == 37.5665 &&
        config.get("retrievalConfig").get("latLng").get("longitude").as_double() == 126.978 && config.get("retrievalConfig").get("languageCode").as_string() == "ko-KR");
  auto audio = wire.root().get("generationConfig").get("responseFormat").get("audio");
  CHECK(audio.get("mimeType").as_string() == "AUDIO_MP3" && audio.get("sampleRate").as_uint() == 24000u && audio.get("bitRate").as_uint() == 64000u && audio.get("delivery").as_string() == "INLINE");

  // A rotated MCP credential keeps the history replayable; another header name, URL or tool option does not.
  auto next = value; next.messages.push_back(gemini_reply(descriptor, encoded)); next.messages.push_back(Message{"", Role::User, {Text{"again"}}});
  auto changed = next; changed.mcp_servers[0].headers[0].second = "Bearer two"; encoded_ok(descriptor, changed);
  changed = next; changed.mcp_servers[0].headers[0].first = "X-Key"; replay_rejected(gemini::encode(descriptor, changed, false));
  changed = next; changed.mcp_servers[0].url = "https://other.example.test/mcp"; replay_rejected(gemini::encode(descriptor, changed, false));
  changed = next; std::get<gemini::FileSearchTool>(changed.builtin_tools[3]).top_k = 4; replay_rejected(gemini::encode(descriptor, changed, false));
  changed = next; std::get<gemini::ComputerUseTool>(changed.builtin_tools[2]).excluded_functions.clear(); replay_rejected(gemini::encode(descriptor, changed, false));
  changed = next; changed.audio_format->delivery = "URI"; replay_rejected(gemini::encode(descriptor, changed, false));

  auto refused = [&](auto edit) { auto bad = value; edit(bad); invalid(gemini::encode(descriptor, bad, false)); };
  refused([](auto& r) { std::get<gemini::GoogleSearchTool>(r.builtin_tools[0]).end_time.reset(); });
  refused([](auto& r) { std::get<gemini::GoogleSearchTool>(r.builtin_tools[0]).start_time = "yesterday"; });
  refused([](auto& r) { std::get<gemini::ComputerUseTool>(r.builtin_tools[2]).environment = "browser"; });
  refused([](auto& r) { std::get<gemini::ComputerUseTool>(r.builtin_tools[2]).excluded_functions = {"a", "a"}; });
  refused([](auto& r) { std::get<gemini::FileSearchTool>(r.builtin_tools[3]).store_names.clear(); });
  refused([](auto& r) { std::get<gemini::FileSearchTool>(r.builtin_tools[3]).store_names = {"corpora/notes"}; });
  refused([](auto& r) { std::get<gemini::FileSearchTool>(r.builtin_tools[3]).top_k = 0u; });
  refused([](auto& r) { r.mcp_servers[0].url = "http://mcp.example.test/mcp"; });
  refused([](auto& r) { r.mcp_servers.push_back(r.mcp_servers[0]); });
  refused([](auto& r) { r.mcp_servers[0].headers.push_back({"authorization", "again"}); });
  refused([](auto& r) { r.mcp_servers[0].headers[0].second = "line\nbreak"; });
  refused([](auto& r) { r.mcp_servers[0].timeout = "30"; });
  refused([](auto& r) { r.retrieval_config->latitude = 91.0; });
  refused([](auto& r) { r.retrieval_config->longitude.reset(); });
  refused([](auto& r) { r.retrieval_config->language_code = "ko KR"; });
  refused([](auto& r) { r.audio_format->delivery = "uri"; });
  refused([](auto& r) { r.audio_format->bit_rate = 0u; });

  // Next to a client function only a built-in tool turns the invocations on: the service requires them there, not
  // for an MCP server, which may still ask for them. Computer use runs nothing on the service, so it neither needs
  // them nor may ask for them.
  auto schema = own(R"({"type":"object","properties":{}})");
  auto mcp_only = request<gemini::Request>(); mcp_only.mcp_servers = {server}; mcp_only.tools = {gemini::ToolDefinition{"lookup", "Looks up", schema}};
  CHECK(!parse(encoded_ok(descriptor, mcp_only).body).root().get("toolConfig").valid());
  mcp_only.server_tool_invocations = true;
  CHECK(parse(encoded_ok(descriptor, mcp_only).body).root().get("toolConfig").get("includeServerSideToolInvocations").as_bool());
  auto computer_only = request<gemini::Request>(); computer_only.builtin_tools = {gemini::ComputerUseTool{}}; computer_only.tools = mcp_only.tools;
  CHECK(!parse(encoded_ok(descriptor, computer_only).body).root().get("toolConfig").valid());
  auto bad = computer_only; bad.server_tool_invocations = true; invalid(gemini::encode(descriptor, bad, false));
  // The service's computer use functions may be required or allowed by name, which the request never declared.
  auto forced = request<gemini::Request>(); forced.builtin_tools = {gemini::ComputerUseTool{}}; forced.tool_choice = gemini::ToolChoice{gemini::ToolChoiceMode::Any, {"navigate"}};
  CHECK(parse(encoded_ok(descriptor, forced).body).root().get("toolConfig").get("functionCallingConfig").get("allowedFunctionNames").at(0).as_string() == "navigate");

  // Computer use: the model calls a function the service declared; the caller answers {"url": ...} and a screenshot.
  auto browse = request<gemini::Request>(); browse.builtin_tools = {gemini::ComputerUseTool{}};
  const auto sent = encoded_ok(descriptor, browse);
  Accumulator accumulator; gemini::Codec codec(descriptor, gemini::Mode::Buffered, accumulator, sent.context);
  codec.buffered(R"({"modelVersion":"fixture-model","responseId":"g3","candidates":[{"index":0,"content":{"role":"model","parts":[{"functionCall":{"id":"c1","name":"navigate","args":{"url":"https://example.com"}},"thoughtSignature":"c2ln"}]},"finishReason":"STOP"}]})", {});
  CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));
  const auto& completion = std::get<Completion>(*accumulator.outcome());
  const auto& call = std::get<ToolCall>(completion.messages.front().parts.front());
  CHECK(call.kind == ToolCallKind::ClientExecuted && call.name == "navigate" && call.id == "c1" && completion.stop.kind == StopKind::ToolUse);
  auto png = std::make_shared<const std::string>("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==");
  ToolResult answer; answer.tool_use_id = "c1"; answer.content = R"({"url":"https://example.com"})"; answer.content_parts = {Media::image("image/png", png)};
  auto turn = browse; turn.messages.push_back(completion.messages.front()); turn.messages.push_back(Message{"", Role::Tool, {answer}});
  const auto replayed = parse(encoded_ok(descriptor, turn).body);
  const auto response = replayed.root().get("contents").at(2).get("parts").at(0).get("functionResponse");
  CHECK(response.get("name").as_string() == "navigate" && response.get("response").get("url").as_string() == "https://example.com" &&
        response.get("parts").at(0).get("inlineData").get("mimeType").as_string() == "image/png");
  auto mixed = answer; mixed.content_parts.push_back(Text{"note"});
  auto bad_turn = turn; bad_turn.messages.back() = Message{"", Role::Tool, {mixed}}; invalid(gemini::encode(descriptor, bad_turn, false));
  auto not_object = answer; not_object.content = "plain";
  bad_turn.messages.back() = Message{"", Role::Tool, {not_object}}; invalid(gemini::encode(descriptor, bad_turn, false));
  // Without computer use the same call is a function nobody declared.
  Accumulator plain; gemini::Codec plain_codec(descriptor, gemini::Mode::Buffered, plain, encoded_ok(descriptor, request<gemini::Request>()).context);
  plain_codec.buffered(R"({"modelVersion":"fixture-model","responseId":"g4","candidates":[{"index":0,"content":{"role":"model","parts":[{"functionCall":{"id":"c1","name":"navigate","args":{}}}]},"finishReason":"STOP"}]})", {});
  CHECK(plain.outcome() && std::holds_alternative<Failure>(*plain.outcome()) && std::get<Failure>(*plain.outcome()).error.kind == ErrorKind::Unsupported);
}
// The service refuses its own MCP toolCall (it has no toolType) when it comes back. The policy lists that shape, so
// the SDK refuses the history before dispatch; removing the entry lifts the refusal, and the caller may project the
// answer into portable history instead.
void gemini_unreplayable_service_parts() {
  const std::string source = R"({"descriptor_version":1,"revision":1,"id":"media-controls","family":"google.generate","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1beta/models/fixture-model:generateContent","streaming":"/v1beta/models/fixture-model:streamGenerateContent?alt=sse"}}})";
  auto with_policy = [&](descriptor::PolicySnapshot policy) {
    auto loaded = policy ? descriptor::load(source, std::move(policy)) : descriptor::load(source);
    CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
    return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
  };
  const auto descriptor = with_policy({});
  std::string text(config_defaults::descriptor_policy_json);
  const std::string entry = R"("unreplayable_parts":[{"member":"toolCall","missing":"toolType"}])";
  const auto at = text.find(entry); CHECK(at != std::string::npos);
  text.replace(at, entry.size(), R"("unreplayable_parts":[])");
  auto lifted_policy = descriptor::load_policy(text, config_defaults::codec_defaults_json);
  CHECK(std::holds_alternative<descriptor::PolicySnapshot>(lifted_policy));
  const auto lifted = with_policy(std::get<descriptor::PolicySnapshot>(lifted_policy));

  auto value = request<gemini::Request>();
  gemini::McpServer server; server.name = "docs"; server.url = "https://mcp.example.test/mcp";
  value.mcp_servers = {server}; value.server_tool_invocations = true;
  auto answer = [&](const descriptor::ValidatedDescriptor& d, std::string_view call) {
    Accumulator accumulator; gemini::Codec codec(d, gemini::Mode::Buffered, accumulator, encoded_ok(d, value).context);
    codec.buffered(std::string(R"({"modelVersion":"fixture-model","responseId":"g5","candidates":[{"index":0,"content":{"role":"model","parts":[)") + std::string(call) +
                   R"(,{"toolResponse":{"id":"m1","response":{"result":"y"}}},{"text":"found","thoughtSignature":"c2ln"}]},"finishReason":"STOP"}]})", {});
    CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));
    return std::get<Completion>(*accumulator.outcome()).messages.front();
  };
  const std::string typeless = R"({"toolCall":{"id":"m1","toolName":"docs:search","args":{"q":"x"}},"thoughtSignature":"c2ln"})";
  const std::string typed = R"({"toolCall":{"id":"m1","toolType":"GOOGLE_SEARCH_WEB","args":{"q":"x"}},"thoughtSignature":"c2ln"})";
  auto turn = [&](const Message& reply) { auto next = value; next.messages.push_back(reply); next.messages.push_back(Message{"", Role::User, {Text{"again"}}}); return next; };
  const auto refused = gemini::encode(descriptor, turn(answer(descriptor, typeless)), false);
  CHECK(std::holds_alternative<Error>(refused) && std::get<Error>(refused).kind == ErrorKind::Unsupported &&
        std::get<Error>(refused).safe_message == "captured part is one the service refuses to take back");
  encoded_ok(descriptor, turn(answer(descriptor, typed)));
  encoded_ok(lifted, turn(answer(lifted, typeless)));
  // A client function call or code execution part never matches.
  CHECK(descriptor.family_policy().unreplayable_parts.size() == 1);

  // The projection keeps the text and the client calls; the caller sends it as portable history.
  const auto reply = answer(descriptor, typeless);
  const auto projected = gemini::project_portable(reply);
  CHECK(projected.message.role == Role::Assistant && !projected.message.native && !projected.message.wire_output && projected.message.parts.size() == 1 &&
        std::get<Text>(projected.message.parts[0]).value == "found" && projected.dropped == (std::vector<std::string>{"toolCall", "toolResponse"}));
  auto portable = turn(projected.message); portable.history_mode = gemini::HistoryMode::PortableForeign;
  const auto wire = parse(encoded_ok(descriptor, portable).body);
  const auto parts = wire.root().get("contents").at(1).get("parts");
  CHECK(parts.size() == 1 && parts.at(0).get("text").as_string() == "found" && !parts.at(0).get("thoughtSignature").valid());
  // Without the portable mode the projected message is not native history.
  CHECK(std::holds_alternative<Error>(gemini::encode(descriptor, turn(projected.message), false)));
}
void interaction_nonthinking_transcription_request() {
  const auto descriptor = descriptor_for("google.interactions");
  auto value = request<interactions::Request>();
  value.model = "gemini-3.5-transcribe";
  value.transcription_config = interactions::TranscriptionConfig{{"en-US"}, {}, interactions::TranscriptionMode::Verbatim, true, true};
  for (const bool streaming : {false, true}) {
    auto encoded = interactions::encode(descriptor, value, streaming);
    CHECK(std::holds_alternative<interactions::EncodedRequest>(encoded));
    auto wire = parse(std::get<interactions::EncodedRequest>(encoded).body);
    CHECK(!wire.root().get("generation_config").get("thinking_summaries").valid());
    auto explicit_control = value; explicit_control.thinking_summaries = true;
    encoded = interactions::encode(descriptor, explicit_control, streaming);
    CHECK(std::holds_alternative<interactions::EncodedRequest>(encoded));
    wire = parse(std::get<interactions::EncodedRequest>(encoded).body);
    CHECK(wire.root().get("generation_config").get("thinking_summaries").as_string() == "auto");
  }
}
void interaction_boundaries_and_binding() {
  const auto descriptor = descriptor_for("google.interactions");
  auto value = request<interactions::Request>();
  value.response_format = {interactions::AudioResponseFormat{std::string("audio/mp3"), 24000, 64000, interactions::Delivery::Inline}};
  value.speech_config = interactions::SpeakerConfig{{{std::string("Kore"), std::string("Joe"), std::string("en-US")}}, true};
  auto encoded = interactions::encode(descriptor, value, false); CHECK(std::holds_alternative<interactions::EncodedRequest>(encoded));
  auto wire = parse(std::get<interactions::EncodedRequest>(encoded).body); CHECK(wire.root().get("response_format").is_object());
  CHECK(wire.root().get("generation_config").get("speech_config").get("mode").as_string() == "conversational");
  auto next = value; next.messages.push_back(interaction_reply(descriptor, std::get<interactions::EncodedRequest>(encoded))); next.messages.push_back(Message{"", Role::User, {Text{"again"}}});
  CHECK(std::holds_alternative<interactions::EncodedRequest>(interactions::encode(descriptor, next, false)));
  auto changed = next; std::get<interactions::AudioResponseFormat>(changed.response_format[0]).bit_rate = 128000; replay_rejected(interactions::encode(descriptor, changed, false));
  changed = next; std::get<interactions::SpeakerConfig>(*changed.speech_config).conversational = false; replay_rejected(interactions::encode(descriptor, changed, false));
  // The service names the encodings: a bit rate beside WAV is sent, and only a MIME type of another kind is refused.
  auto wav = value; std::get<interactions::AudioResponseFormat>(wav.response_format[0]).mime_type = "audio/wav";
  CHECK(std::holds_alternative<interactions::EncodedRequest>(interactions::encode(descriptor, wav, false)));
  auto bad = value; std::get<interactions::AudioResponseFormat>(bad.response_format[0]).mime_type = "image/png"; invalid(interactions::encode(descriptor, bad, false));
  value = request<interactions::Request>();
  value.response_format = {interactions::TextResponseFormat{}, interactions::ImageResponseFormat{{}, std::string("1:8"), std::string("4K"), {}}};
  encoded = interactions::encode(descriptor, value, false); CHECK(std::holds_alternative<interactions::EncodedRequest>(encoded));
  wire = parse(std::get<interactions::EncodedRequest>(encoded).body); CHECK(wire.root().get("response_format").is_array());
  bad = value; std::get<interactions::TextResponseFormat>(bad.response_format[0]).schema = own(R"({"type":"object"})"); invalid(interactions::encode(descriptor, bad, false));
  std::get<interactions::TextResponseFormat>(bad.response_format[0]).mime_type = "application/json";
  CHECK(std::holds_alternative<interactions::EncodedRequest>(interactions::encode(descriptor, bad, false)));
  value = request<interactions::Request>(); value.response_format = {interactions::VideoResponseFormat{std::string("16:9"), std::string("4k"), std::string("8.5s"), {}, interactions::Delivery::Uri}};
  CHECK(std::holds_alternative<interactions::EncodedRequest>(interactions::encode(descriptor, value, false)));
  bad = value; std::get<interactions::VideoResponseFormat>(bad.response_format[0]).duration = "8.5"; invalid(interactions::encode(descriptor, bad, false));
  value = request<interactions::Request>(); value.transcription_config = interactions::TranscriptionConfig{{"en-US"}, {}, interactions::TranscriptionMode::Verbatim, true, true};
  encoded = interactions::encode(descriptor, value, false); CHECK(std::holds_alternative<interactions::EncodedRequest>(encoded));
  wire = parse(std::get<interactions::EncodedRequest>(encoded).body); const auto transcription = wire.root().get("generation_config").get("transcription_config");
  CHECK(!transcription.get("diarization_mode").valid() && transcription.get("mode").get("diarization_mode").as_string() == "speaker");
  next = value; next.messages.push_back(interaction_reply(descriptor, std::get<interactions::EncodedRequest>(encoded), true)); next.messages.push_back(Message{"", Role::User, {Text{"again"}}});
  CHECK(std::holds_alternative<interactions::EncodedRequest>(interactions::encode(descriptor, next, false)));
  changed = next; changed.transcription_config->word_timestamps = false; replay_rejected(interactions::encode(descriptor, changed, false));
  changed = next; changed.messages[1].wire_output = own(R"([{"type":"thought","signature":"sig","summary":[{"type":"image","mime_type":"image/jpeg","data":"BAUG"},{"type":"text","text":"brief"}]},{"type":"model_output","content":[{"type":"text","text":"answer"}]}])");
  replay_rejected(interactions::encode(descriptor, changed, false));
  bad = value; bad.transcription_config->mode = interactions::TranscriptionMode::Smart; invalid(interactions::encode(descriptor, bad, false));
  bad = value; bad.transcription_config->custom_vocabulary = {"NeoGraph"}; invalid(interactions::encode(descriptor, bad, false));
}
void responses_format_binding() {
  const auto descriptor = descriptor_for("openai.responses"); auto value = request<responses::Request>();
  value.hosted_tools.push_back(responses::ImageGenerationTool{{}, {}, std::string("webp")});
  auto encoded = responses::encode(descriptor, value, false); CHECK(std::holds_alternative<responses::EncodedRequest>(encoded));
  auto wire = parse(std::get<responses::EncodedRequest>(encoded).body); CHECK(wire.root().get("tools").at(0).get("output_format").as_string() == "webp");
  Accumulator accumulator; responses::Codec codec(descriptor, responses::Mode::Buffered, accumulator, std::get<responses::EncodedRequest>(encoded).context);
  codec.buffered(R"({"id":"r1","object":"response","created_at":1,"model":"fixture-model","status":"completed","output":[{"type":"message","id":"m1","role":"assistant","status":"completed","content":[{"type":"output_text","text":"answer","annotations":[]}]}],"error":null,"incomplete_details":null})", {});
  CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));
  auto next = value; next.messages.push_back(std::get<Completion>(*accumulator.outcome()).messages.front()); next.messages.push_back(Message{"", Role::User, {Text{"again"}}});
  CHECK(std::holds_alternative<responses::EncodedRequest>(responses::encode(descriptor, next, false)));
  std::get<responses::ImageGenerationTool>(next.hosted_tools[0]).output_format = "jpeg"; replay_rejected(responses::encode(descriptor, next, false));
  // The service names the formats: a well-formed new one is sent, a malformed one is refused.
  std::get<responses::ImageGenerationTool>(value.hosted_tools[0]).output_format = "gif"; CHECK(std::holds_alternative<responses::EncodedRequest>(responses::encode(descriptor, value, false)));
  std::get<responses::ImageGenerationTool>(value.hosted_tools[0]).output_format = "Not A Token"; invalid(responses::encode(descriptor, value, false));
}
// Values the providers extend (languages, ratios, sizes, encodings, schema keywords) are checked by shape: a new,
// well-formed value is sent, and a malformed one is refused.
void open_value_shapes() {
  auto generate = descriptor_for("google.generate");
  auto value = request<gemini::Request>();
  value.response_modalities = {gemini::ResponseModality::Audio};
  value.speech_config = gemini::SpeechConfig{gemini::VoiceConfig{std::string("Kore"), {}}, {}, std::string("sw-KE")};
  CHECK(std::holds_alternative<gemini::EncodedRequest>(gemini::encode(generate, value, false)));
  value.speech_config->language_code = "en US"; invalid(gemini::encode(generate, value, false));
  value = request<gemini::Request>(); value.response_modalities = {gemini::ResponseModality::Image};
  value.image_config = gemini::ImageConfig{std::string("7:3"), std::string("8K")};
  CHECK(std::holds_alternative<gemini::EncodedRequest>(gemini::encode(generate, value, false)));
  for (const auto& [ratio, size] : std::vector<std::pair<std::string, std::string>>{{"wide", "8K"}, {"7:3", "8 K"}, {"7:", "8K"}}) {
    auto bad = value; bad.image_config = gemini::ImageConfig{ratio, size}; invalid(gemini::encode(generate, bad, false));
  }
  auto interactions_descriptor = descriptor_for("google.interactions");
  auto ask = request<interactions::Request>();
  ask.response_format = {interactions::VideoResponseFormat{std::string("1:1"), std::string("8k"), {}, {}}, interactions::ImageResponseFormat{std::string("image/webp"), std::string("3:1"), std::string("8K"), {}}};
  CHECK(std::holds_alternative<interactions::EncodedRequest>(interactions::encode(interactions_descriptor, ask, false)));
  auto bad_ask = ask; std::get<interactions::VideoResponseFormat>(bad_ask.response_format[0]).resolution = "8 k"; invalid(interactions::encode(interactions_descriptor, bad_ask, false));
  bad_ask = ask; std::get<interactions::ImageResponseFormat>(bad_ask.response_format[1]).mime_type = "audio/wav"; invalid(interactions::encode(interactions_descriptor, bad_ask, false));
  // Chat names a new audio encoding and passes a schema keyword the SDK does not know.
  auto chat_descriptor = descriptor_for("openai.chat"); chat::Request speak; speak.model = "fixture-model";
  speak.messages.push_back({Role::User, "Speak"}); speak.modalities = {chat::OutputModality::Audio}; speak.audio = chat::AudioOutput{"alloy", "ogg"};
  speak.response_format = ResponseFormat{ResponseFormat::Kind::JsonSchema, "answer", {}, own(R"({"type":"object","x-provider-hint":{"any":true},"properties":{}})"), {}};
  auto spoken = chat::encode(chat_descriptor, speak, false); CHECK(std::holds_alternative<chat::EncodedRequest>(spoken));
  CHECK(std::get<chat::EncodedRequest>(spoken).context->audio_format() == "ogg");
  auto bad_speak = speak; bad_speak.audio->format = "OGG!"; CHECK(std::holds_alternative<Error>(chat::encode(chat_descriptor, bad_speak, false)));
  // A known keyword keeps its shape check.
  bad_speak = speak; bad_speak.response_format->schema = own(R"({"type":"object","minLength":-1})"); CHECK(std::holds_alternative<Error>(chat::encode(chat_descriptor, bad_speak, false)));
}
void decode_only_audio_scope() {
  const auto descriptor = descriptor_for("openai.chat"); chat::Request value; value.model = "fixture-model";
  value.messages.push_back({Role::User, "Speak"}); value.modalities = {chat::OutputModality::Audio}; value.audio = chat::AudioOutput{"alloy", "wav"};
  auto encoded = chat::encode(descriptor, value, false); CHECK(std::holds_alternative<chat::EncodedRequest>(encoded));
  const auto context = std::get<chat::EncodedRequest>(encoded).context->decoding_only(); CHECK(context->audio_format() == "wav" && !context->replay_eligible());
  Accumulator accumulator; chat::Codec codec(descriptor, chat::Mode::Buffered, accumulator, {}, context);
  codec.buffered(R"({"id":"c1","object":"chat.completion","created":1,"model":"fixture-model","choices":[{"index":0,"message":{"role":"assistant","content":null,"audio":{"id":"a1","data":"AQID","expires_at":123,"transcript":"Hello"}},"finish_reason":"stop"}]})", {});
  CHECK(accumulator.outcome() && std::holds_alternative<Completion>(*accumulator.outcome()));
  const auto& completion = std::get<Completion>(*accumulator.outcome()); CHECK(std::get<Media>(completion.messages.front().parts.front()).mime == "audio/wav");
}
}
int main() {
  try { gemini_nonthinking_media_request(); interaction_nonthinking_transcription_request(); gemini_boundaries_and_binding(); gemini_builtin_tools_and_audio_format(); gemini_service_tool_options(); gemini_unreplayable_service_parts(); interaction_boundaries_and_binding(); responses_format_binding(); decode_only_audio_scope(); open_value_shapes(); }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
