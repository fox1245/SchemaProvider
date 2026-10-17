// Error::reason: the fixed table behind it is complete, well formed and unique, an accumulator failure carries its code,
// and a transport failure that takes precedence carries none. Usage: sp_error_reason_tests <src dir>
#include "codecs/accumulator.h"
#include "codecs/reason.h"
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace sp;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #condition); } while (false)

struct Source { reason::Source source; const char* prefix; };
const Source sources[]{{reason::Source::Chat, "chat"}, {reason::Source::Messages, "messages"}, {reason::Source::Responses, "responses"},
                       {reason::Source::Gemini, "gemini"}, {reason::Source::Interactions, "interactions"},
                       {reason::Source::Accumulator, "accumulator"}};
// The strings for an abnormal close belong to the transport: the close error's kind is the transport's, and so is its lack of a reason.
const std::set<std::string> transport_strings{"abnormal response close", "abnormal Gemini response close", "abnormal interaction close"};

bool identifier_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
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
void table_is_well_formed() {
  std::set<std::string> codes;
  for (const auto& [source, prefix] : sources) {
    const auto entries = reason::entries(source);
    CHECK(!entries.empty());
    std::set<std::string> messages;
    for (const auto& entry : entries) {
      CHECK(!entry.message.empty() && well_formed(entry.code));
      CHECK(entry.code.starts_with(std::string(prefix) + '.'));
      CHECK(messages.insert(std::string(entry.message)).second);       // one code per failure string
      CHECK(codes.insert(std::string(entry.code)).second);             // one failure string per code, across sources
      CHECK(reason::code(source, entry.message) == entry.code);
      CHECK(!transport_strings.contains(std::string(entry.message)));
    }
    // Lookup is exact: neighbours of a known string are not strings of the table.
    const auto& first = entries.front();
    CHECK(reason::code(source, std::string(first.message) + " ").empty() && reason::code(source, first.message.substr(1)).empty() && reason::code(source, {}).empty());
  }
  CHECK(codes.size() > 400);
}

// Literals of every failure a decoder source can raise: the arguments after the kind of fail() and reject(), and the
// text of an `Error name{kind, "text"}` that is later passed to fail(). Comments and literals are skipped as code.
size_t skip_literal(const std::string& s, size_t i, std::string* text) {
  const char quote = s[i++];
  while (i < s.size() && s[i] != quote) {
    if (s[i] == '\\' && i + 1 < s.size()) ++i;
    if (text) text->push_back(s[i]);
    ++i;
  }
  return i + 1;
}
size_t skip_trivia(const std::string& s, size_t i) {
  if (s.compare(i, 2, "//") == 0) { const auto end = s.find('\n', i); return end == std::string::npos ? s.size() : end; }
  if (s.compare(i, 2, "/*") == 0) { const auto end = s.find("*/", i + 2); return end == std::string::npos ? s.size() : end + 2; }
  return i;
}
// i is just past the opening bracket; collects the literals after the first top-level comma and returns the index past the match.
size_t arguments(const std::string& s, size_t i, std::vector<std::string>& out) {
  int depth = 1;
  bool after_kind = false;
  while (i < s.size() && depth) {
    if (const auto next = skip_trivia(s, i); next != i) { i = next; continue; }
    const char c = s[i];
    if (c == '"') { std::string text; i = skip_literal(s, i, &text); if (after_kind) out.push_back(text); continue; }
    if (c == '\'') { i = skip_literal(s, i, nullptr); continue; }
    if (c == '(' || c == '{' || c == '[') ++depth;
    else if (c == ')' || c == '}' || c == ']') --depth;
    else if (c == ',' && depth == 1) after_kind = true;
    ++i;
  }
  return i;
}
std::vector<std::string> failure_strings(const std::string& s) {
  std::vector<std::string> out;
  for (size_t i = 0; i < s.size();) {
    if (const auto next = skip_trivia(s, i); next != i) { i = next; continue; }
    if (s[i] == '"' || s[i] == '\'') { i = skip_literal(s, i, nullptr); continue; }
    if (!(std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '_') || (i && (identifier_char(s[i - 1]) || s[i - 1] == ':' || s[i - 1] == '.' || s[i - 1] == '>'))) { ++i; continue; }
    size_t end = i;
    while (end < s.size() && identifier_char(s[end])) ++end;
    const std::string word = s.substr(i, end - i);
    size_t at = end;
    while (at < s.size() && std::isspace(static_cast<unsigned char>(s[at]))) ++at;
    if ((word == "fail" || word == "reject") && at < s.size() && s[at] == '(') i = arguments(s, at + 1, out);
    else if (word == "Error" && at < s.size() && s[at] == '{') i = arguments(s, at + 1, out);
    else if (word == "Error") {
      size_t name = at;
      while (name < s.size() && identifier_char(s[name])) ++name;
      while (name < s.size() && std::isspace(static_cast<unsigned char>(s[name]))) ++name;
      if (name > at && name < s.size() && s[name] == '{') { i = arguments(s, name + 1, out); continue; }
      i = end;
    } else i = end;
  }
  return out;
}
void every_failure_string_has_a_code(const std::filesystem::path& root) {
  size_t checked = 0;
  for (const auto& [source, prefix] : sources) {
    std::ifstream file(root / "codecs" / (std::string(prefix) + ".cpp"), std::ios::binary);
    CHECK(file.good());
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto found = failure_strings(text);
    CHECK(found.size() > 20);
    std::set<std::string> used;
    for (const auto& message : found) {
      if (transport_strings.contains(message)) continue;
      if (reason::code(source, message).empty()) throw std::runtime_error(std::string(prefix) + ": no reason code for \"" + message + "\"");
      used.insert(message);
      ++checked;
    }
    // And no entry has outlived its failure site.
    for (const auto& entry : reason::entries(source))
      if (!used.contains(std::string(entry.message))) throw std::runtime_error(std::string(prefix) + ": unused entry \"" + std::string(entry.message) + "\"");
  }
  CHECK(checked > 400);
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
    if (argc != 2) throw std::runtime_error("usage: sp_error_reason_tests <src dir>");
    table_is_well_formed();
    every_failure_string_has_a_code(argv[1]);
    accumulator_failures();
    std::cout << "error reason tests passed\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
