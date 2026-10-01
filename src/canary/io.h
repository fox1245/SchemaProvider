#pragma once
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace sp::canary::detail {
[[noreturn]] inline void fail() { throw std::runtime_error("canary input or persistence failure"); }
class Fd {
 public:
  explicit Fd(int fd) : fd_(fd) { if (fd < 0) fail(); }
  ~Fd() { ::close(fd_); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const { return fd_; }
 private:
  int fd_;
};
inline void regular(int fd, bool owner_private) {
  struct stat st{};
  if (::fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
      (owner_private && (st.st_uid != ::geteuid() || (st.st_mode & 0077)))) fail();
}
inline std::string read_bounded(int fd, std::size_t limit) {
  std::string result;
  char bytes[4096];
  for (;;) {
    auto n = ::read(fd, bytes, sizeof(bytes));
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) fail();
    if (!n) return result;
    if (static_cast<std::size_t>(n) > limit - result.size()) fail();
    result.append(bytes, static_cast<std::size_t>(n));
  }
}
inline std::string read_file(const std::string& path, std::size_t limit, bool owner_private) {
  Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  regular(fd.get(), owner_private);
  return read_bounded(fd.get(), limit);
}
inline void write_all(int fd, std::string_view text) {
  while (!text.empty()) {
    auto n = ::write(fd, text.data(), text.size());
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) fail();
    text.remove_prefix(static_cast<std::size_t>(n));
  }
}
} // namespace sp::canary::detail
