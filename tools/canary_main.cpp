#include "canary/canary.h"
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

int main(int argc, char** argv) {
  using namespace sp::canary;
  bool execution_started = false;
  try {
    std::optional<std::string> profile_path, ledger_path, env_file;
    bool execute = false, loopback = false, help = false;
    for (int i = 1; i < argc; ++i) {
      std::string_view flag(argv[i]);
      auto path = [&](std::optional<std::string>& target) {
        if (target || i + 1 == argc || std::string_view(argv[i + 1]).empty() || std::string_view(argv[i + 1]).starts_with("--"))
          throw std::invalid_argument("invalid arguments");
        target = argv[++i];
      };
      if (flag == "--profile") path(profile_path);
      else if (flag == "--ledger") path(ledger_path);
      else if (flag == "--env-file") path(env_file);
      else if (flag == "--execute" && !execute) execute = true;
      else if (flag == "--test-loopback" && !loopback) loopback = true;
      else if (flag == "--help" && !help) help = true;
      else throw std::invalid_argument("invalid arguments");
    }
    if (help) { std::cout << help_text(); return 0; }
    if (!execute) { std::cout << plan_json() << '\n'; return 0; }
    if (!profile_path || !ledger_path) throw std::invalid_argument("invalid arguments");
    auto profile = parse_profile(read_profile_file(*profile_path), loopback);
    auto key = credential(profile.provider(), env_file);
    execution_started = true;
    auto report = run(profile, *ledger_path, std::move(key));
    std::cout << report_json(report) << '\n';
    bool missing = false, failed = false;
    for (const auto& item : report.cases) { missing |= item.state == State::NotRun; failed |= item.state == State::Failed; }
    return failed ? 2 : missing ? 3 : 0;
  } catch (...) {
    // Paths, parser context, credentials and live response errors are never echoed.
    if (execution_started)
      std::cerr << "{\"version\":1,\"state\":\"failed\",\"reason\":\"execution_failure\",\"reservations\":\"consult_ledger_no_refunds\"}\n";
    else
      std::cerr << "{\"version\":1,\"state\":\"not_run\",\"reason\":\"invalid_input_or_persistence\"}\n";
    return 2;
  }
}
