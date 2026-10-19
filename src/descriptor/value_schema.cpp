#include "descriptor/value_schema.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace sp::descriptor {
namespace {
constexpr std::size_t max_depth = 8, max_members = 256, max_string = 1u << 20;

struct Failed { ConfigError error; };
[[noreturn]] void fail(const std::string& pointer, std::string expected) { throw Failed{ConfigError{pointer, std::move(expected), 0, "value schema rejected"}}; }
std::string at(const std::string& pointer, std::string_view key) {
  std::string out = pointer + "/";
  for (const char c : key) out += c == '~' ? std::string("~0") : c == '/' ? std::string("~1") : std::string(1, c);
  return out;
}
bool name_token(std::string_view name) {
  return !name.empty() && name.size() <= 128 && std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
  });
}
std::uint64_t count(json::Value value, const std::string& pointer, std::uint64_t limit) {
  if (!value.is_uint() || !value.as_uint() || value.as_uint() > limit) fail(pointer, "positive integer up to " + std::to_string(limit));
  return value.as_uint();
}
void closed(json::Value value, std::initializer_list<std::string_view> keys, const std::string& pointer) {
  for (auto member : value.members())
    if (std::find(keys.begin(), keys.end(), member.key) == keys.end()) fail(at(pointer, member.key), "declared schema member");
}
ValueSchema parse(json::Value value, const std::string& pointer, std::size_t depth) {
  if (depth > max_depth) fail(pointer, "schema nested at most 8 deep");
  if (!value.is_object()) fail(pointer, "schema object");
  ValueSchema out;
  if (auto one_of = value.get("one_of"); one_of.valid()) {
    closed(value, {"one_of"}, pointer);
    if (!one_of.is_array() || one_of.size() < 2 || one_of.size() > 16) fail(at(pointer, "one_of"), "2 to 16 alternatives");
    out.kind = ValueSchema::Kind::OneOf;
    std::size_t i = 0;
    for (auto alternative : one_of.elements()) out.children.push_back(parse(alternative, at(at(pointer, "one_of"), std::to_string(i++)), depth + 1));
    return out;
  }
  const auto type = value.get("type");
  if (!type.is_string()) fail(at(pointer, "type"), "schema type");
  const auto name = type.as_string();
  auto bound = [&](std::string_view key) -> std::optional<double> {
    const auto v = value.get(key);
    if (!v.valid()) return std::nullopt;
    if (!v.is_number() || !std::isfinite(v.as_double())) fail(at(pointer, key), "finite number");
    if (name == "integer" && !v.is_int() && !v.is_uint()) fail(at(pointer, key), "integer");
    return v.as_double();
  };
  if (name == "boolean" || name == "null") {
    closed(value, {"type"}, pointer);
    out.kind = name == "boolean" ? ValueSchema::Kind::Boolean : ValueSchema::Kind::Null;
  } else if (name == "integer" || name == "number") {
    closed(value, {"type", "minimum", "maximum"}, pointer);
    out.kind = name == "integer" ? ValueSchema::Kind::Integer : ValueSchema::Kind::Number;
    out.minimum = bound("minimum"); out.maximum = bound("maximum");
    if (out.minimum && out.maximum && *out.minimum > *out.maximum) fail(pointer, "minimum not above maximum");
  } else if (name == "string") {
    closed(value, {"type", "max_length", "token"}, pointer);
    out.kind = ValueSchema::Kind::String;
    out.max_length = count(value.get("max_length"), at(pointer, "max_length"), max_string);
    if (const auto token = value.get("token"); token.valid()) {
      if (!token.is_bool()) fail(at(pointer, "token"), "boolean");
      out.token = token.as_bool();
    }
  } else if (name == "enum") {
    closed(value, {"type", "values"}, pointer);
    out.kind = ValueSchema::Kind::Enum;
    const auto values = value.get("values");
    if (!values.is_array() || !values.size() || values.size() > max_members) fail(at(pointer, "values"), "1 to 256 strings");
    std::set<std::string_view> seen;
    for (auto item : values.elements()) {
      if (!item.is_string() || item.as_string().empty() || item.as_string().size() > 256 || !seen.insert(item.as_string()).second)
        fail(at(pointer, "values"), "unique nonempty strings");
      out.values.emplace_back(item.as_string());
    }
  } else if (name == "array") {
    closed(value, {"type", "items", "max_items"}, pointer);
    out.kind = ValueSchema::Kind::Array;
    out.max_items = count(value.get("max_items"), at(pointer, "max_items"), 4096);
    out.children.push_back(parse(value.get("items"), at(pointer, "items"), depth + 1));
  } else if (name == "object") {
    closed(value, {"type", "properties", "required"}, pointer);
    out.kind = ValueSchema::Kind::Object;
    const auto properties = value.get("properties");
    if (!properties.is_object() || properties.size() > max_members) fail(at(pointer, "properties"), "object of up to 256 properties");
    std::vector<std::pair<std::string, ValueSchema>> listed;
    for (auto member : properties.members()) {
      if (!name_token(member.key)) fail(at(at(pointer, "properties"), member.key), "property name token");
      listed.emplace_back(std::string(member.key), parse(member.value, at(at(pointer, "properties"), member.key), depth + 1));
    }
    std::sort(listed.begin(), listed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [property, schema] : listed) { out.names.push_back(std::move(property)); out.children.push_back(std::move(schema)); }
    if (const auto required = value.get("required"); required.valid()) {
      if (!required.is_array()) fail(at(pointer, "required"), "array of property names");
      std::set<std::string_view> seen;
      for (auto item : required.elements()) {
        if (!item.is_string() || !std::binary_search(out.names.begin(), out.names.end(), item.as_string()) || !seen.insert(item.as_string()).second)
          fail(at(pointer, "required"), "unique listed property names");
        out.required.emplace_back(item.as_string());
      }
    }
  } else if (name == "map") {
    closed(value, {"type", "values", "max_items"}, pointer);
    out.kind = ValueSchema::Kind::Map;
    out.max_items = count(value.get("max_items"), at(pointer, "max_items"), 4096);
    out.children.push_back(parse(value.get("values"), at(pointer, "values"), depth + 1));
  } else fail(at(pointer, "type"), "boolean, null, integer, number, string, enum, array, object or map");
  return out;
}
bool token_text(std::string_view text) {
  return std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' || c == '/' || c == '-';
  });
}
std::optional<std::string> check(const ValueSchema& schema, json::Value value, const std::string& pointer, std::size_t depth) {
  using Kind = ValueSchema::Kind;
  if (depth > max_depth + 1) return pointer + ": value nested too deep";
  switch (schema.kind) {
    case Kind::Boolean: return value.is_bool() ? std::nullopt : std::optional<std::string>(pointer + ": boolean");
    case Kind::Null: return value.is_null() ? std::nullopt : std::optional<std::string>(pointer + ": null");
    case Kind::Integer:
    case Kind::Number: {
      const bool integer = value.is_int() || value.is_uint();
      if (!value.is_number() || (schema.kind == Kind::Integer && !integer) || !std::isfinite(value.as_double()))
        return pointer + (schema.kind == Kind::Integer ? ": integer" : ": finite number");
      const double n = value.is_uint() ? static_cast<double>(value.as_uint()) : value.is_int() ? static_cast<double>(value.as_int()) : value.as_double();
      if ((schema.minimum && n < *schema.minimum) || (schema.maximum && n > *schema.maximum)) return pointer + ": number within the declared range";
      return std::nullopt;
    }
    case Kind::String: {
      if (!value.is_string() || value.as_string().size() > schema.max_length) return pointer + ": string up to " + std::to_string(schema.max_length) + " bytes";
      const auto text = value.as_string();
      if (schema.token && (text.empty() || !token_text(text))) return pointer + ": token";
      if (std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 && c != '\t' && c != '\n' && c != '\r'; })) return pointer + ": text without control characters";
      return std::nullopt;
    }
    case Kind::Enum:
      if (value.is_string() && std::find(schema.values.begin(), schema.values.end(), value.as_string()) != schema.values.end()) return std::nullopt;
      return pointer + ": one of the declared values";
    case Kind::Array: {
      if (!value.is_array() || value.size() > schema.max_items) return pointer + ": array of up to " + std::to_string(schema.max_items) + " items";
      std::size_t i = 0;
      for (auto item : value.elements()) if (auto error = check(schema.children[0], item, pointer + "/" + std::to_string(i++), depth + 1)) return error;
      return std::nullopt;
    }
    case Kind::Object: {
      if (!value.is_object()) return pointer + ": object";
      std::set<std::string_view> seen;
      for (auto member : value.members()) {
        const auto found = std::lower_bound(schema.names.begin(), schema.names.end(), member.key);
        if (found == schema.names.end() || *found != member.key) return at(pointer, member.key) + ": declared property";
        if (!seen.insert(member.key).second) return at(pointer, member.key) + ": unique property";
        if (auto error = check(schema.children[static_cast<std::size_t>(found - schema.names.begin())], member.value, at(pointer, member.key), depth + 1)) return error;
      }
      for (const auto& name : schema.required) if (!seen.contains(name)) return at(pointer, name) + ": required property";
      return std::nullopt;
    }
    case Kind::Map: {
      if (!value.is_object() || value.size() > schema.max_items) return pointer + ": object of up to " + std::to_string(schema.max_items) + " members";
      std::set<std::string_view> seen;
      for (auto member : value.members()) {
        if (!name_token(member.key)) return at(pointer, member.key) + ": member name token";
        if (!seen.insert(member.key).second) return at(pointer, member.key) + ": unique member";
        if (auto error = check(schema.children[0], member.value, at(pointer, member.key), depth + 1)) return error;
      }
      return std::nullopt;
    }
    case Kind::OneOf:
      for (const auto& alternative : schema.children) if (!check(alternative, value, pointer, depth + 1)) return std::nullopt;
      return pointer + ": one of the declared alternatives";
  }
  return pointer + ": declared value";
}
} // namespace

std::variant<ValueSchema, ConfigError> parse_value_schema(json::Value value, const std::string& pointer) {
  try { return parse(value, pointer, 0); } catch (Failed& failed) { return std::move(failed.error); }
}
std::optional<std::string> check_value(const ValueSchema& schema, json::Value value) { return check(schema, value, "", 0); }

} // namespace sp::descriptor
