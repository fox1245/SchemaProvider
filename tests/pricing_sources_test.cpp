// The product sources hold no model id and no price: ids, prices and limits are catalogue data. A C++ lexer written
// here extracts the string literals of every source under src/, and the scan fails when:
// (a) a literal names a model id of the data (config/models.json ids, aliases, served names and alias targets; the
//     models of config/model-catalog.json; tests/fixtures/pricing/known_models.json);
// (b) a literal of the pricing sources is decimal price text with a fraction;
// (c) a literal that starts with the item unit prefix names a kind the one kind table (descriptor::item_kinds) does not.
// Usage: sp_pricing_sources_tests <repository root>
#include "descriptor/catalogue.h"
#include "json/json.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {
using namespace sp;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (false)

// A token of C++ source text, as far as the scan needs it. A literal's text is its body as written between its
// delimiters, escapes kept.
struct Token {
  enum class Kind { Identifier, String, Character, Number, Punctuation } kind = Kind::Punctuation;
  std::string text;
  std::size_t line = 0;
};
bool identifier_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool identifier_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool digit(char c) { return c >= '0' && c <= '9'; }

// Skips comments, and reads identifiers, string and character literals with their encoding prefixes, raw strings with
// their delimiters, and preprocessing numbers with digit separators (`1'000'000`) and exponent signs.
std::vector<Token> lex(std::string_view s) {
  std::vector<Token> out;
  std::size_t i = 0, line = 1;
  // Moves to `end`, counting the lines passed.
  const auto move_to = [&](std::size_t end) {
    end = std::min(end, s.size());
    for (; i < end; ++i)
      if (s[i] == '\n') ++line;
  };
  // A quoted literal whose opening quote is at `i`.
  const auto quoted = [&](Token::Kind kind) {
    const char quote = s[i];
    const auto start = line;
    std::size_t end = i + 1;
    while (end < s.size() && s[end] != quote && s[end] != '\n') end += s[end] == '\\' ? 2 : 1;
    if (end >= s.size() || s[end] != quote) throw std::runtime_error("unterminated literal on line " + std::to_string(start));
    out.push_back({kind, std::string(s.substr(i + 1, end - i - 1)), start});
    move_to(end + 1);
  };
  // A raw string whose opening quote is at `i`: `"delimiter( ... )delimiter"`.
  const auto raw = [&]() {
    const auto start = line;
    const auto open = s.find('(', i + 1);
    if (open == std::string_view::npos) throw std::runtime_error("unterminated raw string on line " + std::to_string(start));
    const std::string closing = ")" + std::string(s.substr(i + 1, open - i - 1)) + "\"";
    const auto end = s.find(closing, open + 1);
    if (end == std::string_view::npos) throw std::runtime_error("unterminated raw string on line " + std::to_string(start));
    out.push_back({Token::Kind::String, std::string(s.substr(open + 1, end - open - 1)), start});
    move_to(end + closing.size());
  };
  while (i < s.size()) {
    const char c = s[i];
    if (c == '\n') { ++line; ++i; continue; }
    if (s.compare(i, 2, "//") == 0) { move_to(s.find('\n', i)); continue; }
    if (s.compare(i, 2, "/*") == 0) {
      const auto end = s.find("*/", i + 2);
      if (end == std::string_view::npos) throw std::runtime_error("unterminated comment on line " + std::to_string(line));
      move_to(end + 2);
      continue;
    }
    if (identifier_start(c)) {
      std::size_t end = i;
      while (end < s.size() && identifier_char(s[end])) ++end;
      const auto word = s.substr(i, end - i);
      const char next = end < s.size() ? s[end] : '\0';
      const bool raw_prefix = word == "R" || word == "u8R" || word == "uR" || word == "UR" || word == "LR";
      const bool prefix = word == "u8" || word == "u" || word == "U" || word == "L";
      if (raw_prefix && next == '"') { move_to(end); raw(); continue; }
      if (prefix && (next == '"' || next == '\'')) { move_to(end); quoted(next == '"' ? Token::Kind::String : Token::Kind::Character); continue; }
      out.push_back({Token::Kind::Identifier, std::string(word), line});
      move_to(end);
      continue;
    }
    if (digit(c) || (c == '.' && i + 1 < s.size() && digit(s[i + 1]))) {
      std::size_t end = i + 1;
      while (end < s.size()) {
        const char d = s[end];
        if (identifier_char(d) || d == '.') ++end;
        else if (d == '\'' && end + 1 < s.size() && identifier_char(s[end + 1])) end += 2;
        else if ((d == '+' || d == '-') && (s[end - 1] == 'e' || s[end - 1] == 'E' || s[end - 1] == 'p' || s[end - 1] == 'P')) ++end;
        else break;
      }
      out.push_back({Token::Kind::Number, std::string(s.substr(i, end - i)), line});
      move_to(end);
      continue;
    }
    if (c == '"') { quoted(Token::Kind::String); continue; }
    if (c == '\'') { quoted(Token::Kind::Character); continue; }
    if (!std::isspace(static_cast<unsigned char>(c))) out.push_back({Token::Kind::Punctuation, std::string(1, c), line});
    ++i;
  }
  return out;
}
std::vector<std::string> strings_of(std::string_view source) {
  std::vector<std::string> out;
  for (const auto& token : lex(source))
    if (token.kind == Token::Kind::String) out.push_back(token.text);
  return out;
}

void lexer_cases() {
  CHECK(strings_of("// \"a\" it's\nx /* \"b\" ' */ y").empty());
  CHECK(strings_of("n = 1'000'000; m = 0x1'2'3; e = 1.5e+3; \"c\"") == std::vector<std::string>{"c"});
  CHECK(strings_of("'\"' \"d\" '\\'' \"e\"") == (std::vector<std::string>{"d", "e"}));
  CHECK(strings_of("R\"x(a\"b)x\" u8R\"(e)\" LR\"--(f)\"g)--\"") == (std::vector<std::string>{"a\"b", "e", "f)\"g"}));
  CHECK(strings_of("u8\"g\" L\"h\" u\"i\" U\"j\" \"k\\\"l\" \"m\"sv") == (std::vector<std::string>{"g", "h", "i", "j", "k\\\"l", "m"}));
  CHECK(strings_of("FOOR\"n\" uR'x'") == (std::vector<std::string>{"n"}));
  CHECK(strings_of("#include \"core/pricing.h\"\nconst char* x = \"o\"; // \"p\"\n") == (std::vector<std::string>{"core/pricing.h", "o"}));
  const auto tokens = lex("a\n/* two\nlines */ \"q\"\nR\"(r\ns)\" \"t\"");
  std::vector<std::pair<std::string, std::size_t>> lines;
  for (const auto& token : tokens)
    if (token.kind == Token::Kind::String) lines.emplace_back(token.text, token.line);
  CHECK(lines == (std::vector<std::pair<std::string, std::size_t>>{{"q", 3}, {"r\ns", 4}, {"t", 5}}));
  bool refused = false;
  try { lex("\"open"); } catch (const std::runtime_error&) { refused = true; }
  CHECK(refused);
  std::cout << "case passed: the lexer reads literals, comments, separators and raw strings\n";
}

// A model id the data names, and whether it names a profile that every id it starts covers.
struct Id {
  std::string text;
  bool prefix = false;
};
// A character an id continues with: a literal names an id only where it is bounded by the literal's ends or by a
// character that is not one of these.
bool id_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' || c == '/' || c == ':'; }
bool names(std::string_view literal, const Id& id) {
  for (auto at = literal.find(id.text); at != std::string_view::npos; at = literal.find(id.text, at + 1)) {
    const auto after = at + id.text.size();
    if ((at == 0 || !id_char(literal[at - 1])) && (id.prefix || after == literal.size() || !id_char(literal[after]))) return true;
  }
  return false;
}
// The declarations whose literals are the item grammar's own vocabulary, which check (c) does not read: the prefix,
// which names no kind, and the kind table that check (c) reads its kinds from.
struct Exclusion {
  const char* file;
  const char* declaration;
};
const Exclusion vocabulary[]{{"src/descriptor/catalogue.h", "item_unit_prefix"}, {"src/descriptor/catalogue.h", "item_kinds"}};
// The sources of money, where no literal may be a price.
const char* const pricing_sources[]{"src/core/pricing.cpp", "src/core/pricing.h", "src/core/pricing_internal.h", "src/core/endpoint_pricing.cpp",
                                    "src/core/endpoint_pricing.h", "src/runtime/cost.cpp", "src/runtime/cost.h", "src/descriptor/catalogue.cpp",
                                    "src/descriptor/catalogue.h"};
// Not product library code: neither directory is installed, and both hold model ids by design (the live canary's
// profiles and the qualification records).
const char* const excluded_directories[]{"src/canary", "src/qualification"};

struct Scan {
  std::size_t files = 0, literals = 0, vocabulary_literals = 0;
  std::vector<std::string> findings;
};
// Scans one source, named by its path relative to the repository root.
void scan(std::string_view path, std::string_view source, const std::vector<Id>& corpus, Scan& out) {
  ++out.files;
  const auto tokens = lex(source);
  const bool pricing = std::find(std::begin(pricing_sources), std::end(pricing_sources), path) != std::end(pricing_sources);
  // The literals of a vocabulary declaration: from its name followed by `=`, `[` or `{` up to the `;` that ends it.
  std::vector<bool> in_vocabulary(tokens.size(), false);
  for (const auto& exclusion : vocabulary) {
    if (path != exclusion.file) continue;
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
      const auto& next = tokens[i + 1];
      if (tokens[i].kind != Token::Kind::Identifier || tokens[i].text != exclusion.declaration || next.kind != Token::Kind::Punctuation ||
          (next.text != "=" && next.text != "[" && next.text != "{"))
        continue;
      for (std::size_t j = i + 1; j < tokens.size() && tokens[j].text != ";"; ++j) in_vocabulary[j] = tokens[j].kind == Token::Kind::String;
    }
  }
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const auto& token = tokens[i];
    if (token.kind != Token::Kind::String) continue;
    ++out.literals;
    const auto where = std::string(path) + ":" + std::to_string(token.line) + ": \"" + token.text + "\"";
    for (const auto& id : corpus)
      if (names(token.text, id)) out.findings.push_back(where + " names the model " + id.text);
    if (pricing && token.text.find('.') != std::string::npos && descriptor::valid_price_text(token.text))
      out.findings.push_back(where + " is a price");
    if (in_vocabulary[i]) { ++out.vocabulary_literals; continue; }
    if (token.text.starts_with(descriptor::item_unit_prefix)) {
      const auto rest = std::string_view(token.text).substr(descriptor::item_unit_prefix.size());
      const auto kind = rest.substr(0, rest.find_first_of("[/+"));
      if (std::none_of(std::begin(descriptor::item_kinds), std::end(descriptor::item_kinds), [&](const auto& known) { return known.name == kind; }))
        out.findings.push_back(where + " names an item kind the kind table does not");
    }
  }
}

void scan_cases() {
  const std::vector<Id> corpus{{"vendor-fixture/model-1", false}, {"vendor-profile-", true}};
  const std::string source = R"src(
const char* model = "vendor-fixture/model-1";
const char* longer = "vendor-fixture/model-10";
const char* path = "a/vendor-fixture/model-1";
const char* phrase = "send vendor-fixture/model-1 now";
const char* dated = "vendor-profile-7";
const char* price = "0.25";
const char* whole = "25";
const char* text = "version 1.5";
const char* unit = "item:video";
const char* known = "item:image[size=1]/batch";
// "vendor-fixture/model-1" "item:video" "0.5"
)src";
  Scan priced;
  scan("src/core/endpoint_pricing.cpp", source, corpus, priced);
  CHECK(priced.findings.size() == 5 && priced.literals == 10);
  CHECK(priced.findings[0].find(":2:") != std::string::npos && priced.findings[1].find(":5:") != std::string::npos &&
        priced.findings[2].find(":6:") != std::string::npos && priced.findings[3].find("is a price") != std::string::npos &&
        priced.findings[4].find("item kind") != std::string::npos);
  // Outside the pricing sources a decimal is not checked.
  Scan other;
  scan("src/codecs/other.cpp", source, corpus, other);
  CHECK(other.findings.size() == 4);
  // A vocabulary declaration is skipped by check (c) only where it is declared, and only its own literals.
  const std::string grammar = "inline constexpr std::string_view item_unit_prefix = \"item:\";\nconst char* after = \"item:\";\n";
  Scan declared;
  scan("src/descriptor/catalogue.h", grammar, {}, declared);
  CHECK(declared.vocabulary_literals == 1 && declared.findings.size() == 1 && declared.findings[0].find(":2:") != std::string::npos);
  Scan elsewhere;
  scan("src/descriptor/policy.h", grammar, {}, elsewhere);
  CHECK(elsewhere.vocabulary_literals == 0 && elsewhere.findings.size() == 2);
  std::cout << "case passed: the scan finds an id, a price and an unknown item kind in a literal\n";
}

std::string read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("cannot read " + path.string());
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
json::Document parsed(const std::filesystem::path& path) {
  auto result = json::parse(read(path));
  if (auto* error = std::get_if<json::ParseError>(&result)) throw std::runtime_error(path.string() + ": " + error->message);
  return std::get<json::Document>(std::move(result));
}
std::vector<Id> corpus_of(const std::filesystem::path& root) {
  std::vector<Id> corpus;
  auto add = [&](std::string_view id, bool prefix) {
    if (!id.empty() && std::none_of(corpus.begin(), corpus.end(), [&](const Id& known) { return known.text == id && known.prefix == prefix; }))
      corpus.push_back({std::string(id), prefix});
  };
  auto loaded = descriptor::load_catalogue_file((root / "config" / "models.json").string());
  if (const auto* error = std::get_if<descriptor::ConfigError>(&loaded)) throw std::runtime_error("models.json refused at " + error->pointer);
  const auto& catalogue = *std::get<descriptor::CatalogueSnapshot>(loaded);
  for (const auto& entry : catalogue.entries()) {
    add(entry.id, entry.prefix);
    for (const auto& alias : entry.aliases) add(alias, false);
    for (const auto& served : entry.served_as) add(served, false);
    if (entry.alias_of) add(*entry.alias_of, false);
  }
  const auto from_models = corpus.size();
  CHECK(from_models >= catalogue.entries().size() && from_models > 0);
  const auto rates = parsed(root / "config" / "model-catalog.json");
  std::size_t listed = 0;
  for (const auto member : rates.root().get("models").members()) { add(member.key, false); ++listed; }
  CHECK(listed > 0);
  const auto known = parsed(root / "tests" / "fixtures" / "pricing" / "known_models.json");
  CHECK(known.root().get("version").is_uint() && known.root().get("version").as_uint() == 1);
  std::size_t named = 0;
  for (const auto value : known.root().get("models").elements()) {
    CHECK(value.is_string() && !value.as_string().empty());
    add(value.as_string(), false);
    ++named;
  }
  CHECK(named > 0);
  return corpus;
}

void product_sources(const std::filesystem::path& root) {
  const auto corpus = corpus_of(root);
  Scan out;
  std::vector<std::string> pricing_seen, vocabulary_files;
  for (const auto& item : std::filesystem::recursive_directory_iterator(root / "src")) {
    if (!item.is_regular_file()) continue;
    const auto extension = item.path().extension().string();
    if (extension != ".h" && extension != ".cpp") continue;
    const auto relative = item.path().lexically_relative(root).generic_string();
    if (std::any_of(std::begin(excluded_directories), std::end(excluded_directories),
                    [&](const char* directory) { return relative.starts_with(std::string(directory) + "/"); }))
      continue;
    if (std::find(std::begin(pricing_sources), std::end(pricing_sources), relative) != std::end(pricing_sources)) pricing_seen.push_back(relative);
    const auto before = out.vocabulary_literals;
    scan(relative, read(item.path()), corpus, out);
    if (out.vocabulary_literals > before) vocabulary_files.push_back(relative);
  }
  for (const auto& finding : out.findings) std::cerr << finding << '\n';
  CHECK(out.findings.empty());
  // The scan read what it names: every pricing source, the vocabulary declarations, and a good number of literals.
  CHECK(pricing_seen.size() == std::size(pricing_sources));
  CHECK(vocabulary_files == std::vector<std::string>{"src/descriptor/catalogue.h"} && out.vocabulary_literals >= 1 + 2 * std::size(descriptor::item_kinds));
  CHECK(out.files > 100 && out.literals > 5000);
  std::cout << "case passed: no model id, price or unknown item kind in " << out.files << " product sources (" << out.literals << " literals, "
            << corpus.size() << " model ids)\n";
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("usage: sp_pricing_sources_tests <repository root>");
    lexer_cases();
    scan_cases();
    product_sources(argv[1]);
    std::cout << "pricing sources passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "pricing sources test failed: " << error.what() << '\n';
    return 1;
  }
}
