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
  // A tier with item rows only: the token units it prices at the standard rates have no row in it, which is not the
  // tier's price for them.
  const auto items_only = entry({{"input", "0.000001"}, {"output", "0.000002"}, {"item:image/flex", "0.02"}});
  const auto no_tokens = bound(call, *items_only);
  CHECK(only(no_tokens, UnboundedBy::TierPrice, "vendor/model") && no_tokens.unbounded[0].detail.find("input") != std::string::npos);
  // With no standard rows of them either, the token units have no rate in the tier.
  const auto flex_only = entry({{"item:image/flex", "0.02"}});
  const auto unlisted = bound(call, *flex_only);
  CHECK(refuses(unlisted, UnboundedBy::ModelPrice, "input") && refuses(unlisted, UnboundedBy::ModelPrice, "output") && unlisted.unbounded.size() == 2);
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

// The image tokens of the input are among its input tokens: an image quantity the input quantities cannot hold refuses.
void image_tokens() {
  constexpr std::uint64_t input = 1'000'000'000, image = 3'000'000'000;
  const auto c = entry({{"input", usd(input)}, {"input_image", usd(image)}, {"output", "0"}});
  const auto no_image_rate = entry({{"input", usd(input)}, {"output", "0"}});
  // 1000 input tokens, 400 of them image tokens: one input line of 1000 at the dearest input rate.
  Call call{facts("images", "edit", {quantity("input", 1000), quantity("input_image", 400), quantity("output", 0)})};
  auto b = bound(call, *c);
  CHECK(totals(b, rounded(1000 * image)) && b.lines.size() == 1 && line(b, "input", "input_image", 1000, image));
  call.facts = facts("images", "edit", {quantity("input", 400), quantity("input_image", 400)});
  CHECK(totals(bound(call, *c), rounded(400 * image)));
  // A stated input with an unstated image count: the image tokens are among the stated input.
  call.facts = facts("images", "edit", {quantity("input", 1000), quantity("input_image", std::nullopt)});
  CHECK(totals(bound(call, *c), rounded(1000 * image)));
  // Image tokens and no input quantity, with the image rate and without it, stated and unstated.
  call.facts = facts("images", "edit", {quantity("input_image", 1'000'000)});
  CHECK(only(bound(call, *c), UnboundedBy::Unmodeled, "input_image"));
  CHECK(only(bound(call, *no_image_rate), UnboundedBy::Unmodeled, "input_image"));
  call.facts = facts("images", "edit", {quantity("input_image", std::nullopt)});
  CHECK(only(bound(call, *c), UnboundedBy::Unmodeled, "input_image"));
  // More image tokens than input tokens, and image tokens beside an input of none.
  call.facts = facts("images", "edit", {quantity("input", 100), quantity("input_image", 1'000'000)});
  CHECK(only(bound(call, *c), UnboundedBy::Unmodeled, "input_image"));
  call.facts = facts("images", "edit", {quantity("input", 0), quantity("input_image", 5)});
  CHECK(only(bound(call, *c), UnboundedBy::Unmodeled, "input_image"));
  // No image tokens need no input.
  call.facts = facts("images", "edit", {quantity("input_image", 0)});
  CHECK(totals(bound(call, *no_image_rate), 0));
  // An unstated input keeps its rule: it needs a count unless every rate is a declared zero, and then the image rate
  // must still be listed.
  call.facts = facts("images", "edit", {quantity("input", std::nullopt, Source::InputLimit), quantity("input_image", 5)});
  CHECK(totals(bound(call, *entry({{"input", "0"}, {"input_image", "0"}, {"output", "0"}})), 0));
  CHECK(refuses(bound(call, *entry({{"input", "0"}, {"output", "0"}})), UnboundedBy::ModelPrice, "input_image"));
  CHECK(only(bound(call, *c), UnboundedBy::InputBound, "input"));
  std::cout << "case passed: image tokens are among the input tokens\n";
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

// With the input length not stated, a unit has a rate only when a row of it applies at every length, a row without a
// band in the call's tier; the rate is then the dearest over every row. A unit whose rows are all banded has no row
// below its band. A stated length picks the bands as before. The estimate, without the usage's input, refuses the same.
void unstated_length() {
  constexpr std::uint64_t rate = 1'000'000'000, image = 4 * cent, fee = 1'000'000'000'000;
  const auto decide = [](std::optional<std::uint64_t> measured) {
    return Call{facts("decisions", "decide", {quantity("input", std::nullopt, Source::InputLimit), quantity("output", 10)}), {}, measured};
  };
  const auto none = [](const Call& call, const descriptor::ModelCatalogue& c) {
    pricing::EndpointCounted counted;
    counted.model = ServedModel{call.facts.model, call.facts.model, ServedRelation::Exact};
    return pricing::endpoint_estimate(call.facts, counted, c);
  };
  // Input priced only above a band, at zero and above zero: unstated, refused; above the band, bounded; below, refused.
  const auto zero_band = entry({{"input+200000", "0"}, {"output", "0"}});
  // With the count unknown too, the input also has no count (InputBound), as for any input without a rate.
  CHECK(refuses(bound(decide(std::nullopt), *zero_band), UnboundedBy::ModelPrice, "input"));
  CHECK(totals(bound(decide(200001), *zero_band), 0));
  CHECK(only(bound(decide(1000), *zero_band), UnboundedBy::ModelPrice, "input"));
  CHECK(!none(decide(std::nullopt), *zero_band).total);
  const auto rate_band = entry({{"input+200000", usd(rate)}, {"output", "0"}});
  CHECK(refuses(bound(decide(std::nullopt), *rate_band), UnboundedBy::ModelPrice, "input"));
  CHECK(totals(bound(decide(200001), *rate_band), rounded(200001 * rate)));
  CHECK(only(bound(decide(1000), *rate_band), UnboundedBy::ModelPrice, "input"));
  // The same for the output set; a banded row of another member beside the base row is taken at its dearest.
  CHECK(only(bound(decide(std::nullopt), *entry({{"input", "0"}, {"output+200000", "0"}})), UnboundedBy::ModelPrice, "output"));
  CHECK(totals(bound(decide(std::nullopt), *entry({{"input", "0"}, {"cache_read+200000", "0"}, {"output", "0"}})), 0));
  // An item priced only above a band, at zero and above zero.
  const auto images = [](std::optional<std::uint64_t> measured, Dimensions dimensions = {}) {
    return Call{facts("images", "generate", {quantity("item:image", 1), quantity("input", std::nullopt, Source::InputLimit), quantity("output", 0)},
                      std::move(dimensions)), {}, measured};
  };
  for (const std::uint64_t price : {std::uint64_t{0}, image}) {
    const auto item_band = entry({{"input", "0"}, {"output", "0"}, {"item:image+200000", usd(price)}});
    CHECK(only(bound(images(std::nullopt), *item_band), UnboundedBy::ModelPrice, "item:image[operation=generate]"));
    CHECK(totals(bound(images(200001), *item_band), rounded(price)));
    CHECK(only(bound(images(1000), *item_band), UnboundedBy::ModelPrice, "item:image[operation=generate]"));
    CHECK(!none(images(std::nullopt), *item_band).total);
  }
  // Only banded rows of the qualifier the call offers, beside a row without a band of another qualifier.
  const auto qualified = entry({{"input", "0"}, {"output", "0"}, {"item:image[quality=low]", usd(image)}, {"item:image[quality=high]+200000", usd(image)}});
  CHECK(only(bound(images(std::nullopt, {{"quality", "high"}}), *qualified), UnboundedBy::ModelPrice, "item:image[operation=generate,quality=high]"));
  CHECK(totals(bound(images(std::nullopt, {{"quality", "low"}}), *qualified), rounded(image)));
  // A row without a band beside banded ones: the dearest of every row, as before.
  CHECK(totals(bound(images(std::nullopt), *entry({{"input", "0"}, {"output", "0"}, {"item:image", usd(image)}, {"item:image+200000", usd(2 * image)}})),
               rounded(2 * image)));
  // A request fee only above a band: unstated, refused; above the band, charged; below it no fee row applies, as before.
  const auto fee_band = entry({{"input", "0"}, {"output", "0"}, {"request+200000", usd(fee)}});
  CHECK(only(bound(decide(std::nullopt), *fee_band), UnboundedBy::ModelPrice, "request"));
  CHECK(totals(bound(decide(200001), *fee_band), rounded(fee)));
  CHECK(totals(bound(decide(1000), *fee_band), 0));
  CHECK(!none(decide(std::nullopt), *fee_band).total);
  // The image tokens of an input, a unit the call needs when it has references.
  const auto references = Call{facts("images", "edit", {quantity("input", std::nullopt, Source::InputLimit), quantity("input_image", std::nullopt, Source::InputLimit),
                                                        quantity("output", 0)})};
  CHECK(refuses(bound(references, *entry({{"input", "0"}, {"input_image+200000", "0"}, {"output", "0"}})), UnboundedBy::ModelPrice, "input_image"));
  CHECK(totals(bound(references, *entry({{"input", "0"}, {"input_image", "0"}, {"output", "0"}})), 0));
  std::cout << "case passed: an unstated input length needs a row without a band\n";
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

// The estimate of a finished call (pricing::endpoint_estimate) over hand-written facts and results. -----------------
using Names = std::vector<std::string>;
using Items = std::vector<std::pair<std::string, std::uint64_t>>;
Usage tokens(std::optional<std::uint64_t> input, std::optional<std::uint64_t> output, std::uint64_t cache_read = 0, std::uint64_t reasoning = 0) {
  Usage u;
  if (input) u.input_total = Count{*input, Evidence::Reported};
  if (output) u.output_total = Count{*output, Evidence::Reported};
  if (cache_read) u.cache_read = Count{cache_read, Evidence::Reported};
  if (reasoning) u.reasoning = Count{reasoning, Evidence::Reported};
  u.stage = UsageStage::Final;
  return u;
}
pricing::EndpointEstimate estimated(const EndpointFacts& f, const Usage* u, Items items, const descriptor::ModelCatalogue& c,
                                    std::optional<std::uint64_t> image_tokens = std::nullopt, std::string_view tier = {}) {
  pricing::EndpointCounted counted;
  counted.usage = u;
  counted.items = std::move(items);
  counted.input_image_tokens = image_tokens;
  counted.model = ServedModel{f.model, f.model, ServedRelation::Exact};
  return pricing::endpoint_estimate(f, counted, c, tier);
}
bool estimates(const pricing::EndpointEstimate& e, std::uint64_t nano) {
  return e.total && e.total->nano_usd == nano && e.total->evidence == Evidence::Derived && e.total->rounding == CostRounding::CeilingCatalogueProduct &&
         e.unpriced.empty() && e.entry == "vendor/model" && !e.requested_entry;
}
bool unpriced(const pricing::EndpointEstimate& e, const Names& names) { return !e.total && e.lines.empty() && e.unpriced == names; }
bool line(const pricing::EndpointEstimate& e, std::string_view unit, std::string_view row, std::uint64_t quantity, std::uint64_t rate) {
  return std::any_of(e.lines.begin(), e.lines.end(), [&](const pricing::EndpointLine& l) {
    return l.unit == unit && l.row == row && l.quantity == quantity && l.rate == rate;
  });
}

// Images: the tokens as the chat estimate prices them, the input's image tokens at their own rate, the request fee, the
// images returned at the most specific row, and the references; fewer images than asked mark a lower bound, more are
// priced as returned and named beyond the bound.
void images_estimate() {
  constexpr std::uint64_t input = 5'000'000'000, cache_read = 500'000'000, input_image = 10'000'000'000, output = 40'000'000'000,
                          request = 1'000'000'000'000, image = 4 * cent, high = 17 * cent, high_square = 16 * cent, reference = 2 * cent;
  const auto c = entry({{"input", usd(input)}, {"cache_read", usd(cache_read)}, {"input_image", usd(input_image)}, {"output", usd(output)},
                        {"request", usd(request)}, {"item:image", usd(image)}, {"item:image[quality=high]", usd(high)},
                        {"item:image[quality=high,size=1024x1024]", usd(high_square)}, {"item:input_image", usd(reference)}},
                       R"("input_limit":32000,"output_limit":6000)");
  // The references are a count the request fixes.
  auto quantities = [](std::optional<std::uint64_t> count, std::uint64_t references) {
    auto taken = quantity("item:input_image", references);
    taken.exact = true;
    return std::vector{quantity("item:image", count), taken, quantity("input", std::nullopt, Source::InputLimit),
                       quantity("input_image", references), quantity("output", std::nullopt, Source::OutputLimit)};
  };
  const Dimensions square{{"quality", "high"}, {"size", "1024x1024"}};
  // 1200 input tokens, 200 of them cache reads and 400 image tokens; 3000 output tokens; two images and one reference.
  auto f = facts("images", "generate", quantities(2, 1), square);
  const auto u = tokens(1200, 3000, 200);
  auto e = estimated(f, &u, {{"item:image", 2}}, *c, 400);
  const auto exact = rounded(600 * input + 400 * input_image + 200 * cache_read + 3000 * output + request + 2 * high_square + reference);
  CHECK(estimates(e, exact) && e.assumptions.empty() && !e.lower_bound);
  CHECK(line(e, "input", "input", 600, input) && line(e, "input", "input_image", 400, input_image) && line(e, "input", "cache_read", 200, cache_read) &&
        line(e, "output", "output", 3000, output) && line(e, "request", "request", 1, request) &&
        line(e, "item:image", "item:image[quality=high,size=1024x1024]", 2, high_square) && line(e, "item:input_image", "item:input_image", 1, reference) &&
        e.lines.size() == 7);
  // The bound of the same call at the measured input is at least the estimate, and nothing exceeds it.
  Call call{f, {}, 1200};
  auto b = bound(call, *c);
  CHECK(b.total && b.total->nano_usd >= e.total->nano_usd && pricing::beyond_bound(b, e).empty());
  // Fewer images than asked: priced as returned, and the provider may bill the others.
  f = facts("images", "generate", quantities(3, 1), square);
  e = estimated(f, &u, {{"item:image", 1}}, *c, 400);
  CHECK(estimates(e, rounded(600 * input + 400 * input_image + 200 * cache_read + 3000 * output + request + high_square + reference)) && e.lower_bound);
  CHECK(line(e, "item:image", "item:image[quality=high,size=1024x1024]", 1, high_square));
  // More images than asked: priced as returned; the bound of one image is exceeded by them and by the total.
  const auto items_only = entry({{"input", "0"}, {"output", "0"}, {"item:image", usd(image)}, {"item:image[quality=high]", usd(high)},
                                 {"item:image[quality=high,size=1024x1024]", usd(high_square)}, {"item:input_image", "0"}, {"input_image", "0"}},
                                R"("input_limit":32000,"output_limit":6000)");
  f = facts("images", "generate", quantities(1, 0), square);
  call = Call{f, {}, 1200};
  b = bound(call, *items_only);
  CHECK(totals(b, rounded(high)));
  const auto none = tokens(1200, 3000);
  e = estimated(f, &none, {{"item:image", 3}}, *items_only);
  CHECK(estimates(e, rounded(3 * high_square)) && !e.lower_bound && e.total->nano_usd > b.total->nano_usd);
  CHECK(pricing::beyond_bound(b, e) == Names({"item:image", "total"}));
  // No count asked: the bound has none, the estimate prices the images returned, and nothing marks a lower bound.
  f = facts("images", "generate", quantities(std::nullopt, 0), square);
  e = estimated(f, &none, {{"item:image", 2}}, *items_only);
  CHECK(estimates(e, rounded(2 * high_square)) && !e.lower_bound);
  // Image tokens beyond the uncached input, and image tokens with no rate of their own.
  f = facts("images", "edit", quantities(1, 1));
  e = estimated(f, &u, {{"item:image", 1}}, *c, 1001);
  CHECK(unpriced(e, {"input_image"}));
  const auto no_image_rate = entry({{"input", usd(input)}, {"output", usd(output)}, {"item:image", usd(image)}, {"item:input_image", usd(reference)}});
  e = estimated(f, &u, {{"item:image", 1}}, *no_image_rate, 400);
  CHECK(unpriced(e, {"input_image"}));
  e = estimated(f, &u, {{"item:image", 1}}, *no_image_rate, 0);
  CHECK(estimates(e, rounded(1200 * input + 3000 * output + image + reference)) && e.assumptions == Names({"cache_read at input"}));
  std::cout << "case passed: the images estimate\n";
}

// One rounding: half a nano-USD of tokens and half of an item, at the bound's own quantities, are one nano-USD, which is
// the bound; rounded apart they would be two.
void estimate_rounds_once() {
  const auto c = entry({{"input", usd(500'000)}, {"output", "0"}, {"item:image", usd(500'000)}});
  const auto f = facts("images", "generate", {quantity("item:image", 1), quantity("input", 1), quantity("output", 0)});
  const auto u = tokens(1, 0);
  const auto e = estimated(f, &u, {{"item:image", 1}}, *c);
  const auto b = bound(Call{f}, *c);
  CHECK(totals(b, 1) && estimates(e, 1) && pricing::beyond_bound(b, e).empty());
  std::cout << "case passed: the estimate rounds once\n";
}

// Tokens: the chat estimate's own sum; a side the operation does not count needs no counter; a usage that reports no
// counter needs declared zeros; the bands, the tier and a variable row.
void token_estimates() {
  constexpr std::uint64_t input = 300'000'000, output = 1'200'000'000, cache_read = 30'000'000, reasoning = 2'400'000'000, request = 7'000'000;
  const auto c = entry({{"prompt", usd(input)}, {"completion", usd(output)}, {"input_cache_read", usd(cache_read)}, {"internal_reasoning", usd(reasoning)},
                        {"request", usd(request)}, {"input/flex", usd(input / 2)}, {"output/flex", usd(output / 2)}},
                       R"("input_limit":50000,"output_limit":1000)");
  const auto decide = facts("decisions", "decide", {quantity("input", std::nullopt, Source::InputLimit), quantity("output", std::nullopt, Source::OutputLimit)});
  auto u = tokens(700, 300, 100, 50);
  auto e = estimated(decide, &u, {}, *c);
  CHECK(estimates(e, rounded(600 * input + 100 * cache_read + 250 * output + 50 * reasoning + request)));
  CHECK(line(e, "input", "prompt", 600, input) && line(e, "input", "input_cache_read", 100, cache_read) && line(e, "output", "completion", 250, output) &&
        line(e, "output", "internal_reasoning", 50, reasoning) && line(e, "request", "request", 1, request));
  // The same sum as the chat estimate of the same usage.
  const auto chat = pricing::estimate(*c, "vendor.endpoint", origin, ServedModel{"vendor/model", "vendor/model", ServedRelation::Exact}, u);
  CHECK(chat.total && chat.total->nano_usd == e.total->nano_usd && chat.assumptions == e.assumptions);
  // A tier's own rates; a unit priced at the standard rates without a row in the tier (here the cache reads, the
  // reasoning and the request fee) is not priced at another of the tier's rates.
  CHECK(unpriced(estimated(decide, &u, {}, *c, std::nullopt, "flex"), {"tier"}));
  const auto flex = entry({{"prompt", usd(input)}, {"completion", usd(output)}, {"input/flex", usd(input / 2)}, {"output/flex", usd(output / 2)}},
                          R"("input_limit":50000,"output_limit":1000)");
  e = estimated(decide, &u, {}, *flex, std::nullopt, "flex");
  CHECK(estimates(e, rounded(700 * (input / 2) + 300 * (output / 2))) && e.assumptions == Names({"cache_read at input"}));
  CHECK(unpriced(estimated(decide, &u, {}, *c, std::nullopt, "priority"), {"tier"}));
  // A counter the operation needs and the usage lacks.
  auto partial = tokens(700, std::nullopt);
  CHECK(unpriced(estimated(decide, &partial, {}, *c), {"output"}));
  partial = tokens(std::nullopt, 300);
  CHECK(unpriced(estimated(decide, &partial, {}, *c), {"input"}));
  // A partial usage is a lower bound.
  u.stage = UsageStage::Partial;
  CHECK(estimated(decide, &u, {}, *c).lower_bound);
  // Embeddings count no output: an input counter alone prices the call.
  const auto embed = facts("embeddings", "embed", {quantity("input", std::nullopt, Source::InputLimit, 3)});
  const auto input_only = tokens(900, std::nullopt);
  CHECK(estimates(estimated(embed, &input_only, {}, *c), rounded(900 * input + request)));
  // No counter at all: the operation's token rates must be declared zeros.
  const auto none = Usage{};
  CHECK(unpriced(estimated(embed, &none, {}, *c), {"input"}));
  CHECK(unpriced(estimated(embed, nullptr, {}, *c), {"input"}));
  const auto zero = entry({{"input", "0"}, {"output", "0"}, {"request", usd(request)}});
  CHECK(estimates(estimated(embed, nullptr, {}, *zero), rounded(request)));
  CHECK(unpriced(estimated(decide, nullptr, {}, *entry({{"input", "0"}, {"output", "0.000001"}})), {"output"}));
  // Bands: the usage's input tokens pick them; a variable row above the band leaves the input unpriced above it only.
  const auto banded = entry({{"input", usd(input)}, {"input+1000", "-1"}, {"output", usd(output)}, {"output+500", usd(2 * output)}});
  u = tokens(1000, 10);
  CHECK(estimates(estimated(decide, &u, {}, *banded), rounded(1000 * input + 10 * 2 * output)));
  u = tokens(500, 10);
  CHECK(estimates(estimated(decide, &u, {}, *banded), rounded(500 * input + 10 * output)));
  u = tokens(1001, 10);
  CHECK(unpriced(estimated(decide, &u, {}, *banded), {"input"}));
  // Without the usage's input tokens no band can be chosen: a banded fee or item row leaves no total.
  const auto speak = facts("speech", "speak", {quantity("item:character", 10), quantity("input", std::nullopt, Source::InputLimit),
                                               quantity("output", std::nullopt, Source::OutputLimit)});
  const auto banded_fee = entry({{"input", "0"}, {"output", "0"}, {"item:character", usd(cent)}, {"request+1000", usd(request)}});
  CHECK(unpriced(estimated(speak, nullptr, {}, *banded_fee), {"input"}));
  const auto banded_item = entry({{"input", "0"}, {"output", "0"}, {"item:character", usd(cent)}, {"item:character+1000", usd(2 * cent)}});
  CHECK(unpriced(estimated(speak, nullptr, {}, *banded_item), {"input"}));
  std::cout << "case passed: token estimates\n";
}

// Each kind of item count: what the result counts, a quantity the request fixes, the request's bound with an assumption,
// and none; zero counts and declared zeros; units the operation cannot count.
void item_estimates() {
  constexpr std::uint64_t document = 100'000'000'000, search_unit = 2 * cent, character = 15'000'500'000, second = 10 * cent, high_second = 30 * cent;
  // Rerank: the documents the request fixes, the search units the result counts; the input at a declared zero.
  const auto ranks = entry({{"input", "0"}, {"item:document", usd(document)}, {"item:search_unit", usd(search_unit)}});
  const auto rank = facts("rerank", "rank", {quantity("item:document", 3), quantity("item:search_unit", std::nullopt), quantity("input", std::nullopt, Source::InputLimit)});
  auto with_exact = rank;
  with_exact.quantities[0].exact = true;
  Usage units;
  units.total = Count{40, Evidence::Reported};
  units.stage = UsageStage::Final;
  auto e = estimated(with_exact, &units, {{"item:search_unit", 2}}, *ranks);
  CHECK(estimates(e, rounded(3 * document + 2 * search_unit)) && e.assumptions.empty());
  CHECK(line(e, "item:document", "item:document", 3, document) && line(e, "item:search_unit", "item:search_unit", 2, search_unit));
  CHECK(unpriced(estimated(with_exact, &units, {}, *ranks), {"item:search_unit"}));
  // A declared zero needs no count; a count of zero needs no rate.
  CHECK(estimates(estimated(with_exact, &units, {}, *entry({{"input", "0"}, {"item:document", usd(document)}, {"item:search_unit", "0"}})), rounded(3 * document)));
  CHECK(estimates(estimated(with_exact, &units, {{"item:search_unit", 0}}, *entry({{"input", "0"}, {"item:document", usd(document)}})), rounded(3 * document)));
  // An input rate above zero with no input counter.
  CHECK(unpriced(estimated(with_exact, &units, {{"item:search_unit", 2}}, *entry({{"input", "0.000001"}, {"item:document", usd(document)},
                                                                                {"item:search_unit", usd(search_unit)}})), {"input"}));
  // A count the request states but does not fix is the most it allows, assumed.
  e = estimated(rank, &units, {{"item:search_unit", 2}}, *ranks);
  CHECK(estimates(e, rounded(3 * document + 2 * search_unit)) && e.assumptions == Names({"item:document at the request's bound"}));
  // Speech: the request's bytes; a usage that counts nothing; a row qualified by a value no dimension holds.
  const auto speaker = entry({{"input", "0"}, {"output", "0"}, {"item:character", usd(character)}});
  auto speak = facts("speech", "speak", {quantity("item:character", 10), quantity("input", std::nullopt, Source::InputLimit),
                                         quantity("output", std::nullopt, Source::OutputLimit)}, {{"voice", "alloy"}});
  speak.quantities[0].exact = true;
  const Usage nothing;
  CHECK(estimates(estimated(speak, &nothing, {}, *speaker), rounded(10 * character)));
  auto paced = speak;
  paced.dimensions = {{"speed", ""}, {"voice", "alloy"}};
  CHECK(unpriced(estimated(paced, &nothing, {}, *entry({{"input", "0"}, {"output", "0"}, {"item:character", usd(character)},
                                                        {"item:character[speed=1.5]", usd(2 * character)}})), {"item:character"}));
  // A unit the operation cannot count, priced above zero, and the same at a declared zero.
  CHECK(unpriced(estimated(speak, &nothing, {}, *entry({{"input", "0"}, {"output", "0"}, {"item:character", usd(character)}, {"item:second", "0.01"}})),
                 {"item:second"}));
  CHECK(estimates(estimated(speak, &nothing, {}, *entry({{"input", "0"}, {"output", "0"}, {"item:character", usd(character)}, {"item:second", "0"}})),
                  rounded(10 * character)));
  // Transcription: the reported seconds, above the seconds the host stated for the bound.
  const auto transcriber = entry({{"input", "0"}, {"output", "0"}, {"item:second", usd(second)}});
  const auto transcribe = facts("transcription", "transcribe", {quantity("item:second", std::nullopt, Source::StatedSeconds),
                                                                quantity("input", std::nullopt, Source::InputLimit), quantity("output", std::nullopt, Source::OutputLimit)});
  const auto heard = tokens(0, 0);
  e = estimated(transcribe, &heard, {{"item:second", 7}}, *transcriber);
  CHECK(estimates(e, rounded(7 * second)) && line(e, "item:second", "item:second", 7, second));
  const auto stated = bound(Call{transcribe, {}, std::nullopt, 5}, *transcriber);
  CHECK(totals(stated, rounded(5 * second)) && pricing::beyond_bound(stated, e) == Names({"item:second", "total"}));
  CHECK(unpriced(estimated(transcribe, &heard, {}, *transcriber), {"item:second"}));
  // Videos: no count of seconds in the result, so the request's duration, assumed; a resolution row.
  const auto video = entry({{"input", "0"}, {"output", "0"}, {"item:second", usd(second)}, {"item:second[resolution=1080p]", usd(high_second)}});
  const auto submit = facts("videos", "submit", {quantity("item:second", 8), quantity("input", std::nullopt, Source::InputLimit),
                                                 quantity("output", std::nullopt, Source::OutputLimit)}, {{"resolution", "1080p"}});
  e = estimated(submit, &nothing, {}, *video);
  CHECK(estimates(e, rounded(8 * high_second)) && e.assumptions == Names({"item:second at the request's bound"}));
  CHECK(unpriced(estimated(facts("videos", "submit", {quantity("item:second", std::nullopt), quantity("input", 0), quantity("output", 0)}), &nothing, {}, *video),
                 {"item:second"}));
  // A variable row that applies, and another that does not.
  const auto variable = entry({{"input", "0"}, {"output", "0"}, {"item:second", usd(second)}, {"item:second[resolution=1080p]", "-1"}});
  CHECK(unpriced(estimated(submit, &nothing, {}, *variable), {"item:second"}));
  auto low = submit;
  low.dimensions = {{"resolution", "720p"}};
  CHECK(estimates(estimated(low, &nothing, {}, *variable), rounded(8 * second)));
  // A quantity of a unit the estimate does not price; then the entry's seconds are a unit the call does not count.
  CHECK(unpriced(estimated(facts("videos", "submit", {quantity("pixels", 4)}), &nothing, {}, *video), {"pixels", "item:second"}));
  std::cout << "case passed: item estimates\n";
}

// What the policy states about the operation, the entry, the dimensions and the gateway.
void estimate_refusals() {
  const auto c = entry({{"input", "0.000001"}, {"output", "0.000002"}, {"item:image", "0.04"}}, R"("input_limit":1000,"output_limit":100)");
  const auto quantities = std::vector{quantity("item:image", 1), quantity("input", std::nullopt, Source::InputLimit), quantity("output", std::nullopt, Source::OutputLimit)};
  auto f = facts("images", "generate", quantities);
  const auto u = tokens(100, 10);
  CHECK(estimated(f, &u, {{"item:image", 1}}, *c).total);
  // Not billed: zero, from nothing the result reports and with no entry needed.
  const auto empty = catalogue(R"({"version":1,"models":[]})");
  auto poll = facts("videos", "poll", {});
  poll.billing = descriptor::EndpointBilling::None;
  auto e = estimated(poll, &u, {}, *empty);
  CHECK(e.total && e.total->nano_usd == 0 && e.unpriced.empty() && e.lines.empty());
  f.billing = descriptor::EndpointBilling::Unstated;
  CHECK(unpriced(estimated(f, &u, {}, *c), {"billing"}));
  f.billing = descriptor::EndpointBilling::Carried;
  CHECK(unpriced(estimated(f, &u, {}, *c), {"carried"}));
  f = facts("images", "generate", quantities);
  // No entry, no price in the tier, and a served model's own entry.
  f.model = "vendor/unknown";
  CHECK(unpriced(estimated(f, &u, {{"item:image", 1}}, *c), {"model"}));
  f.model = "vendor/model";
  CHECK(unpriced(estimated(f, &u, {{"item:image", 1}}, *c, std::nullopt, "flex"), {"tier"}));
  const auto two = catalogue(R"({"version":1,"models":[{"id":"vendor/model","pricing":{"input":"0.000001","output":"0.000002","item:image":"0.04"}},
    {"id":"vendor/served","pricing":{"input":"0.000002","output":"0.000002","item:image":"0.05"}}]})");
  pricing::EndpointCounted counted;
  counted.usage = &u;
  counted.items = {{"item:image", 1}};
  counted.model = ServedModel{"vendor/model", "vendor/served", ServedRelation::Routed};
  e = pricing::endpoint_estimate(f, counted, *two);
  CHECK(e.total && e.entry == "vendor/served" && !e.requested_entry && e.total->nano_usd == rounded(110 * std::uint64_t{2'000'000'000} + 5 * cent));
  counted.model.served = "vendor/elsewhere";
  e = pricing::endpoint_estimate(f, counted, *two);
  CHECK(e.total && e.entry == "vendor/model" && e.requested_entry);
  // Dimensions out of order, an operation other than the call's, and no operation.
  f = facts("images", "generate", quantities, {{"size", "a"}, {"quality", "high"}});
  CHECK(unpriced(estimated(f, &u, {{"item:image", 1}}, *c), {"dimensions"}));
  f = facts("images", "generate", quantities, {{"operation", "edit"}});
  CHECK(unpriced(estimated(f, &u, {{"item:image", 1}}, *c), {"dimensions"}));
  f = facts("images", "", quantities);
  CHECK(unpriced(estimated(f, &u, {{"item:image", 1}}, *c), {"operation"}));
  // At a routing gateway the bound has none, and the estimate is at the listed rates.
  f = facts("images", "generate", quantities);
  f.routed_gateway = true;
  e = estimated(f, &u, {{"item:image", 1}}, *c);
  CHECK(e.total && e.assumptions == Names({"listed rates"}));
  // Tokens above the measured count the bound took, priced as reported and named beyond it.
  f.routed_gateway = false;
  const auto b = bound(Call{f, {}, 50}, *c);
  e = estimated(f, &u, {{"item:image", 1}}, *c);
  CHECK(b.total && e.total && pricing::beyond_bound(b, e) == Names({"input"}));
  // Past 64 bits.
  const auto huge = entry({{"input", "0"}, {"output", "0"}, {"item:image", "18000"}});
  CHECK(unpriced(estimated(facts("images", "generate", {quantity("item:image", most)}), nullptr, {{"item:image", most}}, *huge), {"overflow"}));
  std::cout << "case passed: estimate refusals\n";
}

// The input length picks the bands only when the usage reports an input counter (input, uncached input, cache reads or
// writes). Output counters alone do not state it, so a banded row of the output, the request fee or an item unit leaves
// no total, as it does when the usage reports no counter.
void estimate_length_from_input() {
  constexpr std::uint64_t output = 2'000'000'000, long_output = 6'000'000'000, image = 4 * cent, long_image = 8 * cent, request = 1'000'000'000'000;
  auto images_asked = quantity("item:image", 2);
  images_asked.exact = true;
  const auto pictures = facts("images", "generate", {images_asked, quantity("output", 1000)});
  const auto banded = entry({{"input", "0"}, {"output", usd(output)}, {"output+128000", usd(long_output)}, {"item:image", usd(image)},
                             {"item:image+128000", usd(long_image)}});
  const auto output_only = tokens(std::nullopt, 1000);
  CHECK(unpriced(estimated(pictures, &output_only, {{"item:image", 2}}, *banded), {"input"}));
  // With an input counter the length picks the band: below it, above it, and from the uncached input alone.
  auto u = tokens(100, 1000);
  CHECK(estimates(estimated(pictures, &u, {{"item:image", 2}}, *banded), rounded(1000 * output + 2 * image)));
  u = tokens(200'000, 1000);
  CHECK(estimates(estimated(pictures, &u, {{"item:image", 2}}, *banded), rounded(1000 * long_output + 2 * long_image)));
  u = tokens(std::nullopt, 1000);
  u.input_uncached = Count{200'000, Evidence::Reported};
  CHECK(estimates(estimated(pictures, &u, {{"item:image", 2}}, *banded), rounded(1000 * long_output + 2 * long_image)));
  // A banded output row alone, and a banded request fee alone.
  const auto decide = facts("decisions", "decide", {quantity("output", 1000)});
  CHECK(unpriced(estimated(decide, &output_only, {}, *entry({{"output", usd(output)}, {"output+128000", usd(long_output)}})), {"input"}));
  CHECK(unpriced(estimated(decide, &output_only, {}, *entry({{"output", usd(output)}, {"request", usd(request)}, {"request+128000", usd(2 * request)}})),
                 {"input"}));
  // Without a band the output counter alone prices the call.
  CHECK(estimates(estimated(decide, &output_only, {}, *entry({{"output", usd(output)}, {"request", usd(request)}})), rounded(1000 * output + request)));
  std::cout << "case passed: the input length is known only from an input counter\n";
}

// The estimate prices the served model's entry and the bound the requested one: when they differ, beyond_bound names
// `entry`, whether the estimate is above the bound or not.
void estimate_by_another_entry() {
  constexpr std::uint64_t cheap = 1'000'000'000, dear = 8'000'000'000;
  const auto c = catalogue(R"({"version":1,"models":[{"id":"vendor/model","pricing":{"input":")" + usd(cheap) + R"(","output":"0"}},
    {"id":"vendor/served","pricing":{"input":")" + usd(dear) + R"(","output":"0"}},
    {"id":"vendor/cheaper","pricing":{"input":")" + usd(cheap / 2) + R"(","output":"0"}}]})");
  const auto f = facts("embeddings", "embed", {quantity("input", 2'000'000)});
  const auto b = bound(Call{f}, *c);
  CHECK(totals(b, rounded(2'000'000 * cheap)) && b.entry == "vendor/model");
  const auto u = tokens(2'000'000, std::nullopt);
  pricing::EndpointCounted counted;
  counted.usage = &u;
  counted.model = ServedModel{"vendor/model", "vendor/served", ServedRelation::Routed};
  auto e = pricing::endpoint_estimate(f, counted, *c);
  CHECK(e.total && e.entry == "vendor/served" && !e.requested_entry && e.total->nano_usd == rounded(2'000'000 * dear));
  CHECK(pricing::beyond_bound(b, e) == Names({"entry", "total"}));
  counted.model.served = "vendor/cheaper";
  e = pricing::endpoint_estimate(f, counted, *c);
  CHECK(e.total && e.entry == "vendor/cheaper" && e.total->nano_usd == rounded(2'000'000 * (cheap / 2)) && pricing::beyond_bound(b, e) == Names({"entry"}));
  // The requested model's entry, served as it is or because the served model has none: nothing is named.
  counted.model = ServedModel{"vendor/model", "vendor/model", ServedRelation::Exact};
  CHECK(pricing::beyond_bound(b, pricing::endpoint_estimate(f, counted, *c)).empty());
  counted.model = ServedModel{"vendor/model", "vendor/elsewhere", ServedRelation::Routed};
  e = pricing::endpoint_estimate(f, counted, *c);
  CHECK(e.total && e.requested_entry && pricing::beyond_bound(b, e).empty());
  std::cout << "case passed: an estimate by another entry than the bound's is named\n";
}

// A quantity of at most zero on a token side: tokens the usage reports on that side are priced as reported and named
// beyond the bound, whose line of the side has none.
void estimate_zero_quantity() {
  constexpr std::uint64_t input = 2'000'000'000, output = 3'000'000'000;
  const auto c = entry({{"input", usd(input)}, {"output", usd(output)}});
  auto f = facts("decisions", "decide", {quantity("input", 0), quantity("output", 100)});
  auto b = bound(Call{f}, *c);
  CHECK(totals(b, rounded(100 * output)));
  auto u = tokens(1'000'000, 50);
  auto e = estimated(f, &u, {}, *c);
  CHECK(estimates(e, rounded(1'000'000 * input + 50 * output)) && line(e, "input", "input", 1'000'000, input));
  CHECK(pricing::beyond_bound(b, e) == Names({"input", "total"}));
  f = facts("decisions", "decide", {quantity("input", 100), quantity("output", 0)});
  b = bound(Call{f}, *c);
  CHECK(totals(b, rounded(100 * input)));
  u = tokens(50, 1000);
  e = estimated(f, &u, {}, *c);
  CHECK(estimates(e, rounded(50 * input + 1000 * output)) && line(e, "output", "output", 1000, output));
  CHECK(pricing::beyond_bound(b, e) == Names({"output", "total"}));
  std::cout << "case passed: tokens on a side of at most zero are priced and named\n";
}

// The input length rebuilt from the uncached input, the cache reads and the cache writes past 64 bits is above every
// band, not the wrapped remainder.
void estimate_length_overflow() {
  constexpr std::uint64_t output = 1'000'000'000, long_output = 3'000'000'000;
  const auto c = entry({{"input", "0"}, {"output", usd(output)}, {"output+1000", usd(long_output)}});
  const auto f = facts("decisions", "decide", {quantity("input", std::nullopt, Source::InputLimit), quantity("output", std::nullopt, Source::OutputLimit)});
  Usage u;
  u.input_uncached = Count{most - 499, Evidence::Reported};
  u.cache_read = Count{1000, Evidence::Reported};
  u.output_total = Count{1'000'000, Evidence::Reported};
  u.stage = UsageStage::Final;
  const auto e = estimated(f, &u, {}, *c);
  CHECK(estimates(e, rounded(1'000'000 * long_output)) && line(e, "output", "output+1000", 1'000'000, long_output));
  std::cout << "case passed: an input length past 64 bits is above every band\n";
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
  // The value the call offers for `name`: empty for a member whose value it does not state; nothing when it offers none.
  std::optional<std::string> value_of(const std::string& name) const {
    for (const auto& [key, value] : offered)
      if (key == name) return value;
    return std::nullopt;
  }
  // A row that names a dimension the call offers without a value may apply.
  bool names_unstated(const Row& r) const {
    return std::any_of(r.qualifier.begin(), r.qualifier.end(), [&](const auto& d) { const auto v = value_of(d.first); return v && v->empty(); });
  }
  bool may_apply(const Row& r, std::uint64_t input_length) const {
    return r.tier == g.tier && (r.above == 0 || input_length > r.above) && std::all_of(r.qualifier.begin(), r.qualifier.end(), [&](const auto& d) {
             const auto v = value_of(d.first);
             return v && (v->empty() || *v == d.second);
           });
  }
  // The unit with the call's operation and the offered dimensions some row of the unit names: no other value is printed.
  std::string with_dimensions(const std::string& unit) const {
    std::string out = unit + "[";
    for (const auto& [name, value] : offered) {
      const bool read = name == "operation" || std::any_of(g.rows.begin(), g.rows.end(), [&](const auto& row) {
        return row.second.item && "item:" + row.second.name == unit && row.second.qualifier.count(name);
      });
      if (read) out += (out.back() == '[' ? "" : ",") + name + (value.empty() ? "" : "=" + value);
    }
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
    // Under a tier, a unit (a token unit, or an item kind whatever its qualifier) with a row at the standard rates and
    // none in the tier.
    const auto unit_of = [](const Row& r) { return r.item ? "item:" + r.name : r.name; };
    if (!g.tier.empty() && std::any_of(g.rows.begin(), g.rows.end(), [&](const auto& item) {
          return item.second.tier.empty() && std::none_of(g.rows.begin(), g.rows.end(), [&](const auto& other) {
                   return other.second.tier == g.tier && unit_of(other.second) == unit_of(item.second);
                 });
        })) {
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
    // Image tokens are among the input tokens.
    std::optional<std::uint64_t> image_count = 0;
    for (const auto& q : g.facts.quantities)
      if (q.unit == "input_image") image_count = count(q) && image_count ? std::optional<std::uint64_t>(*image_count + *count(q)) : std::nullopt;
    if (image_tokens && (!has_input || (input_count && image_count && *image_count > *input_count))) refuse(UnboundedBy::Unmodeled, "input_image");
    std::uint64_t sum = 0;
    // A token unit: the dearest fixed rate of its rows that apply; whether one of them is variable; whether `base` has one.
    auto token = [&](const std::vector<std::string>& units, std::string_view base, std::optional<std::uint64_t> n, UnboundedBy unknown,
                     const std::string& unit, bool images) {
      if (n && *n == 0) return;
      std::optional<std::uint64_t> dearest;
      bool variable = false, based = false, imaged = false, based_everywhere = false, imaged_everywhere = false;
      for (const auto& [key, r] : g.rows) {
        if (r.item || std::find(units.begin(), units.end(), r.name) == units.end() || !applies(r, length)) continue;
        if (!r.rate) { variable = true; continue; }
        if (r.name == base) { based = true; based_everywhere = based_everywhere || r.above == 0; }
        if (r.name == "input_image") { imaged = true; imaged_everywhere = imaged_everywhere || r.above == 0; }
        if (!dearest || *r.rate > *dearest) dearest = r.rate;
      }
      // With the length not stated, the call may be above no band: the units it needs must have a row without one.
      const bool unstated = length == most;
      bool ok = true;
      if (variable) { refuse(UnboundedBy::ModelPrice, unit); ok = false; }
      if (!based) { refuse(UnboundedBy::ModelPrice, unit); ok = false; }
      else if (unstated && !based_everywhere) { refuse(UnboundedBy::ModelPrice, unit); ok = false; }
      if (images && !imaged) { refuse(UnboundedBy::ModelPrice, "input_image"); ok = false; }
      else if (images && unstated && !imaged_everywhere) { refuse(UnboundedBy::ModelPrice, "input_image"); ok = false; }
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
        // A row of the unit in the tier, in any band, that names a member the call does not state the value of.
        if (std::any_of(g.rows.begin(), g.rows.end(), [&](const auto& item) {
              const auto& r = item.second;
              return r.item && "item:" + r.name == q.unit && r.tier == g.tier && names_unstated(r);
            })) {
          refuse(UnboundedBy::ModelPrice, with_dimensions(q.unit));
          continue;
        }
        std::optional<std::uint64_t> dearest;
        bool variable = false, everywhere = false;
        for (const auto& [key, r] : g.rows) {
          if (!r.item || "item:" + r.name != q.unit || !applies(r, length)) continue;
          everywhere = everywhere || r.above == 0;
          if (!r.rate) variable = true;
          else if (!dearest || *r.rate > *dearest) dearest = r.rate;
        }
        if (variable || !dearest || (length == most && !everywhere)) refuse(UnboundedBy::ModelPrice, with_dimensions(q.unit));
        else if (*dearest > 0 && !n) refuse(UnboundedBy::Quantity, q.unit);
        else if (n) sum += *n * *dearest;
      }
    }
    // The request fee.
    std::optional<std::uint64_t> fee;
    bool fee_everywhere = false;
    for (const auto& [key, r] : g.rows) {
      if (r.item || r.name != "request" || !applies(r, length)) continue;
      if (!r.rate) { refuse(UnboundedBy::ModelPrice, "request"); fee.reset(); break; }
      fee_everywhere = fee_everywhere || r.above == 0;
      if (!fee || *r.rate > *fee) fee = r.rate;
    }
    if (fee && length == most && !fee_everywhere) refuse(UnboundedBy::ModelPrice, "request");
    else if (fee) sum += *fee;
    // Item rows of kinds no quantity counts.
    for (const auto& [key, r] : g.rows)
      if (r.item && std::find(kinds.begin(), kinds.end(), r.name) == kinds.end() && may_apply(r, length) && !(r.rate && *r.rate == 0))
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
      // A member the call has without a value it can state is offered with an empty value.
      if (const auto quality = next(4)) dimensions.emplace_back("quality", quality == 1 ? "high" : quality == 2 ? "low" : quality == 3 ? "high" : "");
      if (next(2)) dimensions.emplace_back("size", next(1) ? "a" : "b");
      if (next(5) == 0) dimensions.emplace_back("style", "");
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
      // other tier. A flex call's other tier is another tier than the standard rates, whose rows refuse it when the
      // tier has no row of their unit; such a row is added below now and then.
      const std::string other_tier = g.tier.empty() ? "flex" : "priority";
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
          if (next(5) == 0) r.qualifier["style"] = "vivid";
          g.rows[key_of(r)] = r;
        }
      }
      // A flex call's entry with a row at the standard rates: a refusal unless the tier has a row of its unit.
      if (!g.tier.empty() && next(3) == 0) {
        Row r{tokens[next(5)], false, {}, "", 0, rate(false)};
        g.rows[key_of(r)] = r;
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

// The property of the estimate: for generated catalogues (token rates, declared zeros, qualified and unqualified item
// rows, some qualified by the operation, tiers, bands, variable prices, rows naming a member the call offers without a
// value), calls and results within the request (each counted quantity at most the bound's count of its unit), the
// estimate is at most the bound of the same call whenever both have a total, and at most the bound at the result's own
// quantities. When the bound has no total for a price, the estimate has a total only where the result counted none of
// the unit that lacks the price, which is decided here from the generated rows. A result beyond its request is named by
// beyond_bound.
void estimate_within_the_bound() {
  std::uint64_t seed = 20261125;
  auto next = [&](std::uint64_t limit) { seed = seed * 6364136223846793005u + 1442695040888963407u; return (seed >> 33) % (limit + 1); };
  // A second stream for the calls a model other than the requested one serves, so the calls above are generated as before.
  std::uint64_t side_seed = 20261120;
  auto side = [&](std::uint64_t limit) { side_seed = side_seed * 6364136223846793005u + 1442695040888963407u; return (side_seed >> 33) % (limit + 1); };
  std::size_t served_calls = 0, served_above = 0, served_named = 0;
  struct Shape {
    const char* endpoint;
    std::vector<const char*> operations;
  };
  const Shape shapes[]{{"images", {"generate", "edit"}}, {"decisions", {"decide"}}, {"embeddings", {"embed"}}, {"rerank", {"rank"}},
                       {"speech", {"speak"}}, {"transcription", {"transcribe", "translate"}}, {"videos", {"submit"}}};
  const char* const kinds[]{"image", "input_image", "document", "search_unit", "character", "second"};
  const char* const token_names[]{"input", "cache_read", "input_image", "output", "reasoning", "request"};
  std::size_t calls = 0, both = 0, bound_only = 0, estimate_only = 0, neither = 0, tight = 0, beyond = 0;
  std::map<std::string, std::size_t> explained;
  for (const auto& shape : shapes) {
    for (int round = 0; round < 2000; ++round, ++calls) {
      Generated g;
      const std::string endpoint = shape.endpoint;
      const std::string operation = shape.operations[next(shape.operations.size() - 1)];
      Dimensions dimensions;
      if (const auto quality = next(4)) dimensions.emplace_back("quality", quality == 1 ? "high" : quality == 2 ? "low" : quality == 3 ? "high" : "");
      if (next(2)) dimensions.emplace_back("size", next(1) ? "a" : "b");
      if (next(5) == 0) dimensions.emplace_back("style", "");
      auto stated = [&](std::uint64_t top) { return next(6) == 0 ? std::optional<std::uint64_t>() : std::optional<std::uint64_t>(next(top)); };
      auto fixed = [](EndpointQuantity q) { q.exact = true; return q; };
      std::vector<EndpointQuantity> q;
      std::vector<std::string> used;
      if (endpoint == "images") {
        const auto references = next(2);
        q = {quantity("item:image", stated(3)), fixed(quantity("item:input_image", references)), quantity("input", std::nullopt, Source::InputLimit),
             quantity("output", std::nullopt, Source::OutputLimit)};
        if (references) q.push_back(quantity("input_image", std::nullopt, Source::InputLimit));
        used = {"image", "input_image"};
      } else if (endpoint == "decisions") {
        q = {quantity("input", std::nullopt, Source::InputLimit), quantity("output", std::nullopt, Source::OutputLimit)};
      } else if (endpoint == "embeddings") {
        q = {quantity("input", std::nullopt, Source::InputLimit, 1 + next(2))};
      } else if (endpoint == "rerank") {
        q = {fixed(quantity("item:document", next(4))), quantity("item:search_unit", std::nullopt), quantity("input", std::nullopt, Source::InputLimit)};
        used = {"document", "search_unit"};
      } else if (endpoint == "speech") {
        q = {fixed(quantity("item:character", next(12))), quantity("input", std::nullopt, Source::InputLimit), quantity("output", std::nullopt, Source::OutputLimit)};
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
        if (next(29) == 0) return std::nullopt;
        if (next(3) == 0) return 0;
        return item ? (1 + next(49)) * 1'000'000'000'000u + next(999) : (1 + next(4999)) * 1'000'000 + next(999);
      };
      const std::string other_tier = g.tier.empty() ? "flex" : "";
      const bool elsewhere = next(29) == 0;
      auto tier = [&]() { return elsewhere || next(9) == 0 ? other_tier : g.tier; };
      auto band = [&]() { return next(2) == 0 ? std::uint64_t{1000} : std::uint64_t{0}; };
      for (const auto* name : token_names) {
        const bool base = std::string_view(name) == "input" || std::string_view(name) == "output";
        if (next(base ? 25 : 2) == 0) continue;
        for (std::uint64_t i = 0, n = 1 + next(1); i < n; ++i) {
          Row r{name, false, {}, tier(), i == 0 ? 0 : band(), rate(false)};
          g.rows[key_of(r)] = r;
        }
      }
      for (const auto* kind : kinds) {
        const bool counted = std::find(used.begin(), used.end(), kind) != used.end();
        // The first row of a kind the call counts has no qualifier and no band, so that a fair share of calls is priced.
        for (std::uint64_t i = 0, n = counted ? 1 + next(2) : (next(25) == 0 ? 1 : 0); i < n; ++i) {
          Row r{kind, true, {}, tier(), next(3) == 0 && !(counted && i == 0) ? std::uint64_t{1000} : std::uint64_t{0}, rate(true)};
          if (counted && i == 0) { g.rows[key_of(r)] = r; continue; }
          if (next(2) == 0) r.qualifier["quality"] = next(1) ? "high" : "low";
          if (next(2) == 0) r.qualifier["size"] = next(1) ? "a" : "b";
          if (next(3) == 0) r.qualifier["operation"] = next(1) ? operation : std::string("other");
          if (next(7) == 0) r.qualifier["style"] = "vivid";
          g.rows[key_of(r)] = r;
        }
      }
      if (next(6)) g.input_limit = 500 + next(2500);
      if (next(5) == 0) g.context_window = 500 + next(2500);
      if (next(7)) g.output_limit = 100 + next(900);
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
      const auto b = bound(Call{g.facts, g.tier, g.measured, g.seconds, g.attempts, g.charge_on_accept}, *c);
      const Oracle oracle(g);

      // A result within the request: each count at most the bound's count of its unit.
      std::optional<std::uint64_t> input_count, output_count, image_count;
      bool has_output = false, has_images = false;
      for (const auto& quantity_of : g.facts.quantities) {
        if (quantity_of.unit == "input") input_count = oracle.count(quantity_of);
        if (quantity_of.unit == "output") { has_output = true; output_count = oracle.count(quantity_of); }
        if (quantity_of.unit == "input_image") { has_images = true; image_count = oracle.count(quantity_of); }
      }
      const bool report = next(endpoint == "speech" || endpoint == "videos" ? 1 : 5) != 0;
      Usage usage;
      std::uint64_t total_in = 0, cache_read = 0, images = 0, total_out = 0, reasoning = 0;
      if (report) {
        total_in = input_count ? next(*input_count) : next(3000);
        if (next(1)) cache_read = next(total_in);
        usage.input_total = Count{total_in, Evidence::Reported};
        if (cache_read) usage.cache_read = Count{cache_read, Evidence::Reported};
        if (has_images && next(1)) images = next(std::min(total_in - cache_read, image_count.value_or(total_in - cache_read)));
        if (has_output) {
          total_out = output_count ? next(*output_count) : next(2000);
          if (next(1)) reasoning = next(total_out);
          usage.output_total = Count{total_out, Evidence::Reported};
          if (reasoning) usage.reasoning = Count{reasoning, Evidence::Reported};
        }
        usage.stage = UsageStage::Final;
      }
      pricing::EndpointCounted counted;
      counted.usage = report || next(1) ? &usage : nullptr;
      if (report && has_images) counted.input_image_tokens = images;
      counted.model = ServedModel{g.facts.model, next(1) ? g.facts.model : std::string(), next(1) ? ServedRelation::Exact : ServedRelation::NotReported};
      for (const auto& quantity_of : g.facts.quantities) {
        const auto n = oracle.count(quantity_of);
        if (quantity_of.unit == "item:image") counted.items.emplace_back(quantity_of.unit, n ? next(*n) : next(3));
        if (quantity_of.unit == "item:search_unit" && next(3)) counted.items.emplace_back(quantity_of.unit, next(5));
        if (quantity_of.unit == "item:second" && quantity_of.source == Source::StatedSeconds && next(3))
          counted.items.emplace_back(quantity_of.unit, n ? next(*n) : next(5));
      }
      const auto e = pricing::endpoint_estimate(g.facts, counted, *c, g.tier);

      // The bound at the result's own quantities, for one attempt.
      auto at_result = g.facts;
      for (auto& quantity_of : at_result.quantities) {
        if (report && quantity_of.unit == "input") quantity_of.at_most = total_in;
        if (report && quantity_of.unit == "output") quantity_of.at_most = total_out;
        if (report && quantity_of.unit == "input_image") quantity_of.at_most = images;
        for (const auto& [unit, n] : counted.items)
          if (unit == quantity_of.unit) quantity_of.at_most = n;
      }
      const auto own = bound(Call{at_result, g.tier, g.measured, g.seconds, 1, true}, *c);

      const auto describe = [&](const std::string& why) {
        std::string detail = std::string(shape.endpoint) + " round " + std::to_string(round) + ": " + why + "; " + json + " estimate ";
        detail += e.total ? std::to_string(e.total->nano_usd) : "none";
        for (const auto& name : e.unpriced) detail += " " + name;
        detail += " bound ";
        detail += b.total ? std::to_string(b.total->nano_usd) : "none";
        for (const auto& reason : b.unbounded) detail += " " + std::string(pricing::to_string(reason.by)) + "(" + reason.subject + ")";
        detail += " own ";
        detail += own.total ? std::to_string(own.total->nano_usd) : "none";
        for (const auto& reason : own.unbounded) detail += " " + std::string(pricing::to_string(reason.by)) + "(" + reason.subject + ")";
        return detail;
      };
      if (e.total && own.total) {
        ++tight;
        if (e.total->nano_usd > own.total->nano_usd) throw std::runtime_error(describe("the estimate is above the bound at its own quantities"));
      }
      if (b.total && e.total) {
        ++both;
        // The invariant holds for a result priced by the entry the bound used, the requested model's.
        if (e.entry != b.entry) throw std::runtime_error(describe("the estimate is priced by another entry than the bound's"));
        if (e.total->nano_usd > b.total->nano_usd) throw std::runtime_error(describe("the estimate is above the bound"));
        if (!pricing::beyond_bound(b, e).empty()) throw std::runtime_error(describe("a result within the request is named beyond the bound"));
      } else if (b.total) {
        ++bound_only;
      } else if (!e.total) {
        ++neither;
      } else {
        ++estimate_only;
      }
      // The estimate has a total and the bound at the result's own quantities has none: each of its reasons must be a
      // unit the result counted none of, decided from the generated rows, or a count the estimate needs no more.
      std::string own_why;
      if (e.total && !own.total) {
        // The rows of a token unit that apply at the result's own input length.
        const auto rows_at = [&](const std::string& name, bool variable) {
          return std::any_of(g.rows.begin(), g.rows.end(), [&](const auto& item) {
            const auto& r = item.second;
            return !r.item && r.name == name && r.tier == g.tier && (r.above == 0 || total_in > r.above) && r.rate.has_value() != variable;
          });
        };
        const auto text = total_in - cache_read - images;
        const auto read_listed = rows_at("cache_read", false) || rows_at("cache_read", true);
        const auto reasoning_priced = rows_at("reasoning", false) && !rows_at("reasoning", true);
        const auto at_input = text + (read_listed ? 0 : cache_read), at_output = total_out - (reasoning_priced ? reasoning : 0);
        bool free_item = false;
        for (const auto& reason : own.unbounded) {
          bool zero = false;
          if (report && reason.by == UnboundedBy::ModelPrice && reason.subject == "input")
            zero = (rows_at("input", false) && !rows_at("input", true) ? true : at_input == 0) && (!rows_at("cache_read", true) || cache_read == 0) &&
                   (!rows_at("input_image", true) || images == 0);
          if (report && reason.by == UnboundedBy::ModelPrice && reason.subject == "output")
            zero = (rows_at("output", false) && !rows_at("output", true) ? true : at_output == 0) && (!rows_at("reasoning", true) || reasoning == 0);
          // A count the request does not state, of an item unit whose dearest row is above zero and whose most specific
          // row, the estimate's, is a declared zero.
          if (reason.by == UnboundedBy::Quantity) {
            const auto rate = pricing::item_price(*c->entry("vendor/model"), reason.subject, oracle.offered, g.tier, report ? total_in : 0);
            zero = rate && rate->first == 0;
            free_item = free_item || zero;
          }
          if (!zero) throw std::runtime_error(describe("the estimate prices a unit the bound at the result's quantities cannot"));
        }
        own_why = free_item ? "the most specific row of an item unit is a declared zero, so the estimate needs no count the request leaves out"
                            : "the bound's token rate is missing or variable for a counter the result counted zero of";
      }
      if (e.total && !b.total) {
        // Why the bound has no total while the estimate has one.
        const bool counts_only = std::all_of(b.unbounded.begin(), b.unbounded.end(), [](const pricing::UnboundedReason& r) {
          return r.by == UnboundedBy::InputBound || r.by == UnboundedBy::OutputCap || r.by == UnboundedBy::Quantity;
        });
        // A row with a variable price, or of a kind the call does not count, in a band the request may reach and the
        // result's input does not.
        const bool banded = std::any_of(g.rows.begin(), g.rows.end(), [&](const auto& item) {
          const auto& r = item.second;
          const bool uncounted = r.item && std::none_of(g.facts.quantities.begin(), g.facts.quantities.end(),
                                                        [&](const EndpointQuantity& x) { return x.unit == "item:" + r.name; });
          return r.tier == g.tier && r.above > 0 && total_in <= r.above && (!input_count || *input_count > r.above) && (!r.rate || uncounted);
        });
        if (!own.total) ++explained[own_why];
        else if (counts_only) ++explained["the request states no count the bound needs; the result counts it"];
        else if (banded) ++explained["a row the bound cannot price is in a band the request may reach and the result's input does not"];
        else ++explained["a unit the bound cannot price is counted zero by the result"];
      }

      // The same result served by another model: its entry has the requested entry's rows at other rates (half, the
      // same, twice or three times), or prices only another tier, so that the requested entry prices the call.
      if (side(3) == 0) {
        ++served_calls;
        const auto factor = side(3);
        const bool other_only = side(5) == 0;
        std::string rows;
        for (const auto& [key, r] : g.rows) {
          // Moved to the other tier, a row of the call's tier keeps a key of its own; the rows already there are left out.
          if (other_only && r.tier != g.tier) continue;
          Row moved = r;
          if (other_only) moved.tier = other_tier;
          const auto rate = r.rate ? std::optional<std::uint64_t>(factor == 0 ? *r.rate / 2 : *r.rate * factor) : std::nullopt;
          rows += (rows.empty() ? "\"" : ",\"") + key_of(moved) + "\":\"" + (rate ? usd(*rate) : std::string("-1")) + "\"";
        }
        const std::string served = R"({"id":"vendor/served")" + (rows.empty() ? std::string() : R"(,"pricing":{)" + rows + "}");
        const auto with_served = catalogue(json + "}," + served + "}]}");
        auto by_served = counted;
        by_served.model = ServedModel{g.facts.model, "vendor/served", ServedRelation::Routed};
        const auto s = pricing::endpoint_estimate(g.facts, by_served, *with_served, g.tier);
        const auto sb = bound(Call{g.facts, g.tier, g.measured, g.seconds, g.attempts, g.charge_on_accept}, *with_served);
        if (sb.total && s.total) {
          const auto names = pricing::beyond_bound(sb, s);
          if (s.entry == sb.entry) {
            if (s.total->nano_usd > sb.total->nano_usd || !names.empty())
              throw std::runtime_error(describe("a result priced by the bound's entry, the served model having none in the tier, exceeds the bound"));
          } else {
            ++served_named;
            if (names.empty() || names.front() != "entry") throw std::runtime_error(describe("an estimate by another entry than the bound's is not named"));
            const bool above = s.total->nano_usd > sb.total->nano_usd;
            served_above += above ? 1 : 0;
            if (above != (std::find(names.begin(), names.end(), "total") != names.end()))
              throw std::runtime_error(describe("an estimate by another entry above the bound is not named so"));
          }
        }
      }

      // A result beyond its request: more images than asked, more seconds than stated, more input than counted.
      if (next(3) == 0) {
        auto more = counted;
        std::string unit;
        for (auto& [name, n] : more.items) {
          for (const auto& quantity_of : g.facts.quantities) {
            const auto top = oracle.count(quantity_of);
            if (quantity_of.unit == name && top && unit.empty() && (name == "item:image" || name == "item:second")) {
              n = *top + 1 + next(2);
              unit = name;
            }
          }
        }
        Usage larger = usage;
        if (unit.empty() && report && input_count) {
          larger.input_total = Count{*input_count + 1 + next(100), Evidence::Reported};
          more.usage = &larger;
          unit = "input";
        }
        if (!unit.empty()) {
          const auto above = pricing::endpoint_estimate(g.facts, more, *c, g.tier);
          // A unit priced at zero by every line of both costs nothing at any count, and is not named.
          const auto billed = [&](const std::vector<pricing::EndpointLine>& lines) {
            return std::any_of(lines.begin(), lines.end(), [&](const pricing::EndpointLine& l) { return l.unit == unit && l.rate > 0; });
          };
          if (b.total && above.total && (billed(b.lines) || billed(above.lines))) {
            ++beyond;
            const auto names = pricing::beyond_bound(b, above);
            if (std::find(names.begin(), names.end(), unit) == names.end()) throw std::runtime_error(describe("a result beyond the request is not named: " + unit));
          }
        }
      }
    }
  }
  std::string counts = std::to_string(calls) + " calls, " + std::to_string(both) + " both present, " + std::to_string(bound_only) + " bound only, " +
                       std::to_string(estimate_only) + " estimate only, " + std::to_string(neither) + " neither, " + std::to_string(tight) +
                       " at the result's own bound, " + std::to_string(beyond) + " beyond the request named";
  std::cout << "estimate property: " << counts << "\n";
  for (const auto& [why, n] : explained) std::cout << "  estimate only, " << n << ": " << why << "\n";
  if (both < calls / 5) throw std::runtime_error("too few calls with both totals: " + counts);
  if (beyond == 0 || tight < calls / 5) throw std::runtime_error("too few results checked: " + counts);
  std::cout << "estimate property, served by another model: " << served_calls << " calls, " << served_named << " named by entry, " << served_above
            << " above the bound\n";
  if (served_named == 0 || served_above == 0) throw std::runtime_error("too few calls served by another model");
  std::cout << "case passed: the estimate stays within the bound\n";
}
}  // namespace

// Every case runs, and each failing one is reported, so one failure does not hide the cases after it.
// Every member of a bound, so a failed comparison shows what the code answered.
std::string described(const pricing::EndpointBound& b) {
  std::string out = b.total ? "total " + std::to_string(b.total->nano_usd) : std::string("no total");
  for (const auto& reason : b.unbounded) out += " | " + std::string(pricing::to_string(reason.by)) + " [" + reason.subject + "] " + reason.detail;
  out += " | entry '" + b.entry + "' attempts " + std::to_string(b.attempts) + " lines";
  for (const auto& l : b.lines) out += " " + l.unit + "=" + l.row + ":" + std::to_string(l.quantity) + "x" + std::to_string(l.rate);
  return out;
}
#define SAME(actual, expected) do { const std::string got_ = (actual), want_ = (expected); \
  if (got_ != want_) throw std::runtime_error("line " + std::to_string(__LINE__) + ":\n    got      " + got_ + "\n    expected " + want_); } while (false)
CarriedRequest carried(std::string id, std::string model, std::optional<std::uint64_t> output, std::optional<std::uint64_t> invocations = 1,
                       std::string tier = {}) {
  CarriedRequest r;
  r.custom_id = std::move(id);
  r.model = std::move(model);
  r.output_tokens = output;
  r.invocations = invocations;
  r.facts.service_tier = std::move(tier);
  return r;
}
// A batch submit's facts: the requests it carries, priced under the tier `batch`.
Call submit(std::vector<CarriedRequest> requests) {
  Call call{facts("batches", "submit", {})};
  call.facts.billing = descriptor::EndpointBilling::Carried;
  call.facts.carried_tier = "batch";
  call.facts.model.clear();
  call.facts.carried = std::move(requests);
  return call;
}

// A call that bills the chat requests it carries: each request bounded on its own at the carried tier and summed.
void carried_requests() {
  // Batch rates with a fraction of a nano-USD per token; the standard rates are dearer.
  constexpr std::uint64_t in = 500'000'001, out = 2'000'000'003, cheap_in = 250'000'000, cheap_out = 1'000'000'000, fee = 1'500'000'000'007;
  const auto c = catalogue(std::string(R"({"version":1,"models":[)") +
      R"({"id":"vendor/model","input_limit":2000,"output_limit":100,"pricing":{"input":")" + usd(1'000'000'000) + R"(","output":")" + usd(4'000'000'000) +
      R"(","input/batch":")" + usd(in) + R"(","output/batch":")" + usd(out) + R"("}},)" +
      R"({"id":"vendor/other","input_limit":3000,"pricing":{"input/batch":")" + usd(cheap_in) + R"(","output/batch":")" + usd(cheap_out) +
      R"(","request/batch":")" + usd(fee) + R"("}},)" +
      R"({"id":"vendor/standard","input_limit":2000,"pricing":{"input":")" + usd(1'000'000'000) + R"(","output":")" + usd(4'000'000'000) + R"("}},)" +
      R"({"id":"vendor/cached","input_limit":2000,"pricing":{"input":")" + usd(1'000'000'000) + R"(","output":")" + usd(4'000'000'000) +
      R"(","cache_read":")" + usd(100'000'000) + R"(","input/batch":")" + usd(in) + R"(","output/batch":")" + usd(out) + R"("}}]})");
  const auto first = rounded(300 * in + 16 * out), second = rounded(3000 * cheap_in + 50 * cheap_out + fee);
  auto call = submit({carried("b-1", "vendor/model", 16), carried("b-2", "vendor/other", 50)});
  call.attempts = 3;
  // One attempt: the submit is charged on acceptance. The measured input of b-1 replaces its model's input limit.
  auto bounded = [&](const Call& at, std::vector<std::pair<std::string, std::uint64_t>> measured) {
    pricing::EndpointWorstCaseInput input;
    input.facts = &at.facts;
    input.service_tier = at.tier;
    input.attempts = at.attempts;
    input.charge_on_accept = at.charge_on_accept;
    input.measured_carried_input_tokens = std::move(measured);
    return pricing::endpoint_worst_case(input, *c);
  };
  SAME(described(bounded(call, {{"b-1", 300}, {"elsewhere", 1}})),
       "total " + std::to_string(first + second) + " | entry '' attempts 1 lines b-1:nano_usd=vendor/model:" + std::to_string(first) +
       "x1000000 b-2:nano_usd=vendor/other:" + std::to_string(second) + "x1000000");
  // Not charged on acceptance: every attempt bills the whole batch.
  call.charge_on_accept = false;
  CHECK(totals(bounded(call, {{"b-1", 300}}), 3 * (first + second)));
  call.attempts = 0;
  SAME(described(bounded(call, {})), "no total | Invocations [] the call states no attempts | entry '' attempts 0 lines");
  call.attempts = 1;
  call.charge_on_accept = true;
  // One entry for every request is the bound's entry; the service tier of the input is not read for carried requests.
  auto same_model = submit({carried("b-1", "vendor/model", 16), carried("b-2", "vendor/model", 20)});
  same_model.tier = "flex";
  SAME(described(bounded(same_model, {})), "total " + std::to_string(rounded(2000 * in + 16 * out) + rounded(2000 * in + 20 * out)) +
       " | entry 'vendor/model' attempts 1 lines b-1:nano_usd=vendor/model:" + std::to_string(rounded(2000 * in + 16 * out)) +
       "x1000000 b-2:nano_usd=vendor/model:" + std::to_string(rounded(2000 * in + 20 * out)) + "x1000000");

  // Each request's reasons keep their kind, the subject prefixed by its custom id.
  SAME(described(bounded(submit({carried("b-1", "vendor/standard", 16), carried("b-2", "vendor/model", std::nullopt), carried("b-3", "vendor/none", 5),
                                 carried("b-4", "vendor/model", 5, std::nullopt)}), {})),
       "no total | TierPrice [b-1:vendor/standard] the entry prices no unit under service tier 'batch' | OutputCap [b-2:] the request states no "
       "output cap | ModelPrice [b-3:vendor/none] the catalogue has no price for the model at the origin | Invocations [b-4:] the request does not "
       "state how many model invocations it may bill | entry '' attempts 0 lines");
  // A unit priced at the standard rates and not under the carried tier: not priced at the tier's other rates.
  SAME(described(bounded(submit({carried("b-1", "vendor/cached", 16)}), {})),
       "no total | TierPrice [b-1:vendor/cached] the entry prices cache_read at the standard rates but not under service tier 'batch' | entry '' "
       "attempts 0 lines");
  // A request of its own service tier, a custom id twice, a measured input twice.
  SAME(described(bounded(submit({carried("b-1", "vendor/model", 16, 1, "flex"), carried("b-2", "vendor/model", 16), carried("b-2", "vendor/model", 16),
                                 carried("b-3", "vendor/model", 16)}), {{"b-3", 10}, {"b-3", 10}})),
       "no total | TierPrice [b-1:vendor/model] a batch item names its own service tier | Unmodeled [b-2:] the custom id names more than one "
       "carried request | Unmodeled [b-3:] the request's measured input tokens are stated more than once | entry '' attempts 0 lines");
  // No carried tier, a routing gateway, and nothing carried.
  auto untiered = submit({carried("b-1", "vendor/model", 16)});
  untiered.facts.carried_tier.clear();
  SAME(described(bounded(untiered, {})), "no total | TierPrice [batches/submit] the call names no tier for the requests it carries | entry '' attempts 0 lines");
  // At a routing gateway each request is bounded at its entry's route ceiling under the carried tier: none listed is
  // no bound, and a ceiling at the standard rates is not the tier's.
  auto routed = submit({carried("b-1", "vendor/model", 16)});
  routed.facts.routed_gateway = true;
  SAME(described(bounded(routed, {})),
       "no total | RouteCeiling [b-1:vendor/model] the entry lists no route ceiling (ceiling_input, ceiling_output), so the router may pick any "
       "price | entry '' attempts 0 lines");
  constexpr std::uint64_t ceil_in = 700'000'001, ceil_out = 3'000'000'003;
  const auto ceilinged = catalogue(std::string(R"({"version":1,"models":[)") +
      R"({"id":"vendor/routed","input_limit":2000,"pricing":{"input":")" + usd(1'000'000'000) + R"(","output":")" + usd(4'000'000'000) +
      R"(","ceiling_input":")" + usd(1'000'000'000) + R"(","ceiling_output":")" + usd(4'000'000'000) + R"(","input/batch":")" + usd(in) +
      R"(","output/batch":")" + usd(out) + R"(","ceiling_input/batch":")" + usd(ceil_in) + R"(","ceiling_output/batch":")" + usd(ceil_out) + R"("}},)" +
      R"({"id":"vendor/untiered-ceiling","input_limit":2000,"pricing":{"input/batch":")" + usd(in) + R"(","output/batch":")" + usd(out) +
      R"(","ceiling_input":")" + usd(ceil_in) + R"(","ceiling_output":")" + usd(ceil_out) + R"("}}]})");
  auto routed_ceiling = submit({carried("b-1", "vendor/routed", 16)});
  routed_ceiling.facts.routed_gateway = true;
  pricing::EndpointWorstCaseInput at_ceiling;
  at_ceiling.facts = &routed_ceiling.facts;
  at_ceiling.attempts = 1;
  at_ceiling.charge_on_accept = true;
  SAME(described(pricing::endpoint_worst_case(at_ceiling, *ceilinged)),
       "total " + std::to_string(rounded(2000 * ceil_in + 16 * ceil_out)) + " | entry 'vendor/routed' attempts 1 lines b-1:nano_usd=vendor/routed:" +
       std::to_string(rounded(2000 * ceil_in + 16 * ceil_out)) + "x1000000");
  auto untiered_ceiling = submit({carried("b-1", "vendor/untiered-ceiling", 16)});
  untiered_ceiling.facts.routed_gateway = true;
  at_ceiling.facts = &untiered_ceiling.facts;
  SAME(described(pricing::endpoint_worst_case(at_ceiling, *ceilinged)),
       "no total | RouteCeiling [b-1:vendor/untiered-ceiling] the entry lists no route ceiling (ceiling_input, ceiling_output), so the router may "
       "pick any price | entry '' attempts 0 lines");
  auto from_file = submit({});
  from_file.facts.input_file = "file-abc";
  SAME(described(bounded(from_file, {})), "no total | Unmodeled [batches/submit] the batch's requests are in an uploaded file | entry '' attempts 0 lines");
  SAME(described(bounded(submit({}), {})), "no total | Unmodeled [batches/submit] the call states none of the requests it carries | entry '' attempts 0 lines");

  // Sums past 64 bits: of the requests, and of the attempts.
  constexpr std::uint64_t dear = 10'000'000'000'000'000'000u;  // 1e19 nano-USD per million tokens
  const auto huge = catalogue(R"({"version":1,"models":[{"id":"vendor/dear","input_limit":1,"output_limit":1000000,"pricing":{"input/batch":"0","output/batch":")" +
                              usd(dear) + R"("}}]})");
  pricing::EndpointWorstCaseInput input;
  auto one = submit({carried("b-1", "vendor/dear", 1'000'000)});
  input.facts = &one.facts;
  input.attempts = 1;
  input.charge_on_accept = true;
  CHECK(totals(pricing::endpoint_worst_case(input, *huge), 10'000'000'000'000'000'000u));
  auto two = submit({carried("b-1", "vendor/dear", 1'000'000), carried("b-2", "vendor/dear", 1'000'000)});
  input.facts = &two.facts;
  SAME(described(pricing::endpoint_worst_case(input, *huge)), "no total | Overflow [] the bound does not fit 64 bits | entry '' attempts 0 lines");
  input.facts = &one.facts;
  input.attempts = 2;
  input.charge_on_accept = false;
  SAME(described(pricing::endpoint_worst_case(input, *huge)), "no total | Overflow [] the bound over the attempts does not fit 64 bits | entry '' attempts 0 lines");
  std::cout << "case passed: carried requests\n";
}

// A tier the facts name: the call is priced at that tier's rows only, whether the input names the tier or not; an
// entry with no row in it, a unit it prices at the standard rates and not in the tier, and another tier in the input
// all refuse.
void tier_from_facts() {
  constexpr std::uint64_t input = 1'000'000'000, output = 2'000'000'000, flex_input = 500'000'000, flex_output = 1'000'000'000;
  const auto tiered = entry({{"input", usd(input)}, {"output", usd(output)}, {"item:image", usd(4 * cent)}, {"input/flex", usd(flex_input)},
                             {"output/flex", usd(flex_output)}, {"item:image/flex", usd(2 * cent)}});
  const auto quantities = std::vector{quantity("item:image", 1), quantity("input", 100), quantity("output", 100)};
  Call call{facts("images", "generate", quantities)};
  CHECK(totals(bound(call, *tiered), rounded(100 * input + 100 * output + 4 * cent)));
  call.facts.service_tier = "flex";
  auto b = bound(call, *tiered);
  CHECK(totals(b, rounded(100 * flex_input + 100 * flex_output + 2 * cent)));
  CHECK(line(b, "input", "input/flex", 100, flex_input) && line(b, "output", "output/flex", 100, flex_output) &&
        line(b, "item:image", "item:image/flex", 1, 2 * cent));
  // The same tier named by the input too; another tier there.
  call.tier = "flex";
  CHECK(totals(bound(call, *tiered), rounded(100 * flex_input + 100 * flex_output + 2 * cent)));
  call.tier = "priority";
  CHECK(only(bound(call, *tiered), UnboundedBy::TierPrice, "vendor/model"));
  call.tier.clear();
  // A tier the entry prices nothing in.
  call.facts.service_tier = "priority";
  CHECK(only(bound(call, *tiered), UnboundedBy::TierPrice, "vendor/model"));
  // A unit priced at the standard rates without a row in the tier: a token unit, the request fee and an item unit. A
  // qualified row of a unit the tier prices is not one: the tier's own rows of the unit state its price.
  call.facts.service_tier = "flex";
  CHECK(totals(bound(call, *entry({{"input", usd(input)}, {"output", usd(output)}, {"item:image", usd(4 * cent)}, {"item:image[quality=high]", usd(8 * cent)},
                                   {"input/flex", usd(flex_input)}, {"output/flex", usd(flex_output)}, {"item:image/flex", usd(2 * cent)}})),
               rounded(100 * flex_input + 100 * flex_output + 2 * cent)));
  for (const auto& [unit, rate] : std::vector<std::pair<std::string, std::uint64_t>>{{"cache_read", 100'000'000}, {"request", 1'000'000'000'000},
                                                                                     {"item:input_image", cent}}) {
    auto prices = std::vector<std::pair<std::string, std::string>>{{"input", usd(input)}, {"output", usd(output)}, {"item:image", usd(4 * cent)},
                                                                   {"input/flex", usd(flex_input)}, {"output/flex", usd(flex_output)},
                                                                   {"item:image/flex", usd(2 * cent)}, {unit, usd(rate)}};
    const auto outside = entry(prices);
    const auto refused = bound(call, *outside);
    CHECK(only(refused, UnboundedBy::TierPrice, "vendor/model"));
    CHECK(refused.unbounded[0].detail.find("under service tier 'flex'") != std::string::npos);
    // At the standard rates the same entry is bounded.
    Call standard = call;
    standard.facts.service_tier.clear();
    CHECK(bound(standard, *outside).total || unit == "item:input_image");
  }
  // The estimate prices at the facts' tier too, and refuses where the bound does.
  auto f = facts("images", "generate", quantities);
  f.service_tier = "flex";
  auto u = tokens(100, 100);
  auto e = estimated(f, &u, {{"item:image", 1}}, *tiered);
  CHECK(estimates(e, rounded(100 * flex_input + 100 * flex_output + 2 * cent)));
  CHECK(unpriced(estimated(f, &u, {{"item:image", 1}}, *tiered, std::nullopt, "priority"), {"tier"}));
  CHECK(estimates(estimated(f, &u, {{"item:image", 1}}, *tiered, std::nullopt, "flex"), rounded(100 * flex_input + 100 * flex_output + 2 * cent)));
  CHECK(unpriced(estimated(f, &u, {{"item:image", 1}}, *entry({{"input", usd(input)}, {"output", usd(output)}, {"item:image", usd(4 * cent)},
                                                               {"cache_read", usd(100'000'000)}, {"input/flex", usd(flex_input)},
                                                               {"output/flex", usd(flex_output)}, {"item:image/flex", usd(2 * cent)}})),
                 {"tier"}));
  f.service_tier = "priority";
  CHECK(unpriced(estimated(f, &u, {{"item:image", 1}}, *tiered), {"tier"}));
  std::cout << "case passed: a tier the facts name\n";
}

// Embeddings at a routing gateway: every input token at the lower of the route ceiling and the request's own cap
// (`provider.max_price.prompt`), with the pin a host attaches; no cap, no ceiling, a request fee, image inputs and a
// variable ceiling refuse.
void gateway_embeddings() {
  constexpr std::uint64_t listed = 20'000'000, ceiling = 30'000'000;
  const auto u900 = tokens(900, std::nullopt);
  const auto c = entry({{"input", usd(listed)}, {"ceiling_input", usd(ceiling)}}, R"("input_limit":2048)");
  Call call{facts("embeddings", "embed", {quantity("input", std::nullopt, Source::InputLimit, 3)})};
  call.facts.routed_gateway = true;
  const std::string unpinned = "an endpoint call at a routing gateway has no per-item route ceiling and no pin";
  // No cap: today's refusal.
  auto b = bound(call, *c);
  CHECK(only(b, UnboundedBy::RouteCeiling, "vendor/model") && b.unbounded[0].detail == unpinned && !b.route_ceiling && !b.pin);
  // A cap with no prompt amount is no cap on the input.
  call.facts.max_price = OpenRouterMaxPrice{};
  call.facts.max_price->completion_nano_usd = 1;
  CHECK(only(bound(call, *c), UnboundedBy::RouteCeiling, "vendor/model"));
  // A cap below the ceiling: the cap prices the input, and the request already carries the pin.
  call.facts.max_price = OpenRouterMaxPrice{};
  call.facts.max_price->prompt_nano_usd = 25'000'000;
  b = bound(call, *c);
  CHECK(totals(b, rounded(std::uint64_t{3 * 2048} * 25'000'000)) && b.lines.size() == 1 && line(b, "input", "provider.max_price.prompt", 3 * 2048, 25'000'000));
  CHECK(b.route_ceiling && b.pinned && b.pin && b.pin->prompt_nano_usd == 25'000'000 && !b.pin->completion_nano_usd && b.attempts == 1 &&
        b.entry == "vendor/model");
  // A measured count, capped at the limit times the inputs.
  call.measured = 1000;
  CHECK(totals(bound(call, *c), rounded(std::uint64_t{1000} * 25'000'000)));
  call.measured.reset();
  // A cap above the ceiling: the ceiling prices it, and the pin the request must carry is the ceiling.
  call.facts.max_price->prompt_nano_usd = 40'000'000;
  b = bound(call, *c);
  CHECK(totals(b, rounded(3 * 2048 * ceiling)) && line(b, "input", "ceiling_input", 3 * 2048, ceiling));
  CHECK(b.route_ceiling && !b.pinned && b.pin && b.pin->prompt_nano_usd == ceiling);
  // A cap equal to the ceiling; and the request's other amounts kept in the pin.
  call.facts.max_price->prompt_nano_usd = ceiling;
  call.facts.max_price->completion_nano_usd = 7;
  b = bound(call, *c);
  CHECK(totals(b, rounded(3 * 2048 * ceiling)) && b.pinned && b.pin->prompt_nano_usd == ceiling && b.pin->completion_nano_usd == 7);
  // A cap of zero is no cap the bound takes: how the gateway answers it is not known.
  call.facts.max_price = OpenRouterMaxPrice{};
  call.facts.max_price->prompt_nano_usd = 0;
  b = bound(call, *c);
  CHECK(only(b, UnboundedBy::RouteCeiling, "vendor/model") && b.unbounded[0].detail == unpinned && !b.pin);
  const auto unpinned_estimate = estimated(call.facts, &u900, {}, *c);
  CHECK(estimates(unpinned_estimate, rounded(900 * listed)) && unpinned_estimate.assumptions == Names({"listed rates"}));
  call.facts.max_price->prompt_nano_usd = 25'000'000;
  // A cap one nano-USD above the ceiling, as the JSON number nearest the ceiling reads (0.00003 USD per million reads
  // as 30001 nano-USD), is the pin a host writes back: the request carries it.
  const auto at_ceiling = entry({{"input", usd(20'000)}, {"ceiling_input", usd(30'000)}}, R"("input_limit":2048)");
  call.facts.max_price->prompt_nano_usd = 30'001;
  b = bound(call, *at_ceiling);
  CHECK(totals(b, rounded(std::uint64_t{3 * 2048} * 30'000)) && b.pinned && b.pin->prompt_nano_usd == 30'000);
  call.facts.max_price->prompt_nano_usd = 30'002;
  b = bound(call, *at_ceiling);
  CHECK(totals(b, rounded(std::uint64_t{3 * 2048} * 30'000)) && !b.pinned && b.pin->prompt_nano_usd == 30'000);
  // A ceiling below the input price the entry lists bounds nothing: a cap above it refuses, and a cap below it still
  // bounds the call, since the router keeps to the cap.
  const auto stale = entry({{"input", usd(30'000'000)}, {"ceiling_input", usd(20'000'000)}}, R"("input_limit":2048)");
  call.facts.max_price->prompt_nano_usd = 50'000'000;
  b = bound(call, *stale);
  CHECK(only(b, UnboundedBy::RouteCeiling, "vendor/model") && b.unbounded[0].detail.find("bounds nothing") != std::string::npos);
  call.facts.max_price->prompt_nano_usd = 10'000'000;
  b = bound(call, *stale);
  CHECK(totals(b, rounded(std::uint64_t{3 * 2048} * 10'000'000)) && b.pinned);
  call.facts.max_price->prompt_nano_usd = 25'000'000;
  // No route ceiling: the router may pick any price below the cap only if the ceiling is listed.
  b = bound(call, *entry({{"input", usd(listed)}}, R"("input_limit":2048)"));
  CHECK(only(b, UnboundedBy::RouteCeiling, "vendor/model") && b.unbounded[0].detail.find("ceiling_input") != std::string::npos);
  // A variable ceiling.
  CHECK(only(bound(call, *entry({{"input", usd(listed)}, {"ceiling_input", "-1"}}, R"("input_limit":2048)")), UnboundedBy::ModelPrice, "ceiling_input"));
  // A request fee above zero, listed or as a ceiling, which no amount of the cap is known to bind; declared zeros do not refuse.
  CHECK(only(bound(call, *entry({{"input", usd(listed)}, {"ceiling_input", usd(ceiling)}, {"request", "0.001"}}, R"("input_limit":2048)")),
             UnboundedBy::RouteCeiling, "request"));
  CHECK(only(bound(call, *entry({{"input", usd(listed)}, {"ceiling_input", usd(ceiling)}, {"ceiling_request", "0.001"}}, R"("input_limit":2048)")),
             UnboundedBy::RouteCeiling, "request"));
  CHECK(totals(bound(call, *entry({{"input", usd(listed)}, {"ceiling_input", usd(ceiling)}, {"request", "0"}, {"ceiling_request", "0"}},
                                  R"("input_limit":2048)")),
               rounded(std::uint64_t{3 * 2048} * 25'000'000)));
  // No input limit and no measured count.
  CHECK(only(bound(call, *entry({{"input", usd(listed)}, {"ceiling_input", usd(ceiling)}})), UnboundedBy::InputBound, "input"));
  // Image inputs: the cap is not known to bind image tokens.
  call.facts.quantities.push_back(quantity("input_image", std::nullopt, Source::InputLimit, 3));
  b = bound(call, *entry({{"input", usd(listed)}, {"input_image", usd(listed)}, {"ceiling_input", usd(ceiling)}}, R"("input_limit":2048)"));
  CHECK(only(b, UnboundedBy::RouteCeiling, "vendor/model") && b.unbounded[0].detail == unpinned);
  call.facts.quantities.pop_back();
  // The tier's own ceiling.
  call.facts.service_tier = "flex";
  CHECK(totals(bound(call, *entry({{"input/flex", usd(5'000'000)}, {"ceiling_input/flex", usd(10'000'000)}}, R"("input_limit":2048)")),
               rounded(std::uint64_t{3 * 2048} * 10'000'000)));
  call.facts.service_tier.clear();

  // The estimate: the reported tokens at the listed rate, and never above the cap, which the gateway enforces.
  auto u = tokens(900, std::nullopt);
  auto e = estimated(call.facts, &u, {}, *c);
  CHECK(estimates(e, rounded(900 * listed)) && e.assumptions == Names({"listed rates"}));
  CHECK(pricing::beyond_bound(bound(call, *c), e).empty() && e.total->nano_usd <= bound(call, *c).total->nano_usd);
  call.facts.max_price->prompt_nano_usd = 10'000'000;
  e = estimated(call.facts, &u, {}, *c);
  CHECK(estimates(e, rounded(std::uint64_t{900} * 10'000'000)) && e.assumptions == Names({"listed rates", "input at the pin"}));
  CHECK(pricing::beyond_bound(bound(call, *c), e).empty());
  std::cout << "case passed: embeddings at a routing gateway\n";
}

// Every other endpoint at a routing gateway keeps no bound, with the request's cap and an entry that lists every unit
// and every route ceiling: images, decisions, rerank, speech, transcription and videos.
void gateway_refusals() {
  const auto c = entry({{"input", "0.000001"}, {"input_image", "0.000001"}, {"output", "0.000002"}, {"item:image", "0.04"}, {"item:input_image", "0.01"},
                        {"item:document", "0.001"}, {"item:search_unit", "0.002"}, {"item:character", "0.00001"}, {"item:second", "0.0001"},
                        {"ceiling_input", "0.000002"}, {"ceiling_output", "0.000004"}},
                       R"("input_limit":1000,"output_limit":1000)");
  const auto in = quantity("input", std::nullopt, Source::InputLimit), out = quantity("output", std::nullopt, Source::OutputLimit);
  const std::vector<EndpointFacts> calls{
      facts("images", "generate", {quantity("item:image", 1), quantity("item:input_image", 0), in, quantity("input_image", 0), out}),
      facts("decisions", "decide", {in, out}),
      facts("rerank", "rank", {quantity("item:document", 2), quantity("item:search_unit", std::nullopt), in}),
      facts("speech", "speak", {quantity("item:character", 12), in, out}),
      facts("transcription", "transcribe", {quantity("item:second", std::nullopt, Source::StatedSeconds), in, out}),
      facts("videos", "submit", {quantity("item:second", 5), in, out})};
  for (auto f : calls) {
    f.routed_gateway = true;
    f.max_price = OpenRouterMaxPrice{1'000'000'000, 1'000'000'000, 1'000'000'000, 1'000'000'000, 1'000'000'000};
    Call call{f};
    call.seconds = 10;
    const auto b = bound(call, *c);
    CHECK(only(b, UnboundedBy::RouteCeiling, "vendor/model") &&
          b.unbounded[0].detail == "an endpoint call at a routing gateway has no per-item route ceiling and no pin" && !b.route_ceiling && !b.pin);
  }
  std::cout << "case passed: other endpoints at a routing gateway\n";
}

// Over generated rates, caps, counts and usages within the bound's count: the estimate of a pinned embeddings call at a
// routing gateway is never above its bound while the listed rate is at most the route ceiling.
void gateway_estimate_within_the_bound() {
  std::uint64_t seed = 20261120;
  auto next = [&](std::uint64_t limit) { seed = seed * 6364136223846793005u + 1442695040888963407u; return (seed >> 33) % (limit + 1); };
  std::size_t compared = 0;
  for (int i = 0; i < 2000; ++i) {
    const std::uint64_t listed = next(50'000'000), ceiling = listed + next(50'000'000), cap = next(120'000'000), limit = 1 + next(4096),
                        inputs = 1 + next(8);
    const auto c = entry({{"input", usd(listed)}, {"ceiling_input", usd(ceiling)}}, "\"input_limit\":" + std::to_string(limit));
    Call call{facts("embeddings", "embed", {quantity("input", std::nullopt, Source::InputLimit, inputs)})};
    call.facts.routed_gateway = true;
    call.facts.max_price = OpenRouterMaxPrice{};
    call.facts.max_price->prompt_nano_usd = cap;
    if (next(1)) call.measured = next(limit * inputs);
    const auto b = bound(call, *c);
    CHECK(b.total && b.lines.size() == 1);
    CHECK(b.lines[0].rate == std::min(ceiling, cap) && b.pinned == (cap <= ceiling));
    const auto reported = next(b.lines[0].quantity);
    auto u = tokens(reported, std::nullopt);
    const auto e = estimated(call.facts, &u, {}, *c);
    CHECK(e.total && e.total->nano_usd <= b.total->nano_usd && pricing::beyond_bound(b, e).empty());
    CHECK(e.total->nano_usd == rounded(reported * std::min(listed, cap)));
    ++compared;
  }
  CHECK(compared == 2000);
  std::cout << "case passed: the estimate at a routing gateway within the bound\n";
}

// A cached audio input token (#219) is among the input rates of an endpoint call's bound, and the request's cap on the
// prompt price at a routing gateway binds neither its estimate nor its bound: `provider.max_price` names no such price.
void cached_audio_rates() {
  constexpr std::uint64_t input = 20'000'000, ceiling = 30'000'000, cached_audio = 50'000'000, cap = 25'000'000;
  const auto direct = entry({{"input", usd(input)}, {"cache_read_audio", usd(cached_audio)}}, R"("input_limit":2048)");
  Call call{facts("embeddings", "embed", {quantity("input", std::nullopt, Source::InputLimit, 3)})};
  auto b = bound(call, *direct);
  CHECK(totals(b, rounded(3 * 2048 * cached_audio)) && line(b, "input", "cache_read_audio", 3 * 2048, cached_audio));
  const auto routed = entry({{"input", usd(input)}, {"ceiling_input", usd(ceiling)}, {"cache_read_audio", usd(cached_audio)}}, R"("input_limit":2048)");
  call.facts.routed_gateway = true;
  call.facts.max_price = OpenRouterMaxPrice{};
  call.facts.max_price->prompt_nano_usd = cap;
  b = bound(call, *routed);
  CHECK(totals(b, rounded(3 * 2048 * cached_audio)) && line(b, "input", "cache_read_audio", 3 * 2048, cached_audio));
  // 900 input tokens, 600 of them cached audio: 300 at the input rate (below the cap) and 600 at the listed cached
  // audio rate, not at the cap.
  auto u = tokens(900, std::nullopt, 600);
  u.extra["promptTokensDetails.AUDIO"] = Count{600, Evidence::Reported};
  u.extra["cacheTokensDetails.AUDIO"] = Count{600, Evidence::Reported};
  const auto e = estimated(call.facts, &u, {}, *routed);
  CHECK(estimates(e, rounded(300 * input + 600 * cached_audio)) && line(e, "input", "cache_read_audio", 600, cached_audio));
  CHECK(e.total->nano_usd <= b.total->nano_usd);
  // A variable cached audio rate leaves the pinned bound without a total.
  const auto variable = entry({{"input", usd(input)}, {"ceiling_input", usd(ceiling)}, {"cache_read_audio", "-1"}}, R"("input_limit":2048)");
  CHECK(refuses(bound(call, *variable), UnboundedBy::ModelPrice, "cache_read_audio"));
  std::cout << "case passed: cached audio rates in endpoint bounds and estimates\n";
}

int main() {
  const std::pair<const char*, void (*)()> cases[]{
      {"cached_audio_rates", cached_audio_rates},
      {"bound_covers_every_consistent_result", bound_covers_every_consistent_result}, {"estimate_within_the_bound", estimate_within_the_bound},
      {"images", images}, {"decisions", decisions}, {"embeddings", embeddings}, {"rerank", rerank}, {"speech", speech},
      {"transcription", transcription}, {"videos", videos}, {"billing", billing}, {"refusals", refusals}, {"zeros", zeros},
      {"image_tokens", image_tokens}, {"bands", bands}, {"unstated_length", unstated_length}, {"attempts", attempts}, {"overflow", overflow},
      {"images_estimate", images_estimate}, {"estimate_rounds_once", estimate_rounds_once}, {"token_estimates", token_estimates},
      {"item_estimates", item_estimates}, {"estimate_refusals", estimate_refusals}, {"estimate_length_from_input", estimate_length_from_input},
      {"estimate_by_another_entry", estimate_by_another_entry}, {"estimate_zero_quantity", estimate_zero_quantity},
      {"estimate_length_overflow", estimate_length_overflow}, {"carried_requests", carried_requests}, {"tier_from_facts", tier_from_facts},
      {"gateway_embeddings", gateway_embeddings}, {"gateway_refusals", gateway_refusals},
      {"gateway_estimate_within_the_bound", gateway_estimate_within_the_bound}};
  int failed = 0;
  for (const auto& [name, run] : cases) {
    try {
      run();
    } catch (const std::exception& error) {
      std::cerr << "endpoint pricing test failed: " << name << ": " << error.what() << '\n';
      ++failed;
    }
  }
  if (failed) return 1;
  std::cout << "endpoint pricing properties passed\n";
  return 0;
}
