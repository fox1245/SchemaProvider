#pragma once

#include "descriptor/policy.h"
#include "json/json.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <string>
#include <vector>

namespace sp {
struct OpenRouterRouting {
  std::optional<bool> zdr, allow_fallbacks, require_parameters;
  std::vector<std::string> only, order, ignore;
  std::optional<std::string> data_collection;
};
struct ResponseFormat {
  enum class Kind { JsonObject, JsonSchema };
  Kind kind = Kind::JsonObject;
  std::string name, description;
  std::shared_ptr<const json::Document> schema;
  std::optional<bool> strict;
};

namespace request_controls {
inline bool bounded_text(std::string_view text, std::size_t maximum, bool empty = false) {
  if ((!empty && text.empty()) || text.size() > maximum) return false;
  for (unsigned char c : text) if (c < 32 || c == 127) return false;
  return true;
}
inline bool identifier(std::string_view name) {
  if (name.empty() || name.size() > 64) return false;
  for (unsigned char c : name)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  return true;
}
inline std::optional<std::string> validate_routing(
    const descriptor::ValidatedDescriptor& descriptor, const OpenRouterRouting& routing) {
  if (!descriptor::contains(descriptor.family_policy().openrouter_origins, descriptor.base_url()))
    return "OpenRouter routing requires a declared OpenRouter origin";
  if (routing.data_collection && *routing.data_collection != "allow" && *routing.data_collection != "deny")
    return "data_collection must be allow or deny";
  auto valid_list = [](const std::vector<std::string>& values) {
    if (values.size() > 64) return false;
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (!bounded_text(values[i], 128)) return false;
      for (std::size_t j = 0; j < i; ++j) if (values[i] == values[j]) return false;
    }
    return true;
  };
  if (!valid_list(routing.only) || !valid_list(routing.order) || !valid_list(routing.ignore))
    return "routing lists require bounded unique nonempty provider names";
  for (const auto& name : routing.ignore)
    if (descriptor::contains(routing.only, name) || descriptor::contains(routing.order, name))
      return "ignored providers cannot be selected or ordered";
  if (!routing.only.empty()) for (const auto& name : routing.order)
    if (!descriptor::contains(routing.only, name)) return "ordered providers must belong to only";
  return {};
}

// Schema structure is compiled, while schema content remains immutable caller data.
// Walk budgets apply to all values, including annotations and enum/const payloads.
inline bool bounded_schema_value(json::Value value, std::size_t depth,
                                 std::size_t maximum_depth, std::size_t& members) {
  if (!value.valid() || depth > maximum_depth) return false;
  if (!value.is_object() && !value.is_array()) return true;
  if (value.size() > members) return false;
  members -= value.size();
  if (value.is_object()) {
    for (auto member : value.members())
      if (!bounded_text(member.key, 128, true) ||
          !bounded_schema_value(member.value, depth + 1, maximum_depth, members)) return false;
  } else for (auto element : value.elements())
    if (!bounded_schema_value(element, depth + 1, maximum_depth, members)) return false;
  return true;
}
inline bool schema_type(json::Value type) {
  if (!type.is_string()) return false;
  const auto name = type.as_string();
  return name == "object" || name == "array" || name == "string" || name == "number" ||
         name == "integer" || name == "boolean" || name == "null";
}
inline bool schema_node(json::Value node) {
  if (node.is_bool()) return true;
  if (!node.is_object()) return false;
  for (auto member : node.members()) {
    const auto key = member.key;
    const auto value = member.value;
    if (key == "type") {
      if (value.is_array()) {
        if (!value.size() || value.size() > 7) return false;
        for (std::size_t i = 0; i < value.size(); ++i) {
          if (!schema_type(value.at(i))) return false;
          for (std::size_t j = 0; j < i; ++j) if (json::equal(value.at(i), value.at(j))) return false;
        }
      } else if (!schema_type(value)) return false;
    } else if (key == "properties" || key == "patternProperties" || key == "$defs" || key == "definitions" || key == "dependentSchemas") {
      if (!value.is_object()) return false;
      for (auto child : value.members()) if (!schema_node(child.value)) return false;
    } else if (key == "required" || key == "dependentRequired") {
      auto names = [](json::Value list) {
        if (!list.is_array()) return false;
        std::set<std::string_view> seen;
        for (auto name : list.elements())
          if (!name.is_string() || !bounded_text(name.as_string(), 128, true) ||
              !seen.insert(name.as_string()).second) return false;
        return true;
      };
      if (key == "required") { if (!names(value)) return false; }
      else {
        if (!value.is_object()) return false;
        for (auto child : value.members()) if (!names(child.value)) return false;
      }
    } else if (key == "items" || key == "additionalProperties" || key == "unevaluatedProperties" ||
               key == "unevaluatedItems" || key == "contains" || key == "propertyNames" ||
               key == "not" || key == "if" || key == "then" || key == "else") {
      if (!schema_node(value)) return false;
    } else if (key == "anyOf" || key == "allOf" || key == "oneOf" || key == "prefixItems") {
      if (!value.is_array() || !value.size()) return false;
      for (auto child : value.elements()) if (!schema_node(child)) return false;
    } else if (key == "enum") {
      if (!value.is_array() || !value.size()) return false;
    } else if (key == "minLength" || key == "maxLength" || key == "minItems" || key == "maxItems" ||
               key == "minProperties" || key == "maxProperties" || key == "minContains" || key == "maxContains") {
      if (!value.is_uint()) return false;
    } else if (key == "minimum" || key == "maximum" || key == "exclusiveMinimum" || key == "exclusiveMaximum" || key == "multipleOf") {
      if (!value.is_number() || !std::isfinite(value.as_double()) ||
          (key == "multipleOf" && value.as_double() <= 0)) return false;
    } else if (key == "uniqueItems" || key == "readOnly" || key == "writeOnly" || key == "deprecated") {
      if (!value.is_bool()) return false;
    } else if (key == "$ref" || key == "$schema" || key == "$id" || key == "$anchor" || key == "$comment" ||
               key == "title" || key == "description" || key == "format" || key == "pattern" ||
               key == "contentEncoding" || key == "contentMediaType") {
      if (!value.is_string()) return false;
    } else if (key == "examples") {
      if (!value.is_array()) return false;
    } else if (key != "const" && key != "default") return false;
  }
  for (const auto& bounds : {std::pair{"minLength", "maxLength"}, std::pair{"minItems", "maxItems"},
                             std::pair{"minProperties", "maxProperties"}, std::pair{"minContains", "maxContains"}}) {
    const auto lower = node.get(bounds.first), upper = node.get(bounds.second);
    if (lower.valid() && upper.valid() && lower.as_uint() > upper.as_uint()) return false;
  }
  return true;
}
inline std::optional<std::string> validate_response_format(
    const ResponseFormat& format, const descriptor::CodecResources& resources) {
  if (format.kind == ResponseFormat::Kind::JsonObject) {
    if (!format.name.empty() || !format.description.empty() || format.schema || format.strict)
      return "json_object cannot carry schema controls";
    return {};
  }
  if (format.kind != ResponseFormat::Kind::JsonSchema) return "unknown response format kind";
  if (!identifier(format.name) || format.description.size() > resources.request_bytes ||
      !format.schema || !format.schema->root().is_object())
    return "json_schema requires a bounded identifier and object schema";
  auto members = resources.native_members;
  const auto depth = std::min(resources.json_depth, resources.native_depth);
  if (!bounded_schema_value(format.schema->root(), 4, depth, members) || !schema_node(format.schema->root()))
    return "response schema exceeds limits or contains unsupported keyword types";
  return {};
}
inline void write_routing(json::BoundedWriter& body, const OpenRouterRouting& routing) {
  body.raw("{");
  bool comma = false;
  auto boolean = [&](std::string_view key, const std::optional<bool>& value) {
    if (!value) return;
    if (comma) body.raw(",");
    comma = true;
    body.quoted(key).raw(*value ? ":true" : ":false");
  };
  boolean("zdr", routing.zdr);
  boolean("allow_fallbacks", routing.allow_fallbacks);
  boolean("require_parameters", routing.require_parameters);
  auto list = [&](std::string_view key, const std::vector<std::string>& values) {
    if (values.empty()) return;
    if (comma) body.raw(",");
    comma = true;
    body.quoted(key).raw(":[");
    bool item_comma = false;
    for (const auto& value : values) {
      if (item_comma) body.raw(",");
      item_comma = true;
      body.quoted(value);
    }
    body.raw("]");
  };
  list("only", routing.only);
  list("order", routing.order);
  list("ignore", routing.ignore);
  if (routing.data_collection) {
    if (comma) body.raw(",");
    body.raw("\"data_collection\":").quoted(*routing.data_collection);
  }
  body.raw("}");
}
inline void write_response_format(json::BoundedWriter& body, const ResponseFormat& format, bool responses) {
  if (format.kind == ResponseFormat::Kind::JsonObject) { body.raw("{\"type\":\"json_object\"}"); return; }
  body.raw("{\"type\":\"json_schema\",");
  if (!responses) body.raw("\"json_schema\":{");
  body.raw("\"name\":").quoted(format.name);
  if (!format.description.empty()) body.raw(",\"description\":").quoted(format.description);
  body.raw(",\"schema\":").value(format.schema->root(), 4);
  if (format.strict) body.raw(*format.strict ? ",\"strict\":true" : ",\"strict\":false");
  if (!responses) body.raw("}");
  body.raw("}");
}
} // namespace request_controls
} // namespace sp
