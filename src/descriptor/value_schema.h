#pragma once
// A closed schema language for values the policy admits (#32): the policy names an extension key or a tool option and
// declares the shape of its value, and nothing else is sent. It has no references, no patterns and no defaults.
//   {"type":"boolean"} | {"type":"null"}
//   {"type":"integer","minimum":N,"maximum":N} | {"type":"number","minimum":N,"maximum":N}   (bounds optional)
//   {"type":"string","max_length":N,"token":true}   (token: letters, digits and "._:/-")
//   {"type":"enum","values":["a","b"]}
//   {"type":"array","items":<schema>,"max_items":N}
//   {"type":"object","properties":{"name":<schema>},"required":["name"]}   (closed: only the listed properties)
//   {"one_of":[<schema>, ...]}
#include "descriptor/descriptor.h"
#include "json/json.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sp::descriptor {

struct ValueSchema {
  enum class Kind : std::uint8_t { Boolean, Null, Integer, Number, String, Enum, Array, Object, OneOf };
  Kind kind = Kind::Null;
  std::optional<double> minimum, maximum;
  std::size_t max_length = 0, max_items = 0;
  bool token = false;
  std::vector<std::string> values;  // enum
  // Object properties in name order, with their schemas at the same index; array items at children[0]; one_of
  // alternatives in order.
  std::vector<std::string> names, required;
  std::vector<ValueSchema> children;
};

// Parses a schema at `pointer` of a policy file. Depth and sizes are bounded.
std::variant<ValueSchema, ConfigError> parse_value_schema(json::Value value, const std::string& pointer);
// Empty when `value` conforms; otherwise an RFC 6901 pointer into the value and what was expected there.
std::optional<std::string> check_value(const ValueSchema& schema, json::Value value);

} // namespace sp::descriptor
