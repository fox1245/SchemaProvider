#include "canary/canary.h"
#include "canary/io.h"
#include <limits>

namespace sp::canary {
namespace {
using detail::fail;
std::uint64_t add(std::uint64_t a, std::uint64_t b) {
  if (b > std::numeric_limits<std::uint64_t>::max() - a) fail();
  return a + b;
}
std::uint64_t charge(std::uint64_t tokens, std::uint64_t rate) {
  if (rate && tokens > std::numeric_limits<std::uint64_t>::max() / rate) fail();
  auto product = tokens * rate;
  return product / 1000000 + (product % 1000000 != 0);
}
} // namespace
std::uint64_t reserved_cost(const Bounds& b) {
  return add(charge(b.input_tokens, b.input_rate), charge(b.output_tokens, b.output_rate));
}
} // namespace sp::canary
