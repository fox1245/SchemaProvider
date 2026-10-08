#include "qualification/meter.h"
#include "crypto/crypto.h"
#include "json/json.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <mutex>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace sp::qualification {
namespace {
constexpr std::uint64_t call_limit = 630, money_limit = 1000000;
constexpr std::uint64_t extension_calls = 480, extension_money = 3000000;
constexpr std::uint64_t maximum_calls = call_limit + extension_calls;
struct Failure { Error error; };
[[noreturn]] void fail(Denial code) {
  std::string_view message = "qualification admission denied";
  switch (code) {
    case Denial::Configuration: message = "invalid approved qualification configuration"; break;
    case Denial::Storage: message = "qualification durable storage unavailable"; break;
    case Denial::Corruption: message = "qualification durable storage corrupt"; break;
    case Denial::Identity: message = "qualification campaign identity changed"; break;
    case Denial::Calls: message = "qualification call grant exhausted"; break;
    case Denial::Money: message = "qualification money grant exhausted"; break;
    case Denial::Bounds: message = "qualification request bounds invalid"; break;
    case Denial::Overflow: message = "qualification arithmetic overflow"; break;
    case Denial::UnknownModel: message = "qualification model or family not catalogued"; break;
    case Denial::DuplicateRequest: message = "qualification request already reserved"; break;
    case Denial::InvalidClaim: message = "qualification claim does not bind reservation"; break;
    case Denial::SettlementConflict: message = "qualification settlement conflicts"; break;
  }
  throw Failure{{code, message}};
}
std::uint64_t add(std::uint64_t a, std::uint64_t b) {
  if (b > UINT64_MAX - a) fail(Denial::Overflow);
  return a + b;
}
class Fd {
 public:
  explicit Fd(int fd = -1) : fd_(fd) { if (fd < 0) fail(Denial::Storage); }
  ~Fd() { if (fd_ >= 0) ::close(fd_); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  int get() const noexcept { return fd_; }
 private:
  int fd_;
};
struct Identity { std::uint64_t dev{}, ino{}; };
Identity identity(int fd, bool private_file = true) {
  struct stat s{};
  if (::fstat(fd, &s)) fail(Denial::Storage);
  if (!S_ISREG(s.st_mode) || s.st_nlink != 1 || s.st_uid != ::geteuid() ||
      (private_file && (s.st_mode & 077) != 0)) fail(Denial::Storage);
  return {static_cast<std::uint64_t>(s.st_dev), static_cast<std::uint64_t>(s.st_ino)};
}
void directory(int fd) {
  struct stat s{};
  if (::fstat(fd, &s) || !S_ISDIR(s.st_mode) || s.st_uid != ::geteuid() || (s.st_mode & 022))
    fail(Denial::Storage);
}
void lock(int fd) {
  while (::flock(fd, LOCK_EX)) if (errno != EINTR) fail(Denial::Storage);
}
void sync(int fd) { if (::fsync(fd)) fail(Denial::Storage); }
std::string read_all(int fd, std::size_t limit) {
  struct stat s{};
  if (::fstat(fd, &s) || s.st_size < 0 || static_cast<std::uint64_t>(s.st_size) > limit)
    fail(Denial::Corruption);
  std::string bytes(static_cast<std::size_t>(s.st_size), '\0');
  std::size_t pos = 0;
  while (pos < bytes.size()) {
    auto n = ::pread(fd, bytes.data() + pos, bytes.size() - pos, static_cast<off_t>(pos));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) fail(Denial::Storage);
    pos += static_cast<std::size_t>(n);
  }
  return bytes;
}
void append(int fd, std::string_view bytes) {
  if (::lseek(fd, 0, SEEK_END) < 0) fail(Denial::Storage);
  while (!bytes.empty()) {
    auto n = ::write(fd, bytes.data(), bytes.size());
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) fail(Denial::Storage);
    bytes.remove_prefix(static_cast<std::size_t>(n));
  }
  sync(fd); // A partial/failed transaction is left in place and fails closed.
}
std::string hash(std::string_view bytes) {
  const auto digest = crypto::sha256(bytes);
  constexpr char hex[] = "0123456789abcdef";
  std::string out(64, '\0');
  for (std::size_t i = 0; i < digest.size(); ++i) {
    out[i * 2] = hex[digest[i] >> 4]; out[i * 2 + 1] = hex[digest[i] & 15];
  }
  return out;
}
bool digest(std::string_view s) {
  return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}
std::uint64_t integer(std::string_view value) {
  std::uint64_t n{};
  if (value.empty() || (value.size() > 1 && value.front() == '0')) fail(Denial::Corruption);
  auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
  if (ec != std::errc{} || end != value.data() + value.size()) fail(Denial::Corruption);
  return n;
}
struct Cells {
  std::array<std::string_view, 20> value{};
  std::size_t count{};
};
Cells split(std::string_view row) {
  Cells out;
  while (true) {
    auto pos = row.find(' ');
    if (out.count == out.value.size()) fail(Denial::Corruption);
    auto cell = row.substr(0, pos);
    if (cell.empty()) fail(Denial::Corruption);
    out.value[out.count++] = cell;
    if (pos == std::string_view::npos) return out;
    row.remove_prefix(pos + 1);
  }
}
std::string_view line(std::string_view& rest) {
  auto pos = rest.find('\n');
  if (pos == std::string_view::npos) fail(Denial::Corruption);
  auto out = rest.substr(0, pos);
  rest.remove_prefix(pos + 1);
  return out;
}
std::optional<std::uint64_t> optional_integer(std::string_view cell) {
  return cell == "-" ? std::nullopt : std::optional(integer(cell));
}
std::string cell(std::optional<std::uint64_t> n) { return n ? std::to_string(*n) : "-"; }
std::string outcome_text(const Outcome& o) {
  return std::string(o.final_consistent ? "1" : "0") + ' ' + cell(o.usage.uncached_input) + ' ' +
      cell(o.usage.cache_read_input) + ' ' + cell(o.usage.cache_write_5m_input) + ' ' +
      cell(o.usage.cache_write_1h_input) + ' ' + cell(o.usage.output) + ' ' + cell(o.usage.thinking) + ' ' + cell(o.usage.total_input);
}
std::uint64_t config_number(json::Value v) {
  if (!v.is_uint()) fail(Denial::Configuration);
  return v.as_uint();
}
void approved(std::string_view raw) {
  auto parsed = json::parse(raw);
  auto* document = std::get_if<json::Document>(&parsed);
  if (!document) fail(Denial::Configuration);
  auto root = document->root();
  if (!root.is_object() || root.size() != 14) fail(Denial::Configuration);
  for (auto [name, value] : root.members()) {
    (void)value;
    constexpr std::array<std::string_view, 14> names{"version", "scope", "additional_calls", "micro_usd",
      "baseline_vision_calls", "baseline_vision_exposure_micro_usd", "baseline_ledger", "meter_ledger",
      "catalog", "owner_selection", "billing_guarantee", "unknown_cost_policy", "unused_old_margin", "excluded_paid_scope"};
    if (std::find(names.begin(), names.end(), name) == names.end()) fail(Denial::Configuration);
  }
  auto eq = [&](std::string_view key, std::string_view expected) {
    auto v = root.get(key);
    if (!v.is_string() || v.as_string() != expected) fail(Denial::Configuration);
  };
  if (config_number(root.get("version")) != 1 || config_number(root.get("additional_calls")) != call_limit ||
      config_number(root.get("micro_usd")) != money_limit || config_number(root.get("baseline_vision_calls")) != 99 ||
      config_number(root.get("baseline_vision_exposure_micro_usd")) != 18979680) fail(Denial::Configuration);
  eq("scope", "five_current_chat_api_qualification"); eq("baseline_ledger", "build/m5-live.ledger");
  eq("meter_ledger", "build/qualification-meter.ledger"); eq("catalog", "config/model-catalog.json");
  eq("billing_guarantee", "local_meter_and_stop_not_invoice"); eq("unknown_cost_policy", "retain_reservation");
  eq("unused_old_margin", "not_renewed");
  auto owner = root.get("owner_selection");
  if (!owner.is_string() || owner.as_string().empty()) fail(Denial::Configuration);
  auto excluded = root.get("excluded_paid_scope");
  if (!excluded.is_array() || excluded.size() != 3) fail(Denial::Configuration);
  std::size_t index = 0;
  for (auto name : {"video_generation", "image_generation", "remote_graphrag"}) {
    auto v = excluded.at(index++);
    if (!v.is_string() || v.as_string() != name) fail(Denial::Configuration);
  }
}
void baseline(std::string_view bytes) {
  if (!bytes.starts_with("SPCANARY1\n")) fail(Denial::Corruption);
  bytes.remove_prefix(10);
  struct Old { std::uint64_t calls{}, tokens{}, cost{}; };
  std::array<Old, 9> old{};
  while (!bytes.empty()) {
    auto c = split(line(bytes));
    if (c.count != 4) fail(Denial::Corruption);
    auto provider = integer(c.value[0]);
    if (provider >= old.size()) fail(Denial::Corruption);
    Old next{integer(c.value[1]), integer(c.value[2]), integer(c.value[3])};
    auto& previous = old[provider];
    if (next.calls != add(previous.calls, 1) || next.tokens <= previous.tokens || next.cost <= previous.cost)
      fail(Denial::Corruption);
    previous = next;
  }
  std::uint64_t calls = 0, cost = 0;
  for (std::size_t i = 4; i < old.size(); ++i) {
    calls = add(calls, old[i].calls); cost = add(cost, old[i].cost);
  }
  if (calls != 99 || cost != 18979680) fail(Denial::Identity);
}
Fd file(int dir, const char* name, int flags) {
  Fd fd(::openat(dir, name, flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
  (void)identity(fd.get());
  return fd;
}
std::string config(int dir, const char* name) {
  Fd fd(::openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
  (void)identity(fd.get(), false);
  return read_all(fd.get(), 1 << 20);
}
struct Extension {
  Identity identity;
  std::string declaration_hash, prefix_hash;
  std::uint64_t prefix_rows{}, prefix_calls{}, prefix_spent{}, prefix_held{};
};
std::optional<Extension> extension(int dir, std::string_view approval_hash,
    std::string_view baseline_hash, std::string_view catalog_hash, std::string_view activation_hash) {
  int descriptor = ::openat(dir, "qualification-authorization-extension.json",
      O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (descriptor < 0 && errno == ENOENT) return std::nullopt;
  Fd fd(descriptor);
  auto id = identity(fd.get(), false);
  auto raw = read_all(fd.get(), 1 << 20);
  auto parsed = json::parse(raw);
  auto* document = std::get_if<json::Document>(&parsed);
  if (!document) fail(Denial::Configuration);
  auto root = document->root();
  constexpr std::array<std::string_view, 14> names{"version", "scope", "additional_calls", "micro_usd",
      "original_authorization", "baseline_ledger", "meter_ledger", "catalog", "activation_anchor",
      "owner_selection", "billing_guarantee", "unknown_cost_policy", "excluded_paid_scope", "binding"};
  if (!root.is_object() || root.size() != names.size()) fail(Denial::Configuration);
  for (auto [name, value] : root.members()) {
    (void)value;
    if (std::find(names.begin(), names.end(), name) == names.end()) fail(Denial::Configuration);
  }
  auto eq = [](json::Value object, std::string_view key, std::string_view expected, Denial denial) {
    auto value = object.get(key);
    if (!value.is_string() || value.as_string() != expected) fail(denial);
  };
  if (config_number(root.get("version")) != 1 ||
      config_number(root.get("additional_calls")) != extension_calls ||
      config_number(root.get("micro_usd")) != extension_money) fail(Denial::Configuration);
  eq(root, "scope", "five_current_chat_api_qualification", Denial::Configuration);
  eq(root, "original_authorization", "config/qualification-authorization.json", Denial::Configuration);
  eq(root, "baseline_ledger", "build/m5-live.ledger", Denial::Configuration);
  eq(root, "meter_ledger", "build/qualification-meter.ledger", Denial::Configuration);
  eq(root, "catalog", "config/model-catalog.json", Denial::Configuration);
  eq(root, "activation_anchor", "build/m5-live.ledger.qualification-binding", Denial::Configuration);
  eq(root, "owner_selection", "추가 480회·US$3 승인", Denial::Configuration);
  eq(root, "billing_guarantee", "local_meter_and_stop_not_invoice", Denial::Configuration);
  eq(root, "unknown_cost_policy", "retain_reservation", Denial::Configuration);
  auto excluded = root.get("excluded_paid_scope");
  if (!excluded.is_array() || excluded.size() != 3) fail(Denial::Configuration);
  std::size_t index = 0;
  for (auto name : {"video_generation", "image_generation", "remote_graphrag"}) {
    auto value = excluded.at(index++);
    if (!value.is_string() || value.as_string() != name) fail(Denial::Configuration);
  }
  auto binding = root.get("binding");
  constexpr std::array<std::string_view, 9> bindings{"original_authorization_sha256", "baseline_sha256",
      "catalog_sha256", "activation_sha256", "meter_prefix_sha256", "meter_prefix_rows",
      "meter_prefix_calls", "meter_prefix_spent_micro_usd", "meter_prefix_held_micro_usd"};
  if (!binding.is_object() || binding.size() != bindings.size()) fail(Denial::Configuration);
  for (auto [name, value] : binding.members()) {
    (void)value;
    if (std::find(bindings.begin(), bindings.end(), name) == bindings.end()) fail(Denial::Configuration);
  }
  // Recheck mutable configuration paths even on a previously opened Meter.
  if (hash(config(dir, "qualification-authorization.json")) != approval_hash ||
      hash(config(dir, "model-catalog.json")) != catalog_hash) fail(Denial::Identity);
  eq(binding, "original_authorization_sha256", approval_hash, Denial::Identity);
  eq(binding, "baseline_sha256", baseline_hash, Denial::Identity);
  eq(binding, "catalog_sha256", catalog_hash, Denial::Identity);
  eq(binding, "activation_sha256", activation_hash, Denial::Identity);
  auto prefix = binding.get("meter_prefix_sha256");
  if (!prefix.is_string() || !digest(prefix.as_string())) fail(Denial::Configuration);
  return Extension{id, hash(raw), std::string(prefix.as_string()),
      config_number(binding.get("meter_prefix_rows")), config_number(binding.get("meter_prefix_calls")),
      config_number(binding.get("meter_prefix_spent_micro_usd")),
      config_number(binding.get("meter_prefix_held_micro_usd"))};
}
struct Entry {
  std::uint64_t sequence{}, input{}, output{}, reserve{};
  std::string_view request, fingerprint, model, family;
  std::optional<Outcome> settled;
  SettlementKind kind = SettlementKind::UnknownHold;
  std::uint64_t charged{};
};
struct State {
  State() = default;
  State(const State&) = delete;
  State& operator=(const State&) = delete;
  State(State&&) noexcept = default;
  State& operator=(State&&) noexcept = default;
  std::string bytes, chain;
  std::uint64_t rows{};
  Totals totals;
  // Replays move between frames; keep fixed-capacity storage off consumer stacks
  // and transfer ownership without copying entries or their views into bytes.
  std::unique_ptr<Entry[]> entries = std::make_unique<Entry[]>(maximum_calls);
};
void prefix_binding(const Extension& approved_extension, const State& state, std::string_view prefix) {
  if (hash(prefix) != approved_extension.prefix_hash || state.rows != approved_extension.prefix_rows ||
      state.totals.calls != approved_extension.prefix_calls ||
      state.totals.spent_micro_usd != approved_extension.prefix_spent ||
      state.totals.held_micro_usd != approved_extension.prefix_held) fail(Denial::Identity);
}
std::pair<SettlementKind, std::uint64_t> price_outcome(const Catalog& catalog, const Entry& entry, const Outcome& o) {
  if (o.final_consistent) {
    auto priced = catalog.usage_cost(*catalog.find(entry.model, entry.family), o.usage, entry.input, entry.output);
    if (auto* amount = std::get_if<PricedUsage>(&priced); amount && amount->micro_usd <= entry.reserve)
      return {amount->kind == PriceKind::Exact ? SettlementKind::Exact : SettlementKind::UpperBound, amount->micro_usd};
  }
  return {SettlementKind::UnknownHold, entry.reserve};
}
void account(Totals& totals, const Entry& entry, SettlementKind kind, std::uint64_t charged) {
  if (kind == SettlementKind::UnknownHold) ++totals.unknown_settlements;
  else {
    totals.held_micro_usd -= entry.reserve;
    totals.spent_micro_usd = add(totals.spent_micro_usd, charged);
    if (kind == SettlementKind::Exact) ++totals.exact_settlements;
    else ++totals.upper_bound_settlements;
  }
}
State replay(int fd, std::string_view grant, const Catalog& catalog, const Extension* approved_extension) {
  State state;
  state.bytes = read_all(fd, 1 << 20);
  std::string_view rest(state.bytes);
  const auto header = line(rest);
  if (!header.starts_with("SPQUAL1 ") || header.substr(8) != grant) fail(Denial::Identity);
  state.chain = hash(std::string_view(state.bytes).substr(0, header.size() + 1));
  while (!rest.empty()) {
    auto row = line(rest);
    auto c = split(row);
    if (c.count < 4 || integer(c.value[0]) != add(state.rows, 1) ||
        c.value[c.count - 2] != state.chain || !digest(c.value[c.count - 1])) fail(Denial::Corruption);
    auto hash_start = row.rfind(' ');
    if (hash(row.substr(0, hash_start)) != c.value[c.count - 1]) fail(Denial::Corruption);
    if (c.value[1] == "R") {
      if (c.count != 11 || state.totals.calls >= state.totals.call_limit ||
          !digest(c.value[2]) || !digest(c.value[3])) fail(Denial::Corruption);
      for (std::size_t i = 0; i < state.totals.calls; ++i)
        if (state.entries[i].request == c.value[2]) fail(Denial::Corruption);
      auto* price = catalog.find(c.value[4], c.value[5]);
      if (!price) fail(Denial::Identity);
      auto in = integer(c.value[6]), out = integer(c.value[7]), reserve = integer(c.value[8]);
      auto expected = catalog.reserve_cost(*price, in, out);
      auto* amount = std::get_if<std::uint64_t>(&expected);
      if (!amount || *amount != reserve || reserve > state.totals.money_limit_micro_usd -
          add(state.totals.spent_micro_usd, state.totals.held_micro_usd)) fail(Denial::Corruption);
      auto& entry = state.entries[state.totals.calls++];
      entry = {state.rows + 1, in, out, reserve, c.value[2], c.value[3], c.value[4], c.value[5], std::nullopt};
      state.totals.held_micro_usd = add(state.totals.held_micro_usd, reserve);
    } else if (c.value[1] == "S") {
      if (c.count != 15) fail(Denial::Corruption);
      auto reservation = integer(c.value[2]);
      Entry* entry = nullptr;
      for (std::size_t i = 0; i < state.totals.calls; ++i)
        if (state.entries[i].sequence == reservation) entry = &state.entries[i];
      if (!entry || entry->settled || (c.value[3] != "0" && c.value[3] != "1")) fail(Denial::Corruption);
      Outcome o{c.value[3] == "1", {optional_integer(c.value[4]), optional_integer(c.value[5]),
          optional_integer(c.value[6]), optional_integer(c.value[7]), optional_integer(c.value[8]),
          optional_integer(c.value[9]), optional_integer(c.value[10])}};
      auto [kind, charged] = price_outcome(catalog, *entry, o);
      if (integer(c.value[11]) != static_cast<std::uint64_t>(kind) || integer(c.value[12]) != charged)
        fail(Denial::Corruption);
      account(state.totals, *entry, kind, charged);
      entry->settled = o;
      entry->kind = kind; entry->charged = charged;
    } else if (c.value[1] == "A") {
      if (c.count != 9 || state.totals.extension_authorization_events ||
          integer(c.value[5]) != extension_calls || integer(c.value[6]) != extension_money)
        fail(Denial::Corruption);
      if (!approved_extension || c.value[2] != approved_extension->declaration_hash ||
          integer(c.value[3]) != approved_extension->identity.dev ||
          integer(c.value[4]) != approved_extension->identity.ino) fail(Denial::Identity);
      prefix_binding(*approved_extension, state, std::string_view(state.bytes).substr(
          0, static_cast<std::size_t>(row.data() - state.bytes.data())));
      state.totals.extension_authorization_events = 1;
      state.totals.extension_calls = extension_calls;
      state.totals.extension_micro_usd = extension_money;
      state.totals.call_limit = add(call_limit, extension_calls);
      state.totals.money_limit_micro_usd = add(money_limit, extension_money);
    } else fail(Denial::Corruption);
    state.chain = c.value[c.count - 1];
    ++state.rows;
  }
  return state;
}
void transaction(int fd, const State& state, std::string payload) {
  auto row = std::to_string(state.rows + 1) + ' ' + payload + ' ' + state.chain;
  row += ' ' + hash(row) + '\n';
  append(fd, row);
}
}  // namespace

struct Meter::Impl {
  Fd build, conf;
  std::shared_ptr<const Catalog> catalog;
  std::string grant, anchor, approval_hash, catalog_hash;
  Identity baseline_id, ledger_id;
  std::string baseline_hash;
  mutable std::mutex mutex;
  Impl(Fd directory_fd, Fd configuration_fd, std::shared_ptr<const Catalog> prices)
      : build(std::move(directory_fd)), conf(std::move(configuration_fd)), catalog(std::move(prices)) {}
  State current_state(int ledger) const {
    auto authorization = extension(conf.get(), approval_hash, baseline_hash, catalog_hash, grant);
    auto state = replay(ledger, grant, *catalog, authorization ? &*authorization : nullptr);
    if (authorization && !state.totals.extension_authorization_events) {
      prefix_binding(*authorization, state, state.bytes);
      transaction(ledger, state, "A " + authorization->declaration_hash + ' ' +
          std::to_string(authorization->identity.dev) + ' ' + std::to_string(authorization->identity.ino) +
          ' ' + std::to_string(extension_calls) + ' ' + std::to_string(extension_money));
      // Replaying keeps Entry views tied to their owning bytes after the append.
      return replay(ledger, grant, *catalog, &*authorization);
    }
    return state;
  }
  Fd locked_ledger() const {
    // Reopen each time: separate open descriptions make flock effective across
    // different Meter instances AND threads, not merely across processes.
    auto binding = file(build.get(), "m5-live.ledger.qualification-binding", O_RDONLY);
    lock(binding.get());
    if (read_all(binding.get(), 4096) != anchor) fail(Denial::Identity);
    auto original = file(build.get(), "m5-live.ledger", O_RDONLY);
    lock(original.get());
    auto old_id = identity(original.get());
    if (old_id.dev != baseline_id.dev || old_id.ino != baseline_id.ino ||
        hash(read_all(original.get(), 65536)) != baseline_hash) fail(Denial::Identity);
    auto ledger = file(build.get(), "qualification-meter.ledger", O_RDWR);
    lock(ledger.get());
    auto id = identity(ledger.get());
    if (id.dev != ledger_id.dev || id.ino != ledger_id.ino) fail(Denial::Identity);
    return ledger;
  }
};
Claim::Claim(std::string grant, std::uint64_t sequence, Binding binding, std::uint64_t reserved)
    : grant_(std::move(grant)), sequence_(sequence), reserved_(reserved), binding_(std::move(binding)) {}
Claim::Claim(Claim&& other) noexcept : grant_(std::move(other.grant_)), sequence_(std::exchange(other.sequence_, 0)),
    reserved_(std::exchange(other.reserved_, 0)), binding_(std::move(other.binding_)) {}
Claim& Claim::operator=(Claim&& other) noexcept {
  if (this != &other) {
    grant_ = std::move(other.grant_); sequence_ = std::exchange(other.sequence_, 0);
    reserved_ = std::exchange(other.reserved_, 0); binding_ = std::move(other.binding_);
  }
  return *this;
}
Meter::Meter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Meter::~Meter() = default;
const Catalog& Meter::catalog() const noexcept { return *impl_->catalog; }
Result<std::unique_ptr<Meter>> Meter::open(const std::string& project_root) {
  try {
    std::error_code ec;
    auto path = std::filesystem::canonical(project_root, ec);
    if (ec) fail(Denial::Storage);
    Fd root(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    directory(root.get());
    Fd conf(::openat(root.get(), "config", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    directory(conf.get());
    auto raw_approval = config(conf.get(), "qualification-authorization.json");
    approved(raw_approval);
    auto raw_catalog = config(conf.get(), "model-catalog.json");
    auto loaded = Catalog::from_json(raw_catalog);
    if (auto* error = std::get_if<Error>(&loaded)) return *error;
    Fd build(::openat(root.get(), "build", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    directory(build.get());
    auto impl = std::make_unique<Impl>(std::move(build), std::move(conf),
        std::get<std::shared_ptr<const Catalog>>(std::move(loaded)));
    impl->approval_hash = hash(raw_approval); impl->catalog_hash = hash(raw_catalog);
    int anchor_fd = ::openat(impl->build.get(), "m5-live.ledger.qualification-binding",
        O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    bool fresh = anchor_fd >= 0;
    if (!fresh) {
      if (errno != EEXIST) fail(Denial::Storage);
      anchor_fd = ::openat(impl->build.get(), "m5-live.ledger.qualification-binding", O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    }
    Fd anchor(anchor_fd);
    (void)identity(anchor.get()); lock(anchor.get());
    auto original = file(impl->build.get(), "m5-live.ledger", O_RDONLY);
    lock(original.get());
    auto bytes = read_all(original.get(), 65536);
    baseline(bytes);
    impl->baseline_id = identity(original.get()); impl->baseline_hash = hash(bytes);
    // Campaign identity is independent of user request profiles, but bound to
    // exact approval, catalogue, baseline identity, and canonical root location.
    std::string base = "SPQUALBIND1 " + hash(path.string()) + ' ' + impl->baseline_hash + ' ' + hash(raw_catalog) +
        ' ' + std::to_string(impl->baseline_id.dev) + ' ' + std::to_string(impl->baseline_id.ino);
    int ledger_fd = ::openat(impl->build.get(), "qualification-meter.ledger",
        (fresh ? O_RDWR | O_CREAT | O_EXCL : O_RDWR) | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    Fd ledger(ledger_fd);
    lock(ledger.get()); impl->ledger_id = identity(ledger.get());
    impl->anchor = base + ' ' + std::to_string(impl->ledger_id.dev) + ' ' + std::to_string(impl->ledger_id.ino) + '\n';
    impl->grant = hash(impl->anchor);
    if (fresh) {
      // Complete and fsync both files and directory BEFORE granting any claim.
      if (!read_all(anchor.get(), 4096).empty() || !read_all(ledger.get(), 1 << 20).empty()) fail(Denial::Corruption);
      append(ledger.get(), "SPQUAL1 " + impl->grant + '\n');
      append(anchor.get(), impl->anchor); sync(impl->build.get());
    } else if (read_all(anchor.get(), 4096) != impl->anchor) fail(Denial::Identity);
    (void)impl->current_state(ledger.get());
    return std::unique_ptr<Meter>(new Meter(std::move(impl)));
  } catch (const Failure& f) { return f.error; }
}
Result<Claim> Meter::reserve(Binding b) {
  try {
    if (!digest(b.client_request_id) || !digest(b.fingerprint)) fail(Denial::Bounds);
    const auto* model = impl_->catalog->find(b.model, b.family);
    if (!model) fail(Denial::UnknownModel);
    auto price = impl_->catalog->reserve_cost(*model, b.input_bound, b.output_bound);
    if (auto* error = std::get_if<Error>(&price)) return *error;
    auto amount = std::get<std::uint64_t>(price);
    std::lock_guard guard(impl_->mutex);
    auto ledger = impl_->locked_ledger();
    auto state = impl_->current_state(ledger.get());
    for (std::size_t i = 0; i < state.totals.calls; ++i)
      if (state.entries[i].request == b.client_request_id) fail(Denial::DuplicateRequest);
    if (state.totals.calls >= state.totals.call_limit) fail(Denial::Calls);
    if (amount > state.totals.money_limit_micro_usd -
        add(state.totals.spent_micro_usd, state.totals.held_micro_usd)) fail(Denial::Money);
    std::string payload = "R " + b.client_request_id + ' ' + b.fingerprint + ' ' + b.model + ' ' + b.family + ' ' +
        std::to_string(b.input_bound) + ' ' + std::to_string(b.output_bound) + ' ' + std::to_string(amount);
    // Allocate opaque claim BEFORE committing; allocation failure cannot return
    // an apparently unreserved request after a successful durable transaction.
    Claim claim(impl_->grant, state.rows + 1, std::move(b), amount);
    transaction(ledger.get(), state, std::move(payload));
    return claim;
  } catch (const Failure& f) { return f.error; }
}
Result<Settlement> Meter::settle(const Claim& claim, const Outcome& o) {
  try {
    if (!claim.sequence_ || claim.grant_ != impl_->grant) fail(Denial::InvalidClaim);
    std::lock_guard guard(impl_->mutex);
    auto ledger = impl_->locked_ledger();
    auto state = impl_->current_state(ledger.get());
    Entry* entry = nullptr;
    for (std::size_t i = 0; i < state.totals.calls; ++i)
      if (state.entries[i].sequence == claim.sequence_) entry = &state.entries[i];
    const auto& b = claim.binding_;
    if (!entry || entry->request != b.client_request_id || entry->fingerprint != b.fingerprint ||
        entry->model != b.model || entry->family != b.family || entry->input != b.input_bound ||
        entry->output != b.output_bound || entry->reserve != claim.reserved_) fail(Denial::InvalidClaim);
    if (entry->settled) {
      if (*entry->settled != o) fail(Denial::SettlementConflict);
      return Settlement{state.totals, entry->kind, entry->charged};
    }
    auto [kind, charged] = price_outcome(*impl_->catalog, *entry, o);
    transaction(ledger.get(), state, "S " + std::to_string(entry->sequence) + ' ' + outcome_text(o) +
        ' ' + std::to_string(static_cast<std::uint64_t>(kind)) + ' ' + std::to_string(charged));
    account(state.totals, *entry, kind, charged);
    return Settlement{state.totals, kind, charged};
  } catch (const Failure& f) { return f.error; }
}
Result<Totals> Meter::totals() const {
  try {
    std::lock_guard guard(impl_->mutex);
    auto ledger = impl_->locked_ledger();
    return impl_->current_state(ledger.get()).totals;
  } catch (const Failure& f) { return f.error; }
}
}  // namespace sp::qualification
