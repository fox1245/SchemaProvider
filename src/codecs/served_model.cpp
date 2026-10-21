#include "codecs/served_model.h"
#include "descriptor/policy.h"
#include <algorithm>

namespace sp::served {
namespace {
// "-" and a suffix of digits and dashes that starts with a digit: dated snapshots
// (claude-haiku-4-5-20251001, gpt-5-nano-2025-08-07) and numbered versions (gemini-2.5-flash-001).
bool snapshot(std::string_view requested, std::string_view served) noexcept {
  if (served.size() < requested.size() + 2 || !served.starts_with(requested) || served[requested.size()] != '-') return false;
  const auto suffix = served.substr(requested.size() + 1);
  return suffix.front() >= '0' && suffix.front() <= '9' &&
         std::all_of(suffix.begin(), suffix.end(), [](char c) { return (c >= '0' && c <= '9') || c == '-'; });
}
bool names(const descriptor::CatalogueEntry& entry, std::string_view name) noexcept {
  auto has = [&](const std::vector<std::string>& values) { return std::find(values.begin(), values.end(), name) != values.end(); };
  return entry.id == name || has(entry.aliases) || has(entry.served_as) || (entry.alias_of && *entry.alias_of == name);
}
bool listed(const descriptor::ValidatedDescriptor& d, std::string_view requested, std::string_view served) noexcept {
  const auto& catalogue = d.policy()->catalogue();
  if (!catalogue) return false;
  // Only named entries relate names; a prefix profile describes a family of ids, not one model.
  for (const auto& [from, to] : {std::pair{requested, served}, std::pair{served, requested}}) {
    const auto match = catalogue->find(d.family(), d.base_url(), from);
    if (match.entry && match.relation != descriptor::ModelRelation::Prefix && names(*match.entry, to)) return true;
  }
  return false;
}
bool usable(std::string_view name) noexcept {
  return !name.empty() && name.size() <= max_upstream_bytes &&
         std::none_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; });
}
}  // namespace

ServedRelation relation(const descriptor::ValidatedDescriptor& d, std::string_view requested, std::string_view served) noexcept {
  if (served.empty()) return ServedRelation::NotReported;
  if (served == requested) return ServedRelation::Exact;
  if (snapshot(requested, served)) return ServedRelation::Snapshot;
  if (listed(d, requested, served)) return ServedRelation::Listed;
  if (descriptor::routed_gateway(d)) return ServedRelation::Routed;
  return ServedRelation::Unexplained;
}

void observe(const descriptor::ValidatedDescriptor& descriptor, Accumulator& accumulator,
             std::shared_ptr<const NativeContext>& context, std::string_view served, bool bind_replay) {
  const std::string_view requested = context ? context->model() : std::string_view{};
  const auto kind = relation(descriptor, requested, served);
  accumulator.served_model({std::string(requested), std::string(served), kind});
  if (!bind_replay || kind != ServedRelation::Unexplained) return;
  if (context && context->replay_eligible()) context = context->decoding_only();
  accumulator.withhold_replay();
}

std::string_view upstream_key(const descriptor::ValidatedDescriptor& descriptor) noexcept {
  return descriptor::routed_gateway(descriptor) ? "provider" : std::string_view{};
}

UpstreamSeen observe_upstream(const descriptor::ValidatedDescriptor& descriptor, json::Value object, std::string& held,
                              Accumulator& accumulator) {
  if (!descriptor::routed_gateway(descriptor)) return UpstreamSeen::Fine;
  const auto member = object.get("provider");
  if (!member.valid() || member.is_null()) return UpstreamSeen::Fine;
  if (!member.is_string() || !usable(member.as_string())) return UpstreamSeen::Unusable;
  const auto name = member.as_string();
  if (held.empty()) {
    held = std::string(name);
    accumulator.served_by(held);
    return UpstreamSeen::Fine;
  }
  return held == name ? UpstreamSeen::Fine : UpstreamSeen::Conflict;
}
}  // namespace sp::served
