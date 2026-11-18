#pragma once
// The embedded descriptor policy with one row of one family's stop table left out: a custom policy written without it.
#include "json/json.h"
#include "sp/config_defaults.h"

#include <string>
#include <string_view>
#include <variant>

namespace policy_edit {
inline std::string without_stop_row(std::string_view family, std::string_view raw) {
  auto parsed = sp::json::parse(sp::config_defaults::descriptor_policy_json);
  const auto root = std::get<sp::json::Document>(parsed).root();
  const auto members = [](sp::json::Value object, auto&& write) {
    std::string out = "{";
    for (auto member : object.members()) {
      const auto value = write(member);
      if (value.empty()) continue;
      if (out.size() > 1) out += ',';
      out += sp::json::quote(member.key) + ':' + value;
    }
    return out + '}';
  };
  return members(root, [&](const sp::json::Member& top) -> std::string {
    if (top.key != "families") return top.value.dump();
    std::string families = "[";
    for (auto entry : top.value.elements()) {
      if (families.size() > 1) families += ',';
      if (entry.get("family").as_string() != family) { families += entry.dump(); continue; }
      families += members(entry, [&](const sp::json::Member& field) -> std::string {
        if (field.key != "stop_reasons") return field.value.dump();
        return members(field.value, [&](const sp::json::Member& row) -> std::string {
          return row.key == raw ? std::string() : row.value.dump();
        });
      });
    }
    return families + ']';
  });
}
}  // namespace policy_edit
