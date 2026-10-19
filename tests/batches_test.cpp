// Batches (D7): OpenAI's JSONL input, each shape's submit, get, cancel, delete and results requests, the batch
// objects the providers answer, and result lines decoded through the family codecs. The fixtures are live answers
// of 2026-10-19 to two-item batches, with one failed line of each kind added.
// Usage: sp_batches_tests <fixture directory>
#include "codecs/batches.h"
#include "descriptor/presets.h"
#include "json/json.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
}

batches::Outcome batch_of(const descriptor::ValidatedDescriptor& d, std::string_view body, batches::Codec::Kind kind = batches::Codec::Kind::Batch) {
  Accumulator accumulator;
  batches::Codec codec(d, accumulator, kind, "subject");
  codec.buffered(body, {});
  CHECK(accumulator.outcome());
  return batches::typed(*accumulator.outcome());
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
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("usage: sp_batches_tests <fixture directory>");
    fixtures = argv[1];
    requests(); batch_objects(); result_lines();
    std::cout << "batch properties passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "batch test failed: " << error.what() << '\n'; return 1; }
}
