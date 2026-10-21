#pragma once

#include "descriptor/owned_keys.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <string>
#include <vector>

namespace sp {
// OpenRouter `provider.max_price`: the highest endpoint price the router may choose for this
// request. When no endpoint is priced at or below it, the request fails rather than routing to a
// dearer one. Amounts are integer nanoUSD (1e-9 USD), the unit of UsdAmount, so a ceiling is
// exact: prompt and completion per million tokens, image per image, audio per audio unit,
// request per request. Each is sent as the exact decimal USD JSON number, with no exponent or
// binary rounding (75'000'000 is 0.075). At least one amount is required; none may exceed
// 2^53 nanoUSD.
struct OpenRouterMaxPrice {
  std::optional<std::uint64_t> prompt_nano_usd, completion_nano_usd, image_nano_usd, audio_nano_usd, request_nano_usd;
};
// The largest amount a `max_price` field may hold, in nanoUSD.
inline constexpr std::uint64_t max_price_limit_nano_usd = std::uint64_t{1} << 53;
struct OpenRouterRouting {
  std::optional<bool> zdr, allow_fallbacks, require_parameters;
  std::vector<std::string> only, order, ignore;
  std::optional<std::string> data_collection;
  std::optional<OpenRouterMaxPrice> max_price;
};
struct ResponseFormat {
  enum class Kind { JsonObject, JsonSchema };
  Kind kind = Kind::JsonObject;
  std::string name, description;
  std::shared_ptr<const json::Document> schema;
  std::optional<bool> strict;
};
// An OpenRouter plugin (web, file-parser, response-healing, context-compression, ...): its id and the plugin's
// other members, sent as given as {"id": ..., ...options}. OpenRouter's catalogue of plugins is not compiled in.
struct OpenRouterPlugin { std::string id; std::shared_ptr<const json::Document> options; };
// OpenRouter image_config, for image output on Chat and Responses. Every present field is sent as given.
struct OpenRouterImageConfig {
  std::optional<std::string> aspect_ratio, image_size, size, quality, background, output_format, moderation;
  std::optional<uint64_t> output_compression;
};

namespace request_controls {
// Shapes of values the providers name and extend: an aspect ratio W:H, and a size or resolution token such as 2K or
// 1080p. The service decides which values a model accepts.
inline bool aspect_ratio_syntax(std::string_view value) noexcept {
  const auto colon = value.find(':');
  const auto digits = [](std::string_view part) {
    return !part.empty() && part.size() <= 4 && std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; });
  };
  return colon != std::string_view::npos && digits(value.substr(0, colon)) && digits(value.substr(colon + 1));
}
inline bool size_token(std::string_view value) noexcept {
  return !value.empty() && value.size() <= 16 && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
  });
}
// A BCP 47 language tag's characters, such as en-US or cmn-CN.
inline bool language_tag(std::string_view value) noexcept {
  return !value.empty() && value.size() <= 35 && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
  });
}
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
  if (routing.max_price) {
    const auto& price = *routing.max_price;
    bool any = false;
    for (const auto* amount : {&price.prompt_nano_usd, &price.completion_nano_usd, &price.image_nano_usd,
                               &price.audio_nano_usd, &price.request_nano_usd}) {
      if (!*amount) continue;
      any = true;
      if (**amount > max_price_limit_nano_usd) return "max_price amounts cannot exceed 2^53 nanoUSD";
    }
    if (!any) return "max_price requires at least one amount";
  }
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
    }
    // Any other keyword (const, default, and the ones JSON Schema or a provider adds) is sent as given.
  }
  for (const auto& bounds : {std::pair{"minLength", "maxLength"}, std::pair{"minItems", "maxItems"},
                             std::pair{"minProperties", "maxProperties"}, std::pair{"minContains", "maxContains"}}) {
    const auto lower = node.get(bounds.first), upper = node.get(bounds.second);
    if (lower.valid() && upper.valid() && lower.as_uint() > upper.as_uint()) return false;
  }
  return true;
}
// Structural part of OpenAI's strict function-schema rules. The root must be an
// object schema, and every object schema reached through properties, items,
// prefixItems, anyOf, $defs or definitions must set "additionalProperties":
// false and list each of its properties in "required". Keywords are deliberately
// not whitelisted: the supported set varies by model and the service still
// judges them. The result is the first violation in document order, as an
// RFC 6901 pointer into the schema plus a reason; none means compatible.
struct StrictSchemaViolation { std::string pointer; std::string_view reason; };
inline void append_pointer_token(std::string& pointer, std::string_view token) {
  pointer += '/';
  for (const char c : token) {
    if (c == '~') pointer += "~0";
    else if (c == '/') pointer += "~1";
    else pointer += c;
  }
}
inline bool strict_object_schema(json::Value node) {
  const auto type = node.get("type");
  if (type.is_string()) return type.as_string() == "object";
  if (type.is_array()) {
    for (auto name : type.elements()) if (name.is_string() && name.as_string() == "object") return true;
    return false;
  }
  return node.get("properties").valid();
}
inline std::optional<StrictSchemaViolation> strict_schema_node(json::Value node, std::string& pointer) {
  if (!node.is_object()) return {};
  if (strict_object_schema(node)) {
    const auto additional = node.get("additionalProperties");
    if (!additional.is_bool() || additional.as_bool())
      return StrictSchemaViolation{pointer, "object schema needs \"additionalProperties\": false"};
    if (const auto properties = node.get("properties"); properties.is_object()) {
      std::set<std::string_view> required;
      if (const auto names = node.get("required"); names.is_array())
        for (auto name : names.elements()) if (name.is_string()) required.insert(name.as_string());
      for (auto property : properties.members())
        if (!required.count(property.key)) {
          append_pointer_token(pointer, "properties");
          append_pointer_token(pointer, property.key);
          return StrictSchemaViolation{pointer, "property is not listed in \"required\""};
        }
    }
  }
  for (auto member : node.members()) {
    const auto key = member.key;
    const auto child = member.value;
    const auto size = pointer.size();
    if (key == "properties" || key == "$defs" || key == "definitions") {
      if (!child.is_object()) continue;
      for (auto entry : child.members()) {
        append_pointer_token(pointer, key);
        append_pointer_token(pointer, entry.key);
        if (auto violation = strict_schema_node(entry.value, pointer)) return violation;
        pointer.resize(size);
      }
    } else if (key == "items" || key == "prefixItems" || key == "anyOf") {
      if (child.is_array()) {
        std::size_t index = 0;
        for (auto element : child.elements()) {
          append_pointer_token(pointer, key);
          append_pointer_token(pointer, std::to_string(index++));
          if (auto violation = strict_schema_node(element, pointer)) return violation;
          pointer.resize(size);
        }
      } else if (key == "items") {
        append_pointer_token(pointer, key);
        if (auto violation = strict_schema_node(child, pointer)) return violation;
        pointer.resize(size);
      }
    }
  }
  return {};
}
inline std::optional<StrictSchemaViolation> strict_schema_violation(json::Value schema) {
  if (!schema.is_object() || !schema.get("type").is_string() || schema.get("type").as_string() != "object")
    return StrictSchemaViolation{"", "root must be an object schema"};
  std::string pointer;
  return strict_schema_node(schema, pointer);
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
    comma = true;
    body.raw("\"data_collection\":").quoted(*routing.data_collection);
  }
  if (routing.max_price) {
    if (comma) body.raw(",");
    body.raw("\"max_price\":{");
    bool price_comma = false;
    // Exact decimal USD: whole dollars, then at most nine fractional digits without trailing
    // zeros. No exponent and no binary64 rounding reach the wire.
    auto price = [&](std::string_view key, const std::optional<std::uint64_t>& nano_usd) {
      if (!nano_usd) return;
      if (price_comma) body.raw(",");
      price_comma = true;
      body.quoted(key).raw(":").raw(std::to_string(*nano_usd / 1000000000U));
      if (auto fraction = *nano_usd % 1000000000U) {
        char digits[10] = {'.'};
        for (std::size_t i = 9; i > 0; --i, fraction /= 10) digits[i] = static_cast<char>('0' + fraction % 10);
        std::size_t end = sizeof digits;
        while (digits[end - 1] == '0') --end;
        body.raw(std::string_view(digits, end));
      }
    };
    price("prompt", routing.max_price->prompt_nano_usd);
    price("completion", routing.max_price->completion_nano_usd);
    price("image", routing.max_price->image_nano_usd);
    price("audio", routing.max_price->audio_nano_usd);
    price("request", routing.max_price->request_nano_usd);
    body.raw("}");
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
// A short configuration token: letters, digits and ":._-/".
inline bool config_token(std::string_view text, std::size_t maximum = 64) noexcept {
  if (text.empty() || text.size() > maximum) return false;
  for (const unsigned char c : text)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ':' || c == '.' ||
          c == '_' || c == '-' || c == '/')) return false;
  return true;
}
inline void write_plugins(json::BoundedWriter& body, const std::vector<OpenRouterPlugin>& plugins) {
  body.raw("[");
  for (std::size_t i = 0; i < plugins.size(); ++i) {
    if (i) body.raw(",");
    body.raw("{\"id\":").quoted(plugins[i].id);
    if (plugins[i].options)
      for (auto member : plugins[i].options->root().members()) body.raw(",").quoted(member.key).raw(":").value(member.value, 3);
    body.raw("}");
  }
  body.raw("]");
}
inline std::optional<std::string> validate_image_config(const OpenRouterImageConfig& config) {
  for (const auto* field : {&config.aspect_ratio, &config.image_size, &config.size, &config.quality, &config.background,
                            &config.output_format, &config.moderation})
    if (*field && !config_token(**field)) return "image_config values must be short tokens";
  if (config.output_compression && *config.output_compression > 100) return "image_config output_compression is 0 to 100";
  return std::nullopt;
}
inline void write_image_config(json::BoundedWriter& body, const OpenRouterImageConfig& config) {
  body.raw("{");
  bool comma = false;
  auto text_field = [&](std::string_view key, const std::optional<std::string>& value) {
    if (!value) return;
    if (comma) body.raw(",");
    comma = true;
    body.quoted(key).raw(":").quoted(*value);
  };
  text_field("aspect_ratio", config.aspect_ratio);
  text_field("image_size", config.image_size);
  text_field("size", config.size);
  text_field("quality", config.quality);
  text_field("background", config.background);
  text_field("output_format", config.output_format);
  if (config.output_compression) {
    if (comma) body.raw(",");
    comma = true;
    body.raw("\"output_compression\":").raw(std::to_string(*config.output_compression));
  }
  text_field("moderation", config.moderation);
  body.raw("}");
}
// Policy-admitted top-level members (#32). Each one must be admitted by the family policy for the descriptor's
// origin, never name a member the encoder owns, and conform to its declared schema. Nothing else is sent.
struct ExtensionRefusal { bool admitted; std::string message; };
// A value the policy admits but that does not conform, and one the policy does not admit at all.
inline ExtensionRefusal invalid(std::string message) { return {true, std::move(message)}; }
inline ExtensionRefusal unsupported(std::string message) { return {false, std::move(message)}; }
inline std::optional<ExtensionRefusal> validate_extensions(const descriptor::ValidatedDescriptor& descriptor,
                                                           const std::shared_ptr<const json::Document>& extensions) {
  if (!extensions) return std::nullopt;
  const auto root = extensions->root();
  if (!root.is_object()) return invalid("extensions must be an object");
  const auto& family = descriptor.family_policy();
  for (auto member : root.members()) {
    if (descriptor::request_key_owned(family.family, member.key)) return invalid("an extension cannot name a member the SDK writes");
    const auto fact = std::find_if(family.extensions.begin(), family.extensions.end(), [&](const descriptor::ExtensionFact& f) {
      return f.key == member.key && descriptor::contains(f.origins, descriptor.base_url());
    });
    if (fact == family.extensions.end()) return unsupported("an extension is not admitted by the policy for this origin");
    if (descriptor::check_value(fact->value, member.value)) return invalid("an extension value does not match its declared schema");
  }
  return std::nullopt;
}
inline void write_extensions(json::BoundedWriter& body, const std::shared_ptr<const json::Document>& extensions) {
  if (!extensions) return;
  for (auto member : extensions->root().members()) body.raw(",").quoted(member.key).raw(":").value(member.value, 1);
}
// False when an extension the policy marks replay-ineligible is present: that turn's answer is not continuable natively.
inline bool extensions_replay_eligible(const descriptor::ValidatedDescriptor& descriptor, const std::shared_ptr<const json::Document>& extensions) {
  if (!extensions || !extensions->root().is_object()) return true;
  const auto& family = descriptor.family_policy();
  for (auto member : extensions->root().members())
    for (const auto& fact : family.extensions)
      if (fact.key == member.key && !fact.replay_eligible) return false;
  return true;
}
// OpenRouter plugins: each id must be admitted by the family policy, and its options must match the declared schema
// (#32). Admission says nothing about the origin; the encoder checks that it is a declared OpenRouter origin.
inline std::optional<ExtensionRefusal> validate_plugins(const descriptor::FamilyPolicy& family, const std::vector<OpenRouterPlugin>& plugins, std::size_t limit) {
  if (plugins.size() > limit) return invalid("OpenRouter plugin count limit exceeded");
  std::set<std::string_view> ids;
  for (const auto& plugin : plugins) {
    if (!config_token(plugin.id) || !ids.insert(plugin.id).second) return invalid("OpenRouter plugins require unique ids");
    const auto fact = std::find_if(family.openrouter_plugins.begin(), family.openrouter_plugins.end(), [&](const auto& f) { return f.name == plugin.id; });
    if (fact == family.openrouter_plugins.end()) return unsupported("an OpenRouter plugin is not admitted by the policy");
    if (plugin.options && (!fact->options || descriptor::check_value(*fact->options, plugin.options->root())))
      return invalid("OpenRouter plugin options do not match the policy's schema");
  }
  return std::nullopt;
}
// A value admitted by the policy for a typed container (a tool's options, a plugin's members): every member must be
// declared by `schema`. `what` names the container in the refusal.
inline std::optional<std::string> validate_options(const std::optional<descriptor::ValueSchema>& schema,
                                                   const std::shared_ptr<const json::Document>& options) {
  if (!options) return std::nullopt;
  if (!schema) return "options are not admitted by the policy";
  if (descriptor::check_value(*schema, options->root())) return "options do not match the policy's schema";
  return std::nullopt;
}
} // namespace request_controls
} // namespace sp
