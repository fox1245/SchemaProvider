#include "canary/canary.h"
#include "canary/io.h"
#include "json/json.h"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <initializer_list>
#include <limits>

namespace sp::canary {
namespace {
using detail::fail;
void keys(json::Value value, std::initializer_list<std::string_view> allowed) {
  if (!value.is_object()) fail();
  for (auto member : value.members())
    if (std::find(allowed.begin(), allowed.end(), member.key) == allowed.end()) fail();
}
std::string text(json::Value root, std::string_view key, std::size_t max) {
  auto value = root.get(key);
  if (!value.is_string() || value.as_string().empty() || value.as_string().size() > max) fail();
  for (unsigned char c : value.as_string()) if (c < 0x21 || c > 0x7e) fail();
  return std::string(value.as_string());
}
std::uint64_t number(json::Value root, std::string_view key) {
  auto value = root.get(key);
  if (!value.is_uint() || value.as_uint() == 0) fail();
  return value.as_uint();
}
bool loopback(std::string_view origin) {
  constexpr std::string_view v4 = "http://127.0.0.1:", v6 = "http://[::1]:";
  if (origin.starts_with(v4)) origin.remove_prefix(v4.size());
  else if (origin.starts_with(v6)) origin.remove_prefix(v6.size());
  else return false;
  unsigned port{};
  const auto [end, ec] = std::from_chars(origin.data(), origin.data() + origin.size(), port);
  return !origin.empty() && origin.front() != '0' && ec == std::errc{} &&
      end == origin.data() + origin.size() && port > 0 && port <= 65535;
}
bool key_character(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || (c >= '0' && c <= '9');
}
void valid_secret(std::string_view value) {
  if (value.size() > 4096) fail();
  for (unsigned char c : value) if (c < 0x21 || c > 0x7e || c == '"' || c == '\'') fail();
}
} // namespace
Profile parse_profile(std::string_view source, bool allow_test_loopback) {
  auto parsed = json::parse(source, {16384, 8});
  auto* doc = std::get_if<json::Document>(&parsed);
  if (!doc) fail();
  auto root = doc->root();
  keys(root, {"version", "provider", "model", "origin", "input_bound_kind", "max_input_tokens",
      "max_output_tokens", "input_micro_usd_per_million", "output_micro_usd_per_million",
      "call_cap", "token_cap", "micro_usd_cap", "thinking_budget", "provenance"});
  if (number(root, "version") != 1) fail();
  Profile result;
  auto provider = text(root, "provider", 16);
  if (provider == "openai") result.provider_ = Provider::OpenAI;
  else if (provider == "anthropic") result.provider_ = Provider::Anthropic;
  else fail();
  result.model_ = text(root, "model", 128);
  for (unsigned char c : result.model_)
    if (!key_character(c) && c != '-' && c != '.') fail();
  result.origin_ = text(root, "origin", 128);
  const auto expected = result.provider_ == Provider::OpenAI ? "https://api.openai.com" : "https://api.anthropic.com";
  result.loopback_ = loopback(result.origin_);
  if (result.origin_ != expected && !(allow_test_loopback && result.loopback_)) fail();
  if (text(root, "input_bound_kind", 32) != "model_context_window") fail();
  auto& b = result.bounds_;
  b.input_tokens = number(root, "max_input_tokens");
  b.output_tokens = number(root, "max_output_tokens");
  b.input_rate = number(root, "input_micro_usd_per_million");
  b.output_rate = number(root, "output_micro_usd_per_million");
  b.calls = number(root, "call_cap"); b.tokens = number(root, "token_cap"); b.micro_usd = number(root, "micro_usd_cap");
  if (b.calls > 16 || b.micro_usd > 10000000 || b.output_tokens > b.input_tokens ||
      b.output_tokens > std::numeric_limits<std::uint64_t>::max() - b.input_tokens) fail();
  if (result.provider_ == Provider::Anthropic) {
    result.thinking_budget_ = number(root, "thinking_budget");
    if (result.thinking_budget_ < 1024 || result.thinking_budget_ >= b.output_tokens) fail();
  } else if (root.get("thinking_budget").valid()) fail();
  // Live campaign targets have independently sourced model-window/rate floors.
  // A profile may over-reserve, but cannot substitute a character-token guess.
  if (!result.loopback_) {
    if (result.provider_ == Provider::OpenAI) {
      if (result.model_ != "gpt-4.1-mini-2025-04-14" || b.input_tokens < 1047576 ||
          b.output_tokens > 32768 || b.input_rate < 400000 || b.output_rate < 1600000) fail();
    } else {
      if (result.model_ != "claude-haiku-4-5-20251001" || b.input_tokens < 200000 ||
          b.output_tokens > 64000 || b.input_rate < 2000000 || b.output_rate < 5000000) fail();
    }
  }
  auto provenance = root.get("provenance");
  keys(provenance, {"model_url", "pricing_url", "verified_at"});
  for (auto key : {"model_url", "pricing_url"}) {
    auto url = text(provenance, key, 2048);
    const bool openai = url.starts_with("https://developers.openai.com/") || url.starts_with("https://platform.openai.com/");
    const bool anthropic = url.starts_with("https://platform.claude.com/") || url.starts_with("https://docs.anthropic.com/");
    if (!(result.provider_ == Provider::OpenAI ? openai : anthropic)) fail();
  }
  auto date = text(provenance, "verified_at", 10);
  if (date.size() != 10 || date[4] != '-' || date[7] != '-') fail();
  for (std::size_t i = 0; i < date.size(); ++i)
    if (i != 4 && i != 7 && (date[i] < '0' || date[i] > '9')) fail();
  int year = 0;
  unsigned month = 0, day = 0;
  std::from_chars(date.data(), date.data() + 4, year);
  std::from_chars(date.data() + 5, date.data() + 7, month);
  std::from_chars(date.data() + 8, date.data() + 10, day);
  if (year < 1 || !std::chrono::year_month_day{std::chrono::year{year}, std::chrono::month{month},
      std::chrono::day{day}}.ok()) fail();
  (void)reserved_cost(b);
  return result;
}
std::string read_profile_file(const std::string& path) { return detail::read_file(path, 16384, false); }
std::string credential(Provider provider, const std::optional<std::string>& env_file) {
  const char* name = provider == Provider::OpenAI ? "OPENAI_API_KEY" : "ANTHROPIC_API_KEY";
  if (!env_file) {
    auto* value = std::getenv(name);
    if (!value) return {};
    std::size_t size = 0;
    while (size <= 4096 && value[size]) ++size;
    if (size > 4096) fail();
    std::string result(value, size); valid_secret(result); return result;
  }
  auto source = detail::read_file(*env_file, 65536, true);
  std::string result;
  bool found = false;
  std::string_view rest(source);
  while (!rest.empty()) {
    auto end = rest.find('\n');
    auto line = rest.substr(0, end);
    rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
    if (line.ends_with('\r')) line.remove_suffix(1);
    if (line.empty() || line.starts_with('#')) continue;
    auto equals = line.find('=');
    if (equals == std::string_view::npos || equals == 0) fail();
    auto key = line.substr(0, equals);
    for (unsigned char c : key) if (!key_character(c)) fail();
    if (key != name) continue;
    if (found) fail();
    found = true; result = line.substr(equals + 1); valid_secret(result);
  }
  return result;
}
} // namespace sp::canary
