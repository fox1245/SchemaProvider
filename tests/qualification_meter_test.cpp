#include "crypto/crypto.h"
#include "qualification/meter.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace sp::qualification;
namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { ++failures; std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)
std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) std::abort();
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void write_file(const std::filesystem::path& path, std::string_view bytes, bool append = false) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC), 0600);
  if (fd < 0) std::abort();
  while (!bytes.empty()) {
    auto n = ::write(fd, bytes.data(), bytes.size());
    if (n <= 0) std::abort();
    bytes.remove_prefix(static_cast<std::size_t>(n));
  }
  if (::fsync(fd) || ::close(fd)) std::abort();
}
// Same synthetic baseline as the campaign fixture: 99 vision calls, 18979680 micro-USD exposure.
std::string synthetic_baseline() {
  std::string baseline = "SPCANARY1\n";
  for (std::uint64_t i = 1; i <= 99; ++i)
    baseline += "4 " + std::to_string(i) + ' ' + std::to_string(i * 100) + ' ' +
        std::to_string(i == 99 ? 18979680 : i * (18979680 / 99)) + '\n';
  return baseline;
}
// The meter refuses group/world-writable directories (fail-closed): set 0700 explicitly, not via umask.
void private_directory(const std::filesystem::path& path) {
  std::filesystem::create_directory(path);
  if (::chmod(path.c_str(), 0700)) std::abort();
  struct stat s{};
  if (::stat(path.c_str(), &s) || (s.st_mode & 022)) {
    std::fprintf(stderr, "fixture directory is group-writable or world-writable (meter refuses it): %s\n", path.c_str());
    std::abort();
  }
}
struct Fixture {
  std::filesystem::path root;
  std::string original;
  explicit Fixture(const std::filesystem::path& source) {
    const auto parent = std::filesystem::canonical(std::filesystem::temp_directory_path());
    auto pattern = (parent / "sp-qualification-XXXXXX").string();
    auto* result = ::mkdtemp(pattern.data());
    if (!result) std::abort();
    root = result;
    private_directory(root / "config");
    private_directory(root / "build");
    write_file(root / "config/qualification-authorization.json", read_file(source / "config/qualification-authorization.json"));
    write_file(root / "config/model-catalog.json", read_file(source / "config/model-catalog.json"));
    original = synthetic_baseline();
    write_file(root / "build/m5-live.ledger", original);
  }
  ~Fixture() { std::filesystem::remove_all(root); }
  std::unique_ptr<Meter> open() {
    auto result = Meter::open(root.string());
    if (auto* error = std::get_if<Error>(&result)) {
      std::fprintf(stderr, "meter fixture failed: %.*s\n", static_cast<int>(error->message.size()), error->message.data());
      std::abort();
    }
    return std::get<std::unique_ptr<Meter>>(std::move(result));
  }
};
std::string id(std::uint64_t n) {
  char bytes[65];
  std::snprintf(bytes, sizeof(bytes), "%064llx", static_cast<unsigned long long>(n));
  return bytes;
}
Binding binding(std::uint64_t n, std::uint64_t input = 0, std::uint64_t output = 1,
                std::string model = "gpt-6-luna", std::string family = "openai.chat") {
  return {id(n), id(n + 10000), std::move(model), std::move(family), input, output};
}
Claim reserve(Meter& meter, Binding b) {
  auto result = meter.reserve(std::move(b));
  if (!std::holds_alternative<Claim>(result)) std::abort();
  return std::get<Claim>(std::move(result));
}
Totals totals(Meter& meter) {
  auto result = meter.totals();
  if (!std::holds_alternative<Totals>(result)) std::abort();
  return std::get<Totals>(result);
}
Outcome known(std::uint64_t in = 0, std::uint64_t out = 0, std::uint64_t thinking = 0) {
  return {true, Usage{in, 0, 0, 0, out, thinking}};
}
template<class T> bool denied(const Result<T>& result, Denial code) {
  auto* error = std::get_if<Error>(&result);
  return error && error->code == code;
}
std::uint64_t cost(const Result<std::uint64_t>& result) {
  if (!std::holds_alternative<std::uint64_t>(result)) std::abort();
  return std::get<std::uint64_t>(result);
}
std::uint64_t cost(const Result<PricedUsage>& result) {
  if (!std::holds_alternative<PricedUsage>(result)) std::abort();
  return std::get<PricedUsage>(result).micro_usd;
}
Settlement settlement(const Result<Settlement>& result) {
  if (!std::holds_alternative<Settlement>(result)) std::abort();
  return std::get<Settlement>(result);
}
std::string sha256(std::string_view bytes) {
  const auto digest = sp::crypto::sha256(bytes);
  constexpr char hex[] = "0123456789abcdef";
  std::string result(64, '\0');
  for (std::size_t i = 0; i < digest.size(); ++i) {
    result[i * 2] = hex[digest[i] >> 4]; result[i * 2 + 1] = hex[digest[i] & 15];
  }
  return result;
}
std::string extension_declaration(const Fixture& fixture, const Totals& before) {
  const auto prefix = read_file(fixture.root / "build/qualification-meter.ledger");
  const auto rows = std::count(prefix.begin(), prefix.end(), '\n') - 1;
  auto hash_file = [&](const char* path) { return sha256(read_file(fixture.root / path)); };
  return "{\"version\":1,\"scope\":\"five_current_chat_api_qualification\","
      "\"additional_calls\":480,\"micro_usd\":3000000,"
      "\"original_authorization\":\"config/qualification-authorization.json\","
      "\"baseline_ledger\":\"build/m5-live.ledger\",\"meter_ledger\":\"build/qualification-meter.ledger\","
      "\"catalog\":\"config/model-catalog.json\","
      "\"activation_anchor\":\"build/m5-live.ledger.qualification-binding\","
      "\"owner_selection\":\"추가 480회·US$3 승인\","
      "\"billing_guarantee\":\"local_meter_and_stop_not_invoice\","
      "\"unknown_cost_policy\":\"retain_reservation\","
      "\"excluded_paid_scope\":[\"video_generation\",\"image_generation\",\"remote_graphrag\"],"
      "\"binding\":{\"original_authorization_sha256\":\"" + hash_file("config/qualification-authorization.json") +
      "\",\"baseline_sha256\":\"" + hash_file("build/m5-live.ledger") +
      "\",\"catalog_sha256\":\"" + hash_file("config/model-catalog.json") +
      "\",\"activation_sha256\":\"" + hash_file("build/m5-live.ledger.qualification-binding") +
      "\",\"meter_prefix_sha256\":\"" + sha256(prefix) +
      "\",\"meter_prefix_rows\":" + std::to_string(rows) +
      ",\"meter_prefix_calls\":" + std::to_string(before.calls) +
      ",\"meter_prefix_spent_micro_usd\":" + std::to_string(before.spent_micro_usd) +
      ",\"meter_prefix_held_micro_usd\":" + std::to_string(before.held_micro_usd) + "}}";
}
void catalog_behavior(const std::filesystem::path& source) {
  auto raw = read_file(source / "config/model-catalog.json");
  auto parsed = Catalog::from_json(raw);
  CHECK(std::holds_alternative<std::shared_ptr<const Catalog>>(parsed));
  auto catalog = std::get<std::shared_ptr<const Catalog>>(parsed);
  auto* gpt = catalog->find("gpt-6-luna", "openai.responses");
  auto* claude = catalog->find("claude-haiku-4-5-20251001", "anthropic.messages");
  auto* gemini = catalog->find("gemini-2.5-flash-lite", "google.interactions");
  CHECK(gpt && claude && gemini);
  CHECK(!catalog->find("gpt-6-luna", "google.generate"));
  CHECK(cost(catalog->usage_cost(*gpt, Usage{600, 0, 0, 0, 30, 20}, 600, 30)) == 75);
  CHECK(cost(catalog->usage_cost(*claude, Usage{100, 100, 100, 100, 40, 20}, 400, 40)) == 635);
  CHECK(cost(catalog->usage_cost(*gemini, Usage{100, 100, 0, 0, 40, 20}, 200, 40)) == 27);
  CHECK(cost(catalog->usage_cost(*gpt, Usage{272000, 0, 0, 0, 100, 50}, 272001, 100)) == 27250);
  CHECK(cost(catalog->usage_cost(*gpt, Usage{272001, 0, 0, 0, 100, 50}, 272001, 100)) == 54476);
  CHECK(cost(catalog->usage_cost(*gpt, Usage{0, 0, 0, 0, 0, 0}, 600, 30)) == 0);
  CHECK(denied(catalog->usage_cost(*gpt, Usage{0, 0, 0, 0, std::nullopt, 0}, 600, 30), Denial::Bounds));
  CHECK(denied(catalog->usage_cost(*gemini, Usage{0, 0, 1, 0, 0, 0}, 600, 30), Denial::Bounds));
  CHECK(denied(catalog->usage_cost(*gpt, Usage{0, 0, 0, 0, 1, 2}, 600, 30), Denial::Bounds));
  CHECK(denied(catalog->usage_cost(*gpt, Usage{UINT64_MAX, 1, 0, 0, 0, 0}, 600, 30), Denial::Overflow));
  CHECK(denied(catalog->reserve_cost(*gpt, UINT64_MAX, 1), Denial::Bounds));
  // A partition with tiny nonzero cache bands must fit even with per-band rounding.
  CHECK(cost(catalog->usage_cost(*claude, Usage{1, 1, 1, 1, 1, 1}, 4, 1)) <= cost(catalog->reserve_cost(*claude, 4, 1)));
  auto pos = raw.find("\"input_rate\": 100000");
  if (pos == std::string::npos) std::abort();
  raw.replace(pos, std::string("\"input_rate\": 100000").size(), "\"input_rate\": 18446744073709551615");
  CHECK(denied(Catalog::from_json(raw), Denial::Overflow));
}
void settlement_and_restart(const std::filesystem::path& source) {
  Fixture f(source);
  auto meter = f.open();
  auto claim = reserve(*meter, binding(1, 600, 30));
  auto held = claim.reserved_micro_usd();
  CHECK(totals(*meter).calls == 1 && totals(*meter).held_micro_usd == held);
  CHECK(denied(meter->reserve(binding(1, 600, 30)), Denial::DuplicateRequest));
  auto reloaded = f.open();
  auto settled = reloaded->settle(claim, known(600, 30, 20));
  CHECK(settlement(settled).totals.spent_micro_usd == 75);
  CHECK(settlement(settled).totals.held_micro_usd == 0);
  CHECK(settlement(reloaded->settle(claim, known(600, 30, 20))).totals.spent_micro_usd == 75);
  CHECK(denied(reloaded->settle(claim, known(599, 30, 20)), Denial::SettlementConflict));
  auto moved = std::move(claim);
  CHECK(denied(meter->settle(claim, known(600, 30, 20)), Denial::InvalidClaim));
  CHECK(settlement(meter->settle(moved, known(600, 30, 20))).totals.calls == 1);
  auto zero = reserve(*meter, binding(2));
  CHECK(settlement(meter->settle(zero, known())).totals.spent_micro_usd == 75);
  auto missing = reserve(*meter, binding(3));
  CHECK(settlement(meter->settle(missing, {true, Usage{0, 0, 0, 0, std::nullopt, 0}})).totals.held_micro_usd == 1);
  auto partial = reserve(*meter, binding(4));
  CHECK(settlement(meter->settle(partial, {false, Usage{0, 0, 0, 0, 0, 0}})).totals.held_micro_usd == 2);
  auto inconsistent = reserve(*meter, binding(5));
  CHECK(settlement(meter->settle(inconsistent, known(0, 2))).totals.held_micro_usd == 3);
  meter.reset(); reloaded.reset();
  meter = f.open();
  CHECK(totals(*meter).calls == 5 && totals(*meter).held_micro_usd == 3 && totals(*meter).spent_micro_usd == 75);
  CHECK(read_file(f.root / "build/m5-live.ledger") == f.original);
  CHECK(totals(*meter).baseline_vision_calls == 99 && totals(*meter).baseline_vision_exposure_micro_usd == 18979680);
}
void nullable_usage_liability(const std::filesystem::path& source) {
  Fixture f(source);
  auto meter = f.open();
  auto exact = reserve(*meter, binding(1, 600, 8192));
  auto no_thinking = Outcome{true, Usage{600, 0, 0, 0, 30, std::nullopt, 600}};
  auto first = settlement(meter->settle(exact, no_thinking));
  CHECK(first.kind == SettlementKind::Exact && first.charged_micro_usd == 75);
  CHECK(first.totals.held_micro_usd == 0 && first.totals.exact_settlements == 1);
  auto upper = reserve(*meter, binding(2, 600, 8192));
  Outcome incomplete{true, Usage{std::nullopt, std::nullopt, std::nullopt,
      std::nullopt, 30, std::nullopt, 600}};
  auto second = settlement(meter->settle(upper, incomplete));
  CHECK(second.kind == SettlementKind::UpperBound && second.charged_micro_usd == 93);
  CHECK(second.totals.held_micro_usd == 0 && second.totals.upper_bound_settlements == 1);
  auto restarted = f.open();
  CHECK(settlement(restarted->settle(upper, incomplete)).charged_micro_usd == 93);
  auto contradiction = reserve(*meter, binding(3, 600, 8192));
  auto bad = no_thinking;
  bad.usage.total_input = 599;
  CHECK(settlement(meter->settle(contradiction, bad)).kind == SettlementKind::UnknownHold);
  auto missing = reserve(*meter, binding(4, 600, 8192));
  auto unknown = incomplete;
  unknown.usage.total_input.reset();
  CHECK(settlement(meter->settle(missing, unknown)).kind == SettlementKind::UnknownHold);
  CHECK(totals(*restarted).unknown_settlements == 2);
}
void money_boundary(const std::filesystem::path& source) {
  Fixture f(source);
  auto meter = f.open();
  for (std::uint64_t i = 1; i <= 3; ++i)
    (void)reserve(*meter, binding(i, 0, 64000, "claude-haiku-4-5-20251001", "anthropic.messages"));
  auto last = reserve(*meter, binding(4, 0, 8000, "claude-haiku-4-5-20251001", "anthropic.messages"));
  CHECK(totals(*meter).held_micro_usd == 1000000 && totals(*meter).calls == 4);
  CHECK(denied(meter->reserve(binding(5)), Denial::Money));
  auto release = meter->settle(last, known(0, 7999));
  CHECK(settlement(release).totals.spent_micro_usd == 39995);
  CHECK(settlement(release).totals.held_micro_usd == 960000);
  (void)reserve(*meter, binding(5, 0, 10));
  CHECK(totals(*meter).held_micro_usd + totals(*meter).spent_micro_usd == 1000000);
  CHECK(denied(meter->reserve(binding(6)), Denial::Money));
}
void call_boundary_and_process_race(const std::filesystem::path& source) {
  Fixture f(source);
  auto meter = f.open();
  for (std::uint64_t i = 1; i < 630; ++i) (void)reserve(*meter, binding(i));
  std::array<pid_t, 2> children{};
  for (std::size_t i = 0; i < children.size(); ++i) {
    children[i] = ::fork();
    if (children[i] < 0) std::abort();
    if (children[i] == 0) {
      auto child_meter = f.open();
      auto result = child_meter->reserve(binding(700 + i));
      ::_exit(std::holds_alternative<Claim>(result) ? 0 : denied(result, Denial::Calls) ? 2 : 3);
    }
  }
  int allowed = 0, exhausted = 0;
  for (auto child : children) {
    int status{};
    if (::waitpid(child, &status, 0) != child || !WIFEXITED(status)) std::abort();
    allowed += WEXITSTATUS(status) == 0;
    exhausted += WEXITSTATUS(status) == 2;
  }
  CHECK(allowed == 1 && exhausted == 1);
  CHECK(totals(*meter).calls == 630 && totals(*meter).held_micro_usd == 630);
  meter.reset(); meter = f.open();
  CHECK(denied(meter->reserve(binding(800)), Denial::Calls));
}
void thread_money_race(const std::filesystem::path& source) {
  Fixture f(source);
  auto meter = f.open();
  for (std::uint64_t i = 1; i <= 3; ++i)
    (void)reserve(*meter, binding(i, 0, 64000, "claude-haiku-4-5-20251001", "anthropic.messages"));
  auto second = f.open();
  std::array<int, 2> outcome{};
  auto attempt = [&](std::size_t i, Meter& m) {
    auto r = m.reserve(binding(50 + i, 0, 8000, "claude-haiku-4-5-20251001", "anthropic.messages"));
    outcome[i] = std::holds_alternative<Claim>(r) ? 1 : denied(r, Denial::Money) ? 2 : 3;
  };
  std::thread a(attempt, 0, std::ref(*meter)), b(attempt, 1, std::ref(*second));
  a.join(); b.join();
  CHECK((outcome[0] == 1 && outcome[1] == 2) || (outcome[0] == 2 && outcome[1] == 1));
  CHECK(totals(*meter).calls == 4 && totals(*meter).held_micro_usd == 1000000);
}
void extension_activation_and_restart(const std::filesystem::path& source) {
  Fixture f(source);
  auto meter = f.open();
  auto exact = reserve(*meter, binding(1, 600, 30));
  CHECK(settlement(meter->settle(exact, known(600, 30, 20))).charged_micro_usd == 75);
  auto unknown = reserve(*meter, binding(2, 600, 8192));
  CHECK(settlement(meter->settle(unknown, {false, {}})).kind == SettlementKind::UnknownHold);
  const auto before = totals(*meter);
  const auto prefix = read_file(f.root / "build/qualification-meter.ledger");
  const auto anchor = read_file(f.root / "build/m5-live.ledger.qualification-binding");
  const auto approval = read_file(f.root / "config/qualification-authorization.json");
  write_file(f.root / "config/qualification-authorization-extension.json", extension_declaration(f, before));
  std::array<pid_t, 2> children{};
  for (auto& child : children) {
    child = ::fork();
    if (child < 0) std::abort();
    if (!child) {
      auto reopened = f.open();
      const auto total = totals(*reopened);
      ::_exit(total.call_limit == 1110 && total.money_limit_micro_usd == 4000000 &&
          total.extension_authorization_events == 1 && total.calls == before.calls &&
          total.spent_micro_usd == before.spent_micro_usd && total.held_micro_usd == before.held_micro_usd ? 0 : 1);
    }
  }
  for (auto child : children) {
    int status{};
    if (::waitpid(child, &status, 0) != child || !WIFEXITED(status)) std::abort();
    CHECK(WEXITSTATUS(status) == 0);
  }
  const auto activated = read_file(f.root / "build/qualification-meter.ledger");
  CHECK(activated.starts_with(prefix));
  CHECK(read_file(f.root / "build/m5-live.ledger.qualification-binding") == anchor);
  CHECK(read_file(f.root / "config/qualification-authorization.json") == approval);
  CHECK(read_file(f.root / "build/m5-live.ledger") == f.original);
  auto reloaded = f.open();
  CHECK(totals(*meter).extension_authorization_events == 1);
  CHECK(totals(*reloaded).original_call_limit == 630 && totals(*reloaded).original_money_limit_micro_usd == 1000000);
  CHECK(totals(*reloaded).extension_calls == 480 && totals(*reloaded).extension_micro_usd == 3000000);
  CHECK(totals(*reloaded).exact_settlements == 1 && totals(*reloaded).unknown_settlements == 1);
  CHECK(settlement(reloaded->settle(exact, known(600, 30, 20))).totals.spent_micro_usd == before.spent_micro_usd);
  CHECK(settlement(reloaded->settle(unknown, {false, {}})).totals.held_micro_usd == before.held_micro_usd);
  CHECK(read_file(f.root / "build/qualification-meter.ledger") == activated);
  for (std::uint64_t i = 3; i <= 1110; ++i) (void)reserve(*meter, binding(i));
  CHECK(totals(*reloaded).calls == 1110);
  CHECK(totals(*reloaded).held_micro_usd == before.held_micro_usd + 1108);
  CHECK(denied(reloaded->reserve(binding(1111)), Denial::Calls));
  meter.reset(); reloaded.reset();
  meter = f.open();
  CHECK(denied(meter->reserve(binding(1112)), Denial::Calls));
}
void extension_money_and_unknown_hold(const std::filesystem::path& source) {
  Fixture f(source);
  auto meter = f.open();
  for (std::uint64_t i = 1; i <= 3; ++i)
    (void)reserve(*meter, binding(i, 0, 64000, "claude-haiku-4-5-20251001", "anthropic.messages"));
  auto original = reserve(*meter, binding(4, 0, 8000, "claude-haiku-4-5-20251001", "anthropic.messages"));
  CHECK(settlement(meter->settle(original, {false, {}})).kind == SettlementKind::UnknownHold);
  CHECK(totals(*meter).held_micro_usd == 1000000);
  CHECK(denied(meter->reserve(binding(5)), Denial::Money));
  write_file(f.root / "config/qualification-authorization-extension.json", extension_declaration(f, totals(*meter)));
  for (std::uint64_t i = 5; i <= 13; ++i)
    (void)reserve(*meter, binding(i, 0, 64000, "claude-haiku-4-5-20251001", "anthropic.messages"));
  (void)reserve(*meter, binding(14, 0, 24000, "claude-haiku-4-5-20251001", "anthropic.messages"));
  auto reopened = f.open();
  CHECK(totals(*reopened).held_micro_usd == 4000000 && totals(*reopened).spent_micro_usd == 0);
  CHECK(totals(*reopened).calls == 14 && totals(*reopened).unknown_settlements == 1);
  CHECK(denied(meter->reserve(binding(15)), Denial::Money));
  CHECK(denied(reopened->reserve(binding(16)), Denial::Money));
  CHECK(settlement(reopened->settle(original, {false, {}})).totals.held_micro_usd == 4000000);
}
void extension_tamper_denies(const std::filesystem::path& source) {
  for (int mutation = 0; mutation != 5; ++mutation) {
    Fixture f(source);
    auto meter = f.open();
    (void)reserve(*meter, binding(1));
    const auto path = f.root / "config/qualification-authorization-extension.json";
    const auto declaration = extension_declaration(f, totals(*meter));
    write_file(path, declaration);
    CHECK(totals(*meter).extension_authorization_events == 1);
    const auto ledger = read_file(f.root / "build/qualification-meter.ledger");
    if (mutation == 0) std::filesystem::remove(path);
    else if (mutation == 1) {
      std::filesystem::rename(path, f.root / "config/original-extension.json");
      write_file(path, declaration);
    } else if (mutation == 2) write_file(path, declaration + "\n");
    else if (mutation == 3) {
      auto changed = declaration;
      const auto pos = changed.find("\"micro_usd\":3000000");
      if (pos == std::string::npos) std::abort();
      changed.replace(pos, std::string_view("\"micro_usd\":3000000").size(), "\"micro_usd\":3000001");
      write_file(path, changed);
    } else {
      auto approval = read_file(f.root / "config/qualification-authorization.json");
      write_file(f.root / "config/qualification-authorization.json", approval + "\n");
    }
    const auto expected = mutation == 3 ? Denial::Configuration : Denial::Identity;
    CHECK(denied(meter->totals(), expected));
    CHECK(denied(meter->reserve(binding(2)), expected));
    CHECK(denied(Meter::open(f.root.string()), expected));
    CHECK(read_file(f.root / "build/qualification-meter.ledger") == ledger);
  }
  {
    Fixture f(source);
    auto meter = f.open();
    (void)reserve(*meter, binding(1));
    auto declaration = extension_declaration(f, totals(*meter));
    const auto pos = declaration.find("\"meter_prefix_calls\":1");
    if (pos == std::string::npos) std::abort();
    declaration.replace(pos, std::string_view("\"meter_prefix_calls\":1").size(), "\"meter_prefix_calls\":2");
    const auto ledger = read_file(f.root / "build/qualification-meter.ledger");
    write_file(f.root / "config/qualification-authorization-extension.json", declaration);
    CHECK(denied(Meter::open(f.root.string()), Denial::Identity));
    CHECK(read_file(f.root / "build/qualification-meter.ledger") == ledger);
  }
  for (bool duplicate : {false, true}) {
    Fixture f(source);
    auto meter = f.open();
    (void)reserve(*meter, binding(1));
    const auto prefix = read_file(f.root / "build/qualification-meter.ledger");
    write_file(f.root / "config/qualification-authorization-extension.json", extension_declaration(f, totals(*meter)));
    CHECK(totals(*meter).extension_authorization_events == 1);
    const auto path = f.root / "build/qualification-meter.ledger";
    const auto ledger = read_file(path);
    const auto row = ledger.substr(prefix.size(), ledger.size() - prefix.size() - 1);
    const auto hash_start = row.rfind(' ');
    auto payload = row.substr(0, hash_start);
    if (duplicate) {
      const auto first_cell = payload.find(' ');
      const auto previous_hash = payload.rfind(' ');
      payload = "3" + payload.substr(first_cell, previous_hash - first_cell) + ' ' + row.substr(hash_start + 1);
      write_file(path, payload + ' ' + sha256(payload) + '\n', true);
    } else {
      const auto amount = payload.find(" 480 3000000 ");
      if (amount == std::string::npos) std::abort();
      payload.replace(amount, std::string_view(" 480 3000000 ").size(), " 480 3000001 ");
      write_file(path, prefix + payload + ' ' + sha256(payload) + '\n');
    }
    CHECK(denied(meter->totals(), Denial::Corruption));
    CHECK(denied(meter->reserve(binding(2)), Denial::Corruption));
    CHECK(denied(Meter::open(f.root.string()), Denial::Corruption));
  }
}
void storage_fail_closed(const std::filesystem::path& source) {
  {
    Fixture f(source);
    write_file(f.root / "build/qualification-meter.ledger", "");
    CHECK(std::holds_alternative<Error>(Meter::open(f.root.string())));
  }
  {
    Fixture f(source);
    write_file(f.root / "build/m5-live.ledger.qualification-binding", "");
    CHECK(std::holds_alternative<Error>(Meter::open(f.root.string())));
  }
  {
    Fixture f(source);
    auto meter = f.open();
    (void)reserve(*meter, binding(1));
    write_file(f.root / "build/qualification-meter.ledger", "2 R partial", true);
    CHECK(denied(meter->totals(), Denial::Corruption));
    CHECK(std::holds_alternative<Error>(Meter::open(f.root.string())));
    CHECK(read_file(f.root / "build/m5-live.ledger") == f.original);
  }
  {
    Fixture f(source);
    auto meter = f.open();
    (void)reserve(*meter, binding(1)); (void)reserve(*meter, binding(2));
    auto path = f.root / "build/qualification-meter.ledger";
    auto bytes = read_file(path);
    auto first = bytes.find('\n') + 1, second = bytes.find('\n', first) + 1;
    write_file(path, bytes.substr(0, first) + bytes.substr(second) + bytes.substr(first, second - first));
    CHECK(denied(meter->totals(), Denial::Corruption));
  }
  {
    Fixture f(source);
    auto meter = f.open();
    (void)reserve(*meter, binding(1));
    std::filesystem::rename(f.root / "build/qualification-meter.ledger", f.root / "build/moved.ledger");
    CHECK(std::holds_alternative<Error>(Meter::open(f.root.string())));
    CHECK(!std::filesystem::exists(f.root / "build/qualification-meter.ledger"));
    write_file(f.root / "build/qualification-meter.ledger", read_file(f.root / "build/moved.ledger"));
    CHECK(denied(Meter::open(f.root.string()), Denial::Identity));
  }
  {
    Fixture f(source);
    auto meter = f.open();
    auto auth = read_file(f.root / "config/qualification-authorization.json");
    auto pos = auth.find("build/qualification-meter.ledger");
    if (pos == std::string::npos) std::abort();
    auth.replace(pos, std::string("build/qualification-meter.ledger").size(), "build/another-funded.ledger");
    write_file(f.root / "config/qualification-authorization.json", auth);
    CHECK(denied(Meter::open(f.root.string()), Denial::Configuration));
    CHECK(!std::filesystem::exists(f.root / "build/another-funded.ledger"));
  }
  {
    Fixture f(source);
    auto meter = f.open();
    std::filesystem::create_hard_link(f.root / "build/qualification-meter.ledger", f.root / "build/alias.ledger");
    CHECK(denied(meter->totals(), Denial::Storage));
  }
  {
    Fixture f(source);
    auto meter = f.open();
    ::chmod((f.root / "build/qualification-meter.ledger").c_str(), 0644);
    CHECK(denied(meter->totals(), Denial::Storage));
  }
  {
    Fixture f(source);
    std::filesystem::create_symlink("m5-live.ledger", f.root / "build/qualification-meter.ledger");
    CHECK(std::holds_alternative<Error>(Meter::open(f.root.string())));
    CHECK(read_file(f.root / "build/m5-live.ledger") == f.original);
  }
  // Every directory the meter opens must refuse group and world write access, whatever the umask was.
  for (const char* name : {"", "config", "build"}) {
    for (const mode_t writable : {static_cast<mode_t>(0020), static_cast<mode_t>(0002)}) {
      Fixture f(source);
      const auto path = *name ? f.root / name : f.root;
      if (::chmod(path.c_str(), 0700 | writable)) std::abort();
      CHECK(std::holds_alternative<Error>(Meter::open(f.root.string())));
    }
  }
}
}  // namespace
int main(int argc, char** argv) {
  const bool diagnostic = argc == 4 && std::string_view(argv[2]) == "--section" &&
      std::string_view(argv[3]) == "thread_money_race";
  if (argc != 2 && !diagnostic) {
    std::fprintf(stderr, "usage: qualification_meter_test <source-root> [--section thread_money_race]\n");
    return 2;
  }
  const std::filesystem::path source(argv[1]);
  // Temporary crash isolation for LLDB; the registered full-suite invocation is unchanged.
  if (diagnostic) {
    std::fprintf(stderr, "[qualification_meter] diagnostic thread_money_race\n");
    thread_money_race(source);
    return failures ? 1 : 0;
  }
  std::fprintf(stderr, "[qualification_meter] catalog_behavior\n");
  catalog_behavior(source);
  std::fprintf(stderr, "[qualification_meter] settlement_and_restart\n");
  settlement_and_restart(source);
  std::fprintf(stderr, "[qualification_meter] nullable_usage_liability\n");
  nullable_usage_liability(source);
  std::fprintf(stderr, "[qualification_meter] money_boundary\n");
  money_boundary(source);
  std::fprintf(stderr, "[qualification_meter] call_boundary_and_process_race\n");
  call_boundary_and_process_race(source);
  std::fprintf(stderr, "[qualification_meter] thread_money_race\n");
  thread_money_race(source);
  std::fprintf(stderr, "[qualification_meter] extension_activation_and_restart\n");
  extension_activation_and_restart(source);
  std::fprintf(stderr, "[qualification_meter] extension_money_and_unknown_hold\n");
  extension_money_and_unknown_hold(source);
  std::fprintf(stderr, "[qualification_meter] extension_tamper_denies\n");
  extension_tamper_denies(source);
  std::fprintf(stderr, "[qualification_meter] storage_fail_closed\n");
  storage_fail_closed(source);
  return failures ? 1 : 0;
}
