// Batches (D7): OpenAI's JSONL input, each shape's submit, get, cancel, delete and results requests, the batch
// objects the providers answer, and result lines decoded through the family codecs. The fixtures are live answers
// of 2026-10-19 to two-item batches, with one failed line of each kind added.
// Usage: sp_batches_tests <fixture directory>
#include "codecs/batches.h"
#include "codecs/batches_internal.h"
#include "descriptor/presets.h"
#include "json/json.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
using namespace sp;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)
std::filesystem::path fixtures;
std::string read(const std::string& name) {
  std::ifstream in(fixtures / name, std::ios::binary);
  CHECK(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
std::vector<std::string> lines(const std::string& name) {
  std::vector<std::string> out;
  std::istringstream in(read(name));
  for (std::string line; std::getline(in, line);) if (!line.empty()) out.push_back(line);
  return out;
}
std::shared_ptr<const json::Document> document(std::string_view text) {
  auto parsed = json::parse(text);
  CHECK(std::holds_alternative<json::Document>(parsed));
  return std::make_shared<const json::Document>(std::move(std::get<json::Document>(parsed)));
}
template <class Request> endpoints::Encoded encoded(const descriptor::ValidatedDescriptor& d, const Request& r) {
  auto result = batches::encode(d, r);
  if (const auto* error = std::get_if<Error>(&result)) throw std::runtime_error("encode refused: " + error->safe_message);
  return std::get<endpoints::Encoded>(std::move(result));
}
template <class Request> ErrorKind refused(const descriptor::ValidatedDescriptor& d, const Request& r) {
  auto result = batches::encode(d, r);
  CHECK(std::holds_alternative<Error>(result));
  return std::get<Error>(result).kind;
}
std::string text_of(const sp::Outcome& outcome) {
  CHECK(std::holds_alternative<Completion>(outcome));
  std::string text;
  for (const auto& message : std::get<Completion>(outcome).messages)
    for (const auto& part : message.parts) if (const auto* value = std::get_if<Text>(&part)) text += value->value;
  return text;
}
const descriptor::ValidatedDescriptor& openai_responses() { static const auto d = descriptor::presets::openai_responses(); return d; }
const descriptor::ValidatedDescriptor& openai_chat() { static const auto d = descriptor::presets::openai_chat(); return d; }
const descriptor::ValidatedDescriptor& anthropic() { static const auto d = descriptor::presets::anthropic_messages(); return d; }
const descriptor::ValidatedDescriptor& google() { static const auto d = descriptor::presets::google_generate(); return d; }
responses::Request responses_item(std::string prompt) {
  auto request = responses::Request::simple("gpt-5-nano", std::move(prompt), 64);
  request.reasoning = responses::ReasoningOptions{"minimal", std::nullopt};
  return request;
}

void requests() {
  // OpenAI: the input file, one line per item, then a submit that names it.
  const auto file = std::get<std::string>(batches::jsonl(openai_responses(), {{"o-1", responses_item("Say: alpha")}, {"o-2", responses_item("Say: beta")}}));
  std::istringstream in(file);
  std::string first;
  std::getline(in, first);
  const auto line = document(first);
  CHECK(line->root().get("custom_id").as_string() == "o-1" && line->root().get("method").as_string() == "POST" &&
        line->root().get("url").as_string() == "/v1/responses" && line->root().get("body").get("model").as_string() == "gpt-5-nano");
  CHECK(std::count(file.begin(), file.end(), '\n') == 2);
  CHECK(std::holds_alternative<Error>(batches::jsonl(openai_responses(), {{"o 1", responses_item("x")}})));
  CHECK(std::holds_alternative<Error>(batches::jsonl(openai_responses(), {{"o-1", responses_item("x")}, {"o-1", responses_item("y")}})));
  CHECK(std::holds_alternative<Error>(batches::jsonl(anthropic(), {{"a-1", messages::Request::simple("claude-haiku-4-5", "x", 16)}})));
  const auto submit = encoded(openai_chat(), batches::Submit{{}, "file-9suwM1zJjY9fNVs7mT5dpw", "/v1/responses"});
  CHECK(submit.path == "/v1/batches" && submit.charge_on_accept &&
        submit.body == R"({"input_file_id":"file-9suwM1zJjY9fNVs7mT5dpw","endpoint":"/v1/responses","completion_window":"24h"})");
  auto metadata = batches::Submit{{}, "file-9", "/v1/chat/completions"};
  metadata.extensions = document(R"({"metadata":{"run":"7"}})");
  CHECK(document(encoded(openai_chat(), metadata).body)->root().get("metadata").get("run").as_string() == "7");
  CHECK(refused(openai_chat(), batches::Submit{{{"o-1", responses_item("x")}}, "file-9", "/v1/responses"}) == ErrorKind::InvalidRequest);
  CHECK(refused(openai_chat(), batches::Submit{{}, "file-9", "v1/responses"}) == ErrorKind::InvalidRequest);
  // The path the lines call is any origin-relative path a descriptor may have, not one under /v1/.
  CHECK(document(encoded(openai_chat(), batches::Submit{{}, "file-9", "/openai/v1/Responses:batch"}).body)->root().get("endpoint").as_string() ==
        "/openai/v1/Responses:batch");
  for (const auto* path : {"/v1/../responses", "/v1/./responses", "/v1//responses", "/v1/%2e%2e/responses", "/v1/responses?x=1", "/v1/responses#x",
                           "//evil.example/v1/responses", "https://evil.example/v1/responses", "/v1/{model}/responses", "/v1/responses x", ""})
    CHECK(refused(openai_chat(), batches::Submit{{}, "file-9", path}) == ErrorKind::InvalidRequest);
  // A line of a descriptor whose path names `{model}` calls its item's model path.
  {
    auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"routed-chat","family":"openai.chat","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/models/{model}/chat","streaming":"/v1/models/{model}/chat"}}})");
    CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
    const auto& routed = std::get<descriptor::ValidatedDescriptor>(loaded);
    const auto lines = batches::jsonl(routed, {{"c-1", chat::Request::simple("gpt-x", "Say: alpha", 16)}});
    CHECK(std::holds_alternative<std::string>(lines));
    CHECK(document(std::get<std::string>(lines).substr(0, std::get<std::string>(lines).find('\n')))->root().get("url").as_string() == "/v1/models/gpt-x/chat");
    // An item whose model cannot take the place of `{model}` is refused, and no file is written.
    for (const auto* model : {"..", ".", "a..b", "a/b", "a?x", "a#x"}) {
      const auto refused_lines = batches::jsonl(routed, {{"c-1", chat::Request::simple("gpt-x", "Say: alpha", 16)}, {"c-2", chat::Request::simple(model, "Say: beta", 16)}});
      CHECK(std::holds_alternative<Error>(refused_lines) && std::get<Error>(refused_lines).kind == ErrorKind::InvalidRequest);
    }
  }
  // Anthropic: the items inline, each as the Messages body it would send.
  const auto inline_submit = encoded(anthropic(), batches::Submit{{{"a-1", messages::Request::simple("claude-haiku-4-5", "Say: alpha", 16)},
                                                                    {"a-2", messages::Request::simple("claude-haiku-4-5", "Say: beta", 16)}}});
  {
    const auto body = document(inline_submit.body);
    const auto requests = body->root().get("requests");
    CHECK(inline_submit.path == "/v1/messages/batches" && inline_submit.auth == "x-api-key" && requests.size() == 2 &&
          requests.at(1).get("custom_id").as_string() == "a-2" && requests.at(0).get("params").get("max_tokens").as_uint() == 16 &&
          requests.at(0).get("params").get("messages").at(0).get("role").as_string() == "user");
  }
  CHECK(refused(anthropic(), batches::Submit{{{"g-1", gemini::Request::simple("gemini-3.5-flash-lite", "x")}}}) == ErrorKind::Unsupported);
  CHECK(refused(anthropic(), batches::Submit{}) == ErrorKind::InvalidRequest);
  // Google: one model per batch, named in the path; each item keyed by its custom id.
  auto google_submit = batches::Submit{{{"g-1", gemini::Request::simple("gemini-3.5-flash-lite", "Say: alpha")}, {"g-2", gemini::Request::simple("gemini-3.5-flash-lite", "Say: beta")}}};
  google_submit.display_name = "sp-probe";
  const auto google_wire = encoded(google(), google_submit);
  {
    const auto body = document(google_wire.body);
    const auto items = body->root().get("batch").get("input_config").get("requests").get("requests");
    CHECK(google_wire.path == "/v1beta/models/gemini-3.5-flash-lite:batchGenerateContent" && body->root().get("batch").get("display_name").as_string() == "sp-probe" &&
          items.size() == 2 && items.at(1).get("metadata").get("key").as_string() == "g-2" && items.at(0).get("request").get("contents").size() == 1);
  }
  auto mixed = google_submit; mixed.items[1].request = gemini::Request::simple("gemini-3.5-flash", "Say: beta");
  CHECK(refused(google(), mixed) == ErrorKind::InvalidRequest);
  // Reads and controls.
  CHECK(encoded(openai_chat(), batches::Get{"batch_6ad6245d717c81908adf45db42ffb5b1"}).path == "/v1/batches/batch_6ad6245d717c81908adf45db42ffb5b1");
  const auto cancel = encoded(anthropic(), batches::Cancel{"msgbatch_018n69fDWEJGz4keUDwo56z9"});
  CHECK(cancel.method == "POST" && cancel.path == "/v1/messages/batches/msgbatch_018n69fDWEJGz4keUDwo56z9/cancel" && cancel.body.empty());
  CHECK(encoded(google(), batches::Delete{"batches/007px40llh0dun3cnjhu2le17battr0ufiui"}).method == "DELETE");
  CHECK(encoded(google(), batches::Get{"batches/007px40llh0dun3cnjhu2le17battr0ufiui"}).path == "/v1beta/batches/007px40llh0dun3cnjhu2le17battr0ufiui");
  CHECK(refused(openai_chat(), batches::Delete{"batch_1"}) == ErrorKind::Unsupported);
  CHECK(refused(google(), batches::Get{"operations/x"}) == ErrorKind::InvalidRequest);
  // `.` and `..` are made of token characters, but a request for them leaves the batches path.
  for (const char* id : {".", ".."}) {
    CHECK(refused(openai_chat(), batches::Get{id}) == ErrorKind::InvalidRequest);
    CHECK(refused(openai_chat(), batches::Cancel{id}) == ErrorKind::InvalidRequest);
    CHECK(refused(anthropic(), batches::Get{id}) == ErrorKind::InvalidRequest);
    CHECK(refused(anthropic(), batches::Cancel{id}) == ErrorKind::InvalidRequest);
    CHECK(refused(anthropic(), batches::Delete{id}) == ErrorKind::InvalidRequest);
    CHECK(refused(google(), batches::Get{std::string("batches/") + id}) == ErrorKind::InvalidRequest);
    CHECK(refused(google(), batches::Cancel{std::string("batches/") + id}) == ErrorKind::InvalidRequest);
    CHECK(refused(google(), batches::Delete{std::string("batches/") + id}) == ErrorKind::InvalidRequest);
  }
  CHECK(encoded(openai_chat(), batches::Get{"batch..1"}).path == "/v1/batches/batch..1");
}

batches::Outcome batch_of(const descriptor::ValidatedDescriptor& d, std::string_view body, batches::Codec::Kind kind = batches::Codec::Kind::Batch) {
  Accumulator accumulator;
  batches::Codec codec(d, accumulator, kind, "subject");
  codec.buffered(body, {});
  CHECK(accumulator.outcome());
  return batches::typed(*accumulator.outcome());
}
// The caps each item's family encoder reads, kept from the one encoding a submit sends, and the bytes and headers of
// the public encoders unchanged by it.
void captured_caps() {
  const auto same = [](const endpoints::Encoded& a, const endpoints::Encoded& b) {
    return a.path == b.path && a.body == b.body && a.model == b.model && a.headers == b.headers && a.method == b.method &&
           a.charge_on_accept == b.charge_on_accept && a.content_type == b.content_type;
  };
  const auto caps_of = [](const std::vector<batches::detail::ItemCaps>& items) {
    std::string out;
    const auto number = [](const std::optional<std::uint64_t>& value) { return value ? std::to_string(*value) : std::string("-"); };
    for (const auto& item : items) out += (out.empty() ? "" : " ") + item.custom_id + "/" + item.model + "/" + number(item.max_output_tokens) + "/" + number(item.model_invocation_limit);
    return out;
  };
  const batches::Submit inline_anthropic{{{"a-1", messages::Request::simple("claude-haiku-4-5", "Say: alpha", 16)},
                                          {"a-2", messages::Request::simple("claude-haiku-4-5", "Say: beta", 32)}}};
  auto anthropic_submit = std::get<batches::detail::EncodedSubmit>(batches::detail::encode_submit(anthropic(), inline_anthropic));
  CHECK(same(anthropic_submit.encoded, encoded(anthropic(), inline_anthropic)));
  CHECK(caps_of(anthropic_submit.items) == "a-1/claude-haiku-4-5/16/1 a-2/claude-haiku-4-5/32/1");
  const batches::Submit inline_google{{{"g-1", gemini::Request::simple("gemini-3.5-flash-lite", "Say: alpha", 8)},
                                       {"g-2", gemini::Request::simple("gemini-3.5-flash-lite", "Say: beta")}}};
  auto google_submit = std::get<batches::detail::EncodedSubmit>(batches::detail::encode_submit(google(), inline_google));
  CHECK(same(google_submit.encoded, encoded(google(), inline_google)));
  CHECK(caps_of(google_submit.items).starts_with("g-1/gemini-3.5-flash-lite/8/1 g-2/gemini-3.5-flash-lite/"));
  // OpenAI's items are in the file: the submit carries none.
  const batches::Submit from_file{{}, "file-abc", "/v1/responses"};
  auto openai_submit = std::get<batches::detail::EncodedSubmit>(batches::detail::encode_submit(openai_responses(), from_file));
  CHECK(same(openai_submit.encoded, encoded(openai_responses(), from_file)) && openai_submit.items.empty());
  // The input file: the same bytes as jsonl, with each item's caps.
  const std::vector<batches::Item> items{{"o-1", responses_item("Say: alpha")}, {"o-2", responses::Request::simple("gpt-5-nano", "Say: beta", 128)}};
  auto lines = std::get<batches::detail::EncodedLines>(batches::detail::encode_lines(openai_responses(), items));
  CHECK(lines.text == std::get<std::string>(batches::jsonl(openai_responses(), items)));
  CHECK(caps_of(lines.items) == "o-1/gpt-5-nano/64/1 o-2/gpt-5-nano/128/1");
  // A refusal is the public encoder's.
  CHECK(std::get<Error>(batches::detail::encode_submit(anthropic(), batches::Submit{})).safe_message == "a batch has items");
  CHECK(std::get<Error>(batches::detail::encode_lines(anthropic(), items)).safe_message == "an OpenAI batch file holds Chat or Responses requests");
}

void batch_objects() {
  const auto openai = std::get<batches::Batch>(batch_of(openai_chat(), read("openai-batch.json")));
  CHECK(openai.id == "batch_6ad6245d717c81908adf45db42ffb5b1" && openai.status == jobs::Status::Completed && openai.counts.total == 2 &&
        openai.counts.succeeded == 2 && openai.counts.pending == 0 && !openai.output_file.empty() && openai.usage.output_total->value == 38 &&
        openai.ended_at && openai.raw);
  const auto claude = std::get<batches::Batch>(batch_of(anthropic(), read("anthropic-batch.json")));
  CHECK(claude.id == "msgbatch_018n69fDWEJGz4keUDwo56z9" && claude.status == jobs::Status::Completed && claude.raw_status == "ended" &&
        claude.counts.succeeded == 2 && claude.counts.total == 2 && claude.results_ready && claude.created_at == 1792418907u);
  const auto gemini = std::get<batches::Batch>(batch_of(google(), read("google-batch.json")));
  CHECK(gemini.id == "batches/007px40llh0dun3cnjhu2le17battr0ufiui" && gemini.status == jobs::Status::Completed && gemini.counts.total == 2 &&
        gemini.counts.succeeded == 2 && gemini.results_ready && gemini.output_file.empty() && batches::inline_lines(gemini).size() == 2);
  // A batch still running.
  CHECK(std::get<batches::Batch>(batch_of(anthropic(), R"({"id":"msgbatch_1","processing_status":"in_progress","request_counts":{"processing":2}})")).status == jobs::Status::Running);
  CHECK(std::get<batches::Batch>(batch_of(openai_chat(), R"({"id":"batch_1","status":"validating","request_counts":{"total":0}})")).status == jobs::Status::Pending);
  CHECK(std::holds_alternative<Failure>(batch_of(anthropic(), R"({"id":"msgbatch_1"})")));
  CHECK(std::holds_alternative<Failure>(batch_of(google(), R"({"name":"operations/x","metadata":{"state":"BATCH_STATE_RUNNING"}})")));
  // Results: OpenAI's files, Anthropic's results path, Google's responses file.
  auto results = encoded(openai_chat(), batches::Results{openai, "/tmp/out.jsonl"});
  CHECK(results.path == "/v1/files/" + openai.output_file + "/content" && results.accept == "*/*" && results.sink == "/tmp/out.jsonl");
  CHECK(refused(openai_chat(), batches::Results{openai, {}, 1u << 20, true}) == ErrorKind::InvalidRequest);   // no error file
  CHECK(encoded(anthropic(), batches::Results{claude}).path == "/v1/messages/batches/msgbatch_018n69fDWEJGz4keUDwo56z9/results");
  auto filed = gemini; filed.output_file = "files/batch-out";
  CHECK(encoded(google(), batches::Results{filed}).path == "/v1beta/files/batch-out:download?alt=media");
  CHECK(refused(google(), batches::Results{gemini}) == ErrorKind::InvalidRequest);   // its results are inline
  auto foreign = claude; foreign.origin = "https://api.openai.com";
  CHECK(refused(anthropic(), batches::Results{foreign}) == ErrorKind::InvalidRequest);
  // The results file is a file id, not a dot segment.
  auto dotted = openai; dotted.output_file = "..";
  CHECK(refused(openai_chat(), batches::Results{dotted}) == ErrorKind::InvalidRequest);
  auto dotted_file = gemini; dotted_file.output_file = "files/..";
  CHECK(refused(google(), batches::Results{dotted_file}) == ErrorKind::InvalidRequest);
  auto dotted_batch = claude; dotted_batch.id = "..";
  CHECK(refused(anthropic(), batches::Results{dotted_batch}) == ErrorKind::InvalidRequest);
}

void result_lines() {
  // Each line decodes as the outcome its own request would have had.
  const auto openai = lines("openai-results.jsonl");
  for (std::size_t i = 0; i < 2; ++i) {
    const auto id = batches::custom_id(openai[i]);
    CHECK(id == "o-" + std::to_string(i + 1));
    auto decoded = batches::decode(openai_responses(), responses_item(i ? "Say: beta" : "Say: alpha"), openai[i]);
    const auto& result = std::get<batches::Line>(decoded);
    CHECK(result.custom_id == *id && text_of(result.outcome).find(i ? "beta" : "alpha") != std::string::npos);
  }
  const auto failed = std::get<batches::Line>(batches::decode(openai_responses(), responses_item("x"), openai[2]));
  CHECK(failed.custom_id == "o-3" && std::get<Failure>(failed.outcome).error.http_status == 400 &&
        std::get<Failure>(failed.outcome).error.reason == "batches.line.failed");
  const auto claude = lines("anthropic-results.jsonl");
  for (std::size_t i = 0; i < 2; ++i) {
    const auto decoded = std::get<batches::Line>(batches::decode(anthropic(), messages::Request::simple("claude-haiku-4-5", i ? "Say: beta" : "Say: alpha", 16), claude[i]));
    CHECK(decoded.custom_id == (i ? "a-2" : "a-1") && text_of(decoded.outcome).find(i ? "beta" : "alpha") != std::string::npos);
    CHECK(std::get<Completion>(decoded.outcome).usage.output_total->value == 4);
    // The model the request named, which a batch estimate prices the line by when the served one has no entry.
    CHECK(std::get<Completion>(decoded.outcome).model.requested == "claude-haiku-4-5");
  }
  const auto request = messages::Request::simple("claude-haiku-4-5", "x", 16);
  CHECK(std::get<Failure>(std::get<batches::Line>(batches::decode(anthropic(), request, claude[2])).outcome).error.reason == "batches.line.failed");
  CHECK(std::get<Failure>(std::get<batches::Line>(batches::decode(anthropic(), request, claude[3])).outcome).error.kind == ErrorKind::Cancelled);
  CHECK(std::get<Failure>(std::get<batches::Line>(batches::decode(anthropic(), request, claude[4])).outcome).error.kind == ErrorKind::DeadlineExceeded);
  const auto gemini = lines("google-results.jsonl");
  for (std::size_t i = 0; i < 2; ++i) {
    CHECK(batches::custom_id(gemini[i]) == (i ? "g-2" : "g-1"));
    const auto decoded = std::get<batches::Line>(batches::decode(google(), gemini::Request::simple("gemini-3.5-flash-lite", i ? "Say: beta" : "Say: alpha"), gemini[i]));
    CHECK(text_of(decoded.outcome) == (i ? "beta" : "alpha"));
  }
  CHECK(std::holds_alternative<Failure>(std::get<batches::Line>(batches::decode(google(), gemini::Request::simple("gemini-3.5-flash-lite", "x"), gemini[2])).outcome));
  // Not a result line at all.
  CHECK(std::holds_alternative<Error>(batches::decode(anthropic(), request, R"({"hello":1})")));
  CHECK(std::holds_alternative<Error>(batches::decode(anthropic(), request, "not json")));
}

// OpenRouter (#228). The shapes follow https://openrouter.ai/docs/batch-quickstart and the API reference pages
// (api-reference/batch/create-a-batch, get-a-batch, list-batches, delete-a-batch), read on 2026-11-21:
// openrouter-batch.json is the quickstart's "Completed batch" with the fields of its 202 answer, its response body
// given created_at and usage; openrouter-chat-batch.json is a chat completions batch written from the get-a-batch schema.
const descriptor::ValidatedDescriptor& router_chat() { static const auto d = descriptor::presets::openrouter_chat(); return d; }
const descriptor::ValidatedDescriptor& router_responses() { static const auto d = descriptor::presets::openrouter_responses(); return d; }
const descriptor::ValidatedDescriptor& router_messages() { static const auto d = descriptor::presets::openrouter_messages(); return d; }
template <class Request> std::string refusal(const descriptor::ValidatedDescriptor& d, const Request& r) {
  auto result = batches::encode(d, r);
  CHECK(std::holds_alternative<Error>(result));
  return std::get<Error>(result).reason;
}
std::vector<std::string> member_names(std::string_view body) {
  std::vector<std::string> out;
  // The document outlives the loop: a temporary in a range-for initializer is destroyed before the loop runs.
  const auto parsed = document(body);
  for (auto member : parsed->root().members()) out.emplace_back(member.key);
  return out;
}
chat::Request chat_item(std::string prompt, std::string model = "vendor/model") { return chat::Request::simple(std::move(model), std::move(prompt), 16); }
chat::Request with_media(Media media) {
  auto request = chat::Request::simple("vendor/model", std::vector<Message>{Message::user_text("Describe it.")}, 16);
  request.canonical_messages.back().parts.push_back(std::move(media));
  return request;
}
Media url_media(MediaKind kind, std::string mime, std::string url) {
  Media media;
  media.kind = kind;
  media.mime = std::move(mime);
  media.source = MediaSource::Url;
  media.reference = std::move(url);
  return media;
}

void openrouter_requests() {
  // One model and one family per batch; the batch's members, then `requests` last, each item the body its family
  // sends on its own.
  const auto submit = encoded(router_chat(), batches::Submit{{{"c-1", chat_item("Say: alpha")}, {"c-2", chat_item("Say: beta")}}});
  CHECK(submit.path == "/api/v1/batches" && submit.method == "POST" && submit.charge_on_accept && submit.model == "vendor/model");
  CHECK((member_names(submit.body) == std::vector<std::string>{"endpoint", "model", "requests"}));
  {
    const auto body = document(submit.body);
    const auto requests = body->root().get("requests");
    CHECK(body->root().get("endpoint").as_string() == "/v1/chat/completions" && body->root().get("model").as_string() == "vendor/model" &&
          requests.size() == 2 && requests.at(1).get("custom_id").as_string() == "c-2" &&
          requests.at(0).get("body").get("messages").at(0).get("content").as_string() == "Say: alpha" &&
          requests.at(0).get("body").get("model").as_string() == "vendor/model");
  }
  // The options the fact admits go before `requests`.
  auto routed = batches::Submit{{{"c-1", chat_item("Say: alpha")}}};
  routed.extensions = document(R"({"callback_url":"https://example.com/hook","provider":{"only":["provider-a"],"allow_fallbacks":false}})");
  CHECK((member_names(encoded(router_chat(), routed).body) == std::vector<std::string>{"endpoint", "model", "callback_url", "provider", "requests"}));
  // Provider `order` and `sort`, and members the fact does not name, are refused before sending.
  for (const auto* options : {R"({"provider":{"order":["provider-a"]}})", R"({"provider":{"sort":"price"}})", R"({"provider":{"zdr":true}})",
                              R"({"completion_window":"24h"})", R"({"metadata":{"run":"7"}})"}) {
    auto refused_options = routed;
    refused_options.extensions = document(options);
    CHECK(refusal(router_chat(), refused_options) == "request.batches.options_invalid");
  }
  // The endpoint of each family's requests.
  CHECK(document(encoded(router_responses(), batches::Submit{{{"r-1", responses::Request::simple("vendor/model", "Say: alpha", 16)}}}).body)
            ->root().get("endpoint").as_string() == "/v1/responses");
  CHECK(document(encoded(router_messages(), batches::Submit{{{"m-1", messages::Request::simple("vendor/model", "Say: alpha", 16)}}}).body)
            ->root().get("endpoint").as_string() == "/v1/messages");
  // Refused before anything is sent: no items, a file, a display name, two models, a request of another family, a
  // refused variant, routing or fallbacks of an item's own, a refused plugin, image or audio output.
  CHECK(refusal(router_chat(), batches::Submit{}) == "request.batches.items_required");
  CHECK(refusal(router_chat(), batches::Submit{{}, "file-9", "/v1/chat/completions"}) == "request.batches.file_unsupported");
  auto named = batches::Submit{{{"c-1", chat_item("x")}}};
  named.display_name = "run";
  CHECK(refusal(router_chat(), named) == "request.batches.display_name_unsupported");
  CHECK(refusal(router_chat(), batches::Submit{{{"c-1", chat_item("x")}, {"c-2", chat_item("y", "vendor/other")}}}) == "request.batches.model_mismatch");
  CHECK(refusal(router_chat(), batches::Submit{{{"c-1", chat_item("x")}, {"c-1", chat_item("y")}}}) == "request.batches.custom_id_invalid");
  CHECK(refusal(router_chat(), batches::Submit{{{"m-1", messages::Request::simple("vendor/model", "x", 16)}}}) == "request.batches.item_family");
  CHECK(refusal(router_chat(), batches::Submit{{{"c-1", chat_item("x", "vendor/model:online")}}}) == "request.batches.item_variant");
  CHECK(refusal(router_chat(), batches::Submit{{{"c-1", chat_item("x", "vendor/model:nitro:online")}}}) == "request.batches.item_variant");
  auto with_provider = chat_item("x");
  with_provider.provider = OpenRouterRouting{};
  with_provider.provider->order = {"provider-a"};
  CHECK(refusal(router_chat(), batches::Submit{{{"c-1", with_provider}}}) == "request.batches.item_routing");
  auto zdr = chat_item("x");
  zdr.provider = OpenRouterRouting{};
  zdr.provider->zdr = true;
  CHECK(refusal(router_chat(), batches::Submit{{{"c-1", zdr}}}) == "request.batches.item_routing");
  auto fallbacks = chat_item("x");
  fallbacks.models = {"vendor/other"};
  CHECK(refusal(router_chat(), batches::Submit{{{"c-1", fallbacks}}}) == "request.batches.item_fallbacks");
  auto web = chat_item("x");
  web.plugins = {OpenRouterPlugin{"web", nullptr}};
  CHECK(refusal(router_chat(), batches::Submit{{{"c-1", web}}}) == "request.batches.item_plugin");
  auto responses_web = responses::Request::simple("vendor/model", "x", 16);
  responses_web.plugins = {OpenRouterPlugin{"web", nullptr}};
  CHECK(refusal(router_responses(), batches::Submit{{{"r-1", responses_web}}}) == "request.batches.item_plugin");
  auto image_out = chat_item("x");
  image_out.modalities = {chat::OutputModality::Text, chat::OutputModality::Image};
  CHECK(refusal(router_chat(), batches::Submit{{{"c-1", image_out}}}) == "request.batches.item_output");
  auto audio_out = chat_item("x");
  audio_out.modalities = {chat::OutputModality::Text, chat::OutputModality::Audio};
  audio_out.audio = chat::AudioOutput{"alloy", "wav"};
  CHECK(refusal(router_chat(), batches::Submit{{{"c-1", audio_out}}}) == "request.batches.item_output");
  auto responses_image = responses::Request::simple("vendor/model", "x", 16);
  responses_image.modalities = {responses::OutputModality::Image};
  CHECK(refusal(router_responses(), batches::Submit{{{"r-1", responses_image}}}) == "request.batches.item_output");
  // Media: public image and document URLs are taken; base64 images, audio input and video are refused.
  CHECK(document(encoded(router_chat(), batches::Submit{{{"c-1", with_media(url_media(MediaKind::Image, "image/png", "https://example.com/shoe.png"))}}}).body)
            ->root().get("requests").size() == 1);
  CHECK(document(encoded(router_chat(), batches::Submit{{{"c-1", with_media(url_media(MediaKind::Document, "application/pdf", "https://example.com/a.pdf"))}}}).body)
            ->root().get("requests").size() == 1);
  CHECK(refusal(router_chat(), batches::Submit{{{"c-1", with_media(Media::image("image/png", std::make_shared<const std::string>("iVBORw0KGgo=")))}}}) == "request.batches.item_media");
  CHECK(refusal(router_chat(), batches::Submit{{{"c-1", with_media(Media::audio("audio/wav", std::make_shared<const std::string>("UklGRg==")))}}}) == "request.batches.item_media");
  CHECK(refusal(router_chat(), batches::Submit{{{"c-1", with_media(url_media(MediaKind::Video, "video/mp4", "https://example.com/v.mp4"))}}}) ==
        "request.batches.item_media");
  // Media in a Chat request's typed messages is held to the same policy.
  auto typed_image = chat_item("Describe it.");
  typed_image.messages.back().media.push_back(Media::image("image/png", std::make_shared<const std::string>("iVBORw0KGgo=")));
  CHECK(refusal(router_chat(), batches::Submit{{{"c-1", typed_image}}}) == "request.batches.item_media");
  auto typed_audio = chat_item("Transcribe it.");
  typed_audio.messages.back().media.push_back(Media::audio("audio/wav", std::make_shared<const std::string>("UklGRg==")));
  CHECK(refusal(router_chat(), batches::Submit{{{"c-1", typed_audio}}}) == "request.batches.item_media");
  auto typed_url = chat_item("Describe it.");
  typed_url.messages.back().media.push_back(url_media(MediaKind::Image, "image/png", "https://example.com/shoe.png"));
  CHECK(document(encoded(router_chat(), batches::Submit{{{"c-1", typed_url}}}).body)->root().get("requests").size() == 1);
  // Hosted tools the batch path does not run, by their wire type.
  auto searching = responses::Request::simple("vendor/model", "x", 16);
  searching.hosted_tools = {responses::OpenRouterServerTool{"openrouter:web_search", nullptr}};
  CHECK(refusal(router_responses(), batches::Submit{{{"r-1", searching}}}) == "request.batches.item_tool");
  auto drawing = responses::Request::simple("vendor/model", "x", 16);
  drawing.hosted_tools = {responses::ImageGenerationTool{}};
  CHECK(refusal(router_responses(), batches::Submit{{{"r-1", drawing}}}) == "request.batches.item_tool");
  auto routed_drawing = responses::Request::simple("vendor/model", "x", 16);
  routed_drawing.hosted_tools = {responses::OpenRouterServerTool{"openrouter:image_generation", nullptr}};
  CHECK(refusal(router_responses(), batches::Submit{{{"r-1", routed_drawing}}}) == "request.batches.item_tool");
  // The provider sorts and the batch variant itself.
  for (const auto* model : {"vendor/model:nitro", "vendor/model:floor", "vendor/model:batch"})
    CHECK(refusal(router_chat(), batches::Submit{{{"c-1", chat_item("x", model)}}}) == "request.batches.item_variant");
  // A callback is an https URL without credentials.
  for (const auto* url : {R"("http://example.com/hook")", R"("https://user:secret@example.com/hook")", R"("https://")", R"(7)"}) {
    auto callback = batches::Submit{{{"c-1", chat_item("x")}}};
    callback.extensions = document(std::string(R"({"callback_url":)") + url + "}");
    const auto reason = refusal(router_chat(), callback);
    CHECK(reason == "request.batches.callback_invalid" || reason == "request.batches.options_invalid");
  }
  {
    auto callback = batches::Submit{{{"c-1", chat_item("x")}}};
    callback.extensions = document(R"({"callback_url":"https://user@example.com/hook"})");
    CHECK(refusal(router_chat(), callback) == "request.batches.callback_invalid");
  }
  // No cancel: refused for that reason. A batch is read, listed and deleted by id.
  CHECK(refusal(router_chat(), batches::Cancel{"batch_123"}) == "request.batches.cancel_unsupported");
  const auto get = encoded(router_chat(), batches::Get{"batch-1794949632-xDGJGOegxjgSVABzTiky"});
  CHECK(get.method == "GET" && get.path == "/api/v1/batches/batch-1794949632-xDGJGOegxjgSVABzTiky" && get.body.empty() && !get.charge_on_accept);
  const auto del = encoded(router_chat(), batches::Delete{"batch_123"});
  CHECK(del.method == "DELETE" && del.path == "/api/v1/batches/batch_123");
  CHECK(refusal(router_chat(), batches::Get{".."}) == "request.batches.id_invalid");
  CHECK(encoded(router_chat(), batches::List{}).path == "/api/v1/batches");
  const auto page = encoded(router_chat(), batches::List{10, "batch_123"});
  CHECK(page.method == "GET" && page.path == "/api/v1/batches?limit=10&after=batch_123");
  CHECK(refusal(router_chat(), batches::List{0, {}}) == "request.batches.limit_invalid");
  CHECK(refusal(router_chat(), batches::List{std::nullopt, "a&b"}) == "request.batches.cursor_invalid");
  CHECK(refusal(openai_chat(), batches::List{}) == "request.batches.path_missing");
  // The other shapes keep their cancel.
  CHECK(encoded(openai_chat(), batches::Cancel{"batch_1"}).path == "/v1/batches/batch_1/cancel");
}

void openrouter_batches() {
  const auto batch = std::get<batches::Batch>(batch_of(router_responses(), read("openrouter-batch.json")));
  CHECK(batch.id == "batch_123" && batch.shape == "openrouter" && batch.origin == "https://openrouter.ai" && batch.status == jobs::Status::Completed &&
        batch.raw_status == "completed" && batch.counts.total == 2 && batch.counts.succeeded == 1 && batch.counts.failed == 1 && batch.counts.pending == 0 &&
        batch.results_ready && batch.created_at == 1782097200u && batch.expires_at == 1782183600u && batch.ended_at == 1782097800u);
  CHECK(batch.usage.input_total->value == 40 && batch.usage.output_total->value == 12 && batch.usage.total->value == 52);
  // usage.cost is what OpenRouter charged for the whole batch, in USD.
  CHECK(batch.usage.provider_cost.source == CostSource::OpenRouterUsd && batch.usage.provider_cost.total && batch.usage.provider_cost.total->nano_usd == 5000 &&
        batch.usage.provider_cost.is_byok == false);
  CHECK(batches::inline_lines(batch).size() == 2);
  // Each status as documented; one the SDK does not know is Unknown, with the word kept.
  const std::pair<const char*, jobs::Status> statuses[]{
      {"validating", jobs::Status::Pending}, {"in_progress", jobs::Status::Running}, {"finalizing", jobs::Status::Running},
      {"completed", jobs::Status::Completed}, {"failed", jobs::Status::Failed}, {"expired", jobs::Status::Expired},
      {"cancelling", jobs::Status::Running}, {"cancelled", jobs::Status::Cancelled}, {"paused", jobs::Status::Unknown}};
  for (const auto& [word, status] : statuses) {
    const auto read_back = std::get<batches::Batch>(batch_of(router_chat(), std::string(R"({"id":"batch_1","status":")") + word +
                                                                                R"(","request_counts":{"total":2,"completed":0,"failed":0},"results":null})"));
    CHECK(read_back.status == status && read_back.raw_status == word && !read_back.results_ready && read_back.counts.pending == 2);
  }
  // A failed batch names why, and carries no results.
  const auto failed = std::get<batches::Batch>(batch_of(router_chat(), R"({"id":"batch_456","object":"batch","status":"failed","request_counts":{"total":2,"completed":0,"failed":0},"usage":null,"results":null,"error":{"message":"Batch request 'review-2' contains unsupported 'input_audio' content."}})"));
  CHECK(failed.status == jobs::Status::Failed && !failed.results_ready && !failed.usage.input_total);
  CHECK(batch.usage.stage == UsageStage::Final);
  // With the account's own provider key: the BYOK flag and the provider's inference estimate are kept (quickstart, BYOK).
  const auto byok = std::get<batches::Batch>(batch_of(router_chat(), R"({"id":"batch_7","status":"completed","request_counts":{"total":1,"completed":1,"failed":0},"usage":{"prompt_tokens":10,"completion_tokens":2,"total_tokens":12,"cost":0.0000004,"is_byok":true,"cost_details":{"upstream_inference_cost":0.000002}},"results":[]})"));
  CHECK(byok.usage.provider_cost.is_byok == true && byok.usage.provider_cost.total && byok.usage.provider_cost.total->nano_usd == 400 &&
        byok.usage.provider_cost.upstream_total && byok.usage.provider_cost.upstream_total->nano_usd == 2000);
  // The usage of a batch still running may grow.
  const auto running = std::get<batches::Batch>(batch_of(router_chat(), R"({"id":"batch_8","status":"in_progress","request_counts":{"total":2,"completed":1,"failed":0},"usage":{"prompt_tokens":10,"completion_tokens":2,"total_tokens":12,"cost":0.0000004}})"));
  CHECK(running.usage.stage == UsageStage::Partial && running.usage.input_total->value == 10);
  // The results are inline: there is no file to download.
  CHECK(refused(router_chat(), batches::Results{batch}) == ErrorKind::InvalidRequest);
  // A deletion names the batch and what each deletion target did.
  const auto deleted = [](std::string_view body) {
    Accumulator accumulator;
    batches::Codec codec(router_chat(), accumulator, batches::Codec::Kind::Deleted, "batch_123");
    codec.buffered(body, {});
    CHECK(accumulator.outcome());
    return batches::typed_deleted(*accumulator.outcome());
  };
  CHECK(std::get<batches::Deleted>(deleted(R"({"id":"batch_123","object":"batch","deletion":{"openrouter":"deleted","upstream":{"provider":"Anthropic","status":"deleted"}}})")).id == "batch_123");
  CHECK(std::get<Failure>(deleted(R"({"id":"batch_123","object":"batch"})")).error.reason == "batches.delete.unconfirmed");
  CHECK(std::get<Failure>(deleted(R"({"id":"batch_9","object":"batch","deletion":{}})")).error.reason == "batches.delete.unconfirmed");
  // A page of batches, newest first, with the cursor of the next.
  const auto paged = [](std::string_view body) {
    Accumulator accumulator;
    batches::Codec codec(router_chat(), accumulator, batches::Codec::Kind::Page, {});
    codec.buffered(body, {});
    CHECK(accumulator.outcome());
    return batches::typed_page(*accumulator.outcome());
  };
  const auto page = std::get<batches::Page>(paged(R"({"object":"list","data":[{"id":"batch_123","object":"batch","status":"in_progress","model":"anthropic/claude-haiku-5.5","provider":"Anthropic","request_counts":{"total":2,"completed":1,"failed":0},"results":null}],"first_id":"batch_123","last_id":"batch_123","has_more":true})"));
  CHECK(page.batches.size() == 1 && page.batches[0].id == "batch_123" && page.batches[0].status == jobs::Status::Running && page.batches[0].counts.pending == 1 &&
        page.next == "batch_123" && page.batches[0].raw);
  CHECK(std::get<batches::Page>(paged(R"({"object":"list","data":[],"first_id":null,"last_id":null,"has_more":false})")).next.empty());
  CHECK(std::get<Failure>(paged(R"({"object":"list","data":{}})")).error.reason == "batches.page.invalid");
  CHECK(std::get<Failure>(paged(R"({"object":"list","data":[],"has_more":true})")).error.reason == "batches.page.cursor_missing");
  CHECK(std::get<Failure>(paged(R"({"object":"list","data":[],"has_more":true,"last_id":""})")).error.reason == "batches.page.cursor_missing");
  CHECK(std::get<Failure>(paged(R"({"object":"list","data":[{"object":"batch"}]})")).error.reason == "batches.page.invalid");
}

std::string results_refusal(const descriptor::ValidatedDescriptor& family, const batches::Batch& batch, const std::vector<batches::Item>& items) {
  const auto decoded = batches::results(family, batch, items);
  CHECK(std::holds_alternative<Error>(decoded));
  return std::get<Error>(decoded).reason;
}
void openrouter_results() {
  // Each result decoded through the family of the batch's requests, matched by custom id, not by order.
  const auto batch = std::get<batches::Batch>(batch_of(router_responses(), read("openrouter-batch.json")));
  const std::vector<batches::Item> items{{"review-1", responses::Request::simple("anthropic/claude-haiku-5.5", "Classify: The fit is perfect.", 16)},
                                         {"review-2", responses::Request::simple("anthropic/claude-haiku-5.5", "Classify: It arrived broken.", 16)}};
  const auto decoded = std::get<std::vector<batches::Line>>(batches::results(router_responses(), batch, items));
  CHECK(decoded.size() == 2 && decoded[0].custom_id == "review-1" && text_of(decoded[0].outcome) == "positive");
  CHECK(decoded[1].custom_id == "review-2" && std::get<Failure>(decoded[1].outcome).error.reason == "batches.line.failed");
  // Another family's descriptor does not decode them: the batch names the path its requests went to.
  const std::vector<batches::Item> chat_items{{"review-1", chat_item("x")}, {"review-2", chat_item("y")}};
  CHECK(results_refusal(router_chat(), batch, chat_items) == "batches.results.family");
  CHECK(results_refusal(router_messages(), batch, items) == "batches.results.family");
  CHECK(results_refusal(router_responses(), batch, {items[0]}) == "batches.results.custom_id");
  CHECK(results_refusal(router_responses(), batch, {items[0], items[0]}) == "batches.results.custom_id");
  auto pending = batch;
  pending.raw = document(R"({"id":"batch_123","status":"in_progress","endpoint":"/v1/responses","results":null})");
  CHECK(results_refusal(router_responses(), pending, items) == "batches.results.missing");
  // A chat completions batch, its results out of order.
  const auto chat_batch = std::get<batches::Batch>(batch_of(router_chat(), read("openrouter-chat-batch.json")));
  CHECK(chat_batch.usage.provider_cost.total && chat_batch.usage.provider_cost.total->nano_usd == 2500);
  const std::vector<batches::Item> chats{{"c-1", chat_item("Say: alpha")}, {"c-2", chat_item("Say: beta")}};
  const auto lines = std::get<std::vector<batches::Line>>(batches::results(router_chat(), chat_batch, chats));
  CHECK(lines.size() == 2 && lines[0].custom_id == "c-2" && text_of(lines[0].outcome) == "beta" && lines[1].custom_id == "c-1" &&
        text_of(lines[1].outcome) == "alpha" && std::get<Completion>(lines[1].outcome).usage.output_total->value == 2);
  CHECK(results_refusal(router_responses(), chat_batch, items) == "batches.results.family");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("usage: sp_batches_tests <fixture directory>");
    fixtures = argv[1];
    requests(); captured_caps(); batch_objects(); result_lines(); openrouter_requests(); openrouter_batches(); openrouter_results();
    std::cout << "batch properties passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "batch test failed: " << error.what() << '\n'; return 1; }
}
