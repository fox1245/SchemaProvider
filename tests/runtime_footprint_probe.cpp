// sp_runtime must not pull in libcurl or libcrypto. A program that links the runtime alone, and only
// uses a Client somebody else constructed, must not have libcurl (or the libraries libcurl needs) or
// OpenSSL's libcrypto mapped into the process. Reading /proc/self/maps makes this Linux only.
//
// The probe takes the address of Client::diagnostics so the linker keeps the client implementation, and
// everything it references, in the binary. Each command-line argument is a substring that must not
// appear in any mapping; without arguments the probe checks for libcurl.
#include "runtime/client.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  volatile auto keep = &sp::runtime::Client::diagnostics;  // reference, never call
  (void)keep;
  std::vector<std::string> forbidden(argv + 1, argv + argc);
  if (forbidden.empty()) forbidden.push_back("libcurl");
  std::ifstream maps("/proc/self/maps");
  if (!maps) {
    std::cerr << "cannot read /proc/self/maps\n";
    return 2;
  }
  std::string line;
  bool any = false;
  while (std::getline(maps, line)) {
    any = true;
    for (const auto& name : forbidden) {
      if (line.find(name) != std::string::npos) {
        std::cerr << name << " is mapped into a runtime-only program:\n  " << line << '\n';
        return 1;
      }
    }
  }
  return any ? 0 : 2;
}
