#include "canary/canary.h"
#include "canary/io.h"
#include "support/runtime_peer.h"
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
using runtime_test::require;
class Directory {
 public:
  Directory() {
    char pattern[] = "/tmp/sp-vision-control-XXXXXX";
    auto* path = ::mkdtemp(pattern); require(path, "temporary directory failed"); path_ = path;
  }
  ~Directory() { std::error_code ignored; std::filesystem::remove_all(path_, ignored); }
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
  std::string file(std::string_view name) const { return path_ + '/' + std::string(name); }
 private:
  std::string path_;
};
void save(const std::string& path, std::string_view bytes) {
  sp::canary::detail::Fd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
  sp::canary::detail::write_all(fd.get(), bytes);
}
std::string profile(std::string_view provider, const std::string& model, std::uint64_t port) {
  const auto url = provider == "vision_messages" ? "https://platform.claude.com/docs/en/about-claude/pricing" :
      provider == "vision_responses" ? "https://developers.openai.com/api/docs/pricing" : "https://ai.google.dev/gemini-api/docs/pricing";
  return "{\"version\":1,\"provider\":" + sp::json::quote(provider) + ",\"model\":" + sp::json::quote(model) +
      ",\"origin\":\"http://127.0.0.1:" + std::to_string(port) +
      "\",\"input_bound_kind\":\"model_context_window\",\"max_input_tokens\":10000,\"max_output_tokens\":8192,"
      "\"input_micro_usd_per_million\":100,\"output_micro_usd_per_million\":100,\"call_cap\":40,"
      "\"token_cap\":1000000000,\"micro_usd_cap\":8000000" +
      std::string(provider == "vision_messages" ? ",\"thinking_budget\":1024" : "") +
      ",\"provenance\":{\"model_url\":" + sp::json::quote(url) + ",\"pricing_url\":" + sp::json::quote(url) +
      ",\"verified_at\":\"2026-10-02\"}}";
}
struct CliResult { int code; std::string text; };
CliResult cli(const char* executable, const Directory& directory) {
  runtime_test::Pipe output;
  const auto source = directory.file("profile"), ledger = directory.file("ledger"), keys = directory.file("keys");
  const auto pid = ::fork(); require(pid >= 0, "CLI fork failed");
  if (!pid) {
    output.reader.reset();
    if (::dup2(output.writer.get(), STDOUT_FILENO) < 0 || ::dup2(output.writer.get(), STDERR_FILENO) < 0) _exit(126);
    output.writer.reset();
    ::execl(executable, executable, "--profile", source.c_str(), "--ledger", ledger.c_str(),
        "--env-file", keys.c_str(), "--execute", "--test-loopback", static_cast<char*>(nullptr)); _exit(127);
  }
  runtime_test::Process child(pid); output.writer.reset();
  auto text = sp::canary::detail::read_bounded(output.reader.get(), 65536);
  siginfo_t status{}; int result;
  do { result = ::waitid(P_PID, pid, &status, WEXITED | WNOWAIT); } while (result < 0 && errno == EINTR);
  require(result == 0 && status.si_code == CLD_EXITED, "CLI terminated abnormally");
  for (auto marker : {"VISION_SYNTHETIC_KEY", "VISION_PRIVATE_SIGNATURE", "VISION_FUNCTION_SIGNATURE",
      "PRIVATE_CONTROL_BODY", "Invalid thought signature", "invalid unrelated signature configuration"})
    require(text.find(marker) == std::string::npos, "CLI leaked raw response/native data");
  return {status.si_status, std::move(text)};
}
std::string error(std::string_view fields) {
  return "{\"error\":{" + std::string(fields) + "},\"private\":\"PRIVATE_CONTROL_BODY\"}";
}
std::string interaction(std::string_view message = "Invalid thought signature", std::string_view code = "invalid_request") {
  return error("\"code\":" + sp::json::quote(code) + ",\"message\":" + sp::json::quote(message));
}
std::string event(std::string_view payload, std::string_view name = "error") {
  return "event: " + std::string(name) + "\ndata: " + std::string(payload) + "\n\n";
}
std::string control(std::string_view body, int status = 400, bool sse = false, bool short_close = false) {
  return "{\"body\":" + sp::json::quote(body) + ",\"status\":" + std::to_string(status) +
      ",\"content_type\":" + sp::json::quote(sse ? "text/event-stream" : "application/json") +
      ",\"short_close\":" + (short_close ? "true" : "false") + '}';
}
struct Scenario {
  std::string_view provider;
  std::string response;
  bool specific = false;
  int status = 400;
  bool sse = false, short_close = false, accepted = false;
  std::string omission_response{};
  bool omission_specific = false;
};
void run(runtime_test::Peer& peer, const char* executable, const Scenario& scenario, unsigned sequence) {
  Directory directory;
  const auto model = "native-control-" + std::to_string(sequence);
  peer.command("{\"arm\":" + sp::json::quote(model) + ",\"scenario\":\"campaign\",\"signature_error\":" +
      control(scenario.response, scenario.status, scenario.sse, scenario.short_close) +
      ",\"omission_error\":" + control(scenario.omission_response.empty() ? scenario.response : scenario.omission_response,
          scenario.status, scenario.sse, scenario.short_close) +
      (scenario.accepted ? ",\"accept_signature\":true,\"accept_omission\":true}" : "}"));
  save(directory.file("profile"), profile(scenario.provider, model, peer.port));
  save(directory.file("keys"), "OPENAI_API_KEY=VISION_SYNTHETIC_KEY\nANTHROPIC_API_KEY=VISION_SYNTHETIC_KEY\nGEMINI_API_KEY=VISION_SYNTHETIC_KEY\n");
  const auto observed = cli(executable, directory);
  require(observed.code == (scenario.accepted || (scenario.specific && scenario.omission_specific) ? 0 : 2),
      "CLI lost control status at scenario " + std::to_string(sequence) + "; exit=" + std::to_string(observed.code) + "; safe report=" + observed.text);
  const auto document = runtime_test::parse(observed.text); const auto root = document.root();
  require(root.get("test_only").as_bool() && !root.get("equivalence_admission").as_bool() &&
      root.get("replay").as_string() == "ReplayAcceptanceUnobservable", "loopback granted live native replay proof");
  require(root.get("positive_retained").as_bool() && root.get("reasoning_removed").as_bool() &&
      root.get(scenario.provider == "vision_responses" ? "ciphertext_mutated" : "signature_mutated").as_bool(),
      "CLI lost positive retention or exact native controls");
  const auto cases = root.get("cases"); require(cases.is_array() && cases.size() == 8, "CLI omitted native control cases");
  for (std::size_t i = 0; i < 6; ++i) {
    const auto item = cases.at(i);
    require(item.get("state").as_string() == "passed" && item.get("dispatched").as_bool() &&
        item.get("vision_correct").as_bool(), "campaign prerequisite failed");
  }
  const auto negative = cases.at(6), omission = cases.at(7);
  const auto rejection = scenario.provider == "vision_responses" ? "ciphertext_rejected" : "signature_rejected";
  require(negative.get("name").as_string() == "vision_signature_negative" &&
      negative.get("reason").as_string() == (scenario.accepted ? "negative_accepted" : scenario.specific ? rejection : "negative_inconclusive") &&
      negative.get("state").as_string() == (scenario.accepted || scenario.specific ? "passed" : "failed"),
      "signature control confused specific proof with unrelated failure");
  require(omission.get("name").as_string() == "vision_reasoning_missing" &&
      omission.get("reason").as_string() == (scenario.accepted ? "omission_accepted" :
          scenario.omission_specific ? "omission_rejected" : "negative_inconclusive") &&
      omission.get("state").as_string() == (scenario.accepted || scenario.omission_specific ? "passed" : "failed"),
      "omission was mislabeled signature proof");
  const auto stats = peer.stats(model); const auto wire = stats.root();
  peer.count(model, 8, scenario.accepted ? 0 : 2);
  require(wire.get("retained").as_uint() == 1 && wire.get("signature_negative").as_uint() == 1 &&
      wire.get("reasoning_missing").as_uint() == 1 && wire.get("images").as_uint() == 8,
      "peer did not verify exact positive, one-byte mutation, omission and original images");
}
} // namespace
int main(int argc, char** argv) {
  try {
    require(argc == 4, "usage: sp_vision_control_runtime_tests NODE VISION_SERVER CANARY_CLI");
    runtime_test::Peer peer(argv[1], argv[2]);
    const auto gemini = error("\"code\":400,\"status\":\"INVALID_ARGUMENT\",\"message\":\"Invalid thought signature\"");
    const auto messages = error("\"type\":\"invalid_request_error\",\"message\":\"messages.1.content.0.signature: Invalid signature in thinking block.\"");
    const auto responses = error("\"type\":\"invalid_request_error\",\"code\":\"invalid_encrypted_content\"");
    const auto envelope = "{\"event_type\":\"error\",\"event_id\":\"private-event\",\"error\":{\"code\":\"invalid_request\",\"message\":\"Invalid thought signature\"},\"private\":\"PRIVATE_CONTROL_BODY\"}";
    const auto sse = event(envelope);
    std::string crlf; for (char c : sse) { if (c == '\n') crlf += '\r'; crlf += c; }
    const std::vector<Scenario> scenarios{
      {"vision_responses", responses, true, 400, false, false, false,
          error("\"type\":\"invalid_request_error\",\"message\":\"Item 'fc_canary' was provided without its required 'reasoning' item: 'rs_canary'.\""), true},
      {"vision_responses", responses, true, 400, false, false, false,
          error("\"type\":\"invalid_request_error\",\"message\":\"Item 'fc_canary' of type 'function_call' was provided without its required 'reasoning' item: 'rs_canary'.\""), true},
      {"vision_responses", responses, true, 400, false, false, false,
          error("\"type\":\"invalid_request_error\",\"message\":\"Invalid unrelated field: Item 'fc_canary' was provided without its required 'reasoning' item: 'rs_canary'.\"")},
      {"vision_responses", responses, true, 400, false, false, false,
          error("\"type\":\"invalid_request_error\",\"message\":\"Item 'fc_canary' was provided without its required 'reasoning' item: 'rs_canary'. Unrelated signature configuration.\"")},
      {"vision_responses", responses, true, 400, false, false, false,
          error("\"type\":\"permission_error\",\"message\":\"Item 'fc_canary' was provided without its required 'reasoning' item: 'rs_canary'.\"")},
      {"vision_gemini", gemini, true}, {"vision_messages", messages, true}, {"vision_responses", responses, true},
      {"vision_interactions", interaction(), true}, {"vision_interactions", sse, true, 400, true},
      {"vision_interactions", crlf, true, 400, true},
      {"vision_interactions", gemini}, {"vision_gemini", interaction()},
      {"vision_gemini", error("\"status\":\"INVALID_ARGUMENT\",\"message\":\"Invalid thought signature\"")},
      {"vision_gemini", error("\"code\":403,\"status\":\"INVALID_ARGUMENT\",\"message\":\"Invalid thought signature\"")},
      {"vision_gemini", error("\"code\":400,\"status\":\"PERMISSION_DENIED\",\"message\":\"Invalid thought signature\"")},
      {"vision_gemini", error("\"code\":400,\"status\":\"INVALID_ARGUMENT\",\"message\":\"invalid unrelated signature configuration\"")},
      {"vision_messages", error("\"type\":\"invalid_request_error\",\"message\":\"messages.1.metadata: Invalid signature in thinking block\"")},
      {"vision_messages", error("\"type\":\"invalid_request_error\",\"message\":\"invalid unrelated signature configuration\"")},
      {"vision_responses", error("\"type\":\"invalid_request_error\",\"code\":\"invalid_request\",\"message\":\"Invalid encrypted content\"")},
      {"vision_interactions", interaction("Invalid request")},
      {"vision_interactions", interaction("invalid unrelated signature configuration")},
      {"vision_interactions", interaction("Invalid thought signature", "permission_denied")},
      {"vision_interactions", error("\"code\":400,\"message\":\"Invalid thought signature\"")},
      {"vision_interactions", error("\"code\":\"invalid_request\",\"status\":\"INVALID_ARGUMENT\",\"message\":\"Invalid thought signature\"")},
      {"vision_interactions", interaction(), false, 403},
      {"vision_interactions", sse, false, 200, true},
      {"vision_interactions", event(envelope, "interaction.completed"), false, 400, true},
      {"vision_interactions", event(interaction()), false, 400, true},
      {"vision_interactions", event("{\"event_type\":\"error\",\"error\":{\"code\":\"invalid_request\",\"message\":\"invalid unrelated signature configuration\"}}"), false, 400, true},
      {"vision_interactions", event("{malformed"), false, 400, true},
      {"vision_interactions", sse.substr(0, sse.size() - 1), false, 400, true},
      {"vision_interactions", sse + "data: {truncated", false, 400, true},
      {"vision_interactions", sse + sse, false, 400, true},
      {"vision_interactions", sse + event("{}", "interaction.completed"), false, 400, true},
      {"vision_interactions", interaction().substr(0, 30)},
      {"vision_interactions", interaction() + interaction()},
      {"vision_interactions", sse, false, 400, true, true},
      {"vision_interactions", error("\"code\":\"invalid_request\",\"message\":\"Invalid thought signature\",\"detail\":\"" + std::string(65536, 'x') + "\"")},
      {"vision_interactions", {}, false, 400, false, false, true},
      {"vision_gemini", {}, false, 400, false, false, true},
      {"vision_messages", {}, false, 400, false, false, true},
      {"vision_responses", {}, false, 400, false, false, true}
    };
    unsigned sequence = 0; for (const auto& scenario : scenarios) run(peer, argv[3], scenario, ++sequence);
    std::cout << "vision native control JSON/SSE classification and privacy passed\n"; return 0;
  } catch (const std::exception& error) {
    std::cerr << "vision control behavior failed: " << error.what() << '\n'; return 1;
  }
}
