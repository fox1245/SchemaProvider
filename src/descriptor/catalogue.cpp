#include "descriptor/catalogue.h"
#include "crypto/crypto.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include "sp/config_defaults.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace sp::descriptor {
namespace {
using json::Value;

// Bounds of a catalogue. A full refresh of the four public listings is about a thousand entries.
constexpr std::size_t max_catalogue_bytes = 8u * 1024u * 1024u;
constexpr std::size_t max_catalogue_depth = 6;
constexpr std::size_t max_entries = 16384;
constexpr std::size_t max_list = 256;
constexpr std::size_t max_name_bytes = 256;
constexpr std::size_t max_token_bytes = 64;
constexpr std::size_t max_control_values = 16;
// The prices one entry holds.
constexpr std::size_t max_prices = 64;

constexpr std::string_view families_known[]{"openai.chat", "anthropic.messages", "openai.responses",
                                            "google.generate", "google.interactions"};

[[noreturn]] void fail(std::string pointer, std::string_view expected) {
  throw ConfigError{std::move(pointer), std::string(expected), 0, "model catalogue value rejected"};
}
std::string at(std::string_view base, std::size_t index) { return std::string(base) + "/" + std::to_string(index); }
std::string at(std::string_view base, std::string_view key) {
  std::string out(base);
  out += '/';
  for (const char c : key) {
    if (c == '~') out += "~0";
    else if (c == '/') out += "~1";
    else out += c;
  }
  return out;
}

// A model name as providers spell it: printable ASCII without spaces (`vendor/model:free`, `~x/y-latest`).
bool model_name(std::string_view value) noexcept {
  return !value.empty() && value.size() <= max_name_bytes &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) { return c > 0x20 && c < 0x7f; });
}
// A listed token: modality, method, parameter, price unit, lifecycle or source.
bool token(std::string_view value) noexcept {
  return !value.empty() && value.size() <= max_token_bytes &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                  c == '-' || c == ':' || c == '+' || c == '/';
         });
}
// Display text: any UTF-8 the JSON parser admitted, without control characters.
bool display(std::string_view value) noexcept {
  return !value.empty() && value.size() <= max_name_bytes &&
         std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}
// A date or timestamp as listed ("2026-10-22", "2026-10-22T00:00:00Z").
bool date(std::string_view value) noexcept {
  return !value.empty() && value.size() <= 40 &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return (c >= '0' && c <= '9') || c == '-' || c == ':' || c == 'T' || c == 'Z' || c == '.' || c == '+';
         });
}
// A decimal USD amount as listed: digits with an optional fraction; a leading '-' is kept, since
// a listing marks a variable price that way (OpenRouter "-1").
bool decimal(std::string_view value) noexcept {
  if (!value.empty() && value.front() == '-') value.remove_prefix(1);
  const auto dot = value.find('.');
  const auto whole = value.substr(0, dot);
  auto digits = [](std::string_view part) {
    return !part.empty() && part.size() <= 24 && std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; });
  };
  return digits(whole) && (dot == std::string_view::npos || digits(value.substr(dot + 1)));
}
bool origin(std::string_view value) noexcept { return valid_origin(value) && !value.ends_with('/'); }
bool known_family(std::string_view family) noexcept {
  return std::find(std::begin(families_known), std::end(families_known), family) != std::end(families_known);
}

void closed(Value value, std::initializer_list<std::string_view> required, std::initializer_list<std::string_view> optional,
            const std::string& pointer) {
  if (!value.is_object()) fail(pointer, "object");
  for (auto member : value.members())
    if (std::find(required.begin(), required.end(), member.key) == required.end() &&
        std::find(optional.begin(), optional.end(), member.key) == optional.end())
      fail(at(pointer, member.key), "declared member");
  for (auto key : required)
    if (!value.get(key).valid()) fail(at(pointer, key), "required member");
}
// An optional member: absent and null both mean not set.
bool present(Value value) { return value.valid() && !value.is_null(); }
std::string text(Value value, const std::string& pointer, bool (*check)(std::string_view) noexcept, std::string_view expected) {
  if (!value.is_string() || !check(value.as_string())) fail(pointer, expected);
  return std::string(value.as_string());
}
std::optional<std::string> optional_text(Value value, const std::string& pointer, bool (*check)(std::string_view) noexcept,
                                         std::string_view expected) {
  if (!present(value)) return std::nullopt;
  return text(value, pointer, check, expected);
}
// Every list member of an entry is a set. It is kept sorted, so the order a provider or a file
// happens to list the items in changes neither the canonical text, the digest nor the diff.
std::vector<std::string> list(Value value, const std::string& pointer, bool (*check)(std::string_view) noexcept,
                              std::string_view expected) {
  std::vector<std::string> out;
  if (!present(value)) return out;
  if (!value.is_array() || value.size() > max_list) fail(pointer, "bounded array");
  std::size_t i = 0;
  for (auto element : value.elements()) {
    auto item = text(element, at(pointer, i++), check, expected);
    if (std::find(out.begin(), out.end(), item) != out.end()) fail(pointer, "unique items");
    out.push_back(std::move(item));
  }
  std::sort(out.begin(), out.end());
  return out;
}
std::optional<std::uint64_t> positive(Value value, const std::string& pointer) {
  if (!present(value)) return std::nullopt;
  if (!value.is_uint() || value.as_uint() == 0) fail(pointer, "positive integer");
  return value.as_uint();
}

DefaultsOverlay overlay(std::string_view family, Value value, const std::string& pointer) {
  closed(value, {}, {"max_output_tokens", "thinking_budget", "include_thoughts", "thinking_summaries", "strict_tools",
                     "reasoning_enabled", "reasoning_effort", "reasoning_summary", "thinking_level", "service_tier",
                     "temperature", "top_p"},
         pointer);
  DefaultsOverlay out;
  out.family = std::string(family);
  auto integer = [&](std::string_view key, auto& field) {
    const auto v = value.get(key);
    if (!v.valid()) return;
    if (v.is_null()) field.emplace(std::nullopt);
    else if (v.is_uint()) field.emplace(v.as_uint());
    else fail(at(pointer, key), "unsigned integer or null");
  };
  auto boolean = [&](std::string_view key, auto& field) {
    const auto v = value.get(key);
    if (!v.valid()) return;
    if (v.is_null()) field.emplace(std::nullopt);
    else if (v.is_bool()) field.emplace(v.as_bool());
    else fail(at(pointer, key), "boolean or null");
  };
  auto string = [&](std::string_view key, auto& field) {
    const auto v = value.get(key);
    if (!v.valid()) return;
    if (v.is_null()) field.emplace(std::nullopt);
    else field.emplace(text(v, at(pointer, key), token, "token or null"));
  };
  auto number = [&](std::string_view key, auto& field) {
    const auto v = value.get(key);
    if (!v.valid()) return;
    if (v.is_null()) field.emplace(std::nullopt);
    else if (v.is_number() && std::isfinite(v.as_double())) field.emplace(v.as_double());
    else fail(at(pointer, key), "finite number or null");
  };
  integer("max_output_tokens", out.max_output_tokens);
  integer("thinking_budget", out.thinking_budget);
  boolean("include_thoughts", out.include_thoughts);
  boolean("thinking_summaries", out.thinking_summaries);
  boolean("strict_tools", out.strict_tools);
  if (const auto v = value.get("reasoning_enabled"); v.valid()) {
    if (!v.is_bool()) fail(at(pointer, "reasoning_enabled"), "boolean");
    out.reasoning_enabled = v.as_bool();
  }
  string("reasoning_effort", out.reasoning_effort);
  string("reasoning_summary", out.reasoning_summary);
  string("thinking_level", out.thinking_level);
  string("service_tier", out.service_tier);
  number("temperature", out.temperature);
  number("top_p", out.top_p);
  return out;
}

CatalogueEntry parse_entry(Value value, const std::string& pointer) {
  closed(value, {"id"},
         {"match", "families", "origins", "aliases", "served_as", "alias_of", "name", "lifecycle", "retires_at", "source",
          "context_window", "input_limit", "output_limit", "input_modalities", "output_modalities", "methods", "parameters",
          "temperature", "pricing", "defaults", "control_values"},
         pointer);
  CatalogueEntry entry;
  entry.id = text(value.get("id"), at(pointer, "id"), model_name, "model name");
  if (const auto match = value.get("match"); present(match)) {
    if (!match.is_string() || (match.as_string() != "exact" && match.as_string() != "prefix"))
      fail(at(pointer, "match"), "\"exact\" or \"prefix\"");
    entry.prefix = match.as_string() == "prefix";
  }
  entry.families = list(value.get("families"), at(pointer, "families"), known_family, "SDK family");
  entry.origins = list(value.get("origins"), at(pointer, "origins"), origin, "canonical origin without path");
  entry.aliases = list(value.get("aliases"), at(pointer, "aliases"), model_name, "model name");
  entry.served_as = list(value.get("served_as"), at(pointer, "served_as"), model_name, "model name");
  if (entry.prefix && (!entry.aliases.empty() || !entry.served_as.empty()))
    fail(pointer, "a prefix profile without aliases or served names");
  entry.alias_of = optional_text(value.get("alias_of"), at(pointer, "alias_of"), model_name, "model name");
  entry.name = optional_text(value.get("name"), at(pointer, "name"), display, "display text");
  entry.lifecycle = optional_text(value.get("lifecycle"), at(pointer, "lifecycle"), token, "token");
  entry.retires_at = optional_text(value.get("retires_at"), at(pointer, "retires_at"), date, "date");
  entry.source = optional_text(value.get("source"), at(pointer, "source"), token, "token");
  entry.context_window = positive(value.get("context_window"), at(pointer, "context_window"));
  entry.input_limit = positive(value.get("input_limit"), at(pointer, "input_limit"));
  entry.output_limit = positive(value.get("output_limit"), at(pointer, "output_limit"));
  entry.input_modalities = list(value.get("input_modalities"), at(pointer, "input_modalities"), token, "token");
  entry.output_modalities = list(value.get("output_modalities"), at(pointer, "output_modalities"), token, "token");
  entry.methods = list(value.get("methods"), at(pointer, "methods"), token, "token");
  entry.parameters = list(value.get("parameters"), at(pointer, "parameters"), token, "token");
  if (const auto t = value.get("temperature"); present(t)) {
    if (!t.is_bool()) fail(at(pointer, "temperature"), "boolean");
    entry.temperature = t.as_bool();
  }
  if (const auto pricing = value.get("pricing"); present(pricing)) {
    if (!pricing.is_object() || pricing.size() > max_prices) fail(at(pointer, "pricing"), "bounded object");
    for (auto member : pricing.members()) {
      const auto where = at(at(pointer, "pricing"), member.key);
      if (!token(member.key)) fail(where, "price unit token");
      entry.pricing.emplace_back(std::string(member.key), text(member.value, where, decimal, "decimal USD text"));
    }
    std::sort(entry.pricing.begin(), entry.pricing.end());
  }
  if (const auto defaults = value.get("defaults"); present(defaults)) {
    if (!defaults.is_object()) fail(at(pointer, "defaults"), "object keyed by family");
    for (auto member : defaults.members()) {
      const auto where = at(at(pointer, "defaults"), member.key);
      if (!known_family(member.key)) fail(where, "SDK family");
      entry.defaults.push_back(overlay(member.key, member.value, where));
    }
    std::sort(entry.defaults.begin(), entry.defaults.end(), [](const auto& a, const auto& b) { return a.family < b.family; });
  }
  // The values the model takes for a control that takes listed values, by the control's policy name.
  if (const auto values = value.get("control_values"); present(values)) {
    const auto where = at(pointer, "control_values");
    if (!values.is_object()) fail(where, "object keyed by control");
    for (auto member : values.members()) {
      const auto control_at = at(where, member.key);
      const auto control = control_from_name(member.key);
      if (!control || !control_takes_values(*control)) fail(control_at, "a control that takes listed values");
      if (!member.value.is_array() || !member.value.size() || member.value.size() > max_control_values)
        fail(control_at, "one to sixteen unique tokens");
      entry.control_values.emplace_back(std::string(member.key), list(member.value, control_at, token, "token"));
    }
    std::sort(entry.control_values.begin(), entry.control_values.end());
  }
  return entry;
}

// Canonical writer. Members always appear in this order; absent ones are left out.
void put_list(std::string& out, std::string_view key, const std::vector<std::string>& values) {
  if (values.empty()) return;
  out += ',' + json::quote(key) + ":[";
  for (std::size_t i = 0; i < values.size(); ++i) out += (i ? "," : "") + json::quote(values[i]);
  out += ']';
}
void put_text(std::string& out, std::string_view key, const std::optional<std::string>& value) {
  if (value) out += ',' + json::quote(key) + ':' + json::quote(*value);
}
void put_number(std::string& out, std::string_view key, const std::optional<std::uint64_t>& value) {
  if (value) out += ',' + json::quote(key) + ':' + std::to_string(*value);
}
std::string number_text(double value) {
  // Shortest text that reads back as the same double. to_chars ignores the C locale, where
  // printf would write a decimal comma that is not JSON.
  char buffer[64];
  const auto written = std::to_chars(buffer, buffer + sizeof buffer, value);
  return std::string(buffer, written.ptr);
}
std::string overlay_json(const DefaultsOverlay& d) {
  std::string out;
  auto sep = [&] { if (!out.empty()) out += ','; };
  auto integer = [&](std::string_view key, const auto& field) {
    if (!field) return;
    sep(); out += json::quote(key) + ':' + (*field ? std::to_string(**field) : std::string("null"));
  };
  auto boolean = [&](std::string_view key, const auto& field) {
    if (!field) return;
    sep(); out += json::quote(key) + ':' + (*field ? (**field ? "true" : "false") : "null");
  };
  auto string = [&](std::string_view key, const auto& field) {
    if (!field) return;
    sep(); out += json::quote(key) + ':' + (*field ? json::quote(**field) : std::string("null"));
  };
  auto number = [&](std::string_view key, const auto& field) {
    if (!field) return;
    sep(); out += json::quote(key) + ':' + (*field ? number_text(**field) : std::string("null"));
  };
  integer("max_output_tokens", d.max_output_tokens);
  integer("thinking_budget", d.thinking_budget);
  boolean("include_thoughts", d.include_thoughts);
  boolean("thinking_summaries", d.thinking_summaries);
  boolean("strict_tools", d.strict_tools);
  if (d.reasoning_enabled) { sep(); out += std::string("\"reasoning_enabled\":") + (*d.reasoning_enabled ? "true" : "false"); }
  string("reasoning_effort", d.reasoning_effort);
  string("reasoning_summary", d.reasoning_summary);
  string("thinking_level", d.thinking_level);
  string("service_tier", d.service_tier);
  number("temperature", d.temperature);
  number("top_p", d.top_p);
  return '{' + out + '}';
}
std::string entry_json(const CatalogueEntry& e) {
  std::string out = "{\"id\":" + json::quote(e.id);
  if (e.prefix) out += ",\"match\":\"prefix\"";
  put_list(out, "families", e.families);
  put_list(out, "origins", e.origins);
  put_list(out, "aliases", e.aliases);
  put_list(out, "served_as", e.served_as);
  put_text(out, "alias_of", e.alias_of);
  put_text(out, "name", e.name);
  put_text(out, "lifecycle", e.lifecycle);
  put_text(out, "retires_at", e.retires_at);
  put_text(out, "source", e.source);
  put_number(out, "context_window", e.context_window);
  put_number(out, "input_limit", e.input_limit);
  put_number(out, "output_limit", e.output_limit);
  put_list(out, "input_modalities", e.input_modalities);
  put_list(out, "output_modalities", e.output_modalities);
  put_list(out, "methods", e.methods);
  put_list(out, "parameters", e.parameters);
  if (e.temperature) out += std::string(",\"temperature\":") + (*e.temperature ? "true" : "false");
  if (!e.control_values.empty()) {
    out += ",\"control_values\":{";
    for (std::size_t i = 0; i < e.control_values.size(); ++i) {
      out += (i ? "," : "") + json::quote(e.control_values[i].first) + ":[";
      const auto& values = e.control_values[i].second;
      for (std::size_t j = 0; j < values.size(); ++j) out += (j ? "," : "") + json::quote(values[j]);
      out += ']';
    }
    out += '}';
  }
  if (!e.pricing.empty()) {
    out += ",\"pricing\":{";
    for (std::size_t i = 0; i < e.pricing.size(); ++i)
      out += (i ? "," : "") + json::quote(e.pricing[i].first) + ':' + json::quote(e.pricing[i].second);
    out += '}';
  }
  if (!e.defaults.empty()) {
    out += ",\"defaults\":{";
    for (std::size_t i = 0; i < e.defaults.size(); ++i)
      out += (i ? "," : "") + json::quote(e.defaults[i].family) + ':' + overlay_json(e.defaults[i]);
    out += '}';
  }
  return out + '}';
}
std::string key_of(const CatalogueEntry& e) { return e.prefix ? e.id + "*" : e.id; }
} // namespace

class CatalogueBuilder {
 public:
  static CatalogueSnapshot build(std::vector<CatalogueEntry> entries) {
    if (entries.size() > max_entries) fail("/models", "at most 16384 entries");
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
      return std::tie(a.id, a.prefix) < std::tie(b.id, b.prefix);
    });
    for (std::size_t i = 1; i < entries.size(); ++i)
      if (entries[i].id == entries[i - 1].id && entries[i].prefix == entries[i - 1].prefix)
        fail("/models", "one entry per id and match");
    auto catalogue = std::shared_ptr<ModelCatalogue>(new ModelCatalogue);
    catalogue->entries_ = std::move(entries);
    const auto& all = catalogue->entries_;
    for (std::size_t i = 0; i < all.size(); ++i) {
      if (all[i].prefix) { catalogue->prefixes_.push_back(i); continue; }
      catalogue->names_.emplace_back(all[i].id, i);
      for (const auto& alias : all[i].aliases) catalogue->names_.emplace_back(alias, i);
    }
    std::sort(catalogue->names_.begin(), catalogue->names_.end());
    std::stable_sort(catalogue->prefixes_.begin(), catalogue->prefixes_.end(),
                     [&](std::size_t a, std::size_t b) { return all[a].id.size() > all[b].id.size(); });
    const auto canonical = catalogue->to_json();
    const auto digest = crypto::sha256(canonical);
    catalogue->digest_.assign(reinterpret_cast<const char*>(digest.data()), digest.size());
    return catalogue;
  }
};

ModelMatch ModelCatalogue::find(std::string_view family, std::string_view origin, std::string_view model) const noexcept {
  auto origin_ok = [&](const CatalogueEntry& e) { return e.origins.empty() || contains(e.origins, origin); };
  auto family_ok = [&](const CatalogueEntry& e) { return e.families.empty() || contains(e.families, family); };
  ModelMatch best;
  int best_rank = -1;
  auto consider = [&](const CatalogueEntry& e, ModelRelation relation, int base) {
    if (!origin_ok(e)) return;
    const int rank = base * 4 + (family_ok(e) ? 2 : 0) + (e.origins.empty() ? 0 : 1);
    if (rank > best_rank) { best_rank = rank; best = {&e, relation, family_ok(e)}; }
  };
  auto first = std::lower_bound(names_.begin(), names_.end(), model,
                                [](const auto& item, std::string_view name) { return item.first < name; });
  for (; first != names_.end() && first->first == model; ++first) {
    const auto& e = entries_[first->second];
    consider(e, e.id == model ? ModelRelation::Exact : ModelRelation::Alias, e.id == model ? 3 : 2);
  }
  if (best.entry) return best;
  std::size_t length = 0;
  for (const auto index : prefixes_) {
    const auto& e = entries_[index];
    if (best.entry && e.id.size() < length) break;
    if (model.size() >= e.id.size() && model.substr(0, e.id.size()) == e.id) {
      consider(e, ModelRelation::Prefix, 1);
      if (best.entry) length = best.entry->id.size();
    }
  }
  return best;
}

const CatalogueEntry* ModelCatalogue::for_each_match(std::string_view family, std::string_view origin, std::string_view model,
                                                     MatchVisitor visit, const void* context, bool profiles_only) const noexcept {
  auto applies = [&](const CatalogueEntry& e) {
    return (e.origins.empty() || contains(e.origins, origin)) && (e.families.empty() || contains(e.families, family));
  };
  auto first = std::lower_bound(names_.begin(), names_.end(), model,
                                [](const auto& item, std::string_view name) { return item.first < name; });
  auto last = first;
  while (last != names_.end() && last->first == model) ++last;
  if (profiles_only) first = last;
  for (auto it = first; it != last; ++it) {
    const auto& e = entries_[it->second];
    if (e.id == model && applies(e) && visit(e, context)) return &e;
  }
  for (const bool scoped : {true, false})
    for (auto it = first; it != last; ++it) {
      const auto& e = entries_[it->second];
      if (e.id != model && e.origins.empty() != scoped && applies(e) && visit(e, context)) return &e;
    }
  for (const auto index : prefixes_) {
    const auto& e = entries_[index];
    if (model.size() >= e.id.size() && model.substr(0, e.id.size()) == e.id && applies(e) && visit(e, context)) return &e;
  }
  return nullptr;
}

const CatalogueEntry* ModelCatalogue::entry(std::string_view id, bool prefix) const noexcept {
  auto found = std::lower_bound(entries_.begin(), entries_.end(), std::pair{id, prefix}, [](const auto& e, const auto& key) {
    return std::tie(e.id, e.prefix) < std::tuple<std::string_view, bool>(key.first, key.second);
  });
  return found != entries_.end() && found->id == id && found->prefix == prefix ? &*found : nullptr;
}

std::string ModelCatalogue::digest_hex() const {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (const unsigned char byte : digest_) { out += hex[byte >> 4]; out += hex[byte & 15]; }
  return out;
}

std::string ModelCatalogue::to_json() const {
  std::string out = "{\"version\":1,\"models\":[";
  for (std::size_t i = 0; i < entries_.size(); ++i) out += (i ? "," : "") + entry_json(entries_[i]);
  return out + "]}";
}

CatalogueResult make_catalogue(std::vector<CatalogueEntry> entries) {
  try {
    // Entries built in memory pass the same checks as loaded ones: write them and read them back.
    std::string text = "{\"version\":1,\"models\":[";
    for (std::size_t i = 0; i < entries.size(); ++i) text += (i ? "," : "") + entry_json(entries[i]);
    return load_catalogue(text + "]}");
  } catch (ConfigError& error) { return std::move(error); }
}

CatalogueResult load_catalogue(std::string_view source) {
  try {
    if (source.size() > max_catalogue_bytes) fail("", "catalogue within 8 MiB");
    auto parsed = json::parse(source, {max_catalogue_bytes, max_catalogue_depth});
    if (auto* error = std::get_if<json::ParseError>(&parsed)) fail(error->pointer, "strict bounded JSON catalogue");
    const auto root = std::get<json::Document>(parsed).root();
    closed(root, {"version", "models"}, {}, "");
    if (!root.get("version").is_uint() || root.get("version").as_uint() != 1) fail("/version", "version 1");
    const auto models = root.get("models");
    if (!models.is_array() || models.size() > max_entries) fail("/models", "at most 16384 entries");
    std::vector<CatalogueEntry> entries;
    entries.reserve(models.size());
    std::size_t i = 0;
    for (auto element : models.elements()) entries.push_back(parse_entry(element, at("/models", i++)));
    return CatalogueBuilder::build(std::move(entries));
  } catch (ConfigError& error) { return std::move(error); }
}

CatalogueResult load_catalogue_file(std::string_view path) {
  if (path.empty() || path.find('\0') != std::string_view::npos)
    return ConfigError{"", "readable bounded catalogue file", 0, "catalogue file path is invalid"};
  std::ifstream stream{std::string(path), std::ios::binary};
  if (!stream.is_open()) return ConfigError{"", "readable bounded catalogue file", 0, "catalogue file could not be opened"};
  std::string contents;
  char buffer[65536];
  while (stream.read(buffer, sizeof buffer) || stream.gcount() > 0) {
    if (contents.size() + static_cast<std::size_t>(stream.gcount()) > max_catalogue_bytes)
      return ConfigError{"", "catalogue within 8 MiB", 0, "catalogue file exceeds the byte limit"};
    contents.append(buffer, static_cast<std::size_t>(stream.gcount()));
  }
  if (stream.bad()) return ConfigError{"", "readable bounded catalogue file", 0, "catalogue file read failed"};
  return load_catalogue(contents);
}

std::string builtin_catalogue_json() { return std::string(config_defaults::model_catalogue_json); }

CatalogueSnapshot builtin_catalogue() {
  static const auto value = [] {
    auto loaded = load_catalogue(config_defaults::model_catalogue_json);
    if (auto* error = std::get_if<ConfigError>(&loaded)) throw std::logic_error(error->pointer + ": " + error->expected);
    return std::get<CatalogueSnapshot>(std::move(loaded));
  }();
  return value;
}

namespace {
template <class T> void take(T& into, const T& from) { if (from) into = from; }
void take_list(std::vector<std::string>& into, const std::vector<std::string>& from) { if (!from.empty()) into = from; }
void merge_entry(CatalogueEntry& into, const CatalogueEntry& from) {
  take_list(into.families, from.families);
  // The origins scope the whole entry, not one member. The later entry's replace the earlier
  // ones, and an entry without origins applies at every origin, so the merged one does too.
  into.origins = from.origins;
  take_list(into.aliases, from.aliases);
  take_list(into.served_as, from.served_as);
  take(into.alias_of, from.alias_of);
  take(into.name, from.name);
  take(into.lifecycle, from.lifecycle);
  take(into.retires_at, from.retires_at);
  take(into.source, from.source);
  take(into.context_window, from.context_window);
  take(into.input_limit, from.input_limit);
  take(into.output_limit, from.output_limit);
  take_list(into.input_modalities, from.input_modalities);
  take_list(into.output_modalities, from.output_modalities);
  take_list(into.methods, from.methods);
  take_list(into.parameters, from.parameters);
  take(into.temperature, from.temperature);
  // A later layer's price for a unit replaces the earlier one; other units stay.
  for (const auto& [unit, price] : from.pricing) {
    auto found = std::find_if(into.pricing.begin(), into.pricing.end(), [&](const auto& p) { return p.first == unit; });
    if (found != into.pricing.end()) found->second = price;
    else into.pricing.emplace_back(unit, price);
  }
  std::sort(into.pricing.begin(), into.pricing.end());
  // Defaults merge per family and per member, like the entry itself.
  for (const auto& overlay : from.defaults) {
    auto found = std::find_if(into.defaults.begin(), into.defaults.end(), [&](const auto& d) { return d.family == overlay.family; });
    if (found == into.defaults.end()) { into.defaults.push_back(overlay); continue; }
    take(found->max_output_tokens, overlay.max_output_tokens);
    take(found->thinking_budget, overlay.thinking_budget);
    take(found->include_thoughts, overlay.include_thoughts);
    take(found->thinking_summaries, overlay.thinking_summaries);
    take(found->strict_tools, overlay.strict_tools);
    take(found->reasoning_enabled, overlay.reasoning_enabled);
    take(found->reasoning_effort, overlay.reasoning_effort);
    take(found->reasoning_summary, overlay.reasoning_summary);
    take(found->thinking_level, overlay.thinking_level);
    take(found->service_tier, overlay.service_tier);
    take(found->temperature, overlay.temperature);
    take(found->top_p, overlay.top_p);
  }
  std::sort(into.defaults.begin(), into.defaults.end(), [](const auto& a, const auto& b) { return a.family < b.family; });
  // A later layer's list for a control replaces the earlier one; other controls stay.
  for (const auto& [control, values] : from.control_values) {
    auto found = std::find_if(into.control_values.begin(), into.control_values.end(), [&](const auto& c) { return c.first == control; });
    if (found != into.control_values.end()) found->second = values;
    else into.control_values.emplace_back(control, values);
  }
  std::sort(into.control_values.begin(), into.control_values.end());
}
} // namespace

CatalogueResult merge_catalogues(std::span<const CatalogueSnapshot> layers) {
  std::vector<CatalogueEntry> merged;
  std::map<std::pair<std::string_view, bool>, std::size_t> index;
  for (const auto& layer : layers) {
    if (!layer) continue;
    for (const auto& entry : layer->entries()) {
      // Keys borrow the layer's ids, which outlive this loop.
      const auto [found, inserted] = index.try_emplace({entry.id, entry.prefix}, merged.size());
      if (inserted) merged.push_back(entry);
      else merge_entry(merged[found->second], entry);
    }
  }
  return make_catalogue(std::move(merged));
}

CatalogueDiff diff_catalogues(const ModelCatalogue& before, const ModelCatalogue& after) {
  CatalogueDiff diff;
  const auto& a = before.entries();
  const auto& b = after.entries();
  std::size_t i = 0, j = 0;
  auto less = [](const CatalogueEntry& x, const CatalogueEntry& y) { return std::tie(x.id, x.prefix) < std::tie(y.id, y.prefix); };
  while (i < a.size() || j < b.size()) {
    if (j == b.size() || (i < a.size() && less(a[i], b[j]))) diff.removed.push_back(key_of(a[i++]));
    else if (i == a.size() || less(b[j], a[i])) diff.added.push_back(key_of(b[j++]));
    else {
      if (entry_json(a[i]) != entry_json(b[j])) diff.changed.push_back(key_of(b[j]));
      ++i; ++j;
    }
  }
  return diff;
}

bool valid_model_name(std::string_view id) noexcept { return model_name(id); }
bool valid_price_text(std::string_view price) noexcept { return decimal(price); }

std::optional<std::uint64_t> price_per_million(std::string_view text) noexcept {
  // nano-USD per million units is USD x 1e15.
  constexpr std::size_t rate_digits = 15;
  constexpr auto most = std::numeric_limits<std::uint64_t>::max();
  if (text.empty() || text.front() == '-') return std::nullopt;
  const auto dot = text.find('.');
  const auto whole = text.substr(0, dot);
  const auto fraction = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
  std::uint64_t value = 0;
  // value x 10 + the digit, unless the character is not a digit or the amount leaves 64 bits. The amount only grows,
  // so one that leaves 64 bits never returns.
  const auto shift = [&value](char c) {
    if (c < '0' || c > '9') return false;
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (value > (most - digit) / 10) return false;
    value = value * 10 + digit;
    return true;
  };
  for (const char c : whole)
    if (!shift(c)) return std::nullopt;
  for (std::size_t i = 0; i < rate_digits; ++i)
    if (!shift(i < fraction.size() ? fraction[i] : '0')) return std::nullopt;
  bool remainder = false;
  for (std::size_t i = rate_digits; i < fraction.size(); ++i) {
    if (fraction[i] < '0' || fraction[i] > '9') return std::nullopt;
    remainder = remainder || fraction[i] != '0';
  }
  if (remainder) {
    if (value == most) return std::nullopt;
    ++value;
  }
  return value;
}

std::string_view plain_price_unit(std::string_view name) noexcept {
  constexpr std::pair<std::string_view, std::string_view> names[]{
      {"prompt", "input"},          {"completion", "output"},
      {"input_cache_read", "cache_read"}, {"input_cache_write", "cache_write"},
      {"input_cache_write_1h", "cache_write_1h"}, {"internal_reasoning", "reasoning"},
      {"audio", "input_audio"},     {"audio_output", "output_audio"}};
  for (const auto& [listed, plain] : names)
    if (name == listed) return plain;
  return name;
}

std::string_view unbanded_unit(std::string_view unit) noexcept {
  const auto plus = unit.rfind('+');
  if (plus == std::string_view::npos) return unit;
  const auto digits = unit.substr(plus + 1);
  if (digits.empty() || digits.size() > max_band_digits || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
    return unit;
  return unit.substr(0, plus);
}

std::string_view listing_name(Listing listing) noexcept {
  switch (listing) {
    case Listing::OpenRouter: return "openrouter";
    case Listing::Gemini: return "gemini";
    case Listing::Anthropic: return "anthropic";
    case Listing::OpenAI: return "openai";
  }
  return {};
}

namespace {
// Listing readers keep what the catalogue can hold and drop the rest member by member.
std::optional<std::string> listed_text(Value value, bool (*check)(std::string_view) noexcept) {
  if (value.is_string() && check(value.as_string())) return std::string(value.as_string());
  return std::nullopt;
}
std::optional<std::uint64_t> listed_count(Value value) {
  if (value.is_uint() && value.as_uint() > 0) return value.as_uint();
  return std::nullopt;
}
std::vector<std::string> listed_tokens(Value value) {
  std::vector<std::string> out;
  if (!value.is_array()) return out;
  for (auto element : value.elements()) {
    if (out.size() == max_list) break;
    if (!element.is_string() || !token(element.as_string())) continue;
    std::string item(element.as_string());
    if (!contains(out, item)) out.push_back(std::move(item));
  }
  return out;
}
bool page_token(std::string_view value) noexcept {
  return !value.empty() && value.size() <= 1024 &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) { return c > 0x20 && c < 0x7f; });
}
// A price member a models listing names in the provider's words: a unit token without the grammar's tier (`/`) or band
// (`+`), and not a unit only the catalogue writes, a route ceiling or a tool fee. Any other key is a skipped price, and
// its text reaches no report.
bool listed_unit(std::string_view key) {
  return token(key) && key.find('/') == std::string_view::npos && key.find('+') == std::string_view::npos &&
         !key.starts_with(tool_fee_prefix) &&
         std::find(std::begin(route_ceiling_units), std::end(route_ceiling_units), key) == std::end(route_ceiling_units);
}
bool supported(Value capabilities, std::string_view name) {
  const auto capability = capabilities.get(name);
  return capability.is_object() && capability.get("supported").is_bool() && capability.get("supported").as_bool();
}

// How pricing (core/pricing.h) bills the tokens of a unit it reads, by plain unit name: at the input or the output price
// of the call; as a part of the input or of the output, at the input or output price where the part has none; as a
// cache write, whose 5-minute and 1-hour prices also stand in for each other; or as the request fee, which nothing
// stands in for. A unit left out takes with it the units its tokens would otherwise be billed at, so that a bound or an
// estimate of a call that bills it finds no price rather than a lower one.
enum class Bills : std::uint8_t { Input, InputPart, CacheWrite, Output, OutputPart, Request };
constexpr std::pair<std::string_view, Bills> billed_units[]{
    {"input", Bills::Input},             {"cache_read", Bills::InputPart},      {"input_audio", Bills::InputPart},
    {"cache_write", Bills::CacheWrite},  {"cache_write_1h", Bills::CacheWrite}, {"output", Bills::Output},
    {"reasoning", Bills::OutputPart},    {"output_image", Bills::OutputPart},   {"output_audio", Bills::OutputPart},
    {"request", Bills::Request}};
// The plain names of the units left out with `unit`.
std::vector<std::string_view> left_out_with(std::string_view unit) {
  const auto plain = plain_price_unit(unit);
  const auto found = std::find_if(std::begin(billed_units), std::end(billed_units), [&](const auto& item) { return item.first == plain; });
  if (found == std::end(billed_units)) return {plain};
  switch (found->second) {
    case Bills::Input: return {"input"};
    case Bills::InputPart: return {plain, "input"};
    case Bills::CacheWrite: return {"cache_write", "cache_write_1h", "input"};
    case Bills::Output: return {"output"};
    case Bills::OutputPart: return {plain, "output"};
    case Bills::Request: return {"request", "input", "output"};
  }
  return {plain};
}
// Whether pricing bills the tokens of `unit` at another unit where `unit` has no price: a part of the input or the
// output, or a cache write.
bool falls_back(std::string_view unit) {
  const auto plain = plain_price_unit(unit);
  const auto found = std::find_if(std::begin(billed_units), std::end(billed_units), [&](const auto& item) { return item.first == plain; });
  return found != std::end(billed_units) &&
         (found->second == Bills::InputPart || found->second == Bills::CacheWrite || found->second == Bills::OutputPart);
}
// Leaves the listed price member `unit` out of the entry, its base price and its bands, together with the units its
// tokens would be billed at instead, and records each member left out. An empty `unit` leaves out every price.
void leave_out(CatalogueEntry& entry, std::string_view unit, std::vector<WithheldUnit>& withheld) {
  withheld.push_back({entry.id, std::string(unit)});
  if (unit.empty()) {
    entry.pricing.clear();
    return;
  }
  const auto names = left_out_with(unit);
  std::vector<std::pair<std::string, std::string>> kept;
  for (auto& price : entry.pricing) {
    const auto listed = unbanded_unit(price.first);
    if (std::find(names.begin(), names.end(), plain_price_unit(listed)) == names.end()) kept.push_back(std::move(price));
    else withheld.push_back({entry.id, std::string(listed)});
  }
  entry.pricing = std::move(kept);
}
// The unit of a key's band: it prices requests with strictly more than `above` input tokens, as `min_prompt_tokens`
// does.
std::string unit_with_band(std::string_view key, std::uint64_t above) { return std::string(key) + '+' + std::to_string(above); }

// A price an override lists: its unit by plain name (plain_price_unit), the member name it was listed under, its text,
// and its amount when an entry can hold it as a price.
struct ConditionalPrice {
  std::string key, spelling, text;
  std::optional<std::uint64_t> rate;
};
// One entry of OpenRouter's `pricing.overrides`, as the price grammar reads it.
struct PriceOverride {
  // `min_prompt_tokens` is listed, and `above` is it when it is a count a band can hold.
  bool threshold = false;
  std::optional<std::uint64_t> above;
  // `utc_start` or `utc_end` is listed, and `start` and `end` are them when they are clock times.
  bool window = false;
  std::optional<std::uint64_t> start, end;
  // A condition the grammar cannot read, a discount, or a member that is not price text.
  bool other = false;
  std::vector<ConditionalPrice> prices;  // the unit members that are text
  std::vector<std::string> unread;       // the other unit members, which are not text, by plain name
  std::vector<std::string> members;      // the units it lists, by plain name, sorted
  bool readable() const {
    return std::all_of(prices.begin(), prices.end(), [](const ConditionalPrice& price) { return price.rate.has_value(); });
  }
  // The override prices requests above `above` input tokens and nothing else, at prices an entry can hold.
  bool band() const { return above && !window && !other && readable(); }
  // The override prices a daily time window and nothing else, at prices an entry can hold.
  bool time_window() const { return start && end && !threshold && !other && readable(); }
  bool lists(const std::string& key) const { return std::binary_search(members.begin(), members.end(), key); }
};
// A clock time as the listing writes it, HHMM from 0000 to 2359.
bool clock_time(Value value) {
  return value.is_uint() && value.as_uint() <= 2359 && value.as_uint() % 100 < 60;
}
PriceOverride read_override(Value element, bool& every_unit, ListingPage& page) {
  PriceOverride out;
  for (auto member : element.members()) {
    if (!present(member.value)) continue;
    if (member.key == "min_prompt_tokens") {
      // Strictly more input tokens than this: the band `+<tokens>`, a count above zero of at most max_band_digits
      // digits. A band of zero is the unit without a band.
      out.threshold = true;
      const auto& value = member.value;
      if (value.is_uint() && value.as_uint() > 0 && std::to_string(value.as_uint()).size() <= max_band_digits) out.above = value.as_uint();
      else out.other = true;
    } else if (member.key == "utc_start" || member.key == "utc_end") {
      // A daily window from its start up to, not including, its end, in UTC.
      out.window = true;
      if (!clock_time(member.value)) out.other = true;
      else (member.key == "utc_start" ? out.start : out.end) = member.value.as_uint();
    } else if (member.key == "discount") {
      // The listing multiplies the override's prices by (1 - discount). A discount from 0 to 1 keeps each at most the
      // price listed; any other may raise every price, the ones the override inherits included.
      out.other = true;
      if (!(member.value.is_number() && member.value.as_double() >= 0 && member.value.as_double() <= 1)) {
        every_unit = true;
        ++page.skipped_override_prices;
      }
    } else if (member.value.is_string()) {
      // A key that is not a unit a listing names is a skipped price.
      if (!listed_unit(member.key)) {
        ++page.skipped_prices;
        continue;
      }
      ConditionalPrice price{std::string(plain_price_unit(member.key)), std::string(member.key), std::string(member.value.as_string()), std::nullopt};
      if (decimal(price.text)) price.rate = price_per_million(price.text);
      out.members.push_back(price.key);
      out.prices.push_back(std::move(price));
    } else {
      // Another condition, or a price that is not text.
      out.other = true;
      if (listed_unit(member.key)) {
        out.unread.emplace_back(plain_price_unit(member.key));
        out.members.push_back(out.unread.back());
      }
    }
  }
  std::sort(out.members.begin(), out.members.end());
  return out;
}
// The input bands of `pricing.overrides`, `<unit>+<tokens>` for each price of an override with `min_prompt_tokens`
// alone, and the units an override the grammar cannot hold leaves out (USAGE.md, refreshing the catalogue).
void read_overrides(Value overrides, CatalogueEntry& e, ListingPage& page) {
  bool every_unit = false;
  std::vector<PriceOverride> listed;
  if (!overrides.is_array()) {
    every_unit = true;
    ++page.skipped_override_prices;
  } else {
    for (auto element : overrides.elements()) {
      if (!element.is_object()) {
        every_unit = true;
        ++page.skipped_override_prices;
        continue;
      }
      listed.push_back(read_override(element, every_unit, page));
    }
  }
  const auto has_unit = [&e](const std::string& unit) {
    return std::any_of(e.pricing.begin(), e.pricing.end(), [&](const auto& price) { return price.first == unit; });
  };
  // Keys are units by plain name. The base price of one is the member that spells it, and where the base lists both
  // spellings, the plain one, as pricing reads them.
  const auto base_unit = [&e](const std::string& key) {
    auto found = e.pricing.end();
    for (auto price = e.pricing.begin(); price != e.pricing.end(); ++price)
      if (plain_price_unit(price->first) == key && (found == e.pricing.end() || price->first == key)) found = price;
    return found;
  };
  const auto base_rate = [&](const std::string& key) -> std::optional<std::uint64_t> {
    const auto found = base_unit(key);
    if (found == e.pricing.end()) return std::nullopt;
    return price_per_million(found->second);
  };
  // The member name a unit is written under: its base price's, else the first an override lists.
  std::map<std::string, std::string> first_spelling;
  for (const auto& o : listed)
    for (const auto& price : o.prices) first_spelling.try_emplace(price.key, price.spelling);
  const auto spelling = [&](const std::string& key) -> std::string {
    if (const auto found = base_unit(key); found != e.pricing.end()) return found->first;
    if (const auto found = first_spelling.find(key); found != first_spelling.end()) return found->second;
    return key;
  };
  // More overrides than an entry holds prices are not bands.
  const bool may_band = listed.size() <= max_prices;
  const auto is_band = [&](std::size_t i) { return may_band && listed[i].band(); };
  struct Candidate {
    std::size_t index;
    std::uint64_t above;
    const ConditionalPrice* price;
  };
  std::map<std::string, std::vector<Candidate>> by_key;  // in listing order
  for (std::size_t i = 0; i < listed.size(); ++i)
    if (is_band(i))
      for (const auto& price : listed[i].prices) by_key[price.key].push_back({i, *listed[i].above, &price});
  // The prices of the overrides that are a time window alone, by key, in listing order.
  std::map<std::string, std::vector<const ConditionalPrice*>> by_window;
  for (const auto& o : listed)
    if (o.time_window())
      for (const auto& price : o.prices) by_window[price.key].push_back(&price);
  // A key with both a threshold and a time window has no one price the windows can be held at.
  const auto mixed = [&](const std::string& key) {
    return by_window.count(key) && std::any_of(listed.begin(), listed.end(), [&](const PriceOverride& o) { return o.threshold && o.lists(key); });
  };
  std::vector<std::pair<std::string, std::string>> bands;
  std::map<std::string, std::uint64_t> lowest_band;  // by key, the lowest price of the bands held
  // The entry's lowest price for a key under any condition: its base price and each band it holds. None where the
  // base price is missing or cannot be read.
  const auto lowest = [&](const std::string& key) -> std::optional<std::uint64_t> {
    auto rate = base_rate(key);
    if (!rate) return std::nullopt;
    if (const auto found = lowest_band.find(key); found != lowest_band.end() && found->second < *rate) rate = found->second;
    return rate;
  };
  // A conditional price the entry does not hold as a band changes nothing when it is at most that price, and otherwise
  // leaves its unit out: the entry never claims a price that may be too low, and never a guessed one. Each is decided
  // once the bands of its key are.
  std::set<std::string> dearer;
  const auto not_held = [&](const std::string& key, const std::optional<std::uint64_t>& rate) {
    ++page.skipped_override_prices;
    if (const auto floor = lowest(key); !rate || !floor || *rate > *floor) dearer.insert(key);
  };
  const auto left_out = [&](const std::string& key) {
    ++page.skipped_override_prices;
    dearer.insert(key);
  };
  auto room = max_prices - e.pricing.size();
  for (const auto& item : by_key) {
    const auto& key = item.first;
    const auto& candidates = item.second;
    if (mixed(key)) {
      for (std::size_t n = 0; n < candidates.size(); ++n) left_out(key);
      continue;
    }
    const auto base = base_rate(key);
    bool hold = candidates.size() <= room;
    for (std::size_t n = 0; hold && n < candidates.size(); ++n) {
      const auto& candidate = candidates[n];
      const auto unit = unit_with_band(spelling(key), candidate.above);
      // "Later entries win per key" and "the highest band wins" agree only when the thresholds increase in listing order.
      if (n && candidate.above <= candidates[n - 1].above) hold = false;
      // A unit name an entry can hold, and not one it already has.
      else if (!token(unit) || has_unit(unit)) hold = false;
      // A later override that leaves the key out may be read as billing it at the base price again, so a band below
      // the base price is held only where none does.
      else if ((!base || *candidate.price->rate < *base) &&
               std::any_of(listed.begin() + static_cast<std::ptrdiff_t>(candidate.index) + 1, listed.end(),
                           [&](const PriceOverride& later) { return !later.lists(key); }))
        hold = false;
    }
    if (!hold) {
      // The key has no band, so its lowest price is its base price.
      for (const auto& candidate : candidates) not_held(key, candidate.price->rate);
      continue;
    }
    room -= candidates.size();
    for (const auto& candidate : candidates) {
      bands.emplace_back(unit_with_band(spelling(key), candidate.above), candidate.price->text);
      const auto [found, inserted] = lowest_band.try_emplace(key, *candidate.price->rate);
      if (!inserted && *candidate.price->rate < found->second) found->second = *candidate.price->rate;
    }
  }
  // A key that time windows alone list is held at the dearest of its base price and their prices, in the text of the
  // dearest window where one is dearest: whichever window is current when the listing is fetched or a call is billed,
  // the entry is the same and not below it. A unit whose tokens are billed at another unit where it has no price (a
  // cache read, reasoning) is left out instead when the base or a window does not list it, since in that window's hours
  // the dearest of the others bounds nothing.
  for (const auto& item : by_window) {
    const auto& key = item.first;
    const auto& prices = item.second;
    const auto at = base_unit(key);
    const auto listed_base = at != e.pricing.end();
    const auto base = listed_base ? price_per_million(at->second) : std::nullopt;
    const bool partial = falls_back(key) && (!listed_base || std::any_of(listed.begin(), listed.end(), [&](const PriceOverride& o) {
                                                return o.time_window() && !o.lists(key);
                                              }));
    if (mixed(key) || (listed_base && !base) || partial || (!listed_base && room == 0)) {
      for (std::size_t n = 0; n < prices.size(); ++n) left_out(key);
      continue;
    }
    const ConditionalPrice* dearest = nullptr;
    for (const auto* price : prices)
      if (!dearest || *price->rate > *dearest->rate) dearest = price;
    page.window_prices_at_dearest += prices.size();
    if (!listed_base) {
      e.pricing.emplace_back(spelling(key), dearest->text);
      --room;
    } else if (*dearest->rate >= *base) {
      at->second = dearest->text;
    }
  }
  for (std::size_t i = 0; i < listed.size(); ++i) {
    if (is_band(i) || listed[i].time_window()) continue;
    for (const auto& price : listed[i].prices) not_held(price.key, price.rate);
    // A member that is not text is a price only where the entry has that unit, and then one that cannot be read.
    for (const auto& key : listed[i].unread)
      if (base_unit(key) != e.pricing.end() || lowest_band.count(key)) not_held(key, std::nullopt);
  }
  e.pricing.insert(e.pricing.end(), bands.begin(), bands.end());
  if (every_unit) {
    leave_out(e, {}, page.withheld);
    return;
  }
  // Named by the member the listing gives the unit under.
  for (const auto& key : dearer) leave_out(e, spelling(key), page.withheld);
}
// A later copy of an id in the same listing page. Its other members merge as a later layer's do (merge_entry). A unit
// both copies price under the same units, base price and bands alike, keeps the dearer price of each; a unit one copy
// prices and the other does not, that the copies band differently, or whose differing prices cannot both be read, is
// left out with the units its tokens would be billed at, since the entry cannot tell which copy bills a call.
void merge_listed_copy(CatalogueEntry& into, const CatalogueEntry& from, std::vector<WithheldUnit>& withheld) {
  using Prices = std::vector<std::pair<std::string, std::string>>;
  const auto by_unit = [](const Prices& pricing) {
    std::map<std::string, Prices> out;
    for (const auto& price : pricing) out[std::string(plain_price_unit(unbanded_unit(price.first)))].push_back(price);
    for (auto& item : out) std::sort(item.second.begin(), item.second.end());
    return out;
  };
  const auto mine = by_unit(into.pricing), theirs = by_unit(from.pricing);
  std::set<std::string> keys;
  for (const auto& item : mine) keys.insert(item.first);
  for (const auto& item : theirs) keys.insert(item.first);
  Prices merged;
  std::vector<std::string> disagree;  // by the member name a copy lists the unit under
  for (const auto& key : keys) {
    const auto a = mine.find(key), b = theirs.find(key);
    const auto& named = a != mine.end() ? a->second : b->second;
    bool same = a != mine.end() && b != theirs.end() && a->second.size() == b->second.size();
    Prices dearer;
    for (std::size_t i = 0; same && i < a->second.size(); ++i) {
      const auto& mine_price = a->second[i];
      const auto& their_price = b->second[i];
      if (mine_price.first != their_price.first) same = false;
      else if (mine_price.second == their_price.second) dearer.push_back(mine_price);
      else if (const auto mine_rate = price_per_million(mine_price.second), their_rate = price_per_million(their_price.second); !mine_rate || !their_rate)
        same = false;
      else dearer.push_back(*mine_rate >= *their_rate ? mine_price : their_price);
    }
    if (same) merged.insert(merged.end(), dearer.begin(), dearer.end());
    else disagree.emplace_back(unbanded_unit(named.front().first));
  }
  merge_entry(into, from);
  std::sort(merged.begin(), merged.end());
  into.pricing = std::move(merged);
  for (const auto& unit : disagree) leave_out(into, unit, withheld);
}
// OpenRouter's `pricing` of one model: its text prices, then the bands and left-out units of its `overrides`.
void read_pricing(Value pricing, CatalogueEntry& e, ListingPage& page) {
  for (auto member : pricing.members()) {
    if (member.key == "overrides") continue;
    // A member that is neither text nor a number is no price.
    if (!member.value.is_string() && !member.value.is_number()) continue;
    if (e.pricing.size() < max_prices && listed_unit(member.key) && member.value.is_string() && decimal(member.value.as_string()))
      e.pricing.emplace_back(std::string(member.key), std::string(member.value.as_string()));
    else ++page.skipped_prices;
  }
  if (const auto overrides = pricing.get("overrides"); present(overrides)) read_overrides(overrides, e, page);
  std::sort(e.pricing.begin(), e.pricing.end());
}
} // namespace

ListingResult parse_listing(Listing listing, std::string_view body, std::string_view origin) {
  try {
    if (!descriptor::valid_origin(origin) || origin.ends_with('/')) fail("", "canonical origin for the listed entries");
    auto parsed = json::parse(body, {32u * 1024u * 1024u, 64});
    if (std::holds_alternative<json::ParseError>(parsed)) fail("", "a JSON listing");
    const auto root = std::get<json::Document>(parsed).root();
    if (!root.is_object()) fail("", "a listing object");
    ListingPage page;
    auto entry_for = [&](Value id) -> CatalogueEntry* {
      std::string_view name = id.is_string() ? id.as_string() : std::string_view{};
      if (listing == Listing::Gemini) {
        if (!name.starts_with("models/")) name = {};
        else name.remove_prefix(7);
      }
      if (!model_name(name)) { ++page.skipped; return nullptr; }
      auto& entry = page.entries.emplace_back();
      entry.id = std::string(name);
      entry.source = std::string(listing_name(listing));
      entry.origins = {std::string(origin)};
      return &entry;
    };
    if (listing == Listing::Gemini) {
      const auto models = root.get("models");
      if (models.valid() && !models.is_array()) fail("/models", "a model array");
      if (models.is_array())
        for (auto model : models.elements()) {
          auto* e = entry_for(model.get("name"));
          if (!e) continue;
          e->name = listed_text(model.get("displayName"), display);
          e->input_limit = listed_count(model.get("inputTokenLimit"));
          e->output_limit = listed_count(model.get("outputTokenLimit"));
          e->methods = listed_tokens(model.get("supportedGenerationMethods"));
          if (contains(e->methods, "generateContent")) e->families = {"google.generate", "google.interactions"};
        }
      // Gemini leaves the token out on the last page. One it sends that cannot be sent back would
      // end the listing early with a partial catalogue, so it fails the listing.
      if (const auto next = root.get("nextPageToken"); present(next)) {
        if (!next.is_string() || !page_token(next.as_string())) fail("/nextPageToken", "the token that continues the listing");
        page.next = std::string(next.as_string());
      }
    } else {
    const auto data = root.get("data");
    if (!data.is_array()) fail("/data", "a model array");
    for (auto model : data.elements()) {
      auto* e = entry_for(model.get("id"));
      if (!e) continue;
      switch (listing) {
        case Listing::OpenRouter: {
          e->name = listed_text(model.get("name"), display);
          if (const auto slug = model.get("canonical_slug"); slug.is_string() && slug.as_string() != e->id && model_name(slug.as_string()))
            e->served_as = {std::string(slug.as_string())};
          e->alias_of = listed_text(model.get("alias_target").get("slug"), model_name);
          e->context_window = listed_count(model.get("context_length"));
          e->output_limit = listed_count(model.get("top_provider").get("max_completion_tokens"));
          e->input_modalities = listed_tokens(model.get("architecture").get("input_modalities"));
          e->output_modalities = listed_tokens(model.get("architecture").get("output_modalities"));
          e->parameters = listed_tokens(model.get("supported_parameters"));
          e->retires_at = listed_text(model.get("expiration_date"), date);
          if (const auto pricing = model.get("pricing"); pricing.is_object()) read_pricing(pricing, *e, page);
          // OpenRouter serves its text models on all three of its API families.
          if (contains(e->output_modalities, "text")) e->families = {"anthropic.messages", "openai.chat", "openai.responses"};
          break;
        }
        case Listing::Anthropic: {
          e->name = listed_text(model.get("display_name"), display);
          e->input_limit = listed_count(model.get("max_input_tokens"));
          e->output_limit = listed_count(model.get("max_tokens"));
          e->lifecycle = listed_text(model.get("lifecycle"), token);
          e->retires_at = listed_text(model.get("retires_at"), date);
          e->families = {"anthropic.messages"};
          const auto capabilities = model.get("capabilities");
          e->input_modalities = {"text"};
          if (supported(capabilities, "image_input")) e->input_modalities.push_back("image");
          if (supported(capabilities, "pdf_input")) e->input_modalities.push_back("document");
          e->output_modalities = {"text"};
          break;
        }
        case Listing::OpenAI:
          // The listing has ids and shutdown dates only.
          e->retires_at = listed_text(model.get("shutdown_date"), date);
          break;
        case Listing::Gemini: break;
      }
    }
    }
    // A listing that names one id twice gives one entry, merged in listing order (merge_listed_copy).
    std::vector<CatalogueEntry> unique;
    std::map<std::string, std::size_t> seen;
    for (auto& entry : page.entries) {
      const auto [found, inserted] = seen.try_emplace(entry.id, unique.size());
      if (inserted) unique.push_back(std::move(entry));
      else merge_listed_copy(unique[found->second], entry, page.withheld);
    }
    page.entries = std::move(unique);
    // An id listed twice keeps out every unit an override of either listing left out.
    const auto withheld = page.withheld;
    for (const auto& unit : withheld) leave_out(page.entries[seen.at(unit.model)], unit.unit, page.withheld);
    std::sort(page.withheld.begin(), page.withheld.end(),
              [](const WithheldUnit& a, const WithheldUnit& b) { return std::tie(a.model, a.unit) < std::tie(b.model, b.unit); });
    page.withheld.erase(std::unique(page.withheld.begin(), page.withheld.end()), page.withheld.end());
    if (listing == Listing::Anthropic && root.get("has_more").is_bool() && root.get("has_more").as_bool()) {
      const auto last = root.get("last_id");
      if (!last.is_string() || !page_token(last.as_string())) fail("/last_id", "the id that continues the listing");
      page.next = std::string(last.as_string());
    }
    return page;
  } catch (ConfigError& error) { return std::move(error); }
}

} // namespace sp::descriptor
