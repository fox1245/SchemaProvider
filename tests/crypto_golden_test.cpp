// Persisted-value goldens. The expected constants were produced by the code before the OpenSSL
// libcrypto dependency was removed (main f9b3306, SHA-256 from OpenSSL libcrypto) and must never change
// while archive format 3, policy identities and canary fingerprints stay readable.
//   crypto_golden_test           compare against the embedded constants
//   crypto_golden_test --print   print the values the current build produces (used to refresh them)
#include "canary/qualification.h"
#include "descriptor/descriptor.h"
#include "descriptor/policy.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace {
std::string hex(std::string_view bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : bytes) { out += digits[c >> 4]; out += digits[c & 15]; }
  return out;
}

std::string default_policy_identity() {
  auto loaded = sp::descriptor::load(
      R"({"descriptor_version":1,"revision":1,"id":"golden","family":"anthropic.messages","connection":{"base_url":"http://127.0.0.1:18080","paths":{"buffered":"/v1/messages","streaming":"/v1/messages"}}})");
  if (!std::holds_alternative<sp::descriptor::ValidatedDescriptor>(loaded)) {
    std::cerr << "descriptor admission failed\n";
    std::exit(2);
  }
  return hex(std::get<sp::descriptor::ValidatedDescriptor>(loaded).policy()->identity());
}

struct Case { const char* name; std::string value; const char* expected; };
}  // namespace

int main(int argc, char** argv) {
  const std::string long_field(100000, 'q');
  const Case cases[] = {
      {"policy identity (default embedded descriptor policy)", default_policy_identity(),
       "81406aab5b478cdb7ffc607bb9a93bc0369f880f0e29cb58a429ba19e946827c"},
      {"canary digest {}", sp::canary::detail::digest({}), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {"canary digest {\"\"}", sp::canary::detail::digest({""}), "ba768b331fd86cec803be04e56ab2b3d4c0e98ef4ee4fcd4e72ad7cce61a1d1f"},
      {"canary digest {\"abc\"}", sp::canary::detail::digest({"abc"}), "aab5f9ae99b2e38fb462025c8f72f570c9c811705d2a4277dc855d7fa293fe97"},
      {"canary digest {\"a\",\"bb\",\"ccc\"}", sp::canary::detail::digest({"a", "bb", "ccc"}), "a145f70c3932de2bf9a41e075cee83ad237b7725157815f7441d25464a0bbda7"},
      {"canary digest {32 bytes of nonce-like data}",
       sp::canary::detail::digest({std::string_view("\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
                                                    "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f", 32)}),
       "cba737e8cfd7630914a57628190dbc49bf2ce79b2f303815f38c3964e0b10824"},
      {"canary digest {100000-byte field, short field}", sp::canary::detail::digest({long_field, "x"}),
       "16cc32a81745f1273875429625928624af46936dd8f62923de9955994a52e7dd"},
  };
  const bool print = argc > 1 && std::strcmp(argv[1], "--print") == 0;
  int failures = 0;
  for (const auto& item : cases) {
    if (print) { std::printf("%s = %s\n", item.name, item.value.c_str()); continue; }
    if (item.value != item.expected) {
      std::cerr << "golden mismatch: " << item.name << "\n  expected " << item.expected << "\n  actual   "
                << item.value << '\n';
      ++failures;
    }
  }
  return failures ? 1 : 0;
}
