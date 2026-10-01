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
      "CANARY_RESPONSE_MARKER", "CANARY_ACCOUNT_ID_MARKER", "CANARY_CIPHER_MARKER", "PRIVATE_PATH_MARKER"})
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
std::string gemini_source(std::string_view model, std::uint64_t port, std::uint64_t calls = 4,
    std::uint64_t tokens = 100000, std::uint64_t cost = 1000000) {
  auto value = source(model, port, false, calls, tokens, cost);
  value = replace(value, "\"provider\":\"openai\"", "\"provider\":\"gemini\"");
  value = replace(value, "\"max_output_tokens\":2048", "\"max_output_tokens\":128");
  value = replace(value, "\"input_micro_usd_per_million\":1000000", "\"input_micro_usd_per_million\":100000");
  value = replace(value, "\"output_micro_usd_per_million\":2000000", "\"output_micro_usd_per_million\":400000");
  value = replace(value, "https://developers.openai.com/api/docs/models/gpt-4.1-mini.md", "https://ai.google.dev/gemini-api/docs/pricing");
  return replace(value, "https://developers.openai.com/api/docs/models/gpt-4.1-mini.md", "https://ai.google.dev/gemini-api/docs/pricing");
}
std::string responses_source(std::string_view model, std::uint64_t port, std::uint64_t calls = 6,
    std::uint64_t tokens = 100000, std::uint64_t cost = 1000000) {
  auto value = source(model, port, false, calls, tokens, cost);
  value = replace(value, "\"provider\":\"openai\"", "\"provider\":\"openai_responses\"");
  value = replace(value, "\"input_micro_usd_per_million\":1000000", "\"input_micro_usd_per_million\":50000");
  return replace(value, "\"output_micro_usd_per_million\":2000000", "\"output_micro_usd_per_million\":400000");
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
  save(env, "# synthetic only\nOPENAI_API_KEY=CANARY_SECRET_KEY_MARKER\nANTHROPIC_API_KEY=other\nGEMINI_API_KEY=gemini-key\n");
  require(credential(Provider::OpenAI, env) == secret && credential(Provider::Anthropic, env) == "other" &&
      credential(Provider::Gemini, env) == "gemini-key", "credential selection crossed provider boundaries");
  save(env, "ANTHROPIC_API_KEY=other\n"); require(credential(Provider::OpenAI, env).empty(), "missing credential not empty");
  require(credential(Provider::Gemini, env).empty(), "missing Gemini credential fell back to another provider");
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

void gemini_smoke(runtime_test::Peer& peer, const char* executable) {
  auto live = gemini_source("gemini-2.5-flash-lite", 18080);
  live = replace(live, "http://127.0.0.1:18080", "https://generativelanguage.googleapis.com");
  live = replace(live, "\"max_input_tokens\":4096", "\"max_input_tokens\":1048576");
  const auto admitted = parse_profile(live);
  require(admitted.provider() == Provider::Gemini && reserved_cost(admitted.bounds()) == 104910,
      "Gemini whole-window reservation was wrong");
  for (auto invalid : {
      replace(live, "gemini-2.5-flash-lite", "gemini-2.5-pro"),
      replace(live, "\"call_cap\":4", "\"call_cap\":5"),
      replace(live, "\"micro_usd_cap\":1000000", "\"micro_usd_cap\":1000001"),
      replace(live, "\"max_output_tokens\":128", "\"max_output_tokens\":129"),
      replace(live, "\"max_input_tokens\":1048576", "\"max_input_tokens\":1048575"),
      replace(live, "\"input_micro_usd_per_million\":100000", "\"input_micro_usd_per_million\":99999"),
      replace(live, "\"output_micro_usd_per_million\":400000", "\"output_micro_usd_per_million\":399999"),
      replace(live, "https://generativelanguage.googleapis.com", "https://api.openai.com"),
      replace(live, "\"version\":1", "\"version\":1,\"thinking_budget\":1024") })
    rejects([&] { parse_profile(invalid); });
  Directory directory;
  const auto model = peer.arm("reject");
  const auto profile = parse_profile(gemini_source(model, peer.port), true);
  const auto ledger = directory.file("ledger");
  // A new provider must not reset or borrow the already spent legacy balances.
  std::string old = "SPCANARY1\n";
  for (std::uint64_t i = 1; i <= 4; ++i)
    old += "0 " + std::to_string(i) + ' ' + std::to_string(i * 1051672) + ' ' + std::to_string(i * 425585) + '\n';
  for (std::uint64_t i = 1; i <= 11; ++i)
    old += "1 " + std::to_string(i) + ' ' + std::to_string(i * 204096) + ' ' + std::to_string(i * 420480) + '\n';
  save(ledger, old);
  auto first = run(profile, ledger, secret);
  require(first.cases.size() == 2 && first.replay == Replay::NotApplicable && first.reserved.calls == 2 &&
      first.reserved.tokens == 8448 && first.reserved.micro_usd == 924, "Gemini ran tools or borrowed legacy budget");
  for (const auto& item : first.cases)
    require(item.state == State::Passed && item.attempts == 1 && item.input_tokens == 2 && item.output_tokens == 3,
        "Gemini compatibility result or usage was lost");
  const auto old_openai = ledger_totals(parse_profile(source("old", peer.port, false), true), ledger);
  const auto old_anthropic = ledger_totals(parse_profile(source("old", peer.port), true), ledger);
  require(old_openai.calls == 4 && old_openai.tokens == 4206688 && old_openai.micro_usd == 1702340 &&
      old_anthropic.calls == 11 && old_anthropic.tokens == 2245056 && old_anthropic.micro_usd == 4625280,
      "legacy call/token/cost balance reset or cross-charged");
  auto second = run(profile, ledger, secret);
  require(second.cases[0].state == State::Passed && second.reserved.calls == 4, "Gemini restart lost balance");
  auto blocked = run(profile, ledger, secret);
  require(blocked.cases[0].state == State::NotRun && blocked.cases[0].reason == Reason::CallBudget,
      "Gemini exceeded hard call allowance");
  peer.count(model, 4); private_text(load(ledger)); private_text(report_json(first));
  const auto restarted_openai = ledger_totals(parse_profile(source("old", peer.port, false), true), ledger);
  const auto restarted_anthropic = ledger_totals(parse_profile(source("old", peer.port), true), ledger);
  require(restarted_openai.calls == old_openai.calls && restarted_openai.tokens == old_openai.tokens &&
      restarted_openai.micro_usd == old_openai.micro_usd && restarted_anthropic.calls == old_anthropic.calls &&
      restarted_anthropic.tokens == old_anthropic.tokens && restarted_anthropic.micro_usd == old_anthropic.micro_usd,
      "restart/exhaustion changed another provider balance");
  for (unsigned axis = 0; axis != 2; ++axis) {
    Directory cap;
    const auto limited_model = peer.arm("reject");
    const auto limited = parse_profile(gemini_source(limited_model, peer.port, 4,
        axis == 0 ? 4224 : 100000, axis == 1 ? 462 : 1000000), true);
    auto result = run(limited, cap.file("ledger"), secret);
    require(result.cases[0].state == State::Passed && result.cases[1].state == State::NotRun &&
        result.cases[1].reason == (axis == 0 ? Reason::TokenBudget : Reason::CostBudget), "Gemini cap ignored");
    peer.count(limited_model, 1);
  }
  for (auto scenario : {"gemini-auth-error", "gemini-corrupt"}) {
    Directory errors;
    const auto failure_model = peer.arm(scenario);
    const auto result = run(parse_profile(gemini_source(failure_model, peer.port), true), errors.file("ledger"), secret);
    const bool authentication = std::string_view(scenario) == "gemini-auth-error";
    require(result.cases[0].state == State::Failed &&
        result.cases[0].failure_kind == (authentication ? sp::ErrorKind::Permission : sp::ErrorKind::ProtocolCorrupt) &&
        result.cases[0].http_status == (authentication ? 403 : 200), "Gemini failure was masked or defaulted to success");
    require(result.reserved.calls == 2, "Gemini failure reservation refunded");
    peer.count(failure_model, 2, authentication ? 2 : 1); private_text(report_json(result));
  }
  const auto cli_model = peer.arm("reject");
  const auto profile_path = directory.file("profile"), key_path = directory.file("keys");
  save(profile_path, gemini_source(cli_model, peer.port)); save(key_path, "GEMINI_API_KEY=CANARY_SECRET_KEY_MARKER\n");
  auto observed = cli(executable, {"--profile", profile_path, "--ledger", directory.file("cli-ledger"),
      "--env-file", key_path, "--execute", "--test-loopback"});
  auto parsed = runtime_test::parse(observed.text);
  require(observed.code == 0 && parsed.root().get("provider").as_string() == "gemini" &&
      parsed.root().get("api_family").as_string() == "openai.chat" &&
      parsed.root().get("verification_scope").as_string() == "text_only_compatibility_smoke",
      "CLI claimed native Gemini or mislabelled provider");
  peer.count(cli_model, 2);
}
void responses_reasoning(runtime_test::Peer& peer, const char* executable) {
  auto live = responses_source("gpt-5-nano-2025-08-07", 18080);
  live = replace(live, "http://127.0.0.1:18080", "https://api.openai.com");
  live = replace(live, "\"max_input_tokens\":4096", "\"max_input_tokens\":400000");
  live = replace(live, "\"max_output_tokens\":2048", "\"max_output_tokens\":8192");
  const auto admitted = parse_profile(live);
  require(reserved_cost(admitted.bounds()) == 23277, "Responses inclusive whole-window reservation wrong");
  for (auto invalid : {
      replace(live, "gpt-5-nano-2025-08-07", "gpt-5-mini"),
      replace(live, "\"call_cap\":6", "\"call_cap\":9"),
      replace(live, "\"micro_usd_cap\":1000000", "\"micro_usd_cap\":1000001"),
      replace(live, "\"max_output_tokens\":8192", "\"max_output_tokens\":8193"),
      replace(live, "\"max_input_tokens\":400000", "\"max_input_tokens\":399999"),
      replace(live, "\"input_micro_usd_per_million\":50000", "\"input_micro_usd_per_million\":49999"),
      replace(live, "\"output_micro_usd_per_million\":400000", "\"output_micro_usd_per_million\":399999"),
      replace(live, "https://api.openai.com", "https://api.anthropic.com") })
    rejects([&] { parse_profile(invalid); });
  for (auto scenario : {"reject", "responses-accept", "responses-unrelated", "responses-omit-reject", "responses-misleading"}) {
    Directory directory;
    const auto model = peer.arm(scenario);
    const auto profile = parse_profile(responses_source(model, peer.port), true);
    const auto ledger = directory.file("ledger");
    // Keep every prior provider's spent counters and cost, not only call counts.
    const std::string old = "SPCANARY1\n0 1 1051672 425585\n1 1 204096 420480\n2 1 1048704 104910\n";
    save(ledger, old);
    runtime_test::LogCapture diagnostics;
    const auto result = run(profile, ledger, secret);
    private_text(diagnostics.finish()); private_text(report_json(result)); private_text(load(ledger));
    require(result.cases.size() == 6 && result.positive_retained && result.reasoning_removed && result.ciphertext_mutated,
        "Responses native/control wire evidence lost");
    require(result.replay == Replay::ReplayAcceptanceUnobservable, "synthetic peer claimed vendor replay verification");
    for (size_t i = 0; i < 4; ++i)
      require(result.cases[i].state == State::Passed && result.cases[i].attempts == 1 &&
          result.cases[i].reasoning_items == 1 && result.cases[i].encrypted_present &&
          result.cases[i].native_complete && result.cases[i].reasoning == 1,
          "Responses reasoning/usage was lost or double-counted");
    const bool accepted = std::string_view(scenario) == "responses-accept";
    const bool unrelated = std::string_view(scenario) == "responses-unrelated" || std::string_view(scenario) == "responses-misleading";
    const bool omitted = std::string_view(scenario) == "responses-omit-reject";
    require(result.cases[4].state == State::Passed &&
        result.cases[4].reason == (omitted ? Reason::OmissionRejected : Reason::OmissionAccepted),
        "missing reasoning observation was misclassified");
    require(result.cases[5].state == (accepted || unrelated ? State::Failed : State::Passed) &&
        result.cases[5].reason == (accepted ? Reason::NegativeAccepted :
            unrelated ? Reason::NegativeInconclusive : Reason::CiphertextRejected),
        "unrelated failure became native validation");
    require(result.reserved.calls == 6 && result.reserved.tokens == 36864 && result.reserved.micro_usd == 6150,
        "Responses controls/failures were refunded or charged twice for reasoning");
    const auto a = ledger_totals(parse_profile(source("old", peer.port, false), true), ledger);
    const auto b = ledger_totals(parse_profile(source("old", peer.port), true), ledger);
    const auto g = ledger_totals(parse_profile(gemini_source("old", peer.port), true), ledger);
    require(a.calls == 1 && a.tokens == 1051672 && a.micro_usd == 425585 &&
        b.calls == 1 && b.tokens == 204096 && b.micro_usd == 420480 &&
        g.calls == 1 && g.tokens == 1048704 && g.micro_usd == 104910,
        "Responses lane reset or cross-charged existing reservations");
    const auto exhausted = run(profile, ledger, secret);
    require(exhausted.cases[0].state == State::NotRun && exhausted.cases[0].reason == Reason::CallBudget &&
        exhausted.reserved.calls == 6, "Responses restart renewed the six-call allowance");
    peer.count(model, 6, accepted ? 0 : omitted ? 2 : 1);
    const auto stats = peer.stats(model);
    require(stats.root().get("retained").as_uint() == 1 && stats.root().get("negative").as_uint() == 1 &&
        stats.root().get("omitted").as_uint() == 1 && stats.root().get("differences").as_uint() == 1,
        "control was not original native replay with exactly one intended mutation");
  }
  for (unsigned axis = 0; axis != 2; ++axis) {
    Directory directory;
    const auto model = peer.arm("reject");
    const auto profile = parse_profile(responses_source(model, peer.port, 6,
        axis == 0 ? 6144 : 100000, axis == 1 ? 1025 : 1000000), true);
    const auto result = run(profile, directory.file("ledger"), secret);
    require(result.cases[0].state == State::Passed && result.cases[1].state == State::NotRun &&
        result.cases[1].reason == (axis == 0 ? Reason::TokenBudget : Reason::CostBudget),
        "Responses sublimit budget axis ignored");
    peer.count(model, 1);
  }
  for (unsigned axis = 0; axis != 2; ++axis) {
    Directory directory;
    const auto model = peer.arm("reject");
    const auto ledger = directory.file("ledger");
    std::string existing = "SPCANARY1\n";
    const auto n = axis == 0 ? 16U : 1U;
    for (unsigned i = 1; i <= n; ++i)
      existing += "0 " + std::to_string(i) + ' ' + std::to_string(i * 6144) + ' ' +
          std::to_string(axis == 0 ? i * 8192 : 9999999) + '\n';
    save(ledger, existing);
    const auto result = run(parse_profile(responses_source(model, peer.port), true), ledger, secret);
    require(result.cases[0].state == State::NotRun &&
        result.cases[0].reason == (axis == 0 ? Reason::CallBudget : Reason::CostBudget) &&
        result.reserved.calls == 0, "Responses bypassed existing cumulative OpenAI limit");
    peer.count(model, 0);
  }
  Directory cli_directory;
  const auto model = peer.arm("reject");
  const auto profile = cli_directory.file("profile"), key = cli_directory.file("keys");
  save(profile, responses_source(model, peer.port)); save(key, "OPENAI_API_KEY=CANARY_SECRET_KEY_MARKER\n");
  const auto observed = cli(executable, {"--profile", profile, "--ledger", cli_directory.file("ledger"),
      "--env-file", key, "--execute", "--test-loopback"});
  const auto parsed = runtime_test::parse(observed.text);
  require(observed.code == 0 && parsed.root().get("provider").as_string() == "openai" &&
      parsed.root().get("api_family").as_string() == "openai.responses" &&
      parsed.root().get("verification_scope").as_string() == "stateless_reasoning_poc" &&
      parsed.root().get("replay").as_string() == "ReplayAcceptanceUnobservable",
      "CLI mislabeled native protocol or simulated vendor validation");
  peer.count(model, 6, 1); private_text(observed.text);
}
} // namespace
int main(int argc, char** argv) {
  try {
    require(argc == 4, "usage: sp_canary_tests NODE CANARY_SERVER CANARY_CLI");
    profiles_and_credentials(); process_ledger();
    runtime_test::Peer peer(argv[1], argv[2]);
    adversarial_guards(peer);
    replay_cases(peer); budget_axes(peer); shared_race(peer); cli_paths(peer, argv[3]);
    gemini_smoke(peer, argv[3]);
    responses_reasoning(peer, argv[3]);
    std::cout << "canary behavior passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "canary behavior failed: " << error.what() << '\n'; return 1;
  }
}
