#pragma once
#include <cstdint>
#include <optional>

namespace sp {
// A cache mark: the prompt up to and including the part, system block or tool definition that carries it may be read
// from the provider's prompt cache. Unset `ttl` leaves the provider's default lifetime (five minutes). A mark names a
// breakpoint in the prompt; it does not promise a hit. The provider caches only a prompt of at least its minimum
// length, and the policy's CacheCapabilities say whether an origin takes marks at all (descriptor/policy.h).
//
// Marks are a billing control. They are not part of the configuration digest or of a message's content digest: moving
// a mark between turns never makes sealed history ineligible for native replay.
enum class CacheTtl { FiveMinutes, OneHour };
struct CacheControl { std::optional<CacheTtl> ttl; };
// What the encoder did with the marks of a request. `requested` counts the marks the typed request carries on the
// parts it sends; `emitted` the ones written to the body. A mark the policy says to ignore at this origin or route is
// requested and not emitted. `tail_suppressed`: the request-level Messages `cache_control` was left out because part
// marks were written.
struct CacheMarkReport {
  std::uint32_t requested = 0, emitted = 0;
  bool tail_suppressed = false;
};
} // namespace sp
