#include "canary/canary.h"
#include "canary/io.h"
#include <array>
#include <charconv>
#include <limits>
#include <sys/file.h>
#include <thread>

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
class LockedLedger {
 public:
  explicit LockedLedger(const std::string& path) : fd_(open_file(path, created_)) {
    detail::regular(fd_.get(), true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (::flock(fd_.get(), LOCK_EX | LOCK_NB)) {
      if ((errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN) ||
          std::chrono::steady_clock::now() >= deadline) fail();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Closing the descriptor releases the lock even on constructor exceptions.
    auto slash = path.find_last_of('/');
    auto directory = slash == std::string::npos ? "." : (slash == 0 ? "/" : path.substr(0, slash));
    detail::Fd parent(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (::fsync(parent.get())) fail();
    auto bytes = detail::read_bounded(fd_.get(), 65536);
    if (bytes.empty()) {
      if (!created_) fail(); // Existing empty storage is corruption, not a new budget.
      detail::write_all(fd_.get(), "SPCANARY1\n");
      if (::fsync(fd_.get())) fail();
      return;
    }
    std::string_view rest(bytes);
    if (!rest.starts_with("SPCANARY1\n")) fail();
    rest.remove_prefix(10);
    while (!rest.empty()) {
      auto newline = rest.find('\n');
      if (newline == std::string_view::npos) fail();
      auto row = rest.substr(0, newline);
      rest.remove_prefix(newline + 1);
      std::array<std::uint64_t, 4> values{};
      for (std::size_t i = 0; i < values.size(); ++i) {
        auto space = row.find(' ');
        auto cell = row.substr(0, space);
        if (cell.empty() || (cell.size() > 1 && cell.front() == '0')) fail();
        auto [end, ec] = std::from_chars(cell.data(), cell.data() + cell.size(), values[i]);
        if (ec != std::errc{} || end != cell.data() + cell.size()) fail();
        if (i + 1 == values.size()) { if (space != std::string_view::npos) fail(); }
        else { if (space == std::string_view::npos) fail(); row.remove_prefix(space + 1); }
      }
      if (values[0] >= totals_.size()) fail();
      auto& total = totals_[values[0]];
      const auto calls = values[0] == 2 ? 4U : 16U;
      const auto cost = values[0] == 2 ? 1000000U : 10000000U;
      if (values[1] != add(total.calls, 1) || values[1] > calls || values[2] <= total.tokens ||
          values[3] <= total.micro_usd || values[3] > cost) fail();
      total = {values[1], values[2], values[3]};
    }
  }
  Totals get(Provider provider) const { return totals_[index(provider)]; }
  void append(Provider provider, Totals value) {
    std::string row = std::to_string(index(provider)) + ' ' + std::to_string(value.calls) + ' ' +
        std::to_string(value.tokens) + ' ' + std::to_string(value.micro_usd) + '\n';
    detail::write_all(fd_.get(), row);
    if (::fsync(fd_.get())) fail();
    totals_[index(provider)] = value;
  }
 private:
  static int open_file(const std::string& path, bool& created) {
    const auto fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    created = fd >= 0;
    if (created) return fd;
    if (errno != EEXIST) fail();
    return ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  }
  static std::size_t index(Provider provider) {
    switch (provider) {
      case Provider::OpenAI: return 0;
      case Provider::Anthropic: return 1;
      case Provider::Gemini: return 2;
    }
    fail();
  }
  bool created_ = false;
  detail::Fd fd_;
  std::array<Totals, 3> totals_{};
};
} // namespace
std::uint64_t reserved_cost(const Bounds& b) {
  return add(charge(b.input_tokens, b.input_rate), charge(b.output_tokens, b.output_rate));
}
Debit reserve(const Profile& profile, const std::string& path) {
  LockedLedger ledger(path);
  const auto old = ledger.get(profile.provider());
  const auto& b = profile.bounds();
  const auto tokens = add(b.input_tokens, b.output_tokens), cost = reserved_cost(b);
  if (old.calls >= b.calls) return {Reservation::Calls, old};
  if (old.tokens > b.tokens || tokens > b.tokens - old.tokens) return {Reservation::Tokens, old};
  if (old.micro_usd > b.micro_usd || cost > b.micro_usd - old.micro_usd) return {Reservation::Cost, old};
  Totals next{add(old.calls, 1), add(old.tokens, tokens), add(old.micro_usd, cost)};
  ledger.append(profile.provider(), next);
  return {Reservation::Allowed, next};
}
Totals ledger_totals(const Profile& profile, const std::string& path) {
  return LockedLedger(path).get(profile.provider());
}
} // namespace sp::canary
