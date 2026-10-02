#include "canary/canary.h"
#include "canary/io.h"
#include "support/runtime_peer.h"
#include <array>
#include <barrier>
#include <filesystem>
#include <future>
#include <iostream>
#include <vector>

namespace {
using namespace sp::canary;
using runtime_test::require;
constexpr std::array<std::string_view, 5> providers{
    "vision_chat", "vision_responses", "vision_messages", "vision_gemini", "vision_interactions"};
constexpr std::array<std::string_view, 5> families{
    "openai.chat", "openai.responses", "anthropic.messages", "google.generate", "google.interactions"};
constexpr std::array<std::string_view, 5> profile_files{
    "vision-chat.json", "vision-responses.json", "vision-messages.json", "vision-gemini.json", "vision-interactions.json"};
constexpr std::string_view legacy =
    "SPCANARY1\n0 1 1051672 425585\n1 1 204096 420480\n2 1 1048704 104910\n3 1 408192 23277\n";
constexpr std::array<Totals, 4> legacy_totals{{
    {1, 1051672, 425585}, {1, 204096, 420480}, {1, 1048704, 104910}, {1, 408192, 23277}}};
constexpr Totals prior_vision{40, 35503472, 7795635};
std::string prior_vision_ledger() {
  constexpr std::array<std::uint64_t, 5> calls{7, 8, 8, 7, 10};
  constexpr std::array<std::uint64_t, 5> tokens{1058192, 1058192, 208192, 1056768, 1056768};
  constexpr std::array<std::uint64_t, 5> cost{268644, 268644, 240960, 108135, 108135};
  std::string result(legacy);
  for (std::size_t lane = 0; lane < calls.size(); ++lane)
    for (std::uint64_t attempt = 1; attempt <= calls[lane]; ++attempt)
      result += std::to_string(lane + 4) + ' ' + std::to_string(attempt) + ' ' +
          std::to_string(attempt * tokens[lane]) + ' ' + std::to_string(attempt * cost[lane]) + '\n';
  return result;
}
class Directory {
 public:
  Directory() {
    char pattern[] = "/tmp/sp-vision-canary-XXXXXX";
    auto* value = ::mkdtemp(pattern); require(value, "temporary directory failed"); path_ = value;
  }
  ~Directory() { std::error_code ignored; std::filesystem::remove_all(path_, ignored); }
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
  std::string file(std::string_view name) const { return path_ + '/' + std::string(name); }
 private:
  std::string path_;
};
void save(const std::string& path, std::string_view text) {
  sp::canary::detail::Fd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
  sp::canary::detail::write_all(fd.get(), text);
}
std::string load(const std::string& path) { return sp::canary::detail::read_file(path, 65536, true); }
void totals(Totals actual, Totals expected, std::string_view why) {
  require(actual.calls == expected.calls && actual.tokens == expected.tokens &&
      actual.micro_usd == expected.micro_usd, why);
}
void totals_json(sp::json::Value actual, Totals expected, std::string_view why) {
  require(actual.is_object(), why);
  totals({actual.get("calls").as_uint(), actual.get("tokens").as_uint(), actual.get("micro_usd").as_uint()}, expected, why);
}
std::string mutate(std::string value, std::string_view old, std::string_view replacement) {
  const auto at = value.find(old); require(at != std::string::npos, "profile mutation missing");
  value.replace(at, old.size(), replacement); return value;
}
template<class F> void rejects(F&& action) {
  bool rejected = false;
  try { action(); } catch (const std::exception&) { rejected = true; }
  require(rejected, "unsafe campaign reservation admitted");
}
std::string source(std::size_t lane, std::string_view model, std::uint64_t port,
    std::uint64_t input_rate = 100, std::uint64_t output_rate = 100, std::uint64_t output_tokens = 8192) {
  const auto url = lane <= 1 ? "https://developers.openai.com/api/docs/pricing" :
      lane == 2 ? "https://platform.claude.com/docs/en/about-claude/pricing" : "https://ai.google.dev/gemini-api/docs/pricing";
  return "{\"version\":1,\"provider\":" + sp::json::quote(providers[lane]) +
      ",\"model\":" + sp::json::quote(model) + ",\"origin\":\"http://127.0.0.1:" + std::to_string(port) +
      "\",\"input_bound_kind\":\"model_context_window\",\"max_input_tokens\":10000,\"max_output_tokens\":" +
      std::to_string(output_tokens) + ",\"input_micro_usd_per_million\":" + std::to_string(input_rate) +
      ",\"output_micro_usd_per_million\":" + std::to_string(output_rate) +
      ",\"call_cap\":100,\"token_cap\":1000000000,\"micro_usd_cap\":19795635" +
      (lane == 2 ? ",\"thinking_budget\":1024" : "") + ",\"provenance\":{\"model_url\":" +
      sp::json::quote(url) + ",\"pricing_url\":" + sp::json::quote(url) + ",\"verified_at\":\"2026-10-02\"}}";
}
std::vector<Profile> profiles(std::uint64_t input_rate = 100, std::uint64_t output_rate = 100,
    std::uint64_t output_tokens = 8192) {
  std::vector<Profile> result;
  for (std::size_t lane = 0; lane != providers.size(); ++lane)
    result.push_back(parse_profile(source(lane, "arbitrary-synthetic-model", 18080,
        input_rate, output_rate, output_tokens), true));
  return result;
}
std::vector<Profile> old_profiles(const std::filesystem::path& root) {
  std::vector<Profile> result;
  for (auto name : {"openai.json", "anthropic.json", "gemini.json", "openai-responses.json"})
    result.push_back(parse_profile(read_profile_file((root / "tools/canary_profiles" / name).string())));
  return result;
}
void preserved(const std::vector<Profile>& old, const std::string& ledger) {
  require(load(ledger).starts_with(legacy), "campaign rewrote historical ledger rows");
  for (std::size_t i = 0; i != old.size(); ++i)
    totals(ledger_totals(old[i], ledger), legacy_totals[i], "campaign reset or cross-charged IDs 0-3");
}
void live_openai_profiles(const std::filesystem::path& root) {
  for (std::size_t lane = 0; lane != 2; ++lane) {
    auto text = read_profile_file((root / "tools/canary_profiles" / profile_files[lane]).string());
    const auto profile = parse_profile(text);
    require(profile.model() == "gpt-6-luna" && !profile.loopback(), "live campaign changed selected OpenAI model");
    require(profile.bounds().input_rate >= 250000 && profile.bounds().output_rate >= 750000,
        "live campaign under-reserved conservative long-context/cache-write rates");
    require(reserved_cost(profile.bounds()) >= 268644,
        "live whole-context plus 8192-output exposure underestimated");
    for (auto invalid : {
        mutate(text, "\"model\":\"gpt-6-luna\"", "\"model\":\"gpt-6-luna-other\""),
        mutate(text, "\"max_input_tokens\":1050000", "\"max_input_tokens\":1049999"),
        mutate(text, "\"input_micro_usd_per_million\":250000", "\"input_micro_usd_per_million\":249999"),
        mutate(text, "\"output_micro_usd_per_million\":750000", "\"output_micro_usd_per_million\":749999")})
      rejects([&] { (void)parse_profile(invalid); });
    rejects([&] { (void)parse_profile(mutate(text, "\"call_cap\":100", "\"call_cap\":101")); });
    rejects([&] { (void)parse_profile(mutate(text, "\"micro_usd_cap\":19795635", "\"micro_usd_cap\":19795636")); });
    rejects([&] { (void)parse_profile(mutate(text, "\"micro_usd_cap\":19795635", "\"micro_usd_cap\":20000000")); });
    const auto loopback = source(lane, "arbitrary-model-not-live", 18080);
    rejects([&] { (void)parse_profile(loopback); });
    require(parse_profile(loopback, true).model() == "arbitrary-model-not-live", "synthetic profile demanded a live model");
  }
}
void aggregate_calls_and_process_restart(const std::vector<Profile>& old) {
  Directory directory; const auto ledger = directory.file("ledger");
  const auto prior = prior_vision_ledger(); save(ledger, prior);
  totals(vision_campaign_totals(ledger), prior_vision, "additional grant lost prior40-call exposure");
  const auto lanes = profiles();
  for (std::size_t i = 0; i != 59; ++i)
    require(reserve(lanes[i % lanes.size()], ledger).result == Reservation::Allowed, "additional60-call allowance prematurely exhausted");
  runtime_test::Pipe gate, results;
  std::array<runtime_test::Process, 2> children;
  for (std::size_t i = 0; i != children.size(); ++i) {
    const auto pid = ::fork(); require(pid >= 0, "reservation fork failed");
    if (!pid) {
      gate.writer.reset(); results.reader.reset();
      char start{}, result = 'E';
      ssize_t count; do { count = ::read(gate.reader.get(), &start, 1); } while (count < 0 && errno == EINTR);
      if (count == 1) try {
        const auto debit = reserve(lanes[3 + i], ledger);
        result = debit.result == Reservation::Allowed ? 'A' : debit.result == Reservation::Calls ? 'C' : 'E';
      } catch (...) {}
      runtime_test::write_all(results.writer.get(), std::string_view(&result, 1)); _exit(0);
    }
    children[i] = runtime_test::Process(pid);
  }
  gate.reader.reset(); results.writer.reset(); runtime_test::write_all(gate.writer.get(), "xx"); gate.writer.reset();
  const auto observed = sp::canary::detail::read_bounded(results.reader.get(), 16);
  require(observed == "AC" || observed == "CA", "cross-process campaign race admitted more or fewer than one final call");
  totals(vision_campaign_totals(ledger), {100, 36594992, 7795755}, "restart lost prior debit or additional60-call boundary");
  const auto before = load(ledger);
  for (const auto& lane : profiles())
    require(reserve(lane, ledger).result == Reservation::Calls, "one of five lanes renewed exhausted campaign calls");
  require(load(ledger) == before, "denied call changed durable exposure"); preserved(old, ledger);
  require(load(ledger).starts_with(prior), "additional grant rewrote prior vision rows");
}
void aggregate_cost_and_threads(const std::vector<Profile>& old) {
  Directory directory; const auto ledger = directory.file("ledger");
  const auto prior = prior_vision_ledger(); save(ledger, prior);
  const auto lanes = profiles(40000000, 50000000, 8000);
  for (std::size_t i = 0; i != 14; ++i)
    require(reserve(lanes[i % lanes.size()], ledger).result == Reservation::Allowed, "additional12M-microUSD allowance prematurely exhausted");
  std::barrier start(6); std::vector<std::future<Reservation>> work;
  for (const auto& lane : lanes) work.push_back(std::async(std::launch::async, [&, lane] {
    start.arrive_and_wait(); return reserve(lane, ledger).result;
  }));
  start.arrive_and_wait(); unsigned admitted = 0, denied = 0;
  for (auto& task : work) {
    const auto result = task.get(); admitted += result == Reservation::Allowed; denied += result == Reservation::Cost;
  }
  require(admitted == 1 && denied == 4, "five-lane cost race overspent or lost final reservation");
  totals(vision_campaign_totals(ledger), {55, 35773472, 19795635}, "additional12M cap lost or prior unused margin renewed");
  const auto before = load(ledger);
  for (const auto& lane : profiles(40000000, 50000000, 8000))
    require(reserve(lane, ledger).result == Reservation::Cost, "restart renewed one campaign lane cost allowance");
  require(load(ledger) == before, "denied cost changed durable exposure"); preserved(old, ledger);
  require(load(ledger).starts_with(prior), "additional cost grant rewrote prior vision rows");
}
struct CliResult { int code; std::string text; };
CliResult cli(const char* executable, const std::string& profile, const std::string& ledger, const std::string& keys) {
  runtime_test::Pipe output;
  const auto pid = ::fork(); require(pid >= 0, "CLI fork failed");
  if (!pid) {
    output.reader.reset();
    if (::dup2(output.writer.get(), STDOUT_FILENO) < 0 || ::dup2(output.writer.get(), STDERR_FILENO) < 0) _exit(126);
    output.writer.reset();
    ::execl(executable, executable, "--profile", profile.c_str(), "--ledger", ledger.c_str(),
        "--env-file", keys.c_str(), "--execute", "--test-loopback", static_cast<char*>(nullptr)); _exit(127);
  }
  runtime_test::Process child(pid); output.writer.reset();
  auto text = sp::canary::detail::read_bounded(output.reader.get(), 65536);
  siginfo_t status{}; int result;
  do { result = ::waitid(P_PID, pid, &status, WEXITED | WNOWAIT); } while (result < 0 && errno == EINTR);
  require(result == 0 && status.si_code == CLD_EXITED, "CLI terminated abnormally");
  for (auto marker : {"VISION_SYNTHETIC_KEY", "VISION_PRIVATE_SIGNATURE", "VISION_FUNCTION_SIGNATURE"})
    require(text.find(marker) == std::string::npos, "CLI leaked private native material");
  return {status.si_status, std::move(text)};
}
void case_passed(sp::json::Value item, std::string_view name) {
  require(item.get("name").as_string() == name && item.get("state").as_string() == "passed" &&
      item.get("dispatched").as_bool() && item.get("attempts").as_uint() == 1 &&
      item.get("image_sent").as_bool(), "CLI campaign case failed or lost dispatch/image evidence");
}
void campaign_report(sp::json::Value root, std::size_t lane, std::uint64_t cumulative, bool wrong_oracle) {
  const bool chat = lane == 0; const auto count = chat ? 6U : 8U;
  require(root.get("test_only").as_bool() && !root.get("equivalence_admission").as_bool() &&
      root.get("api_family").as_string() == families[lane], "CLI mislabelled synthetic family or admitted equivalence");
  require(root.get("replay").as_string() == (chat ? "not_applicable" : "ReplayAcceptanceUnobservable"),
      "synthetic campaign claimed real vendor replay verification");
  totals_json(root.get("reserved"), {count, count * 18192, count * 2}, "CLI dropped control reservations or double-counted reasoning");
  totals_json(root.get("campaign_reserved"), {cumulative, cumulative * 18192, cumulative * 2}, "CLI hid shared five-lane exposure");
  const auto cases = root.get("cases"); require(cases.is_array() && cases.size() == count, "CLI omitted campaign scenarios");
  constexpr std::array<std::string_view, 6> names{"vision_off_buffered", "vision_on_buffered", "vision_on_sse",
      "vision_changed", "vision_tool_first", "vision_tool_positive"};
  for (std::size_t i = 0; i != names.size(); ++i) {
    const auto item = cases.at(i);
    if (wrong_oracle && i == 0) {
      require(item.get("name").as_string() == names[i] && item.get("state").as_string() == "failed" &&
          item.get("reason").as_string() == "incorrect_vision" && item.get("vision_correct").is_bool() &&
          !item.get("vision_correct").as_bool(), "wrong synthetic answer was not a consumer-visible oracle failure");
    } else {
      case_passed(item, names[i]);
      require(item.get("vision_correct").is_bool() && item.get("vision_correct").as_bool(), "CLI did not prove scene oracle or weighted tool argument");
    }
    const bool on = i == 1 || i == 2 || (!chat && i >= 4);
    require(item.get("reasoning_requested").is_bool() && item.get("reasoning_disabled").is_bool() &&
        item.get("default_off").is_bool() && item.get("visible_reasoning").is_bool() &&
        item.get("reasoning_requested").as_bool() == on &&
        item.get("reasoning_disabled").as_bool() == (!on && lane != 4) &&
        item.get("default_off").as_bool() == (!on && lane == 4) &&
        item.get("visible_reasoning").as_bool() == (!chat && on),
        "CLI misreported on/off/default controls or visible native reasoning");
  }
  if (!chat) {
    require(root.get("positive_retained").as_bool() && root.get("reasoning_removed").as_bool() &&
        root.get(lane == 1 ? "ciphertext_mutated" : "signature_mutated").as_bool(), "CLI lost exact native positive/control evidence");
    require(cases.at(4).get("native_present").as_bool() && cases.at(4).get("native_complete").as_bool(),
        "CLI tool-first lost complete captured native reasoning");
    case_passed(cases.at(6), "vision_signature_negative");
    const auto omission = cases.at(7);
    require(omission.get("name").as_string() == "vision_reasoning_missing" && omission.get("state").as_string() == "failed" &&
        omission.get("reason").as_string() == "negative_inconclusive" && omission.get("dispatched").as_bool() &&
        omission.get("attempts").as_uint() == 1 && omission.get("image_sent").as_bool() &&
        cases.at(6).get("http_status").as_uint() == 400 && omission.get("http_status").as_uint() == 400 &&
        cases.at(6).get("reason").as_string() == (lane == 1 ? "ciphertext_rejected" : "signature_rejected"),
        "signature error misclassified as omission proof");
  }
}
void campaign_peer(runtime_test::Peer& peer, const std::string& model, std::size_t lane) {
  const auto count = lane == 0 ? 6U : 8U;
  peer.count(model, count, lane == 0 ? 0 : 2);
  const auto stats = peer.stats(model); const auto root = stats.root();
  require(root.get("images").as_uint() == count && root.get("scene_a").as_uint() == count - 1 &&
      root.get("scene_b").as_uint() == 1 && root.get("tool_first").as_uint() == 1 &&
      root.get("replayed").as_uint() == 1 && root.get("sse").as_uint() == 3,
      "CLI failed changed image, 8192 cap, weighted tool result, or SSE wire oracle");
  require(root.get("on").as_uint() == (lane == 0 ? 2U : 6U) && root.get("off").as_uint() == (lane == 0 ? 4U : 2U),
      "peer did not observe distinct reasoning on/off requests");
  if (lane != 0) require(root.get("retained").as_uint() == 1 && root.get("signature_negative").as_uint() == 1 &&
      root.get("reasoning_missing").as_uint() == 1, "peer did not observe exact positive, one-byte signature control, and exact omission");
}
void executable_campaign(runtime_test::Peer& peer, const char* executable, const std::vector<Profile>& old) {
  Directory directory; const auto ledger = directory.file("ledger"), keys = directory.file("keys"); save(ledger, legacy);
  save(keys, "OPENAI_API_KEY=VISION_SYNTHETIC_KEY\nANTHROPIC_API_KEY=VISION_SYNTHETIC_KEY\nGEMINI_API_KEY=VISION_SYNTHETIC_KEY\n");
  std::array<std::string, 5> files, models; std::uint64_t cumulative = 0;
  for (std::size_t lane = 0; lane != providers.size(); ++lane) {
    models[lane] = peer.arm("campaign"); files[lane] = directory.file(profile_files[lane]);
    save(files[lane], source(lane, models[lane], peer.port));
    const auto observed = cli(executable, files[lane], ledger, keys);
    require(observed.code == (lane == 0 ? 0 : 2), "CLI failed vision/signature proof or falsely certified ambiguous omission");
    cumulative += lane == 0 ? 6 : 8;
    const auto report = runtime_test::parse(observed.text); campaign_report(report.root(), lane, cumulative, false);
    campaign_peer(peer, models[lane], lane); preserved(old, ledger);
  }
  for (std::size_t i = 0; i < 60; ++i) {
    const auto profile = parse_profile(source(i % providers.size(), "synthetic-debit-only", peer.port), true);
    require(reserve(profile, ledger).result == Reservation::Allowed, "CLI exhaustion precondition failed");
  }
  const auto restarted_model = peer.arm("campaign"); save(files[0], source(0, restarted_model, peer.port));
  const auto restarted = cli(executable, files[0], ledger, keys); require(restarted.code == 3, "CLI restart reset98 prior campaign reservations");
  const auto report = runtime_test::parse(restarted.text); const auto cases = report.root().get("cases");
  case_passed(cases.at(0), "vision_off_buffered"); case_passed(cases.at(1), "vision_on_buffered");
  for (std::size_t i = 2; i != 5; ++i) require(cases.at(i).get("state").as_string() == "not_run" &&
      cases.at(i).get("reason").as_string() == "call_budget" && cases.at(i).get("attempts").as_uint() == 0,
      "CLI dispatched beyond durable aggregate attempt cap");
  totals_json(report.root().get("campaign_reserved"), {100, 1819200, 200}, "CLI restart misreported aggregate reservations");
  peer.count(restarted_model, 2); const auto saturated = load(ledger);
  for (std::size_t lane = 0; lane != providers.size(); ++lane) {
    const auto blocked = cli(executable, files[lane], ledger, keys); const auto parsed = runtime_test::parse(blocked.text);
    require(blocked.code == 3 && parsed.root().get("cases").at(0).get("reason").as_string() == "call_budget",
        "one CLI lane borrowed or reset saturated campaign budget");
    peer.count(lane == 0 ? restarted_model : models[lane], lane == 0 ? 2 : 8, lane == 0 ? 0 : 2);
  }
  require(load(ledger) == saturated, "blocked CLI runs appended reservations"); preserved(old, ledger);
  for (std::size_t lane = 0; lane != providers.size(); ++lane) {
    Directory wrong; const auto model = "wrong-oracle-" + std::to_string(lane);
    peer.command("{\"arm\":" + sp::json::quote(model) + ",\"scenario\":\"campaign\",\"wrong_oracle\":true}");
    const auto file = wrong.file("profile"); save(file, source(lane, model, peer.port));
    const auto observed = cli(executable, file, wrong.file("ledger"), keys);
    require(observed.code == 2, "CLI accepted an incorrect scene answer");
    const auto parsed = runtime_test::parse(observed.text); campaign_report(parsed.root(), lane, lane == 0 ? 6 : 8, true);
    campaign_peer(peer, model, lane);
  }
}
} // namespace
int main(int argc, char** argv) {
  try {
    require(argc == 4, "usage: sp_vision_canary_tests NODE VISION_SERVER CANARY_CLI");
    const auto root = std::filesystem::absolute(argv[2]).parent_path().parent_path().parent_path();
    const auto old = old_profiles(root);
    live_openai_profiles(root); aggregate_calls_and_process_restart(old); aggregate_cost_and_threads(old);
    runtime_test::Peer peer(argv[1], argv[2]); executable_campaign(peer, argv[3], old);
    std::cout << "vision canary aggregate budget and five-API CLI behavior passed\n"; return 0;
  } catch (const std::exception& error) {
    std::cerr << "vision canary behavior failed: " << error.what() << '\n'; return 1;
  }
}
