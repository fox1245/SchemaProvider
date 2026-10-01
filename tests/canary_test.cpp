#include "canary/canary.h"
#include "canary/io.h"
#include "support/runtime_peer.h"
#include <barrier>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <sys/stat.h>
#include <thread>

namespace {
using namespace sp::canary;
using runtime_test::require;
constexpr auto secret = "CANARY_SECRET_KEY_MARKER";
class Directory {
 public:
  Directory() {
    char path[] = "/tmp/sp-canary-XXXXXX";
    auto* result = ::mkdtemp(path); require(result, "temporary directory failed"); path_ = result;
  }
  ~Directory() { std::error_code ignored; std::filesystem::remove_all(path_, ignored); }
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
  std::string file(std::string_view name) const { return path_ + '/' + std::string(name); }
 private:
  std::string path_;
};
void save(const std::string& path, std::string_view value) {
  sp::canary::detail::Fd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
  sp::canary::detail::write_all(fd.get(), value);
}
std::string load(const std::string& path) { return sp::canary::detail::read_file(path, 65536, true); }
void private_text(std::string_view text) {
  for (auto marker : {secret, "CANARY_SIGNATURE_MARKER", "CANARY_THINKING_MARKER", "CANARY_REDACTED_MARKER",
      "CANARY_RESPONSE_MARKER", "CANARY_ACCOUNT_ID_MARKER", "PRIVATE_PATH_MARKER"})
    require(text.find(marker) == std::string_view::npos, "private marker leaked");
}
std::string source(std::string_view model, std::uint64_t port, bool anthropic = true,
    std::uint64_t calls = 16, std::uint64_t tokens = 1000000, std::uint64_t cost = 10000000) {
  return "{\"version\":1,\"provider\":" + sp::json::quote(anthropic ? "anthropic" : "openai") +
      ",\"model\":" + sp::json::quote(model) + ",\"origin\":\"http://127.0.0.1:" + std::to_string(port) +
      "\",\"input_bound_kind\":\"model_context_window\",\"max_input_tokens\":4096,\"max_output_tokens\":2048,"
      "\"input_micro_usd_per_million\":1000000,\"output_micro_usd_per_million\":2000000,\"call_cap\":" + std::to_string(calls) +
      ",\"token_cap\":" + std::to_string(tokens) + ",\"micro_usd_cap\":" + std::to_string(cost) +
      (anthropic ? ",\"thinking_budget\":1024" : "") + ",\"provenance\":{\"model_url\":" +
      sp::json::quote(anthropic ? "https://platform.claude.com/docs/en/models/overview" : "https://developers.openai.com/api/docs/models/gpt-4.1-mini.md") +
      ",\"pricing_url\":" + sp::json::quote(anthropic ? "https://platform.claude.com/docs/en/about-claude/pricing" : "https://developers.openai.com/api/docs/models/gpt-4.1-mini.md") +
      ",\"verified_at\":\"2026-10-01\"}}";
}
std::string replace(std::string value, std::string_view old, std::string_view next) {
  auto at = value.find(old); require(at != std::string::npos, "test mutation missing"); value.replace(at, old.size(), next); return value;
}
template<class F> void rejects(F&& f) {
  bool rejected = false;
  try { f(); } catch (const std::exception& error) { rejected = true; private_text(error.what()); }
  require(rejected, "unsafe input accepted");
}
void profiles_and_credentials() {
  Directory directory;
  const auto valid = source("synthetic", 18080);
  rejects([&] { parse_profile(valid); });
  for (auto invalid : {
      replace(valid, "\"version\":1", "\"version\":1,\"version\":1"),
      replace(valid, "\"version\":1", "\"version\":1,\"extra\":1"),
      replace(valid, "\"version\":1", "\"version\":2"),
      replace(valid, "4096", "18446744073709551616"),
      replace(valid, "4096", "18446744073709551615"),
      replace(valid, "4096", "4096.0"),
      replace(valid, "1000000,\"output_micro", "18446744073709551615,\"output_micro"),
      replace(valid, "127.0.0.1", "localhost"),
      replace(valid, "127.0.0.1", "127.0.0.1.evil.test"),
      replace(valid, "http://127.0.0.1:18080", "https://api.anthropic.com/v1"),
      replace(valid, "http://127.0.0.1:18080", "https://api.anthropic.com@evil.test"),
      replace(valid, "model_context_window", "character_guess"),
      replace(valid, "\"call_cap\":16", "\"call_cap\":17"),
      replace(valid, "\"micro_usd_cap\":10000000", "\"micro_usd_cap\":10000001"),
      replace(valid, "\"thinking_budget\":1024", "\"thinking_budget\":2048") })
    rejects([&] { parse_profile(invalid, true); });
  rejects([&] { parse_profile(replace(valid, "2026-10-01", "2026-02-30"), true); });
  auto live = replace(source("claude-haiku-4-5-20251001", 18080), "http://127.0.0.1:18080", "https://api.anthropic.com");
  live = replace(live, "\"max_input_tokens\":4096", "\"max_input_tokens\":200000");
  live = replace(live, "\"input_micro_usd_per_million\":1000000", "\"input_micro_usd_per_million\":2000000");
  live = replace(live, "\"output_micro_usd_per_million\":2000000", "\"output_micro_usd_per_million\":5000000");
  require(!parse_profile(live).loopback(), "live target misclassified");
  rejects([&] { parse_profile(replace(live, "\"max_input_tokens\":200000", "\"max_input_tokens\":199999")); });
  rejects([&] { parse_profile(replace(live, "\"input_micro_usd_per_million\":2000000", "\"input_micro_usd_per_million\":1999999")); });
  rejects([&] { parse_profile(replace(live, "claude-haiku-4-5-20251001", "unreviewed-model")); });
  auto env = directory.file("PRIVATE_PATH_MARKER");
  save(env, "# synthetic only\nOPENAI_API_KEY=CANARY_SECRET_KEY_MARKER\nANTHROPIC_API_KEY=other\n");
  require(credential(Provider::OpenAI, env) == secret && credential(Provider::Anthropic, env) == "other", "credential selection failed");
  save(env, "ANTHROPIC_API_KEY=other\n"); require(credential(Provider::OpenAI, env).empty(), "missing credential not empty");
  save(env, "OPENAI_API_KEY=one\nOPENAI_API_KEY=two\n"); rejects([&] { credential(Provider::OpenAI, env); });
  save(env, "OPENAI_API_KEY=$(echo private)\n"); rejects([&] { credential(Provider::OpenAI, env); });
  save(env, "OPENAI_API_KEY=secret\n"); require(::chmod(env.c_str(), 0644) == 0, "chmod failed");
  rejects([&] { credential(Provider::OpenAI, env); });
  auto link = directory.file("link"); require(::symlink(env.c_str(), link.c_str()) == 0, "symlink failed");
  rejects([&] { credential(Provider::OpenAI, link); });
  Bounds rounding{1, 1, 1, 1, 1, 2, 2}; require(reserved_cost(rounding) == 2, "per-component rounding lost");
  rounding.input_tokens = std::numeric_limits<std::uint64_t>::max(); rounding.input_rate = 2;
  rejects([&] { reserved_cost(rounding); });
}
void adversarial_guards(runtime_test::Peer& peer) {
  Directory directory;
  const auto misleading_model = peer.arm("misleading");
  auto misleading = run(parse_profile(source(misleading_model, peer.port), true), directory.file("control"), secret);
  const bool specific_control = misleading.replay == Replay::ReplayAcceptanceUnobservable &&
      misleading.cases[4].reason == Reason::NegativeInconclusive;
  const auto chat_model = peer.arm("reject");
  auto chat = run(parse_profile(source(chat_model, peer.port, false), true), directory.file("tier"), secret);
  const auto observed = peer.stats(chat_model);
  const bool standard_tier = observed.root().get("invalid").as_uint() == 0;
  const auto profile = parse_profile(source("corruption", peer.port, true, 1), true);
  const auto ledger = directory.file("empty");
  require(reserve(profile, ledger).result == Reservation::Allowed, "corruption precondition failed");
  save(ledger, "");
  bool empty_rejected = false;
  try { (void)reserve(profile, ledger); } catch (...) { empty_rejected = true; }
  std::cout << "canary adversarial standard-tier=" << standard_tier
            << " signature-specific=" << specific_control << " empty-ledger-rejected=" << empty_rejected << '\n';
  require(standard_tier && specific_control && empty_rejected, "canary safety regressions");
}
void replay_cases(runtime_test::Peer& peer) {
  for (auto scenario : {"reject", "accept", "absent", "unrelated"}) {
    Directory directory;
    auto model = peer.arm(scenario);
    auto profile = parse_profile(source(model, peer.port), true);
    runtime_test::LogCapture diagnostics;
    auto report = run(profile, directory.file("ledger"), secret);
    private_text(diagnostics.finish()); private_text(report_json(report)); private_text(load(directory.file("ledger")));
    auto stats = peer.stats(model); auto root = stats.root();
    const bool absent = std::string_view(scenario) == "absent";
    const bool rejected = std::string_view(scenario) == "reject";
    peer.count(model, absent ? 4 : 5, rejected || std::string_view(scenario) == "unrelated" ? 1 : 0);
    require(root.get("positive").as_uint() == 1 && root.get("retained").as_uint() == 1 && root.get("sse").as_uint() == 1, "positive replay or SSE missing at peer");
    require(root.get("negative").as_uint() == (absent ? 0 : 1) && root.get("differences").as_uint() == (absent ? 0 : 1), "negative dispatch was not exactly one byte");
    require(report.positive_retained && report.signature_mutated == !absent, "wire proof flags wrong");
    require(report.native_leaves == (absent ? 1 : 2), "native leaves lost");
    for (std::size_t i = 0; i < 4; ++i) require(report.cases[i].state == State::Passed && report.cases[i].dispatched, "positive scenario failed");
    require(report.reserved.calls == (absent ? 4 : 5) && report.reserved.tokens == report.reserved.calls * 6144 &&
        report.reserved.micro_usd == report.reserved.calls * 8192, "reservation totals wrong");
    require(report.replay == (rejected ? Replay::ReplayVerified : Replay::ReplayAcceptanceUnobservable), "false replay verification");
    if (absent) require(report.cases[4].state == State::NotRun && report.cases[4].reason == Reason::MissingSignature, "absent signature not explicit");
    else if (rejected) require(report.cases[4].state == State::Passed && report.cases[4].reason == Reason::SignatureRejected, "signature rejection not recognized");
    else require(report.cases[4].state == State::Failed && report.cases[4].reason ==
        (std::string_view(scenario) == "accept" ? Reason::NegativeAccepted : Reason::NegativeInconclusive), "control classification wrong");
    require(!report.cases[0].input_tokens && report.cases[0].input_uncached == 2 &&
        report.cases[0].output_tokens == 3 && !report.cases[0].cache_read &&
        !report.cases[0].cache_write && !report.cases[0].estimated_micro_usd,
        "unknown cache counts became known total or cost");
  }
  Directory directory;
  auto model = peer.arm("reject"); auto profile = parse_profile(source(model, peer.port, false), true);
  auto report = run(profile, directory.file("ledger"), secret);
  peer.count(model, 4); require(report.replay == Replay::NotApplicable, "Chat claimed native verification");
  for (const auto& item : report.cases) require(item.state == State::Passed, "Chat loop failed");
  private_text(report_json(report));
  require(report.cases[0].input_tokens == 2 && report.cases[0].output_tokens == 3 &&
      report.cases[0].estimated_micro_usd == 8, "reported Chat usage estimate lost");
  Directory known_directory;
  const auto known_model = peer.arm("known-usage");
  auto known = run(parse_profile(source(known_model, peer.port), true), known_directory.file("ledger"), secret);
  require(known.cases[0].input_tokens == 2 && known.cases[0].output_tokens == 3 &&
      known.cases[0].estimated_micro_usd == 8, "known Messages components did not produce cost estimate");
  peer.count(known_model, 5, 1);
  Directory inconsistent_directory;
  const auto inconsistent_model = peer.arm("inconsistent-usage");
  auto inconsistent = run(parse_profile(source(inconsistent_model, peer.port, false), true),
      inconsistent_directory.file("ledger"), secret);
  require(inconsistent.cases[0].state == State::Failed && inconsistent.cases[0].reason == Reason::InvalidUsage &&
      inconsistent.cases[0].usage_quality == sp::UsageQuality::Inconsistent,
      "contradictory vendor usage was reported as canary success");
  peer.count(inconsistent_model, 4);
}
void budget_axes(runtime_test::Peer& peer) {
  for (unsigned axis = 0; axis < 3; ++axis) {
    Directory directory;
    auto model = peer.arm("reject");
    auto profile = parse_profile(source(model, peer.port, true, axis == 0 ? 1 : 16, axis == 1 ? 6144 : 1000000, axis == 2 ? 8192 : 10000000), true);
    const auto ledger = directory.file("ledger");
    auto first = run(profile, ledger, secret);
    require(first.cases[0].state == State::Passed, "first budgeted call failed");
    require(first.cases[1].state == State::NotRun && first.cases[1].reason ==
        (axis == 0 ? Reason::CallBudget : axis == 1 ? Reason::TokenBudget : Reason::CostBudget), "wrong exhausted axis");
    auto restart = run(profile, ledger, secret);
    require(restart.reserved.calls == 1 && restart.cases[0].state == State::NotRun, "restart reset budget");
    peer.count(model, 1);
    private_text(load(ledger));
  }
  {
    Directory directory; auto model = peer.arm("reject");
    auto profile = parse_profile(source(model, peer.port, true, 4), true);
    auto report = run(profile, directory.file("ledger"), secret);
    peer.count(model, 4);
    require(report.cases[3].state == State::Passed && report.cases[4].state == State::NotRun &&
        report.cases[4].reason == Reason::CallBudget && report.replay == Replay::ReplayAcceptanceUnobservable,
        "budget-blocked negative control fabricated verification");
  }
  {
    Directory directory; auto model = peer.arm("remote-error");
    auto profile = parse_profile(source(model, peer.port, true, 2), true);
    auto report = run(profile, directory.file("ledger"), secret);
    peer.count(model, 2, 2);
    require(report.reserved.calls == 2 && report.cases[0].state == State::Failed && report.cases[1].state == State::Failed && report.cases[2].state == State::NotRun, "failed calls not charged exactly once");
    private_text(report_json(report)); private_text(load(directory.file("ledger")));
  }
  {
    Directory directory; auto model = peer.arm("reject"); auto profile = parse_profile(source(model, peer.port), true);
    auto missing = run(profile, directory.file("never-created"), {});
    for (const auto& item : missing.cases) require(item.state == State::NotRun && item.reason == Reason::MissingCredential, "missing credential green");
    require(!std::filesystem::exists(directory.file("never-created")), "missing credential touched ledger");
    save(directory.file("torn"), "SPCANARY1\n1 1 6144 8192\n1 2 12");
    auto torn = run(profile, directory.file("torn"), secret);
    require(torn.cases[0].state == State::NotRun && torn.cases[0].reason == Reason::LedgerFailure, "torn ledger reset");
    peer.count(model, 0);
  }
  {
    Directory directory; auto model = peer.arm("reject");
    const auto source_text = source(model, peer.port, true, 16, std::numeric_limits<std::uint64_t>::max());
    rejects([&] { parse_profile(replace(source_text, "4096", "18446744073709551615"), true); });
    auto profile = parse_profile(source_text, true);
    save(directory.file("near-overflow"), "SPCANARY1\n1 1 18446744073709551614 1\n");
    auto report = run(profile, directory.file("near-overflow"), secret);
    require(report.cases[0].state == State::NotRun && report.cases[0].reason == Reason::TokenBudget,
        "cumulative token overflow dispatched");
    peer.count(model, 0);
  }
  {
    Directory directory; auto model = peer.arm("reject");
    const auto ledger = directory.file("shared-providers");
    auto anthropic = parse_profile(source(model, peer.port, true, 1), true);
    auto openai = parse_profile(source(model, peer.port, false, 1), true);
    auto a = run(anthropic, ledger, secret), b = run(openai, ledger, secret);
    require(a.cases[0].state == State::Passed && b.cases[0].state == State::Passed &&
        ledger_totals(anthropic, ledger).calls == 1 && ledger_totals(openai, ledger).calls == 1,
        "provider reservations were incorrectly shared");
    peer.count(model, 2);
  }
}
void shared_race(runtime_test::Peer& peer) {
  Directory directory; auto model = peer.arm("reject");
  auto profile = parse_profile(source(model, peer.port, true, 1), true);
  std::barrier start(3);
  auto worker = [&] { start.arrive_and_wait(); return run(profile, directory.file("ledger"), secret); };
  auto a = std::async(std::launch::async, worker), b = std::async(std::launch::async, worker);
  start.arrive_and_wait(); auto first = a.get(), second = b.get();
  peer.count(model, 1);
  require((first.cases[0].state == State::Passed) != (second.cases[0].state == State::Passed), "shared ledger admitted both workers");
  require(ledger_totals(profile, directory.file("ledger")).calls == 1, "race lost durable debit");
}
void process_ledger() {
  Directory directory;
  auto profile = parse_profile(source("process", 18080, true, 1), true);
  runtime_test::Pipe signal;
  const auto pid = ::fork();
  require(pid >= 0, "fork failed");
  if (!pid) {
    try {
      auto debit = reserve(profile, directory.file("ledger"));
      runtime_test::write_all(signal.writer.get(), debit.result == Reservation::Allowed ? "1" : "0");
      _exit(0);
    }
    catch (...) { runtime_test::write_all(signal.writer.get(), "E"); _exit(0); }
  }
  runtime_test::Process child(pid);
  signal.writer.reset();
  bool admitted = false;
  try { admitted = reserve(profile, directory.file("ledger")).result == Reservation::Allowed; }
  catch (...) {} // A competing initializer may conservatively report storage failure.
  char result{}; require(::read(signal.reader.get(), &result, 1) == 1, "child reservation failed");
  require(result == '0' || result == '1' || result == 'E', "unknown child reservation state");
  require(admitted != (result == '1'), "process race did not admit exactly one reservation");
  require(ledger_totals(profile, directory.file("ledger")).calls == 1, "process durable debit lost");
}
struct CliResult { int code; std::string text; };
CliResult cli(const char* executable, std::vector<std::string> arguments) {
  runtime_test::Pipe output;
  auto pid = ::fork(); require(pid >= 0, "CLI fork failed");
  if (!pid) {
    output.reader.reset();
    if (::dup2(output.writer.get(), STDOUT_FILENO) < 0 || ::dup2(output.writer.get(), STDERR_FILENO) < 0) _exit(126);
    output.writer.reset();
    std::vector<char*> argv{const_cast<char*>(executable)};
    for (auto& arg : arguments) argv.push_back(arg.data());
    argv.push_back(nullptr);
    ::execv(executable, argv.data()); _exit(127);
  }
  runtime_test::Process process(pid); output.writer.reset();
  auto text = sp::canary::detail::read_bounded(output.reader.get(), 65536);
  siginfo_t info{}; int result;
  do { result = ::waitid(P_PID, pid, &info, WEXITED | WNOWAIT); } while (result < 0 && errno == EINTR);
  require(result == 0 && info.si_code == CLD_EXITED, "CLI terminated abnormally");
  private_text(text); return {info.si_status, std::move(text)};
}
void cli_paths(runtime_test::Peer& peer, const char* executable) {
  Directory directory;
  const auto profile_file = directory.file("profile"), ledger = directory.file("ledger"), env = directory.file("PRIVATE_PATH_MARKER");
  auto plan = cli(executable, {"--profile", profile_file, "--ledger", ledger, "--env-file", env});
  require(plan.code == 0 && runtime_test::parse(plan.text).root().get("state").as_string() == "not_run" && !std::filesystem::exists(ledger), "plan performed I/O");
  require(cli(executable, {"--help", "--execute"}).code == 0, "help required live inputs");
  auto model = peer.arm("reject"); save(profile_file, source(model, peer.port)); save(env, "OPENAI_API_KEY=other\n");
  auto missing = cli(executable, {"--profile", profile_file, "--ledger", ledger, "--env-file", env, "--execute", "--test-loopback"});
  require(missing.code == 3 && !std::filesystem::exists(ledger), "CLI missing credential was green");
  save(env, "ANTHROPIC_API_KEY=CANARY_SECRET_KEY_MARKER\n");
  auto denied = cli(executable, {"--profile", profile_file, "--ledger", ledger, "--env-file", env, "--execute"});
  require(denied.code == 2 && !std::filesystem::exists(ledger), "loopback execution lacked explicit opt-in");
  peer.count(model, 0);
  auto live = cli(executable, {"--profile", profile_file, "--ledger", ledger, "--env-file", env, "--execute", "--test-loopback"});
  require(live.code == 0 && runtime_test::parse(live.text).root().get("replay").as_string() == "ReplayVerified", "CLI wire execution failed");
  peer.count(model, 5, 1); private_text(load(ledger));
}
} // namespace
int main(int argc, char** argv) {
  try {
    require(argc == 4, "usage: sp_canary_tests NODE CANARY_SERVER CANARY_CLI");
    profiles_and_credentials(); process_ledger();
    runtime_test::Peer peer(argv[1], argv[2]);
    adversarial_guards(peer);
    replay_cases(peer); budget_axes(peer); shared_race(peer); cli_paths(peer, argv[3]);
    std::cout << "canary behavior passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "canary behavior failed: " << error.what() << '\n'; return 1;
  }
}
