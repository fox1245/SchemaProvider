#pragma once

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace runtime_test {

class Fd {
 public:
  Fd() = default;
  explicit Fd(int value) noexcept : value_(value) {}
  ~Fd() { reset(); }
  Fd(Fd&& other) noexcept : value_(other.release()) {}
  Fd& operator=(Fd&& other) noexcept { if (this != &other) reset(other.release()); return *this; }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const noexcept { return value_; }
  explicit operator bool() const noexcept { return value_ >= 0; }
  int release() noexcept { return std::exchange(value_, -1); }
  void reset(int value = -1) noexcept {
    const auto old = std::exchange(value_, value);
    if (old >= 0) ::close(old); // Linux closes the descriptor even on EINTR.
  }
  // Transfer an owned descriptor to the child process's standard stream.
  bool redirect_to(int target) noexcept {
    if (value_ == target) {
      if (::fcntl(target, F_SETFD, 0) < 0) return false;
      release();
    } else {
      if (::dup2(value_, target) < 0) return false;
      reset();
    }
    return true;
  }
 private:
  int value_ = -1;
};

struct Pipe {
  Fd reader, writer;
  Pipe() {
    int descriptors[2];
    if (::pipe2(descriptors, O_CLOEXEC) != 0) throw std::runtime_error("pipe creation failed");
    reader.reset(descriptors[0]); writer.reset(descriptors[1]);
  }
};

class Process {
 public:
  Process() = default;
  explicit Process(pid_t pid, int signal = SIGTERM, bool group = false) noexcept
      : pid_(pid), signal_(signal), group_(group) {}
  ~Process() { reset(); }
  Process(Process&& other) noexcept
      : pid_(std::exchange(other.pid_, -1)), signal_(other.signal_), group_(other.group_) {}
  Process& operator=(Process&& other) noexcept {
    if (this != &other) { reset(); pid_ = std::exchange(other.pid_, -1); signal_ = other.signal_; group_ = other.group_; }
    return *this;
  }
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;
  bool running() const {
    if (pid_ <= 0) return false;
    siginfo_t info{};
    int status;
    do { status = ::waitid(P_PID, static_cast<id_t>(pid_), &info, WEXITED | WNOHANG | WNOWAIT); }
    while (status < 0 && errno == EINTR);
    if (status < 0) throw std::runtime_error("child observation failed");
    return info.si_pid == 0;
  }
  void reset() noexcept {
    if (pid_ <= 0) return;
    // Do not reap before signalling: retaining the child prevents PID reuse.
    if (group_) ::kill(-pid_, signal_);
    ::kill(pid_, signal_);
    while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
    pid_ = -1;
  }
 private:
  pid_t pid_ = -1;
  int signal_ = SIGTERM;
  bool group_ = false;
};

class RedirectFd {
 public:
  RedirectFd(int target, int replacement) : target_(target), saved_(::fcntl(target, F_DUPFD_CLOEXEC, 3)) {
    if (!saved_ || ::dup2(replacement, target) < 0) throw std::runtime_error("diagnostic redirection failed");
  }
  ~RedirectFd() { restore(); }
  RedirectFd(const RedirectFd&) = delete;
  RedirectFd& operator=(const RedirectFd&) = delete;
  void restore() noexcept {
    if (saved_) { ::dup2(saved_.get(), target_); saved_.reset(); }
  }
 private:
  int target_;
  Fd saved_;
};

class LogCapture {
  struct CloseFile { void operator()(std::FILE* file) const noexcept { std::fclose(file); } };
  using File = std::unique_ptr<std::FILE, CloseFile>;
  static File temporary() {
    File file(std::tmpfile());
    if (!file || ::fcntl(::fileno(file.get()), F_SETFD, FD_CLOEXEC) < 0)
      throw std::runtime_error("diagnostic capture creation failed");
    return file;
  }
  File file_ = temporary();
  RedirectFd stderr_{STDERR_FILENO, ::fileno(file_.get())};
  RedirectFd stdout_{STDOUT_FILENO, ::fileno(file_.get())};
 public:
  ~LogCapture() { std::fflush(stderr); std::fflush(stdout); }
  std::string finish() {
    std::fflush(stderr); std::fflush(stdout);
    stdout_.restore(); stderr_.restore();
    std::rewind(file_.get());
    std::string text;
    char buffer[4096];
    while (auto count = std::fread(buffer, 1, sizeof(buffer), file_.get())) {
      if (count > (4U << 20) - text.size()) throw std::runtime_error("diagnostic capture exceeded bound");
      text.append(buffer, count);
    }
    if (std::ferror(file_.get())) throw std::runtime_error("diagnostic capture read failed");
    return text;
  }
};

}  // namespace runtime_test
