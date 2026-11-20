#pragma once
// The encoders' shared handling of cache marks (core/cache_control.h): what the policy says an origin does with one,
// the limits it states, and the bytes of a mark on the wire.
#include "core/cache_control.h"
#include "core/value.h"
#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include "json/json.h"
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

namespace sp::cache_marks {
// The marks the parts of `messages` carry: `all` on any part (a text, media or tool result part, or a part of a tool
// result), `text` on text parts that are not inside a tool result.
struct Tally { std::uint32_t all = 0, text = 0; };
inline Tally tally(std::span<const Message> messages) noexcept {
  Tally result;
  for (const auto& message : messages)
    for (const auto& part : message.parts) {
      if (const auto* text = std::get_if<Text>(&part)) {
        if (text->cache) { ++result.all; ++result.text; }
      } else if (const auto* media = std::get_if<Media>(&part)) {
        if (media->cache) ++result.all;
      } else if (const auto* tool = std::get_if<ToolResult>(&part)) {
        if (tool->cache) ++result.all;
        for (const auto& nested : tool->content_parts) {
          if (const auto* text = std::get_if<Text>(&nested)) result.all += text->cache.has_value();
          else if (const auto* media = std::get_if<Media>(&nested)) result.all += media->cache.has_value();
        }
      }
    }
  return result;
}
// What an encoder does with the marks of a request: nothing to do, write them, or drop them and report that.
enum class Disposition { None, Emit, Drop };
struct Admission {
  Disposition disposition = Disposition::None;
  descriptor::CacheCapabilities capabilities{};
};
inline bool five_minutes(std::optional<CacheTtl> ttl) noexcept { return !ttl || *ttl == CacheTtl::FiveMinutes; }
// What the policy states for a mark's lifetime: whether it is offered and what a write costs.
inline const descriptor::CacheLifetime& lifetime(const descriptor::CacheCapabilities& capabilities, std::optional<CacheTtl> ttl) noexcept {
  return five_minutes(ttl) ? capabilities.five_minutes : capabilities.one_hour;
}
inline std::optional<double> write_multiplier(const descriptor::CacheCapabilities& capabilities, std::optional<CacheTtl> ttl) noexcept {
  return lifetime(capabilities, ttl).write_multiplier;
}
// `,"cache_control":{"type":"ephemeral"[,"ttl":"5m"|"1h"]}`, the member that follows a block's others. The TTL has
// passed check.
inline void write(json::BoundedWriter& body, const CacheControl& mark) {
  body.raw(",\"cache_control\":{\"type\":\"ephemeral\"");
  if (mark.ttl) body.raw(*mark.ttl == CacheTtl::OneHour ? ",\"ttl\":\"1h\"" : ",\"ttl\":\"5m\"");
  body.raw("}");
}
} // namespace sp::cache_marks
