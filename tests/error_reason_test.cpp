// Error::reason: every constant a check raises is enumerated per source, the set is the fixture's (plus the codes this
// table has gained since), codes are unique and well formed, an accumulator failure carries its code, and a transport
// failure that takes precedence carries none. A raise names its constant; no helper takes a bare string, so the
// compiler refuses a literal. Usage: sp_error_reason_tests <reason_codes.json>
#include "codecs/accumulator.h"
#include "codecs/reason.h"
#include "descriptor/presets.h"
#include "json/json.h"
#include "runtime/client.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace sp;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)

// The codes added after the fixture was taken, with their texts: a media kind the policy admits from no source.
const std::pair<std::string, std::string> added[]{
    {"request.media.image_input_not_accepted", "this family does not accept image input"},
    {"request.media.audio_input_not_accepted", "this family does not accept audio input"},
    {"request.media.video_input_not_accepted", "this family does not accept video input"},
    {"request.media.document_input_not_accepted", "this family does not accept document input"}};

// <source>.<subject>.<problem>, optionally with a fourth segment; lower-case letters, digits and '_' only.
bool well_formed(std::string_view code) {
  if (code.empty() || code.size() > 64) return false;
  size_t segments = 1, length = 0;
  for (const char c : code) {
    if (c == '.') { if (!length) return false; ++segments; length = 0; }
    else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') ++length;
    else return false;
  }
  return length && segments >= 3 && segments <= 4;
}
std::set<std::pair<std::string, std::string>> fixture(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  CHECK(file.good());
  const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  auto parsed = json::parse(text, {text.size() + 1, 4});
  CHECK(std::holds_alternative<json::Document>(parsed));
  const auto root = std::get<json::Document>(parsed).root();
  CHECK(root.is_array());
  std::set<std::pair<std::string, std::string>> pinned;
  for (const auto pair : root.elements()) {
    CHECK(pair.is_array() && pair.size() == 2);
    std::vector<std::string> fields;
    for (const auto field : pair.elements()) { CHECK(field.is_string()); fields.emplace_back(field.as_string()); }
    CHECK(pinned.insert({fields[0], fields[1]}).second);
  }
  CHECK(pinned.size() == root.size() && pinned.size() > 1000);
  return pinned;
}
// Every constant of every source: the [code, message] pairs are the fixture's plus the added ones, each code is unique
// across sources and starts with its source's prefix, and each text names one check of its source.
void constants_are_the_fixture(const std::filesystem::path& path) {
  auto expected = fixture(path);
  for (const auto& pair : added) CHECK(expected.insert(pair).second);
  std::set<std::pair<std::string, std::string>> constants;
  std::set<std::string> codes;
  for (const auto& [prefix, all] : reason::sources) {
    CHECK(!all.empty() && !prefix.empty());
    std::set<std::string> messages;
    for (const auto& constant : all) {
      CHECK(!constant.message.empty() && well_formed(constant.code));
      CHECK(constant.code.starts_with(std::string(prefix) + '.'));
      CHECK(messages.insert(std::string(constant.message)).second);  // one code per failure text of a source
      CHECK(codes.insert(std::string(constant.code)).second);        // one source per code
      CHECK(constants.insert({std::string(constant.code), std::string(constant.message)}).second);
    }
  }
  CHECK(constants == expected);
  // The transport's texts have no code, and none is the text of a check.
  const std::set<std::string> transport_texts{"abnormal response close", "abnormal Gemini response close", "abnormal interaction close"};
  std::set<std::string> transport;
  for (const auto& close : reason::transport::all) {
    CHECK(close.code.empty() && transport.insert(std::string(close.message)).second);
    for (const auto& constant : constants) CHECK(constant.second != close.message);
  }
  CHECK(transport == transport_texts);
}
// An error built from a constant carries both its text and its code.
void error_carries_the_constant() {
  const auto error = reason::error(ErrorKind::InvalidRequest, reason::shared_request::route_model_name);
  CHECK(error.kind == ErrorKind::InvalidRequest && error.safe_message == reason::shared_request::route_model_name.message);
  CHECK(error.reason == "request.route.model_name");
}
// The runtime replaces an encoder's text with the generic text of the error kind and keeps the code of the check.
void prepare_keeps_the_refusing_check() {
  sp::runtime::Client client(sp::descriptor::presets::anthropic_messages(), std::string("unused-key"));
  sp::runtime::RunOptions run;
  run.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  auto request = sp::messages::Request::simple("claude-haiku-4-5", "hi", 64);
  request.temperature = 0.5;
  request.model = "claude-opus-4-8";
  const auto refused = [&](sp::runtime::Request value) {
    auto prepared = client.prepare(std::move(value), run);
    const auto result = client.complete(std::move(prepared));
    CHECK(result && std::holds_alternative<Failure>(*result));
    return std::get<Failure>(*result).error;
  };
  const auto temperature = refused(request);
  CHECK(temperature.kind == ErrorKind::InvalidRequest && temperature.safe_message == "invalid request");
  CHECK(temperature.reason == "request.messages.temperature_is_prohibited_for_this_model");
  auto cap = sp::messages::Request::simple("claude-haiku-4-5-20251001", "hi", 64001);
  const auto limit = refused(cap);
  CHECK(limit.kind == ErrorKind::InvalidRequest && limit.reason == "request.choice.output_cap_exceeds_admitted_model_limit");
}

// The accumulator names the check that failed; a transport failure that takes precedence is the transport's, without a reason.
void accumulator_failures() {
  const auto failed = [](Accumulator& a) -> const Failure& {
    CHECK(a.outcome() && std::holds_alternative<Failure>(*a.outcome()));
    return std::get<Failure>(*a.outcome());
  };
  {
    Accumulator a;
    CHECK(a.accept(Event{Begin{"generation"}}));
    CHECK(!a.accept(Event{PartDelta{LocalId{9}, {PartKind::Text, "hello"}}}));
    const auto& error = failed(a).error;
    CHECK(error.kind == ErrorKind::ProtocolCorrupt && error.safe_message == "invalid part delta" && error.reason == "accumulator.part.delta_invalid");
  }
  {
    SemanticLimits limits; limits.max_content_bytes = 4;
    Accumulator a(limits);
    CHECK(a.accept(Event{Begin{"generation"}}) && a.accept(Event{MessageBegin{{0}, {}, Role::Assistant}}) && a.accept(Event{PartBegin{{0}, {1}, PartKind::Text, {}, 0}}));
    CHECK(!a.accept(Event{PartDelta{{1}, {PartKind::Text, "longer than four bytes"}}}));
    const auto& error = failed(a).error;
    CHECK(error.kind == ErrorKind::ResourceLimit && error.reason == "accumulator.part.content_limit");
  }
  {
    // Carried through a sealed text part and a partial output untouched.
    Accumulator a;
    CHECK(a.accept(Event{Begin{"generation"}}) && a.accept(Event{MessageBegin{{0}, {}, Role::Assistant}}) && a.accept(Event{PartBegin{{0}, {1}, PartKind::Text, {}, 0}}));
    CHECK(a.accept(Event{PartDelta{{1}, {PartKind::Text, "kept"}}}));
    CHECK(!a.accept(Event{Commit{""}}));
    const auto& failure = failed(a);
    CHECK(failure.error.reason == "accumulator.commit.evidence_missing" && failure.partial.messages.size() == 1);
  }
  {
    const Error closed{ErrorKind::Transport, "abnormal response close"};
    Accumulator a;
    CHECK(a.accept(Event{Begin{"generation"}}));
    CHECK(!a.accept(Event{PartDelta{LocalId{9}, {PartKind::Text, "hello"}}}, &closed));
    const auto& error = failed(a).error;
    CHECK(error.kind == ErrorKind::Transport && error.safe_message == "abnormal response close" && error.reason.empty());
  }
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("usage: sp_error_reason_tests <reason_codes.json>");
    constants_are_the_fixture(argv[1]);
    error_carries_the_constant();
    prepare_keeps_the_refusing_check();
    accumulator_failures();
    std::cout << "error reason tests passed\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
