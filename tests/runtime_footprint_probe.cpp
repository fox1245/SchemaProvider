// sp_runtime must not pull in libcurl. A program that links the runtime alone, and only uses a
// Client somebody else constructed, must not have libcurl (or the libraries libcurl needs) mapped
// into the process. Reading /proc/self/maps makes this Linux only.
//
// The probe takes the address of Client::diagnostics so the linker keeps the client implementation, and
// everything it references, in the binary; the process is then checked for libcurl.
#include "runtime/client.h"

#include <fstream>
#include <iostream>
#include <string>

int main() {
  volatile auto keep = &sp::runtime::Client::diagnostics;  // reference, never call
  (void)keep;
  std::ifstream maps("/proc/self/maps");
  if (!maps) {
    std::cerr << "cannot read /proc/self/maps\n";
    return 2;
  }
  std::string line;
  bool any = false;
  while (std::getline(maps, line)) {
    any = true;
    if (line.find("libcurl") != std::string::npos) {
      std::cerr << "libcurl is mapped into a runtime-only program:\n  " << line << '\n';
      return 1;
    }
  }
  return any ? 0 : 2;
}
