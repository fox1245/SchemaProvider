// Fixed-input canary fingerprint vectors from the pre-cutover OpenSSL implementation.
// Current embedded policy data is deliberately not a golden: changing admitted facts changes
// its identity. The vectors below protect persisted framing, not incidental configuration bytes.
#ifndef SP_GOLDEN_WITHOUT_CANARY
#include "canary/qualification.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>

namespace {

struct Case { const char* name; std::string value; const char* expected; };
}  // namespace

int main(int argc, char** argv) {
#ifdef SP_GOLDEN_WITHOUT_CANARY
  (void)argc; (void)argv;
  std::cout << "SKIP: canary fingerprint implementation is not built on this platform\n";
  return 77;
#else
  const std::string long_field(100000, 'q');
  const Case cases[] = {
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
#endif
}
