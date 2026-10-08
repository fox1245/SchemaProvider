#pragma once
// The few raw-socket and environment calls the tests make themselves (the loopback peers are Node
// processes; these helpers serve the black hole, the half-written response head and the refused port).
// POSIX behaviour is the plain system call; on Windows each maps to its Winsock/CRT equivalent.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdlib>
#include "win_owner.h"
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/resource.h>
#endif

#include <cerrno>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <cstddef>
#include <cstdlib>
#include <utility>

namespace portable {

#ifdef _WIN32
using socket_t = SOCKET;
using socklen = int;
using PollFd = WSAPOLLFD;
inline constexpr socket_t invalid_socket = INVALID_SOCKET;
// Winsock must be started before the first socket call; the tests' own threads and libcurl share it.
struct WinsockSession {
  WinsockSession() {
    WSADATA data{};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0)
      throw std::runtime_error("Winsock startup failed");
  }
  ~WinsockSession() { ::WSACleanup(); }
};
inline const WinsockSession winsock_session;
inline bool is_valid(socket_t s) noexcept { return s != INVALID_SOCKET; }
inline void close_socket(socket_t s) noexcept { ::closesocket(s); }
inline bool set_nonblocking(socket_t s) noexcept { u_long on = 1; return ::ioctlsocket(s, FIONBIO, &on) == 0; }
// A non-blocking connect() that has not completed yet.
inline bool connect_in_progress() noexcept { return ::WSAGetLastError() == WSAEWOULDBLOCK; }
inline int poll(PollFd* fds, std::size_t count, int timeout_ms) noexcept {
  return ::WSAPoll(fds, static_cast<ULONG>(count), timeout_ms);
}
inline long long receive(socket_t s, char* buffer, std::size_t size) noexcept {
  return ::recv(s, buffer, static_cast<int>((std::min)(size, static_cast<std::size_t>((std::numeric_limits<int>::max)()))), 0);
}
inline long long send_quiet(socket_t s, const char* data, std::size_t size) noexcept {
  return ::send(s, data, static_cast<int>((std::min)(size, static_cast<std::size_t>((std::numeric_limits<int>::max)()))), 0);
}
inline void shutdown_both(socket_t s) noexcept { ::shutdown(s, SD_BOTH); }
inline void set_env(const char* name, const char* value) {
  const auto wide_name = runtime_test::widen(name);
  const auto wide_value = runtime_test::widen(value);
  if (::_wputenv_s(wide_name.c_str(), wide_value.c_str()) != 0)
    throw std::runtime_error("test environment update failed");
}
inline void unset_env(const char* name) { set_env(name, ""); }
#else
using socket_t = int;
using socklen = socklen_t;
using PollFd = pollfd;
inline constexpr socket_t invalid_socket = -1;
inline bool is_valid(socket_t s) noexcept { return s >= 0; }
inline void close_socket(socket_t s) noexcept { ::close(s); }
inline bool set_nonblocking(socket_t s) noexcept {
  const int flags = ::fcntl(s, F_GETFL, 0);
  return flags >= 0 && ::fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
inline bool connect_in_progress() noexcept { return errno == EINPROGRESS; }
inline int poll(PollFd* fds, std::size_t count, int timeout_ms) noexcept {
  return ::poll(fds, static_cast<nfds_t>(count), timeout_ms);
}
inline long long receive(socket_t s, char* buffer, std::size_t size) noexcept { return ::read(s, buffer, size); }
inline long long send_quiet(socket_t s, const char* data, std::size_t size) noexcept {
#ifdef MSG_NOSIGNAL
  return ::send(s, data, size, MSG_NOSIGNAL);
#else
  // Darwin sockets adopted by Socket carry SO_NOSIGPIPE.
  return ::send(s, data, size, 0);
#endif
}
inline void shutdown_both(socket_t s) noexcept { ::shutdown(s, SHUT_RDWR); }
inline void set_env(const char* name, const char* value) {
  if (::setenv(name, value, 1) != 0) throw std::runtime_error("test environment update failed");
}
inline void unset_env(const char* name) {
  if (::unsetenv(name) != 0) throw std::runtime_error("test environment update failed");
}
#endif

// Owns one socket.
class Socket {
 public:
  Socket() = default;
  explicit Socket(socket_t value) : value_(value) {
#if defined(SO_NOSIGPIPE) && !defined(MSG_NOSIGNAL)
    const int enabled = 1;
    if (is_valid(value_) && ::setsockopt(value_, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled) != 0) {
      reset();
      throw std::runtime_error("socket SIGPIPE suppression setup failed");
    }
#endif
  }
  ~Socket() { reset(); }
  Socket(Socket&& other) noexcept : value_(std::exchange(other.value_, invalid_socket)) {}
  Socket& operator=(Socket&& other) noexcept {
    if (this != &other) reset(std::exchange(other.value_, invalid_socket));
    return *this;
  }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  socket_t get() const noexcept { return value_; }
  explicit operator bool() const noexcept { return is_valid(value_); }
  void reset(socket_t value = invalid_socket) noexcept {
    const auto old = std::exchange(value_, value);
    if (is_valid(old)) close_socket(old);
  }
 private:
  socket_t value_ = invalid_socket;
};

#ifdef __APPLE__
inline int darwin_thread_count() {
  thread_act_array_t threads = nullptr;
  mach_msg_type_number_t count = 0;
  if (::task_threads(::mach_task_self(), &threads, &count) != KERN_SUCCESS)
    throw std::runtime_error("cannot enumerate Darwin process threads");
  bool released = true;
  for (mach_msg_type_number_t i = 0; i < count; ++i)
    if (::mach_port_deallocate(::mach_task_self(), threads[i]) != KERN_SUCCESS) released = false;
  if (::vm_deallocate(::mach_task_self(), reinterpret_cast<vm_address_t>(threads),
                      static_cast<vm_size_t>(count) * sizeof(thread_t)) != KERN_SUCCESS)
    released = false;
  if (!released || count > static_cast<unsigned>((std::numeric_limits<int>::max)()))
    throw std::runtime_error("Darwin thread observation cleanup failed");
  return static_cast<int>(count);
}

inline long long darwin_resident_bytes() {
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (::task_info(::mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
    throw std::runtime_error("cannot observe Darwin resident memory");
  if (info.resident_size > static_cast<unsigned long long>((std::numeric_limits<long long>::max)()))
    throw std::runtime_error("Darwin resident memory exceeds observation range");
  return static_cast<long long>(info.resident_size);
}

inline int darwin_descriptor_count() {
  struct rlimit limits{};
  if (::getrlimit(RLIMIT_NOFILE, &limits) != 0)
    throw std::runtime_error("cannot observe Darwin descriptor limit");
  const int limit = limits.rlim_cur == RLIM_INFINITY ? ::getdtablesize() :
      static_cast<int>((std::min)(limits.rlim_cur, static_cast<rlim_t>((std::numeric_limits<int>::max)())));
  if (limit <= 0) throw std::runtime_error("invalid Darwin descriptor limit");
  int count = 0;
  for (int fd = 0; fd < limit; ++fd) {
    if (::fcntl(fd, F_GETFD) >= 0) ++count;
    else if (errno != EBADF) throw std::runtime_error("Darwin descriptor observation failed");
  }
  return count;
}
#endif

}  // namespace portable
