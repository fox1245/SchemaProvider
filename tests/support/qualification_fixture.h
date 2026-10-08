#pragma once

#include "canary/io.h"
#include "qualification/meter.h"
#include "runtime_peer.h"
#include <filesystem>
#include <sys/stat.h>
#include <vector>

namespace qualification_test {
using runtime_test::require;
inline void save(const std::filesystem::path& path, std::string_view bytes) {
  sp::canary::detail::Fd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
  sp::canary::detail::write_all(fd.get(), bytes);
}
inline std::string load(const std::filesystem::path& path) {
  return sp::canary::detail::read_file(path.string(), 1U << 20, true);
}
// The meter refuses group/world-writable directories (fail-closed), so fixtures set 0700 explicitly
// instead of inheriting the caller's umask.
inline void private_directory(const std::filesystem::path& path) {
  std::filesystem::create_directory(path);
  require(::chmod(path.c_str(), 0700) == 0, "fixture chmod 0700 failed");
  struct stat s{};
  require(::stat(path.c_str(), &s) == 0 && !(s.st_mode & 022),
      "fixture directory is group-writable or world-writable (meter refuses it): " + path.string());
}
struct Campaign {
  std::filesystem::path root;
  std::string baseline;
  Campaign(const std::filesystem::path& source, const std::vector<std::string>& models) {
    const auto parent = std::filesystem::canonical(std::filesystem::temp_directory_path());
    auto pattern = (parent / "sp-runner-qualification-XXXXXX").string();
    const auto* directory = ::mkdtemp(pattern.data()); require(directory, "campaign fixture failed"); root = directory;
    private_directory(root / "config");
    private_directory(root / "build");
    save(root / "config/qualification-authorization.json",
        sp::canary::detail::read_file((source / "config/qualification-authorization.json").string(), 16384, false));
    baseline = "SPCANARY1\n";
    for (std::uint64_t i = 1; i <= 99; ++i)
      baseline += "4 " + std::to_string(i) + ' ' + std::to_string(i * 100) + ' ' +
          std::to_string(i == 99 ? 18979680 : i * (18979680 / 99)) + '\n';
    save(root / "build/m5-live.ledger", baseline);
    std::string catalog = R"({"version":1,"currency":"USD","rate_unit":"micro_usd_per_million_tokens","verified_at":"2026-10-02","models":{)";
    bool comma = false;
    for (const auto& model : models) {
      if (comma) catalog += ','; comma = true;
      catalog += sp::json::quote(model) + R"(:{"families":["openai.chat","openai.responses","anthropic.messages","google.generate","google.interactions"],"context_window":200000,"max_input_tokens":10000,"max_output_tokens":64000,"input_rate":100,"cache_read_rate":10,"cache_write_5m_rate":125,"cache_write_1h_rate":200,"output_rate":100,"long_context_threshold":null,"long_context_input_multiplier":1,"long_context_output_multiplier":1,"cache_minimum_tokens":null,"cache_activation":"implicit_default","cache_ttl_seconds":null,"output_includes_thinking":true,"model_url":"https://example.test/model","pricing_url":"https://example.test/price","caching_url":"https://example.test/cache"})";
    }
    save(root / "config/model-catalog.json", catalog + "}}");
  }
  ~Campaign() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
  Campaign(const Campaign&) = delete;
  Campaign& operator=(const Campaign&) = delete;
  std::unique_ptr<sp::qualification::Meter> open() const {
    auto opened = sp::qualification::Meter::open(root.string());
    require(std::holds_alternative<std::unique_ptr<sp::qualification::Meter>>(opened), "fixture meter denied");
    return std::get<std::unique_ptr<sp::qualification::Meter>>(std::move(opened));
  }
  void preserved() const { require(load(root / "build/m5-live.ledger") == baseline, "runner rewrote immutable baseline"); }
};
struct CliResult { int code; std::string text; };
inline CliResult cli(const char* executable, const std::vector<std::string>& arguments) {
  runtime_test::Pipe output;
  const auto pid = ::fork(); require(pid >= 0, "CLI fork failed");
  if (!pid) {
    output.reader.reset();
    if (::dup2(output.writer.get(), STDOUT_FILENO) < 0 || ::dup2(output.writer.get(), STDERR_FILENO) < 0) _exit(126);
    output.writer.reset();
    std::vector<char*> args{const_cast<char*>(executable)};
    for (const auto& argument : arguments) args.push_back(const_cast<char*>(argument.c_str()));
    args.push_back(nullptr); ::execv(executable, args.data()); _exit(127);
  }
  runtime_test::Process child(pid); output.writer.reset();
  auto text = sp::canary::detail::read_bounded(output.reader.get(), 1U << 20);
  siginfo_t status{}; int result;
  do { result = ::waitid(P_PID, pid, &status, WEXITED | WNOWAIT); } while (result < 0 && errno == EINTR);
  require(result == 0 && status.si_code == CLD_EXITED, "CLI terminated abnormally");
  return {status.si_status, std::move(text)};
}
inline sp::json::Document first_report(std::string_view lines) {
  return runtime_test::parse(lines.substr(0, lines.find('\n')));
}
} // namespace qualification_test
