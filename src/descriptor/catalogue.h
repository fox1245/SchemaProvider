#pragma once
// The model catalogue: what the SDK knows about models, as data. It is separate from the
// descriptor policy's replay identity, so a host can refresh or edit it without making sealed
// history ReplayIneligible. Nothing here refuses a model: an id the catalogue does not know is
// admitted with the family defaults, and a lookup only says how the id matched. A fact an entry
// states (an output limit, temperature, control values) may refuse a request that contradicts it.
#include "descriptor/descriptor.h"
#include <array>
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

// One endpoint that serves a model at a routing gateway, as the gateway's endpoints listing gives it (OpenRouter's
// GET /api/v1/models/{author}/{slug}/endpoints). A refresh writes the rows of the models a source names in
// `ceiling_models` (transport/catalogue_refresh.h), every listed endpoint once, from an endpoints listing it read; a
// models listing gives none. Rows are as old as the entry that carries them. The listing's performance statistics are not catalogue data.
struct EndpointRow {
  // The listing's `tag`, which is the endpoint's routing slug (`provider[/variant][/region][/tier]`, as `deepinfra/fp8`
  // or `openai/flex`): it keys the row. `provider_name` is the provider's display name, which is not a slug.
  std::string tag;
  // Empty for the endpoints of the entry's own model. A tier name (`batch`) for the endpoints of a listing variant whose
  // prices are a tier of the entry's (DescriptorPolicy::listing_tier_variants), written onto the base entry as the
  // variant's prices are.
  std::string tier;
  std::optional<std::string> provider_name, quantization, service_tier;
  // The listing's endpoint status, as listed (0, or a negative code).
  std::optional<std::int64_t> status;
  std::optional<std::uint64_t> context_length, max_prompt_tokens, max_completion_tokens;
  std::vector<std::string> supported_parameters;  // sorted
  // The listed `discount`, from 0 to 1. The listing multiplies the row's prices by (1 - discount), so the prices as
  // listed are at least what a call at the endpoint is billed. An endpoint whose discount is not from 0 to 1 has its
  // row without prices and without a discount.
  std::optional<double> discount;
  // Price per unit in USD as listed, by unit name, read as a models listing's prices are: the input bands of its
  // long-context overrides (`prompt+128000`), and a listed member left out for a conditional price the catalogue
  // cannot hold kept at a variable price ("-1") where no unit reads it. Empty: the row states no price.
  std::vector<std::pair<std::string, std::string>> pricing;
  bool operator==(const EndpointRow&) const = default;
};

// One model, or with `prefix` a profile for every id that starts with `id`. The list members
// (families, origins, aliases, served_as and the provider-reported tokens) are sets: a catalogue
// keeps each one sorted, so the order of a listing or a file never shows in `to_json()`, the
// digest or a diff.
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
  // Provider-reported tokens: modalities, methods (Gemini supportedGenerationMethods) and
  // supported request parameters. A non-empty `parameters` states whether the model takes
  // `temperature` (it is listed or not) when `temperature` is unset; an empty one states nothing.
  std::vector<std::string> input_modalities, output_modalities, methods, parameters;
  // Whether the model accepts `temperature`. Unset: `parameters` decide when listed, and otherwise
  // the entry states nothing and a later entry of the lookup may (DescriptorPolicy::temperature_admitted).
  std::optional<bool> temperature;
  // Price per unit in USD, as the decimal text the listing gave ("0.00000015"), by unit name.
  std::vector<std::pair<std::string, std::string>> pricing;
  std::vector<DefaultsOverlay> defaults;
  // The values the model takes for a control that takes listed values (`reasoning_effort`,
  // `reasoning_summary`, `thinking_level`, `service_tier`), by control name, sorted by name, each
  // list sorted. They narrow the family's list for this model and never widen it. A control the
  // entry does not name is not stated (DescriptorPolicy::control_value_admitted).
  std::vector<std::pair<std::string, std::vector<std::string>>> control_values;
  // A routing gateway's endpoints of the model (EndpointRow), sorted by tier, then tag, each tier and tag once. The set
  // is whole per tier: a refresh writes every endpoint the listing names or none of them, and a later layer's rows of a
  // tier replace the earlier layer's rows of that tier all together (merge_catalogues). Empty: the entry states no
  // endpoints.
  std::vector<EndpointRow> endpoints;
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
class DescriptorPolicy;
using CatalogueSnapshot = std::shared_ptr<const ModelCatalogue>;
using CatalogueResult = std::variant<CatalogueSnapshot, ConfigError>;

class ModelCatalogue final {
 public:
  // Sorted by id, exact entries before a prefix profile of the same id.
  const std::vector<CatalogueEntry>& entries() const noexcept { return entries_; }
  // The id, then an alias, then the longest prefix profile. Among entries of the same rank, one
  // that lists `family` wins over one that lists other families, and an entry scoped to `origin`
  // wins over an unscoped one. Entries scoped to other origins never match. An entry's origins are canonical
  // (load_catalogue refuses another spelling) and are compared byte for byte, so pass `origin` in its canonical
  // spelling (descriptor::canonical_origin), as a descriptor's base_url() is.
  ModelMatch find(std::string_view family, std::string_view origin, std::string_view model) const noexcept;
  // The exact or prefix entry with this id, or nullptr.
  const CatalogueEntry* entry(std::string_view id, bool prefix = false) const noexcept;
  // The endpoint rows of the exact entry `model` for `tier` (empty: the model's own endpoints; `batch`: its batch
  // variant's), sorted by tag. Empty when the catalogue has no such entry or the entry states no rows for the tier. A
  // host that picks an endpoint before it sends (the cheapest, or one quantization) reads them here.
  std::span<const EndpointRow> endpoints(std::string_view model, std::string_view tier = {}) const noexcept;
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
  // The entries that apply to `model` at `origin` for `family`, in lookup order, until `visit`
  // returns true for one, which is returned (nullptr when it never does): the exact entry, the
  // entries that have the model as an alias (those scoped to `origin` first, as in find()), then the
  // prefix profiles from the longest. The exact and prefix ranks hold at most one entry each, since
  // a catalogue has one entry per id and match. An entry scoped to other origins, or one that lists
  // other families only, is skipped. `profiles_only` starts at the prefix profiles, for the ids a
  // profile with the id `model` covers.
  using MatchVisitor = bool (*)(const CatalogueEntry&, const void* context) noexcept;
  const CatalogueEntry* for_each_match(std::string_view family, std::string_view origin, std::string_view model,
                                       MatchVisitor visit, const void* context, bool profiles_only = false) const noexcept;
  std::vector<CatalogueEntry> entries_;
  // Indices into entries_: exact entries by id and alias, sorted by name; prefix profiles by
  // decreasing id length.
  std::vector<std::pair<std::string, std::size_t>> names_;
  std::vector<std::size_t> prefixes_;
  std::string digest_;
  friend class CatalogueBuilder;
  friend class DescriptorPolicy;
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
// by member (a member the later entry sets replaces the earlier one, and a control's list in
// `control_values` replaces that control's list only), and a new entry is added.
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
// Every listing, in declaration order.
inline constexpr std::array<Listing, 4> all_listings{Listing::OpenRouter, Listing::Gemini, Listing::Anthropic, Listing::OpenAI};
// Whether an entry can have `id`: printable ASCII without spaces, at most 256 bytes.
bool valid_model_name(std::string_view id) noexcept;
// Whether an entry can hold `price` as a price: digits with an optional fraction, at most 24 digits in each part,
// after an optional '-' that marks a variable price. A listed price that is not is left out
// (ListingPage::skipped_prices).
bool valid_price_text(std::string_view price) noexcept;
// A price as exact nano-USD per million units (USD x 1e15), rounded up past the fifteenth fractional digit: how every
// price of an entry is read (pricing::per_million). Nothing for a negative price (a variable one), a character other
// than a digit or one '.', or an amount that does not fit 64 bits.
std::optional<std::uint64_t> price_per_million(std::string_view price) noexcept;
// The plain name of a price unit that OpenRouter names otherwise (`prompt` is `input`, `input_cache_write_1h` is
// `cache_write_1h`, `input_audio_cache` is `cache_read_audio`, `image_output` is `output_image`); any other name is its
// own. Pricing reads both names as the same unit.
std::string_view plain_price_unit(std::string_view name) noexcept;
// The most digits of the input band of a price unit, `<unit>+<tokens>`.
inline constexpr std::size_t max_band_digits = 19;
// The units of a routing gateway's route ceiling, which a refresh writes from an endpoints listing
// (transport/catalogue_refresh.h) and a models listing never gives.
inline constexpr std::string_view route_ceiling_units[]{"ceiling_input", "ceiling_output", "ceiling_request"};
// What a listing member that no price unit reads is for. A route ceiling covers a token member by its side; an item
// member prices something a request carries, and a bound over it has no total when the request carries it.
enum class ListedRole : std::uint8_t { InputTokens, OutputTokens, EitherTokens, Item };
// The thing a request carries that an item member prices.
enum class ListedFeature : std::uint8_t {
  None,        // a token member: no feature
  ImageInput,  // an image the request takes as input
  WebSearch    // a web search the service runs for the request (an OpenRouter plugin)
};
struct ListedMember {
  std::string_view member;
  ListedRole role;
  ListedFeature feature;
};
// The price members OpenRouter's models listing gives that an entry keeps as listed and no price unit reads, with what
// each prices. The one table of this listing-grammar knowledge: a route ceiling covers the token members
// (transport/catalogue_refresh.cpp lists them among its price members), pricing floors a ceiling by them, and a bound
// over an item member above zero has no total for a request that carries its feature. A member that is in no unit and
// not here is unknown, and a bound over it above zero has no total either. `image_output` is the unit `output_image` and
// `input_audio_cache` the unit `cache_read_audio` (plain_price_unit), not members here.
inline constexpr ListedMember listed_members[]{
    {"image_token", ListedRole::EitherTokens, ListedFeature::None},
    {"image", ListedRole::Item, ListedFeature::ImageInput},
    {"web_search", ListedRole::Item, ListedFeature::WebSearch}};
// The unit a price unit names without its input band: `prompt` for `prompt+272000`; a unit without a band is itself.
std::string_view unbanded_unit(std::string_view unit) noexcept;
// Item units price an item a call makes or takes (an image, a second of audio) rather than a token:
// `item:<kind>[<key>=<value>,...][/<tier>][+<tokens>]`. The qualifier in brackets names request dimensions, and a row
// with one prices only a call that offers each of them with that value. A catalogue holds an item unit only of a kind
// in `item_kinds` and in its one canonical spelling (USAGE.md, pricing by the catalogue); a models listing never
// gives one.
inline constexpr std::string_view item_unit_prefix = "item:";
// The units of a routing gateway's per-item route ceilings, which a models listing never gives either.
inline constexpr std::string_view item_ceiling_prefix = "ceiling_item:";
// An item kind, the unit `item:<name>`, and what one unit of it counts.
struct ItemKind {
  std::string_view name, counts;
};
inline constexpr ItemKind item_kinds[]{
    {"image", "one image the call makes"},
    {"input_image", "one image the call takes as a reference"},
    {"character", "one character of the text to speak, as the provider counts it"},
    {"second", "one second of audio taken or of video made, in whole seconds rounded up"},
    {"document", "one document the request ranks"},
    {"search_unit", "one search unit, as the provider counts it"}};
// The most dimensions the qualifier of one item unit names.
inline constexpr std::size_t max_item_dimensions = 8;
// An item price unit as the grammar reads it, its parts borrowed from the unit's text.
struct ItemPriceKey {
  std::string_view unit;       // `item:image`: the prefix and the kind
  std::string_view qualifier;  // `quality=high,size=1024x1024`; empty when the unit has no qualifier
  std::size_t dimensions = 0;  // how many dimensions the qualifier names
  std::string_view tier;       // the service tier; empty for the standard rates
  std::uint64_t above = 0;     // the input band: calls with strictly more input tokens; 0 for every call
};
// `unit` read as an item price unit, when it is one a catalogue holds; nothing for any other text.
std::optional<ItemPriceKey> item_price_key(std::string_view unit) noexcept;
// Whether a call that offers `dimensions` offers every dimension `qualifier` names, each with a byte-equal value. An
// empty qualifier names none and matches every call; a dimension the call does not offer matches no value.
bool qualifier_offered(std::string_view qualifier, std::span<const std::pair<std::string, std::string>> dimensions) noexcept;
// Whether a call can offer the dimension `key=value` in the qualifier grammar: a key of 1 to 64 lower-case letters,
// digits or '_', and a value of 1 to 64 letters, digits, ':', '.', '_' or '-'. Only such a dimension can match a row
// that names it.
bool valid_dimension_key(std::string_view key) noexcept;
bool valid_dimension_value(std::string_view value) noexcept;
// A listed price member an entry leaves out (ListingPage::withheld): `pricing.overrides` may bill it above every price
// the entry could claim for it, or bill the tokens of a unit left out that would otherwise be priced at it. An empty
// `unit` is every price of the model: conditional prices that cannot be read at all.
struct WithheldUnit {
  std::string model, unit;
  bool operator==(const WithheldUnit&) const = default;
};
struct ListingPage {
  std::vector<CatalogueEntry> entries;
  // The token that asks for the next page, or empty on the last page.
  std::string next;
  // Listed models left out because their id is not a model name the catalogue can hold.
  std::size_t skipped = 0;
  // Listed prices left out because the catalogue cannot hold them: a unit that is a number or text that is not a
  // plain decimal ("1e-7"), or one past the 64 a model keeps.
  std::size_t skipped_prices = 0;
  // Prices of OpenRouter's `pricing.overrides` the entry skips or leaves out, each once (USAGE.md, refreshing the
  // catalogue): a price of an override whose conditions are neither `min_prompt_tokens` alone nor a time window alone,
  // with a discount, or with a price the catalogue cannot read; a key whose thresholds do not increase in listing
  // order, that does not fit the 64 prices of an entry, or whose band is below the base price while a later override
  // leaves the key out; a key with both a threshold and a time window; an override that is not an object, `overrides`
  // that is not an array, and a discount that is not from 0 to 1.
  std::size_t skipped_override_prices = 0;
  // Prices of overrides that are a time window alone (`utc_start` and `utc_end`), held at the dearest of the windows
  // and the base price of their key, each once.
  std::size_t window_prices_at_dearest = 0;
  // The price members left out of the entries for those overrides, by model and member, sorted.
  std::vector<WithheldUnit> withheld;
};
using ListingResult = std::variant<ListingPage, ConfigError>;
// Reads one page of a listing. Every entry gets the listing's `source` and `origins = {origin}`.
// The listing is provider data, so unknown members are ignored and a value the catalogue cannot
// hold is left out; only a body that is not that listing at all is refused.
ListingResult parse_listing(Listing listing, std::string_view body, std::string_view origin);

// The endpoint rows one OpenRouter endpoints listing gives (GET /api/v1/models/{author}/{slug}/endpoints).
struct EndpointsListing {
  // Every listed endpoint, sorted by tag, with an empty tier. Empty when `refused` is set.
  std::vector<EndpointRow> rows;
  // Why the listing gives no rows, as fixed text: an endpoint without a tag the catalogue can hold, a tag listed twice,
  // or more endpoints than an entry holds. Rows are a whole set, so a listing that cannot give one row gives none.
  std::string refused;
  // The rows whose discount is not a number from 0 to 1, by tag: each keeps its tag and loses its prices.
  std::vector<std::string> discounts;
  // The price members a route ceiling reads that a row has no price for, by tag, then member: the member, and why as
  // fixed text, as the ceiling names it: "price missing at an endpoint" (`prompt` or `completion`, which every endpoint
  // lists), "price at an endpoint is not decimal text", "variable price at an endpoint" (a negative price) or "price at an
  // endpoint is too large for the catalogue". A variable or too large price is left out of the row, with the units its
  // tokens would otherwise be billed at; the row keeps its tag and its other prices.
  struct PriceGap {
    std::string tag, price, reason;
    bool operator==(const PriceGap&) const = default;
  };
  std::vector<PriceGap> price_gaps;
  // The price members a row left out for a conditional price it cannot hold (as ListingPage::withheld), with the row's
  // tag in `model`; an empty `unit` is every price of that row.
  std::vector<WithheldUnit> withheld;
};
using EndpointsResult = std::variant<EndpointsListing, ConfigError>;
// Reads the endpoints listing of `model`. The listing is provider data: a member a row cannot hold is left out of it
// (a provider name, a count, a parameter), and the prices are read as a models listing's are. Only a body that is not
// the endpoints listing of `model` is refused: not JSON, a `data.id` of another model, `endpoints` that is not an array,
// or an endpoint that is not an object with a `pricing` object.
EndpointsResult parse_endpoints_listing(std::string_view body, std::string_view model);

} // namespace sp::descriptor
