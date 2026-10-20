// The custody-backend seam of the native archive on a target without file custody (WebAssembly). This test is built
// with native_archive_fs_unavailable.cpp in place of the file backend, so a POSIX host exercises the refusal: the
// archive cannot be provisioned or opened, the error is typed, and nothing is created on disk.
#include "core/native_archive.h"
#include "core/native_archive_fs.h"
#include "descriptor/presets.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

namespace {
using namespace sp;
namespace fs = std::filesystem;
#define CHECK(c) do { if (!(c)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #c); } while (false)

descriptor::ValidatedDescriptor descriptor_value() {
  auto loaded = descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"archive-unavailable","family":"openai.chat","connection":{"base_url":"https://api.example.test","paths":{"buffered":"/v1/chat/completions","streaming":"/v1/chat/completions"}}})");
  CHECK(std::holds_alternative<descriptor::ValidatedDescriptor>(loaded));
  return std::get<descriptor::ValidatedDescriptor>(std::move(loaded));
}

void archive_is_refused() {
  CHECK(!archive_fs::available());
  const auto root = fs::temp_directory_path() / "sp-archive-unavailable-probe";
  fs::remove_all(root);
  const auto directory = (root / "archive").string();
  const auto key_file = (root / "key" / "activation").string();
  for (const bool create : {true, false}) {
    auto activation = create ? NativeArchive::provision(directory, key_file, "owner", descriptor_value())
                             : NativeArchive::open(directory, key_file, "owner", descriptor_value());
    const auto* error = std::get_if<Error>(&activation);
    CHECK(error != nullptr);
    CHECK(error->kind == ErrorKind::Unsupported);
    CHECK(error->retry_safety == RetrySafety::NotSent);
    CHECK(error->safe_message.find("unavailable") != std::string::npos);
    // The message is fixed text: it carries neither path.
    CHECK(error->safe_message.find(root.string()) == std::string::npos);
  }
  // Nothing pretends to persist: no directory, key or record was created.
  CHECK(!fs::exists(root));
}
}  // namespace

int main() {
  try {
    archive_is_refused();
  } catch (const std::exception& error) {
    std::cerr << "native archive unavailable test failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "native archive unavailable: ok\n";
  return 0;
}
