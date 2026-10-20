#pragma once
// The model catalogue: what the SDK knows about models, as data. It is separate from the
// descriptor policy's replay identity, so a host can refresh or edit it without making sealed
// history ReplayIneligible. Nothing here refuses a model: an id the catalogue does not know is
// admitted with the family defaults, and a lookup only says how the id matched.
#include "descriptor/descriptor.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace sp::descriptor {

// Request defaults one catalogue entry sets for one family, over the family's own defaults.
// An absent member keeps the family default. A present member replaces it, and an inner nullopt
// is an explicit JSON null, which leaves the control unset.
struct DefaultsOverlay {
  std::string family;
  std::optional<std::optional<std::uint64_t>> max_output_tokens, thinking_budget;
  std::optional<std::optional<bool>> include_thoughts, thinking_summaries, strict_tools;
  std::optional<bool> reasoning_enabled;
  std::optional<std::optional<std::string>> reasoning_effort, reasoning_summary, thinking_level, service_tier;
  std::optional<std::optional<double>> temperature, top_p;
};

// One model, or with `prefix` a profile for every id that starts with `id`.
struct CatalogueEntry {
  std::string id;
  bool prefix = false;
  // Where the entry applies. Empty `origins` means every origin. `families` lists the wire
  // families that serve the model; empty means not stated. Neither ever refuses a request.
  std::vector<std::string> families, origins;
  // Other names that request the same model, and names a response may report for it.
  std::vector<std::string> aliases, served_as;
  // The model a provider-resolved alias (OpenRouter `~vendor/model-latest`) points to.
  std::optional<std::string> alias_of;
  std::optional<std::string> name, lifecycle, retires_at, source;
  std::optional<std::uint64_t> context_window, input_limit, output_limit;
  // Provider-reported tokens, kept as listed: modalities, methods (Gemini
  // supportedGenerationMethods) and supported request parameters.
  std::vector<std::string> input_modalities, output_modalities, methods, parameters;
  // Whether the model accepts `temperature`. Unset defers to the family's sampling rules.
  std::optional<bool> temperature;
  // Price per unit in USD, as the decimal text the listing gave ("0.00000015"), by unit name.
  std::vector<std::pair<std::string, std::string>> pricing;
  std::vector<DefaultsOverlay> defaults;
};

enum class ModelRelation : std::uint8_t {
  Unknown,  // no entry: the family defaults apply
  Exact,    // the entry's id
  Alias,    // one of the entry's aliases
  Prefix,   // the longest prefix profile that starts the id
};

struct ModelMatch {
  const CatalogueEntry* entry = nullptr;
  ModelRelation relation = ModelRelation::Unknown;
  // False when the entry lists families and not this one. The request is still admitted.
  bool family_listed = false;
};

class ModelCatalogue;
using CatalogueSnapshot = std::shared_ptr<const ModelCatalogue>;
using CatalogueResult = std::variant<CatalogueSnapshot, ConfigError>;

class ModelCatalogue final {
 public:
  // Sorted by id, exact entries before a prefix profile of the same id.
  const std::vector<CatalogueEntry>& entries() const noexcept { return entries_; }
  // The id, then an alias, then the longest prefix profile. Among entries of the same rank, one
  // that lists `family` wins over one that lists other families, and an entry scoped to `origin`
  // wins over an unscoped one. Entries scoped to other origins never match.
  ModelMatch find(std::string_view family, std::string_view origin, std::string_view model) const noexcept;
  // The exact or prefix entry with this id, or nullptr.
  const CatalogueEntry* entry(std::string_view id, bool prefix = false) const noexcept;
  // SHA-256 over the canonical JSON (to_json).
  std::string_view digest() const noexcept { return digest_; }
  std::string digest_hex() const;
  // Canonical JSON: entries in order, members in a fixed order, absent members left out.
  // load_catalogue(to_json()) gives a catalogue with the same digest.
  std::string to_json() const;

  ModelCatalogue(const ModelCatalogue&) = delete;
  ModelCatalogue& operator=(const ModelCatalogue&) = delete;

 private:
  ModelCatalogue() = default;
  std::vector<CatalogueEntry> entries_;
  // Indices into entries_: exact entries by id and alias, sorted by name; prefix profiles by
  // decreasing id length.
  std::vector<std::pair<std::string, std::size_t>> names_;
  std::vector<std::size_t> prefixes_;
  std::string digest_;
  friend class CatalogueBuilder;
};

// Bounded, closed JSON: {"version": 1, "models": [entry, ...]} (USAGE.md, model catalogue).
CatalogueResult load_catalogue(std::string_view json);
CatalogueResult load_catalogue_file(std::string_view path);
// A catalogue from entries already in memory, validated like a loaded one.
CatalogueResult make_catalogue(std::vector<CatalogueEntry> entries);
// The embedded catalogue (config/models.json): verified facts only, not a provider listing.
CatalogueSnapshot builtin_catalogue();
std::string builtin_catalogue_json();

// Layers in order, each over the ones before: an entry with the same id and kind is merged member
// by member (a member the later entry sets replaces the earlier one), and a new entry is added.
// `origins` scope the whole entry, so they are the exception: the later entry's origins replace
// the earlier ones, and an entry without origins applies at every origin, as the merged one then
// does. A layer of unscoped facts, such as the embedded catalogue, put over a refreshed one keeps
// applying at every origin.
CatalogueResult merge_catalogues(std::span<const CatalogueSnapshot> layers);

struct CatalogueDiff {
  // Entry keys: the id, with "*" appended for a prefix profile.
  std::vector<std::string> added, removed, changed;
  bool empty() const noexcept { return added.empty() && removed.empty() && changed.empty(); }
};
CatalogueDiff diff_catalogues(const ModelCatalogue& before, const ModelCatalogue& after);

// The provider model listings a catalogue can be generated from (transport/catalogue_refresh.h
// fetches them).
enum class Listing : std::uint8_t {
  OpenRouter,  // GET /api/v1/models?output_modalities=all, public
  Gemini,      // GET /v1beta/models, paged by nextPageToken
  Anthropic,   // GET /v1/models, paged by after_id
  OpenAI,      // GET /v1/models, ids only
};
// "openrouter", "gemini", "anthropic" or "openai": the `source` of the entries a listing gives.
std::string_view listing_name(Listing) noexcept;
struct ListingPage {
  std::vector<CatalogueEntry> entries;
  // The token that asks for the next page, or empty on the last page.
  std::string next;
  // Listed models left out because their id is not a model name the catalogue can hold.
  std::size_t skipped = 0;
  // Listed prices left out because the catalogue cannot hold them: a unit that is a number or text that is not a
  // plain decimal ("1e-7"), or one past the 64 a model keeps.
  std::size_t skipped_prices = 0;
};
using ListingResult = std::variant<ListingPage, ConfigError>;
// Reads one page of a listing. Every entry gets the listing's `source` and `origins = {origin}`.
// The listing is provider data, so unknown members are ignored and a value the catalogue cannot
// hold is left out; only a body that is not that listing at all is refused.
ListingResult parse_listing(Listing listing, std::string_view body, std::string_view origin);

} // namespace sp::descriptor
