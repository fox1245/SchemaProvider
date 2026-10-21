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
// Applies the policy to `requested` marks at this descriptor's origin and `model`. A policy that states no fact for
// the origin admits none, and one that states marks unsupported refuses them unless it says to ignore them.
inline std::variant<Admission, Error> admit(const descriptor::ValidatedDescriptor& descriptor, std::string_view model,
                                            std::uint32_t requested) {
  if (!requested) return Admission{};
  const auto capabilities = descriptor::cache_capabilities(descriptor, model);
  if (!capabilities) return Error{ErrorKind::Unsupported, "the policy admits no cache marks at this origin"};
  if (capabilities->supports_explicit) return Admission{Disposition::Emit, *capabilities};
  if (capabilities->ignore_unsupported) return Admission{Disposition::Drop, *capabilities};
  return Error{ErrorKind::Unsupported, "cache marks are not supported at this origin"};
}
// For an encoder whose wire has no explicit mark: the marks are dropped and reported, or refused, as the policy says.
inline std::optional<Error> drop(const descriptor::ValidatedDescriptor& descriptor, std::string_view model,
                                 std::span<const Message> messages, CacheMarkReport& report) {
  const auto marks = tally(messages).all;
  auto admitted = admit(descriptor, model, marks);
  if (auto* error = std::get_if<Error>(&admitted)) return std::move(*error);
  report.requested = marks;
  return std::nullopt;
}
inline bool five_minutes(std::optional<CacheTtl> ttl) noexcept { return !ttl || *ttl == CacheTtl::FiveMinutes; }
// What the policy states for a mark's lifetime: whether it is offered and what a write costs.
inline const descriptor::CacheLifetime& lifetime(const descriptor::CacheCapabilities& capabilities, std::optional<CacheTtl> ttl) noexcept {
  return five_minutes(ttl) ? capabilities.five_minutes : capabilities.one_hour;
}
inline std::optional<double> write_multiplier(const descriptor::CacheCapabilities& capabilities, std::optional<CacheTtl> ttl) noexcept {
  return lifetime(capabilities, ttl).write_multiplier;
}
// The marks about to be written, in prompt order, against the capabilities the policy states: their number, whether
// each lifetime is known and offered, and, where the origin wants it, that a longer lifetime never follows a shorter.
inline std::optional<Error> check(const descriptor::CacheCapabilities& capabilities, std::span<const std::optional<CacheTtl>> marks) {
  if (marks.size() > capabilities.max_breakpoints) return Error{ErrorKind::InvalidRequest, "cache mark count exceeds the origin's breakpoint limit"};
  bool shorter = false;
  for (const auto ttl : marks) {
    if (ttl && *ttl != CacheTtl::FiveMinutes && *ttl != CacheTtl::OneHour) return Error{ErrorKind::InvalidRequest, "invalid cache mark TTL"};
    if (!lifetime(capabilities, ttl).offered) return Error{ErrorKind::Unsupported, "cache mark lifetime is not offered at this origin"};
    if (!capabilities.longest_first) continue;
    if (five_minutes(ttl)) shorter = true;
    else if (shorter) return Error{ErrorKind::InvalidRequest, "longer cache lifetimes must come before shorter ones"};
  }
  return std::nullopt;
}
// The services refuse a mark on an empty text block.
inline std::optional<Error> marked_text(const Text& text) {
  if (text.cache && text.value.empty()) return Error{ErrorKind::InvalidRequest, "a cache mark cannot be set on an empty text block"};
  return std::nullopt;
}
// `,"cache_control":{"type":"ephemeral"[,"ttl":"5m"|"1h"]}`, the member that follows a block's others. The TTL has
// passed check.
inline void write(json::BoundedWriter& body, const CacheControl& mark) {
  body.raw(",\"cache_control\":{\"type\":\"ephemeral\"");
  if (mark.ttl) body.raw(*mark.ttl == CacheTtl::OneHour ? ",\"ttl\":\"1h\"" : ",\"ttl\":\"5m\"");
  body.raw("}");
}
} // namespace sp::cache_marks
