#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sp::qualification {
enum class Denial {
  Configuration, Storage, Corruption, Identity, Calls, Money, Bounds,
  Overflow, UnknownModel, DuplicateRequest, InvalidClaim, SettlementConflict
};
struct Error {
  Denial code;
  // Fixed safe diagnostics only: never request, file contents, or credentials.
  std::string_view message;
};
template<class T> using Result = std::variant<T, Error>;

// All input bands are disjoint. Output INCLUDES thinking; thinking is an
// optional reporting subset, never an additional charge. Null is not zero.
// total_input includes ALL cache bands; present totals must agree with complete
// bands. Known total_input permits a labelled conservative upper bound when
// cache bands are absent; unknown input/output liability cannot release a hold.
struct Usage {
  std::optional<std::uint64_t> uncached_input, cache_read_input,
      cache_write_5m_input, cache_write_1h_input, output, thinking;
  std::optional<std::uint64_t> total_input{};
  bool operator==(const Usage&) const = default;
};
enum class PriceKind { Exact, UpperBound };
struct PricedUsage { std::uint64_t micro_usd{}; PriceKind kind = PriceKind::Exact; };
struct ModelPrice {
  std::string model;
  std::vector<std::string> families;
  std::uint64_t context_window{}, max_input{}, max_output{};
  std::uint64_t input_rate{}, cache_read_rate{}, output_rate{};
  std::optional<std::uint64_t> cache_write_5m_rate, cache_write_1h_rate,
      long_context_threshold;
  // Exact rational numerator / 2; no floating-point cost arithmetic.
  std::uint64_t long_input_numerator{}, long_output_numerator{};
  std::string model_url, pricing_url, caching_url;
};
class Catalog {
 public:
  static Result<std::shared_ptr<const Catalog>> from_json(std::string_view raw);
  const ModelPrice* find(std::string_view model, std::string_view family) const noexcept;
  Result<std::uint64_t> reserve_cost(const ModelPrice&, std::uint64_t input_bound,
                                     std::uint64_t output_bound) const;
  Result<PricedUsage> usage_cost(const ModelPrice&, const Usage&,
                                   std::uint64_t input_bound,
                                   std::uint64_t output_bound) const;
  const std::vector<ModelPrice>& models() const noexcept { return models_; }
 private:
  Catalog() = default;
  std::vector<ModelPrice> models_;
};
}  // namespace sp::qualification
