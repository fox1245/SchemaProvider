// The cost bound of a non-chat endpoint call (pricing::endpoint_worst_case) over hand-written facts and small catalogues:
// one case per endpoint kind against amounts computed here with integers, every reason it refuses with, and a property
// over generated catalogues and calls: the bound equals the dearest price a call within its request can reach, and is at
// least the exact price of every result consistent with the request.
#include "core/endpoint_pricing.h"
#include "descriptor/catalogue.h"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace sp;
using pricing::UnboundedBy;
using Source = EndpointQuantity::Source;
using Dimensions = std::vector<std::pair<std::string, std::string>>;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (false)

constexpr std::uint64_t most = std::numeric_limits<std::uint64_t>::max();
constexpr const char* origin = "https://provider.example";

descriptor::CatalogueSnapshot catalogue(std::string_view json) {
  auto loaded = descriptor::load_catalogue(json);
  if (const auto* error = std::get_if<descriptor::ConfigError>(&loaded)) throw std::runtime_error("catalogue refused at " + error->pointer + ": " + error->expected);
  return std::get<descriptor::CatalogueSnapshot>(std::move(loaded));
}
// A rate in nano-USD per million units (USD x 1e15) as the decimal USD text a catalogue holds, written here rather than
// by the library.
std::string usd(std::uint64_t rate) {
  constexpr std::uint64_t scale = 1'000'000'000'000'000u;
  std::string fraction = std::to_string(rate % scale);
  fraction.insert(0, 15 - fraction.size(), '0');
  return std::to_string(rate / scale) + "." + fraction;
}
// One attempt's amount from the exact sum of rates times quantities: rounded up once to whole nano-USD.
std::uint64_t rounded(std::uint64_t sum) { return sum / 1'000'000 + (sum % 1'000'000 != 0 ? 1 : 0); }
// A catalogue of one entry `vendor/model` with these prices and members.
descriptor::CatalogueSnapshot entry(const std::vector<std::pair<std::string, std::string>>& prices, std::string_view members = {}) {
  std::string json = R"({"version":1,"models":[{"id":"vendor/model")";
  if (!members.empty()) json += "," + std::string(members);
  if (!prices.empty()) {
    json += R"(,"pricing":{)";
    for (std::size_t i = 0; i < prices.size(); ++i) json += (i ? "," : "") + std::string("\"") + prices[i].first + "\":\"" + prices[i].second + "\"";
    json += "}";
  }
  return catalogue(json + "}]}");
}
EndpointQuantity quantity(std::string unit, std::optional<std::uint64_t> at_most, Source source = Source::Request, std::uint64_t per = 1) {
  EndpointQuantity q;
  q.unit = std::move(unit);
  q.at_most = at_most;
  q.source = source;
  q.per = per;
  return q;
}
EndpointFacts facts(std::string endpoint, std::string operation, std::vector<EndpointQuantity> quantities, Dimensions dimensions = {}) {
  EndpointFacts f;
  f.endpoint = std::move(endpoint);
  f.operation = std::move(operation);
  f.billing = descriptor::EndpointBilling::Model;
  f.family = "vendor.endpoint";
  f.origin = origin;
  f.model = "vendor/model";
  f.quantities = std::move(quantities);
  f.dimensions = std::move(dimensions);
  return f;
}
struct Call {
  EndpointFacts facts;
  std::string tier;
  std::optional<std::uint64_t> measured, seconds;
  std::uint64_t attempts = 1;
  bool charge_on_accept = true;
};
pricing::EndpointBound bound(const Call& call, const descriptor::ModelCatalogue& c) {
  pricing::EndpointWorstCaseInput in;
  in.facts = &call.facts;
  in.service_tier = call.tier;
  in.measured_input_tokens = call.measured;
  in.stated_seconds = call.seconds;
  in.attempts = call.attempts;
  in.charge_on_accept = call.charge_on_accept;
  return pricing::endpoint_worst_case(in, c);
}
bool totals(const pricing::EndpointBound& b, std::uint64_t nano) {
  return b.total && b.total->nano_usd == nano && b.total->evidence == Evidence::Derived &&
         b.total->rounding == CostRounding::CeilingCatalogueProduct && b.unbounded.empty();
}
// No total, and among the reasons one of this kind with this subject.
bool refuses(const pricing::EndpointBound& b, UnboundedBy by, std::string_view subject) {
  return !b.total && b.lines.empty() && std::any_of(b.unbounded.begin(), b.unbounded.end(), [&](const pricing::UnboundedReason& r) {
    return r.by == by && r.subject == subject && !r.detail.empty();
  });
}
bool only(const pricing::EndpointBound& b, UnboundedBy by, std::string_view subject) { return refuses(b, by, subject) && b.unbounded.size() == 1; }
bool line(const pricing::EndpointBound& b, std::string_view unit, std::string_view row, std::uint64_t quantity, std::uint64_t rate) {
  return std::any_of(b.lines.begin(), b.lines.end(), [&](const pricing::EndpointLine& l) {
    return l.unit == unit && l.row == row && l.quantity == quantity && l.rate == rate;
  });
}

// Rates in nano-USD per million units: 0.01 USD per unit is one cent.
constexpr std::uint64_t cent = 10'000'000'000'000u;

// Images: the input at the dearest input rate (image tokens dearer than text), the output limit, the request fee, the
// images at the dearest row the call's dimensions match, and the reference images.
void images() {
  constexpr std::uint64_t input = 5'000'000'000, cache_read = 500'000'000, input_image = 10'000'000'000, output = 40'000'000'000,
                          request = 1'000'000'000'000, image = 4 * cent, high = 17 * cent, high_square = 16 * cent, edit = 5 * cent,
                          reference = 200'000'000'000;
  const auto c = entry({{"input", usd(input)}, {"cache_read", usd(cache_read)}, {"input_image", usd(input_image)}, {"output", usd(output)},
                        {"request", usd(request)}, {"item:image", usd(image)}, {"item:image[quality=high]", usd(high)},
                        {"item:image[quality=high,size=1024x1024]", usd(high_square)}, {"item:image[operation=edit]", usd(edit)},
                        {"item:input_image", usd(reference)}},
                       R"("input_limit":32000,"output_limit":6000)");
  auto quantities = [](std::optional<std::uint64_t> count, std::uint64_t references) {
    return std::vector{quantity("item:image", count), quantity("item:input_image", references), quantity("input", std::nullopt, Source::InputLimit),
                       quantity("input_image", references), quantity("output", std::nullopt, Source::OutputLimit)};
  };
  // Two high-quality square images from one reference: the dearest row that applies is the one for high quality.
  Call call{facts("images", "generate", quantities(2, 1), {{"quality", "high"}, {"size", "1024x1024"}}), {}, 1200};
  auto b = bound(call, *c);
  CHECK(totals(b, rounded(1200 * input_image + 6000 * output + request + 2 * high + 1 * reference)));
  CHECK(b.entry == "vendor/model" && b.model == "vendor/model" && b.attempts == 1 && b.lines.size() == 5);
  CHECK(line(b, "input", "input_image", 1200, input_image) && line(b, "output", "output", 6000, output) &&
        line(b, "request", "request", 1, request) && line(b, "item:image", "item:image[quality=high]", 2, high) &&
        line(b, "item:input_image", "item:input_image", 1, reference));
  // An edit: the row qualified by the operation applies to it, and not to a generation.
  call.facts = facts("images", "edit", quantities(1, 1), {{"quality", "low"}});
  CHECK(totals(bound(call, *c), rounded(1200 * input_image + 6000 * output + request + edit + reference)));
  call.facts = facts("images", "generate", quantities(1, 1), {{"quality", "low"}});
  CHECK(totals(bound(call, *c), rounded(1200 * input_image + 6000 * output + request + image + reference)));
  // A dimension named by the call as `operation` is the call's own operation.
  call.facts = facts("images", "edit", quantities(1, 1), {{"operation", "edit"}});
  CHECK(totals(bound(call, *c), rounded(1200 * input_image + 6000 * output + request + edit + reference)));
  // Unset size and quality: only the row without a qualifier prices the images, whatever the provider picks.
  call.facts = facts("images", "generate", quantities(3, 0));
  CHECK(totals(bound(call, *c), rounded(1200 * input_image + 6000 * output + request + 3 * image)));
  // No measured input: the input limit.
  call.measured.reset();
  CHECK(totals(bound(call, *c), rounded(32000 * input_image + 6000 * output + request + 3 * image)));
  // A count the request does not state refuses: no default count.
  call.facts = facts("images", "generate", quantities(std::nullopt, 0));
  CHECK(only(bound(call, *c), UnboundedBy::Quantity, "item:image"));
  // Without references, neither the reference unit nor the image token rate is needed; with one, both are.
  const auto plain = entry({{"input", usd(input)}, {"output", usd(output)}, {"item:image", usd(image)}}, R"("input_limit":32000,"output_limit":6000)");
  call.facts = facts("images", "generate", quantities(2, 0));
  CHECK(totals(bound(call, *plain), rounded(32000 * input + 6000 * output + 2 * image)));
  call.facts = facts("images", "generate", quantities(2, 1));
  const auto refused = bound(call, *plain);
  CHECK(refuses(refused, UnboundedBy::ModelPrice, "input_image") && refuses(refused, UnboundedBy::ModelPrice, "item:input_image[operation=generate]") &&
        refused.unbounded.size() == 2);
  std::cout << "case passed: images\n";
}

// Decisions: input and output tokens, at OpenRouter's names as at the plain ones; no output limit refuses.
void decisions() {
  constexpr std::uint64_t input = 300'000'000, output = 1'200'000'000;
  const auto c = entry({{"prompt", usd(input)}, {"completion", usd(output)}}, R"("input_limit":50000,"output_limit":1000)");
  const auto two = std::vector{quantity("input", std::nullopt, Source::InputLimit), quantity("output", std::nullopt, Source::OutputLimit)};
  Call call{facts("decisions", "decide", two), {}, 400};
  auto b = bound(call, *c);
  CHECK(totals(b, rounded(400 * input + 1000 * output)));
  CHECK(line(b, "input", "prompt", 400, input) && line(b, "output", "completion", 1000, output) && b.lines.size() == 2);
  call.measured.reset();
  CHECK(totals(bound(call, *c), rounded(50000 * input + 1000 * output)));
  // A measured count above the limit is capped at it.
  call.measured = 90000;
  CHECK(totals(bound(call, *c), rounded(50000 * input + 1000 * output)));
  const auto unlimited = entry({{"input", usd(input)}, {"output", usd(output)}});
  call.measured = 400;
  CHECK(only(bound(call, *unlimited), UnboundedBy::OutputCap, "output"));
  call.measured.reset();
  const auto both = bound(call, *unlimited);
  CHECK(refuses(both, UnboundedBy::OutputCap, "output") && refuses(both, UnboundedBy::InputBound, "input") && both.unbounded.size() == 2);
  std::cout << "case passed: decisions\n";
}

// Embeddings: the entry's input limit counts per input, so the bound is the limit times the inputs; a measured count
// is capped at that product, not at one input's limit.
void embeddings() {
  constexpr std::uint64_t input = 20'000'000;
  const auto c = entry({{"input", usd(input)}}, R"("input_limit":2048)");
  Call call{facts("embeddings", "embed", {quantity("input", std::nullopt, Source::InputLimit, 3)})};
  auto b = bound(call, *c);
  CHECK(totals(b, rounded(3 * 2048 * input)) && line(b, "input", "input", 3 * 2048, input));
  call.measured = 5000;
  CHECK(totals(bound(call, *c), rounded(5000 * input)));
  call.measured = 7000;
  CHECK(totals(bound(call, *c), rounded(3 * 2048 * input)));
  // The limit times the inputs past 64 bits.
  call.measured.reset();
  call.facts = facts("embeddings", "embed", {quantity("input", std::nullopt, Source::InputLimit, most)});
  CHECK(only(bound(call, *c), UnboundedBy::Overflow, "input"));
  std::cout << "case passed: embeddings\n";
}

// Rerank: documents counted by the request, search units by nothing: a positive rate for them refuses, a declared zero
// does not, and neither does an input rate of zero without a limit.
void rerank() {
  constexpr std::uint64_t document = 100'000'000'000;
  const auto quantities = std::vector{quantity("item:document", 3), quantity("item:search_unit", std::nullopt), quantity("input", std::nullopt, Source::InputLimit)};
  Call call{facts("rerank", "rank", quantities)};
  const auto free_units = entry({{"item:document", usd(document)}, {"item:search_unit", "0"}, {"input", "0"}});
  auto b = bound(call, *free_units);
  CHECK(totals(b, rounded(3 * document)) && line(b, "item:document", "item:document", 3, document) && line(b, "item:search_unit", "item:search_unit", 0, 0));
  const auto billed_units = entry({{"item:document", usd(document)}, {"item:search_unit", "0.002"}, {"input", "0"}});
  CHECK(only(bound(call, *billed_units), UnboundedBy::Quantity, "item:search_unit"));
  const auto billed_input = entry({{"item:document", usd(document)}, {"item:search_unit", "0"}, {"input", "0.000001"}});
  CHECK(only(bound(call, *billed_input), UnboundedBy::InputBound, "input"));
  std::cout << "case passed: rerank\n";
}

// Speech: the characters to speak as the request's UTF-8 bytes; an entry that prices output seconds, which the
// operation cannot count, refuses; a row qualified by another operation does not apply.
void speech() {
  constexpr std::uint64_t character = 15'000'000'000;
  const std::string text = "h\xc3\xa9llo \xe2\x82\xac";  // seven characters, ten bytes
  CHECK(text.size() == 10);
  const auto quantities = std::vector{quantity("item:character", text.size()), quantity("input", std::nullopt, Source::InputLimit),
                                      quantity("output", std::nullopt, Source::OutputLimit)};
  Call call{facts("speech", "speak", quantities, {{"voice", "alloy"}})};
  const auto c = entry({{"item:character", usd(character)}, {"input", "0"}, {"output", "0"}, {"item:second[operation=transcribe]", "0.01"}});
  auto b = bound(call, *c);
  CHECK(totals(b, rounded(10 * character)) && line(b, "item:character", "item:character", 10, character));
  const auto seconds = entry({{"item:character", usd(character)}, {"input", "0"}, {"output", "0"}, {"item:second[operation=speak]", "0.01"}});
  CHECK(only(bound(call, *seconds), UnboundedBy::Unmodeled, "item:second[operation=speak]"));
  // A declared zero of such a unit does not refuse.
  const auto free_seconds = entry({{"item:character", usd(character)}, {"input", "0"}, {"output", "0"}, {"item:second", "0"}});
  CHECK(totals(bound(call, *free_seconds), rounded(10 * character)));
  std::cout << "case passed: speech\n";
}

// Transcription: the audio's seconds as the host states them; unstated, they refuse.
void transcription() {
  constexpr std::uint64_t second = 100'000'000'000, input = 2'500'000'000, output = 10'000'000'000;
  const auto c = entry({{"item:second", usd(second)}, {"input", usd(input)}, {"output", usd(output)}}, R"("input_limit":16000,"output_limit":2000)");
  const auto quantities = std::vector{quantity("item:second", std::nullopt, Source::StatedSeconds), quantity("input", std::nullopt, Source::InputLimit),
                                      quantity("output", std::nullopt, Source::OutputLimit)};
  Call call{facts("transcription", "transcribe", quantities), {}, std::nullopt, 7};
  auto b = bound(call, *c);
  CHECK(totals(b, rounded(7 * second + 16000 * input + 2000 * output)) && line(b, "item:second", "item:second", 7, second));
  call.seconds.reset();
  CHECK(only(bound(call, *c), UnboundedBy::Quantity, "item:second"));
  std::cout << "case passed: transcription\n";
}

// Videos: seconds of the requested duration at the dearest row the resolution and audio match.
void videos() {
  constexpr std::uint64_t base = 5 * cent, full_hd = 15 * cent, full_hd_audio = 30 * cent;
  const auto c = entry({{"item:second", usd(base)}, {"item:second[resolution=1080p]", usd(full_hd)},
                        {"item:second[audio=true,resolution=1080p]", usd(full_hd_audio)}, {"input", "0"}, {"output", "0"}});
  auto quantities = [](std::optional<std::uint64_t> duration) {
    return std::vector{quantity("item:second", duration), quantity("input", std::nullopt, Source::InputLimit), quantity("output", std::nullopt, Source::OutputLimit)};
  };
  Call call{facts("videos", "submit", quantities(8), {{"audio", "true"}, {"resolution", "1080p"}})};
  CHECK(totals(bound(call, *c), rounded(8 * full_hd_audio)));
  call.facts = facts("videos", "submit", quantities(8), {{"audio", "false"}, {"resolution", "1080p"}});
  CHECK(totals(bound(call, *c), rounded(8 * full_hd)));
  call.facts = facts("videos", "submit", quantities(8), {{"resolution", "720p"}});
  CHECK(totals(bound(call, *c), rounded(8 * base)));
  call.facts = facts("videos", "submit", quantities(std::nullopt), {{"resolution", "720p"}});
  CHECK(only(bound(call, *c), UnboundedBy::Quantity, "item:second"));
  // Only qualified rows, and a call that leaves the dimension unset: no row prices it.
  const auto qualified = entry({{"item:second[resolution=1080p]", usd(full_hd)}, {"input", "0"}, {"output", "0"}});
  call.facts = facts("videos", "submit", quantities(8));
  CHECK(only(bound(call, *qualified), UnboundedBy::ModelPrice, "item:second[operation=submit]"));
  std::cout << "case passed: videos\n";
}

// What the policy states about the operation decides before any catalogue lookup.
void billing() {
  const auto empty = catalogue(R"({"version":1,"models":[]})");
  Call call{facts("videos", "poll", {})};
  call.facts.billing = descriptor::EndpointBilling::None;
  call.attempts = 3;
  auto b = bound(call, *empty);
  CHECK(totals(b, 0) && b.lines.empty() && b.model == "vendor/model");
  call.facts = facts("videos", "submit", {quantity("item:second", 8)});
  call.facts.billing = descriptor::EndpointBilling::Unstated;
  CHECK(only(bound(call, *empty), UnboundedBy::Billing, "videos/submit"));
  call.facts.billing = static_cast<descriptor::EndpointBilling>(200);
  CHECK(only(bound(call, *empty), UnboundedBy::Billing, "videos/submit"));
  call.facts = facts("batches", "submit", {});
  call.facts.billing = descriptor::EndpointBilling::Carried;
  call.facts.carried_tier = "batch";
  CHECK(only(bound(call, *empty), UnboundedBy::Unmodeled, "batches/submit"));
  pricing::EndpointWorstCaseInput none;
  CHECK(pricing::endpoint_worst_case(none, *empty).unbounded.size() == 1 && !pricing::endpoint_worst_case(none, *empty).total);
  std::cout << "case passed: billing\n";
}

// Every other reason: the model, the gateway, the tier, the rows, the dimensions and the attempts.
void refusals() {
  const auto quantities = std::vector{quantity("item:image", 1), quantity("input", 100), quantity("output", 100)};
  const auto c = entry({{"input", "0.000001"}, {"output", "0.000002"}, {"item:image", "0.04"}});
  Call call{facts("images", "generate", quantities)};
  CHECK(bound(call, *c).total);
  // An unknown model, and a model whose entry has no prices.
  call.facts.model = "vendor/unknown";
  CHECK(only(bound(call, *c), UnboundedBy::ModelPrice, "vendor/unknown"));
  call.facts.model = "vendor/model";
  CHECK(only(bound(call, *entry({}, R"("input_limit":10)")), UnboundedBy::ModelPrice, "vendor/model"));
  // A routing gateway, with an entry that prices everything.
  call.facts.routed_gateway = true;
  CHECK(only(bound(call, *c), UnboundedBy::RouteCeiling, "vendor/model"));
  call.facts.routed_gateway = false;
  // A route ceiling at an origin that is not a routing gateway.
  CHECK(only(bound(call, *entry({{"input", "0.000001"}, {"output", "0.000002"}, {"item:image", "0.04"}, {"ceiling_input", "0.000003"}})),
             UnboundedBy::RouteCeiling, "vendor/model"));
  // A tier the entry prices nothing in; with rows in it, only those rows.
  call.tier = "flex";
  CHECK(only(bound(call, *c), UnboundedBy::TierPrice, "vendor/model"));
  const auto tiered = entry({{"input", "0.000001"}, {"output", "0.000002"}, {"item:image", "0.04"}, {"input/flex", "0.0000005"},
                             {"output/flex", "0.000001"}, {"item:image/flex", "0.02"}});
  constexpr std::uint64_t flex_input = 500'000'000, flex_output = 1'000'000'000;
  CHECK(totals(bound(call, *tiered), rounded(100 * flex_input + 100 * flex_output + 2 * cent)));
  // A tier with item rows only: the token units have no rate in it.
  const auto items_only = entry({{"input", "0.000001"}, {"output", "0.000002"}, {"item:image/flex", "0.02"}});
  const auto no_tokens = bound(call, *items_only);
  CHECK(refuses(no_tokens, UnboundedBy::ModelPrice, "input") && refuses(no_tokens, UnboundedBy::ModelPrice, "output") && no_tokens.unbounded.size() == 2);
  call.tier.clear();
  // A unit the entry does not list is no price, never a free one.
  const auto no_image = entry({{"input", "0.000001"}, {"output", "0.000002"}});
  CHECK(only(bound(call, *no_image), UnboundedBy::ModelPrice, "item:image[operation=generate]"));
  const auto no_input = entry({{"output", "0.000002"}, {"item:image", "0.04"}, {"cache_read", "0.0000001"}});
  CHECK(only(bound(call, *no_input), UnboundedBy::ModelPrice, "input"));
  // A variable price on a row that applies, for an item and for a token unit.
  const auto variable_image = entry({{"input", "0.000001"}, {"output", "0.000002"}, {"item:image", "0.04"}, {"item:image[quality=high]", "-1"}});
  call.facts = facts("images", "generate", quantities, {{"quality", "high"}});
  CHECK(only(bound(call, *variable_image), UnboundedBy::ModelPrice, "item:image[operation=generate,quality=high]"));
  call.facts = facts("images", "generate", quantities, {{"quality", "low"}});
  CHECK(bound(call, *variable_image).total);
  const auto variable_band = entry({{"input", "0.000001"}, {"input+1000", "-1"}, {"output", "0.000002"}, {"item:image", "0.04"}});
  CHECK(bound(call, *variable_band).total);
  call.facts.quantities[1].at_most = 1001;
  CHECK(only(bound(call, *variable_band), UnboundedBy::ModelPrice, "input"));
  // Dimensions out of order or named twice, and dimensions that name another operation.
  call.facts = facts("images", "generate", quantities, {{"size", "1024x1024"}, {"quality", "high"}});
  CHECK(only(bound(call, *c), UnboundedBy::Unmodeled, "dimensions"));
  call.facts = facts("images", "generate", quantities, {{"quality", "high"}, {"quality", "low"}});
  CHECK(only(bound(call, *c), UnboundedBy::Unmodeled, "dimensions"));
  call.facts = facts("images", "generate", quantities, {{"operation", "edit"}});
  CHECK(only(bound(call, *c), UnboundedBy::Unmodeled, "dimensions"));
  call.facts = facts("images", "", quantities);
  CHECK(only(bound(call, *c), UnboundedBy::Unmodeled, "images"));
  // A quantity of a unit the bound does not price.
  call.facts = facts("images", "generate", {quantity("item:image", 1), quantity("pixels", 4)});
  CHECK(only(bound(call, *c), UnboundedBy::Unmodeled, "pixels"));
  // No attempts.
  call.facts = facts("images", "generate", quantities);
  call.attempts = 0;
  CHECK(only(bound(call, *c), UnboundedBy::Invocations, ""));
  call.charge_on_accept = false;
  CHECK(only(bound(call, *c), UnboundedBy::Invocations, ""));
  std::cout << "case passed: refusals\n";
}

// Declared zeros price at zero and need no count; an absent unit refuses; a zero quantity needs no rate.
void zeros() {
  const auto c = entry({{"input", "0"}, {"cache_read", "0"}, {"output", "0"}, {"item:image", "0"}});
  Call call{facts("images", "generate", {quantity("item:image", std::nullopt), quantity("input", std::nullopt, Source::InputLimit),
                                         quantity("output", std::nullopt, Source::OutputLimit)})};
  auto b = bound(call, *c);
  CHECK(totals(b, 0) && line(b, "item:image", "item:image", 0, 0) && line(b, "input", "cache_read", 0, 0) && line(b, "output", "output", 0, 0));
  // One input-side rate above zero: the input needs a count.
  const auto cached = entry({{"input", "0"}, {"cache_read", "0.000001"}, {"output", "0"}, {"item:image", "0"}});
  CHECK(only(bound(call, *cached), UnboundedBy::InputBound, "input"));
  // Zero of a unit the entry does not list.
  call.facts = facts("images", "generate", {quantity("item:image", 0), quantity("item:input_image", 0), quantity("input_image", 0), quantity("input", 0), quantity("output", 0)});
  const auto bare = entry({{"request", "0.001"}});
  CHECK(totals(bound(call, *bare), rounded(1'000'000'000'000)));
  std::cout << "case passed: zero rates and absent units\n";
}

// Bands: a row above `+<tokens>` applies to a call with strictly more input tokens.
void bands() {
  constexpr std::uint64_t input = 1'000'000'000, long_input = 2'000'000'000, image = 1 * cent, long_image = 2 * cent;
  const auto c = entry({{"input", usd(input)}, {"input+1000", usd(long_input)}, {"output", "0"}, {"item:image", usd(image)},
                        {"item:image+1000", usd(long_image)}});
  Call call{facts("images", "generate", {quantity("item:image", 1), quantity("input", std::nullopt, Source::InputLimit), quantity("output", 10)}), {}, 1000};
  CHECK(totals(bound(call, *c), rounded(1000 * input + image)));
  call.measured = 1001;
  CHECK(totals(bound(call, *c), rounded(1001 * long_input + long_image)));
  // An unstated input length reaches every band; with zero input rates it needs no count, and the items are banded.
  const auto zero_input = entry({{"input", "0"}, {"output", "0"}, {"item:image", usd(image)}, {"item:image+1000", usd(long_image)}});
  call.measured.reset();
  CHECK(totals(bound(call, *zero_input), rounded(long_image)));
  std::cout << "case passed: bands\n";
}

// One attempt for an operation charged once accepted; the retry policy's attempts otherwise.
void attempts() {
  constexpr std::uint64_t input = 300'000'000, output = 1'200'000'000;
  const auto c = entry({{"input", usd(input)}, {"output", usd(output)}}, R"("output_limit":1000)");
  Call call{facts("decisions", "decide", {quantity("input", std::nullopt, Source::InputLimit), quantity("output", std::nullopt, Source::OutputLimit)}), {}, 333};
  const auto one = rounded(333 * input + 1000 * output);
  call.attempts = 1;
  CHECK(totals(bound(call, *c), one) && bound(call, *c).attempts == 1);
  call.attempts = 4;
  CHECK(totals(bound(call, *c), one) && bound(call, *c).attempts == 1);
  call.charge_on_accept = false;
  const auto b = bound(call, *c);
  CHECK(totals(b, 4 * one) && b.attempts == 4 && line(b, "output", "output", 1000, output));
  call.attempts = 1;
  CHECK(totals(bound(call, *c), one));
  std::cout << "case passed: attempts\n";
}

// A product and a sum past 64 bits, and a total over the attempts past 64 bits: no total.
void overflow() {
  const auto c = entry({{"item:second", "18000"}, {"input", "0"}, {"output", "0"}});
  // A product that fits 128 bits but not 64 once rounded, and a sum of two that does not fit 128 bits.
  Call call{facts("videos", "submit", {quantity("item:second", most), quantity("input", 0), quantity("output", 0)})};
  CHECK(only(bound(call, *c), UnboundedBy::Overflow, ""));
  const auto two = entry({{"item:second", "18000"}, {"item:document", "18000"}, {"input", "0"}, {"output", "0"}});
  call.facts.quantities.push_back(quantity("item:document", most));
  CHECK(only(bound(call, *two), UnboundedBy::Overflow, ""));
  call.facts.quantities.pop_back();
  // One attempt fits, at exactly 18000 USD per second for a million seconds; the attempts do not.
  call.facts.quantities[0].at_most = 1'000'000;
  call.charge_on_accept = false;
  call.attempts = 1;
  CHECK(totals(bound(call, *c), 18'000'000'000'000'000'000u));
  call.attempts = 2;
  CHECK(only(bound(call, *c), UnboundedBy::Overflow, ""));
  std::cout << "case passed: overflow\n";
}

// The property: generated catalogues and calls for every kind of quantity, priced here from the generated rows' own
// facts. ---------------------------------------------------------------------------------------------------------
struct Row {
  std::string name;  // a token unit (`input`) or an item kind (`image`)
  bool item = false;
  std::map<std::string, std::string> qualifier;
  std::string tier;
  std::uint64_t above = 0;
  std::optional<std::uint64_t> rate;  // unset: a variable price
};
std::string key_of(const Row& r) {
  std::string key = r.item ? "item:" + r.name : r.name;
  if (!r.qualifier.empty()) {
    key += '[';
    for (const auto& [name, value] : r.qualifier) key += (key.back() == '[' ? "" : ",") + name + "=" + value;
    key += ']';
  }
  if (!r.tier.empty()) key += "/" + r.tier;
  if (r.above) key += "+" + std::to_string(r.above);
  return key;
}
struct Generated {
  std::map<std::string, Row> rows;  // by price key
  std::optional<std::uint64_t> input_limit, context_window, output_limit;
  EndpointFacts facts;
  std::string tier;
  std::optional<std::uint64_t> measured, seconds;
  std::uint64_t attempts = 1;
  bool charge_on_accept = false;
};
struct Expected {
  std::optional<std::uint64_t> total;
  std::vector<std::pair<UnboundedBy, std::string>> reasons;
};
const std::vector<std::string> input_units{"input", "cache_read", "cache_write", "cache_write_1h", "input_audio", "input_image"};
const std::vector<std::string> output_units{"output", "reasoning", "output_image", "output_audio"};

// The quantities and their parts of a generated call, as the oracle reads them.
struct Resolved {
  std::string unit;
  std::optional<std::uint64_t> count;
};
// What the bound must answer, from the generated facts alone.
struct Oracle {
  const Generated& g;
  Dimensions offered;
  std::uint64_t length = most;  // the input length that picks bands
  explicit Oracle(const Generated& generated) : g(generated) {
    offered = g.facts.dimensions;
    offered.emplace_back("operation", g.facts.operation);
    std::sort(offered.begin(), offered.end());
  }
  bool offers(const Row& r) const {
    for (const auto& [name, value] : r.qualifier)
      if (std::find(offered.begin(), offered.end(), std::pair{name, value}) == offered.end()) return false;
    return true;
  }
  bool applies(const Row& r, std::uint64_t input_length) const {
    return r.tier == g.tier && (r.above == 0 || input_length > r.above) && offers(r);
  }
  std::string with_dimensions(const std::string& unit) const {
    std::string out = unit + "[";
    for (const auto& [name, value] : offered) out += (out.back() == '[' ? "" : ",") + name + "=" + value;
    return out + "]";
  }
  std::optional<std::uint64_t> count(const EndpointQuantity& q) const {
    if (q.at_most) return q.at_most;
    switch (q.source) {
      case Source::Request: return std::nullopt;
      case Source::StatedSeconds: return g.seconds;
      case Source::OutputLimit: return g.output_limit && *g.output_limit ? g.output_limit : std::nullopt;
      case Source::InputLimit: {
        std::optional<std::uint64_t> limit;
        for (const auto& value : {g.input_limit, g.context_window})
          if (value && *value && (!limit || *value < *limit)) limit = value;
        if (limit) *limit *= q.per;
        if (g.measured) return limit ? std::min(*g.measured, *limit) : *g.measured;
        return limit;
      }
    }
    return std::nullopt;
  }
  Expected expect() {
    Expected out;
    auto refuse = [&](UnboundedBy by, std::string subject) { out.reasons.emplace_back(by, std::move(subject)); };
    if (g.rows.empty()) { refuse(UnboundedBy::ModelPrice, g.facts.model); return out; }
    if (std::none_of(g.rows.begin(), g.rows.end(), [&](const auto& item) { return item.second.tier == g.tier; })) {
      refuse(UnboundedBy::TierPrice, g.facts.model);
      return out;
    }
    std::optional<std::uint64_t> input_count;
    bool has_input = false, image_tokens = false;
    for (const auto& q : g.facts.quantities) {
      if (q.unit == "input") { has_input = true; input_count = count(q); }
      if (q.unit == "input_image" && (!q.at_most || *q.at_most > 0)) image_tokens = true;
    }
    length = has_input && input_count ? *input_count : most;
    std::uint64_t sum = 0;
    // A token unit: the dearest fixed rate of its rows that apply; whether one of them is variable; whether `base` has one.
    auto token = [&](const std::vector<std::string>& units, std::string_view base, std::optional<std::uint64_t> n, UnboundedBy unknown,
                     const std::string& unit, bool images) {
      if (n && *n == 0) return;
      std::optional<std::uint64_t> dearest;
      bool variable = false, based = false, imaged = false;
      for (const auto& [key, r] : g.rows) {
        if (r.item || std::find(units.begin(), units.end(), r.name) == units.end() || !applies(r, length)) continue;
        if (!r.rate) { variable = true; continue; }
        if (r.name == base) based = true;
        if (r.name == "input_image") imaged = true;
        if (!dearest || *r.rate > *dearest) dearest = r.rate;
      }
      bool ok = true;
      if (variable) { refuse(UnboundedBy::ModelPrice, unit); ok = false; }
      if (!based) { refuse(UnboundedBy::ModelPrice, unit); ok = false; }
      if (images && !imaged) { refuse(UnboundedBy::ModelPrice, "input_image"); ok = false; }
      if (!n) {
        if (!(ok && *dearest == 0)) refuse(unknown, unit);
        return;
      }
      if (ok) sum += *n * *dearest;
    };
    std::vector<std::string> kinds;
    for (const auto& q : g.facts.quantities) {
      if (q.unit == "input") token(input_units, "input", count(q), UnboundedBy::InputBound, "input", image_tokens);
      else if (q.unit == "output") token(output_units, "output", count(q), UnboundedBy::OutputCap, "output", false);
      else if (q.unit.starts_with("item:")) {
        kinds.push_back(q.unit.substr(5));
        const auto n = count(q);
        if (n && *n == 0) continue;
        std::optional<std::uint64_t> dearest;
        bool variable = false;
        for (const auto& [key, r] : g.rows) {
          if (!r.item || "item:" + r.name != q.unit || !applies(r, length)) continue;
          if (!r.rate) variable = true;
          else if (!dearest || *r.rate > *dearest) dearest = r.rate;
        }
        if (variable || !dearest) refuse(UnboundedBy::ModelPrice, with_dimensions(q.unit));
        else if (*dearest > 0 && !n) refuse(UnboundedBy::Quantity, q.unit);
        else if (n) sum += *n * *dearest;
      }
    }
    // The request fee.
    std::optional<std::uint64_t> fee;
    for (const auto& [key, r] : g.rows) {
      if (r.item || r.name != "request" || !applies(r, length)) continue;
      if (!r.rate) { refuse(UnboundedBy::ModelPrice, "request"); fee.reset(); break; }
      if (!fee || *r.rate > *fee) fee = r.rate;
    }
    if (fee) sum += *fee;
    // Item rows of kinds no quantity counts.
    for (const auto& [key, r] : g.rows)
      if (r.item && std::find(kinds.begin(), kinds.end(), r.name) == kinds.end() && applies(r, length) && !(r.rate && *r.rate == 0))
        refuse(UnboundedBy::Unmodeled, key);
    if (!out.reasons.empty()) return out;
    out.total = rounded(sum) * (g.charge_on_accept ? 1 : g.attempts);
    return out;
  }
};

// Every result consistent with the request, as exact amounts of one attempt: the input tokens at none, half and all of
// the bound's count, each at any input rate that applies at that count; the output at none or the cap, at any output
// rate; the request fee of any row that applies; each item count from zero to the request's, at any row that applies.
// Calls `visit` with each total; returns how many it visited.
template <class Visit>
std::size_t consistent_results(const Oracle& o, Visit visit) {
  const auto& g = o.g;
  std::optional<std::uint64_t> input_count, output_count;
  bool has_input = false;
  for (const auto& q : g.facts.quantities) {
    if (q.unit == "input") { has_input = true; input_count = o.count(q); }
    if (q.unit == "output") output_count = o.count(q);
  }
  std::vector<std::uint64_t> lengths{0};
  if (input_count && *input_count) { lengths.push_back(*input_count / 2); lengths.push_back(*input_count); }
  std::size_t visited = 0;
  for (const auto length : lengths) {
    // The band rows that apply to a call of `length` input tokens; with an unstated input, every band.
    const auto band = has_input && input_count ? length : most;
    std::vector<std::vector<std::uint64_t>> parts;
    auto rates = [&](const std::vector<std::string>& units) {
      std::vector<std::uint64_t> out;
      for (const auto& [key, r] : g.rows)
        if (!r.item && std::find(units.begin(), units.end(), r.name) != units.end() && o.applies(r, band) && r.rate) out.push_back(*r.rate);
      return out;
    };
    std::vector<std::uint64_t> in{0};
    for (const auto rate : rates(input_units)) in.push_back(length * rate);
    parts.push_back(in);
    std::vector<std::uint64_t> out{0};
    if (output_count)
      for (const auto rate : rates(output_units)) out.push_back(*output_count * rate);
    parts.push_back(out);
    std::vector<std::uint64_t> fees = rates({"request"});
    if (fees.empty()) fees.push_back(0);
    parts.push_back(fees);
    for (const auto& q : g.facts.quantities) {
      if (!q.unit.starts_with("item:")) continue;
      std::vector<std::uint64_t> items{0};
      const auto n = o.count(q);
      for (const auto& [key, r] : g.rows) {
        if (!r.item || "item:" + r.name != q.unit || !o.applies(r, band) || !r.rate) continue;
        if (!n) continue;  // an unstated count bounds only at a declared zero, which bills nothing
        std::vector<std::uint64_t> counts;
        if (*n <= 4) for (std::uint64_t k = 0; k <= *n; ++k) counts.push_back(k);
        else counts = {0, 1, *n / 2, *n - 1, *n};
        for (const auto k : counts) items.push_back(k * *r.rate);
      }
      parts.push_back(items);
    }
    std::vector<std::size_t> pick(parts.size(), 0);
    for (;;) {
      std::uint64_t sum = 0;
      for (std::size_t i = 0; i < parts.size(); ++i) sum += parts[i][pick[i]];
      visit(rounded(sum) * (g.charge_on_accept ? 1 : g.attempts));
      ++visited;
      std::size_t i = 0;
      while (i < parts.size() && ++pick[i] == parts[i].size()) pick[i++] = 0;
      if (i == parts.size()) break;
    }
  }
  return visited;
}

void bound_covers_every_consistent_result() {
  std::uint64_t seed = 20261119;
  auto next = [&](std::uint64_t limit) { seed = seed * 6364136223846793005u + 1442695040888963407u; return (seed >> 33) % (limit + 1); };
  struct Shape {
    const char* endpoint;
    std::vector<const char*> operations;
  };
  const Shape shapes[]{{"images", {"generate", "edit"}}, {"decisions", {"decide"}}, {"embeddings", {"embed"}}, {"rerank", {"rank"}},
                       {"speech", {"speak"}}, {"transcription", {"transcribe", "translate"}}, {"videos", {"submit"}}};
  const char* const kinds[]{"image", "input_image", "document", "search_unit", "character", "second"};
  const char* const tokens[]{"input", "cache_read", "input_image", "output", "reasoning", "request"};
  const std::string pieces[]{"a", "\xc3\xa9", "\xe2\x82\xac", "\xf0\x9f\x8e\xb5"};  // one to four UTF-8 bytes each
  std::size_t calls = 0, bounded = 0, refused = 0, results = 0;
  std::map<UnboundedBy, std::size_t> reasons;
  for (const auto& shape : shapes) {
    for (int round = 0; round < 2000; ++round, ++calls) {
      Generated g;
      const std::string operation = shape.operations[next(shape.operations.size() - 1)];
      Dimensions dimensions;
      if (next(2)) dimensions.emplace_back("quality", next(1) ? "high" : "low");
      if (next(2)) dimensions.emplace_back("size", next(1) ? "a" : "b");
      std::vector<EndpointQuantity> q;
      std::vector<std::string> used;
      auto stated = [&](std::uint64_t top) { return next(4) == 0 ? std::optional<std::uint64_t>() : std::optional<std::uint64_t>(next(top)); };
      const std::string endpoint = shape.endpoint;
      if (endpoint == "images") {
        const auto references = next(2);
        q = {quantity("item:image", stated(3)), quantity("item:input_image", references), quantity("input", std::nullopt, Source::InputLimit),
             quantity("input_image", references), quantity("output", std::nullopt, Source::OutputLimit)};
        used = {"image", "input_image"};
      } else if (endpoint == "decisions") {
        q = {quantity("input", std::nullopt, Source::InputLimit), quantity("output", std::nullopt, Source::OutputLimit)};
      } else if (endpoint == "embeddings") {
        q = {quantity("input", std::nullopt, Source::InputLimit, 1 + next(2))};
      } else if (endpoint == "rerank") {
        q = {quantity("item:document", next(4)), quantity("item:search_unit", std::nullopt), quantity("input", std::nullopt, Source::InputLimit)};
        used = {"document", "search_unit"};
      } else if (endpoint == "speech") {
        std::string text;
        for (std::uint64_t i = 0, n = next(5); i < n; ++i) text += pieces[next(3)];
        q = {quantity("item:character", text.size()), quantity("input", std::nullopt, Source::InputLimit), quantity("output", std::nullopt, Source::OutputLimit)};
        used = {"character"};
      } else if (endpoint == "transcription") {
        q = {quantity("item:second", std::nullopt, Source::StatedSeconds), quantity("input", std::nullopt, Source::InputLimit),
             quantity("output", std::nullopt, Source::OutputLimit)};
        g.seconds = stated(5);
        used = {"second"};
      } else {
        q = {quantity("item:second", stated(5)), quantity("input", std::nullopt, Source::InputLimit), quantity("output", std::nullopt, Source::OutputLimit)};
        used = {"second"};
      }
      g.facts = facts(endpoint, operation, q, dimensions);
      g.tier = next(4) == 0 ? "flex" : "";
      auto rate = [&](bool item) -> std::optional<std::uint64_t> {
        if (next(39) == 0) return std::nullopt;
        if (next(4) == 0) return 0;
        return item ? (1 + next(49)) * 1'000'000'000'000u + next(999) : (1 + next(4999)) * 1'000'000 + next(999);
      };
      // Mostly the call's own tier, so that a fair share of calls is priced; now and then an entry priced only in the
      // other tier.
      const std::string other_tier = g.tier.empty() ? "flex" : "";
      const bool elsewhere = next(19) == 0;
      auto tier = [&]() { return elsewhere || next(4) == 0 ? other_tier : g.tier; };
      auto band = [&]() { return next(2) == 0 ? std::uint64_t{1000} : std::uint64_t{0}; };
      for (const auto* name : tokens) {
        const bool base = std::string_view(name) == "input" || std::string_view(name) == "output";
        if (next(base ? 9 : 2) == 0) continue;
        for (std::uint64_t i = 0, n = 1 + next(1); i < n; ++i) {
          Row r{name, false, {}, tier(), i == 0 ? 0 : band(), rate(false)};
          g.rows[key_of(r)] = r;
        }
      }
      for (const auto* kind : kinds) {
        const bool counted = std::find(used.begin(), used.end(), kind) != used.end();
        for (std::uint64_t i = 0, n = counted ? 1 + next(2) : (next(11) == 0 ? 1 : 0); i < n; ++i) {
          Row r{kind, true, {}, tier(), band(), rate(true)};
          if (next(2) == 0) r.qualifier["quality"] = next(1) ? "high" : "low";
          if (next(2) == 0) r.qualifier["size"] = next(1) ? "a" : "b";
          if (next(3) == 0) r.qualifier["operation"] = next(1) ? operation : std::string("other");
          g.rows[key_of(r)] = r;
        }
      }
      if (next(4)) g.input_limit = 500 + next(2500);
      if (next(4) == 0) g.context_window = 500 + next(2500);
      if (next(4)) g.output_limit = 100 + next(900);
      if (next(1)) g.measured = next(4000);
      g.attempts = 1 + next(2);
      g.charge_on_accept = next(1) == 0;

      std::string json = R"({"version":1,"models":[{"id":"vendor/model")";
      if (g.input_limit) json += ",\"input_limit\":" + std::to_string(*g.input_limit);
      if (g.context_window) json += ",\"context_window\":" + std::to_string(*g.context_window);
      if (g.output_limit) json += ",\"output_limit\":" + std::to_string(*g.output_limit);
      if (!g.rows.empty()) {
        json += R"(,"pricing":{)";
        bool first = true;
        for (const auto& [key, r] : g.rows) {
          json += (first ? "\"" : ",\"") + key + "\":\"" + (r.rate ? usd(*r.rate) : std::string("-1")) + "\"";
          first = false;
        }
        json += "}";
      }
      const auto c = catalogue(json + "}]}");
      Call call{g.facts, g.tier, g.measured, g.seconds, g.attempts, g.charge_on_accept};
      const auto b = bound(call, *c);
      Oracle oracle(g);
      const auto expected = oracle.expect();
      std::vector<std::pair<UnboundedBy, std::string>> got;
      for (const auto& reason : b.unbounded) got.emplace_back(reason.by, reason.subject);
      auto want = expected.reasons;
      std::sort(got.begin(), got.end());
      std::sort(want.begin(), want.end());
      if (got != want || (b.total ? std::optional<std::uint64_t>(b.total->nano_usd) : std::nullopt) != expected.total) {
        std::string detail = std::string(shape.endpoint) + " round " + std::to_string(round) + ": " + json + " bound ";
        detail += b.total ? std::to_string(b.total->nano_usd) : "none";
        for (const auto& [by, subject] : got) detail += " " + std::string(pricing::to_string(by)) + "(" + subject + ")";
        detail += " expected ";
        detail += expected.total ? std::to_string(*expected.total) : "none";
        for (const auto& [by, subject] : want) detail += " " + std::string(pricing::to_string(by)) + "(" + subject + ")";
        throw std::runtime_error(detail);
      }
      if (!b.total) {
        ++refused;
        for (const auto& reason : b.unbounded) ++reasons[reason.by];
        continue;
      }
      ++bounded;
      const auto total = b.total->nano_usd;
      results += consistent_results(oracle, [&](std::uint64_t exact) { CHECK(exact <= total); });
    }
  }
  // Every kind of reason the generator can cause, and both answers in good number.
  for (const auto by : {UnboundedBy::ModelPrice, UnboundedBy::TierPrice, UnboundedBy::InputBound, UnboundedBy::OutputCap, UnboundedBy::Quantity, UnboundedBy::Unmodeled})
    CHECK(reasons[by] > 0);
  const auto counts = std::to_string(calls) + " calls, " + std::to_string(bounded) + " bounded, " + std::to_string(refused) + " refused, " +
                      std::to_string(results) + " results checked";
  if (!(bounded > calls / 5 && refused > calls / 5)) throw std::runtime_error("too few of one answer: " + counts);
  std::cout << "case passed: the bound covers every consistent result (" << calls << " calls, " << bounded << " bounded, " << refused
            << " refused, " << results << " results checked)\n";
}
}  // namespace

int main() {
  try {
    bound_covers_every_consistent_result();
    images(); decisions(); embeddings(); rerank(); speech(); transcription(); videos();
    billing(); refusals(); zeros(); bands(); attempts(); overflow();
    std::cout << "endpoint pricing properties passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "endpoint pricing test failed: " << error.what() << '\n';
    return 1;
  }
}
