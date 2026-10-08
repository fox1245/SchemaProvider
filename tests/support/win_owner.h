#pragma once
// Windows counterpart of posix_owner.h for the loopback peer harness: owned HANDLEs, a child process
// that is killed with its whole tree (job object), and the stdout/stderr capture the runtime tests use.
// Only the pieces runtime_peer.h and the tests need are provided; the POSIX-only owners (fork, flock
// custody, process groups) have no counterpart here.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <limits>
#include <io.h>
#include <corecrt_startup.h>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace runtime_test {

class Handle {
 public:
  Handle() = default;
  explicit Handle(HANDLE value) noexcept : value_(normalize(value)) {}
  ~Handle() { reset(); }
  Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
  Handle& operator=(Handle&& other) noexcept { if (this != &other) reset(other.release()); return *this; }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  HANDLE get() const noexcept { return value_; }
  explicit operator bool() const noexcept { return value_ != nullptr; }
  HANDLE release() noexcept { return std::exchange(value_, nullptr); }
  void reset(HANDLE value = nullptr) noexcept {
    const auto old = std::exchange(value_, normalize(value));
    if (old) ::CloseHandle(old);
  }
 private:
  // CreateFile reports failure as INVALID_HANDLE_VALUE, most other calls as null.
  static HANDLE normalize(HANDLE value) noexcept { return value == INVALID_HANDLE_VALUE ? nullptr : value; }
  HANDLE value_ = nullptr;
};

inline std::wstring widen(std::string_view text) {
  if (text.empty()) return {};
  if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    throw std::runtime_error("path exceeds Windows conversion bound");
  const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) throw std::runtime_error("path is not valid UTF-8");
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), size) != size)
    throw std::runtime_error("path conversion failed");
  return out;
}

inline std::string narrow(std::wstring_view text) {
  if (text.empty()) return {};
  if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    throw std::runtime_error("path exceeds Windows conversion bound");
  const int size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                        nullptr, 0, nullptr, nullptr);
  if (size <= 0) throw std::runtime_error("path is not valid Unicode");
  std::string out(static_cast<std::size_t>(size), '\0');
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                            out.data(), size, nullptr, nullptr) != size)
    throw std::runtime_error("path conversion failed");
  return out;
}

// Narrow main argv uses the ANSI code page, independently of /utf-8. UCRT's wide parser consumes
// GetCommandLineW; initialize it once, then own UTF-8 arguments for the launcher's explicit contract.
class Arguments {
 public:
  Arguments(int, char**) {
    static const bool initialized = [] {
      if (!*::__p___wargv() && ::_configure_wide_argv(_crt_argv_unexpanded_arguments) != 0)
        throw std::runtime_error("wide command-line parsing failed");
      return true;
    }();
    (void)initialized;
    const int count = *::__p___argc();
    auto** wide = *::__p___wargv();
    if (count < 0 || !wide) throw std::runtime_error("wide command-line arguments unavailable");
    values_.reserve(static_cast<std::size_t>(count));
    pointers_.reserve(static_cast<std::size_t>(count) + 1);
    for (int i = 0; i < count; ++i) values_.push_back(narrow(wide[i]));
    for (auto& value : values_) pointers_.push_back(value.data());
    pointers_.push_back(nullptr);
  }
  int argc() const noexcept { return static_cast<int>(values_.size()); }
  char** argv() noexcept { return pointers_.data(); }
  Arguments(const Arguments&) = delete;
  Arguments& operator=(const Arguments&) = delete;
 private:
  std::vector<std::string> values_;
  std::vector<char*> pointers_;
};

inline std::string environment(std::string_view name) {
  const auto key = widen(name);
  ::SetLastError(ERROR_SUCCESS);
  DWORD size = ::GetEnvironmentVariableW(key.c_str(), nullptr, 0);
  for (;;) {
    if (size == 0) {
      const auto error = ::GetLastError();
      if (error == ERROR_ENVVAR_NOT_FOUND || error == ERROR_SUCCESS) return {};
      throw std::runtime_error("test environment read failed");
    }
    std::wstring value(size, L'\0');
    ::SetLastError(ERROR_SUCCESS);
    const DWORD count = ::GetEnvironmentVariableW(key.c_str(), value.data(), size);
    if (count >= size) { size = count; continue; }
    if (count == 0) {
      if (::GetLastError() == ERROR_ENVVAR_NOT_FOUND || ::GetLastError() == ERROR_SUCCESS) return {};
      throw std::runtime_error("test environment read failed");
    }
    value.resize(count);
    return narrow(value);
  }
}

// One Windows command-line argument, quoted per the CommandLineToArgvW / UCRT rules.
inline void append_argument(std::wstring& line, const std::wstring& argument) {
  if (!line.empty()) line += L' ';
  line += L'"';
  for (std::size_t i = 0;; ++i) {
    std::size_t slashes = 0;
    while (i < argument.size() && argument[i] == L'\\') { ++slashes; ++i; }
    if (i == argument.size()) { line.append(slashes * 2, L'\\'); break; }
    if (argument[i] == L'"') { line.append(slashes * 2 + 1, L'\\'); line += L'"'; }
    else { line.append(slashes, L'\\'); line += argument[i]; }
  }
  line += L'"';
}

// Owns a child and, through a kill-on-close job object, everything it spawns. Destruction terminates
// the tree and waits for the child to be gone, like Process::reset() on POSIX.
class Process {
 public:
  Process() = default;
  Process(Handle process, Handle job) noexcept : process_(std::move(process)), job_(std::move(job)) {}
  ~Process() { reset(); }
  Process(Process&&) noexcept = default;
  Process& operator=(Process&& other) noexcept {
    if (this != &other) { reset(); process_ = std::move(other.process_); job_ = std::move(other.job_); }
    return *this;
  }
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;
  HANDLE native() const noexcept { return process_.get(); }
  HANDLE job() const noexcept { return job_.get(); }
  bool running() const { return process_ && ::WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT; }
  void reset() noexcept {
    if (!process_) return;
    // Closing stdin first gives peers a bounded opportunity to remove ephemeral certificates.
    // Misuse children and stuck peers still fall through to contained termination.
    ::WaitForSingleObject(process_.get(), 500);
    if (job_) ::TerminateJobObject(job_.get(), 1);
    // Also covers launch failure before assignment to the job succeeded.
    ::TerminateProcess(process_.get(), 1);
    ::WaitForSingleObject(process_.get(), 10000);
    job_.reset();
    process_.reset();
  }
 private:
  Handle process_, job_;
};

// stdin/stdout pipes of a spawned child. Parent reads only bytes PeekNamedPipe has reported available;
// stdin uses a nonblocking pipe server, so neither direction can strand a test during peer teardown.
struct Spawned {
  Process process;
  Handle to_child, from_child;
};

inline Spawned spawn_with_pipes(std::string_view executable, std::initializer_list<std::string_view> arguments) {
  static std::atomic<unsigned> sequence{0};
  const std::string pipe_name = "\\\\.\\pipe\\sp-peer-" + std::to_string(::GetCurrentProcessId()) + '-' +
                                std::to_string(::GetTickCount64()) + '-' + std::to_string(++sequence);
  SECURITY_ATTRIBUTES private_end{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
  SECURITY_ATTRIBUTES inherited_end{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};

  Spawned result;
  result.from_child.reset(::CreateNamedPipeA(pipe_name.c_str(),
      PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &private_end));
  if (!result.from_child) throw std::runtime_error("peer stdout pipe creation failed");
  Handle child_out(::CreateFileA(pipe_name.c_str(), GENERIC_WRITE, 0, &inherited_end, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!child_out) throw std::runtime_error("peer stdout pipe open failed");

  const auto input_name = pipe_name + "-stdin";
  result.to_child.reset(::CreateNamedPipeA(input_name.c_str(),
      PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &private_end));
  if (!result.to_child) throw std::runtime_error("peer stdin pipe creation failed");
  Handle child_in(::CreateFileA(input_name.c_str(), GENERIC_READ, 0, &inherited_end, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!child_in) throw std::runtime_error("peer stdin pipe open failed");

  // The child inherits this process's stderr (its diagnostics), duplicated inheritable so that the
  // original handle's flags stay untouched. No stderr at all: give it NUL.
  Handle child_err;
  if (HANDLE own = ::GetStdHandle(STD_ERROR_HANDLE); own && own != INVALID_HANDLE_VALUE) {
    HANDLE copy = nullptr;
    if (::DuplicateHandle(::GetCurrentProcess(), own, ::GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS))
      child_err.reset(copy);
  }
  if (!child_err) child_err.reset(::CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited_end,
                                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!child_err) throw std::runtime_error("peer stderr setup failed");

  // Inherit exactly the three standard handles, whatever else this process has open.
  HANDLE inherit[3] = {child_in.get(), child_out.get(), child_err.get()};
  SIZE_T attribute_bytes = 0;
  ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
  std::vector<char> attribute_storage(attribute_bytes);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
  if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes))
    throw std::runtime_error("peer attribute list setup failed");
  struct AttributeGuard {
    LPPROC_THREAD_ATTRIBUTE_LIST list;
    ~AttributeGuard() { ::DeleteProcThreadAttributeList(list); }
  } attribute_guard{attributes};
  if (!::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr))
    throw std::runtime_error("peer handle list setup failed");

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = child_in.get();
  startup.StartupInfo.hStdOutput = child_out.get();
  startup.StartupInfo.hStdError = child_err.get();
  startup.lpAttributeList = attributes;

  const auto application = widen(executable);
  std::wstring command_line;
  append_argument(command_line, application);
  for (const auto argument : arguments) append_argument(command_line, widen(argument));

  Handle job(::CreateJobObjectW(nullptr, nullptr));
  if (!job) throw std::runtime_error("peer job creation failed");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    throw std::runtime_error("peer job containment setup failed");
  PROCESS_INFORMATION info{};
  if (!::CreateProcessW(application.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, nullptr, nullptr, &startup.StartupInfo, &info))
    throw std::runtime_error("peer CreateProcess failed (" + std::to_string(::GetLastError()) + ")");
  Handle process(info.hProcess), thread(info.hThread);
  // Own the suspended process before any throwing operation; failure cannot leave it behind.
  result.process = Process(std::move(process), std::move(job));
  if (!::AssignProcessToJobObject(result.process.job(), result.process.native()))
    throw std::runtime_error("peer job assignment failed");
  if (::ResumeThread(thread.get()) == static_cast<DWORD>(-1))
    throw std::runtime_error("peer resume failed");
  return result;
}

inline Spawned spawn_with_pipes(std::string_view executable, std::string_view script) {
  return spawn_with_pipes(executable, std::initializer_list<std::string_view>{script});
}

inline std::size_t read_pipe(HANDLE pipe, char* buffer, std::size_t size, long long milliseconds) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  for (;;) {
    DWORD available = 0;
    if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
      throw std::runtime_error("peer exited before reply");
    if (available != 0) {
      DWORD got = 0;
      const auto count = static_cast<DWORD>((std::min)(size, static_cast<std::size_t>(available)));
      if (!::ReadFile(pipe, buffer, count, &got, nullptr) || got == 0)
        throw std::runtime_error("peer control read failed");
      return got;
    }
    if (std::chrono::steady_clock::now() >= deadline)
      return 0;
    ::Sleep(1);
  }
}

inline void write_pipe(HANDLE pipe, std::string_view value, long long milliseconds = 15000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  while (!value.empty()) {
    DWORD written = 0;
    const auto count = static_cast<DWORD>((std::min)(value.size(), std::size_t{65536}));
    if (!::WriteFile(pipe, value.data(), count, &written, nullptr))
      throw std::runtime_error("control write failed");
    value.remove_prefix(written);
    if (!value.empty()) {
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("peer control write timed out");
      if (written == 0) ::Sleep(1);
    }
  }
}

// Redirect the C runtime's stdout and stderr into an anonymous temporary file for the lifetime of the
// object (the POSIX LogCapture does the same with dup2); finish() restores them and returns the text.
class LogCapture {
  struct CloseFile { void operator()(std::FILE* file) const noexcept { std::fclose(file); } };
  using File = std::unique_ptr<std::FILE, CloseFile>;
  static File temporary() {
    File file(std::tmpfile());
    if (!file) throw std::runtime_error("diagnostic capture creation failed");
    return file;
  }
  class Redirect {
   public:
    Redirect(int target, int replacement) : target_(target), saved_(::_dup(target)) {
      if (saved_ < 0) throw std::runtime_error("diagnostic descriptor duplication failed");
      if (::_dup2(replacement, target) < 0) {
        ::_close(saved_);
        saved_ = -1;
        throw std::runtime_error("diagnostic redirection failed");
      }
    }
    ~Redirect() { restore(); }
    Redirect(const Redirect&) = delete;
    Redirect& operator=(const Redirect&) = delete;
    void restore() noexcept {
      if (saved_ >= 0) { ::_dup2(saved_, target_); ::_close(saved_); saved_ = -1; }
    }
   private:
    int target_;
    int saved_;
  };
  File file_ = temporary();
  Redirect stderr_{2, ::_fileno(file_.get())};
  Redirect stdout_{1, ::_fileno(file_.get())};
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
