// Scripted loopback HTTP/1.1 server for transport tests. Runs as a SEPARATE PROCESS so the
// client under test is the only thing contributing to the test process's thread count, and so the
// server's counters are an oracle that shares no code with the transport.
//
// Prints one line on stdout once listening:
//   PORTS http=<p> silent=<p> blackhole=<p>
// and exits when stdin reaches EOF. Counters are served by GET /__stats ("name value" lines).
#include <asio.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using asio::ip::tcp;

namespace {

struct Stats {
  long accepted = 0;
  long open = 0;
  long requests = 0;
  long held = 0;
  long die = 0;
  long flood_written = 0;
  long flood_done = 0;
  long sse_started = 0;
  long silent_accepted = 0;
  long stall_header = 0;
  long ok = 0;
  std::map<std::string, long> by_path;
} g_stats;

std::string param(const std::string& query, const std::string& key, const std::string& fallback) {
  std::size_t pos = 0;
  while (pos < query.size()) {
    const std::size_t amp = query.find('&', pos);
    const std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
    const std::size_t eq = pair.find('=');
    if (eq != std::string::npos && pair.substr(0, eq) == key) return pair.substr(eq + 1);
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return fallback;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string chunk(const std::string& payload) {
  std::ostringstream out;
  out << std::hex << payload.size() << "\r\n" << payload << "\r\n";
  return out.str();
}

struct Conn : std::enable_shared_from_this<Conn> {
  explicit Conn(tcp::socket s) : sock(std::move(s)), timer(sock.get_executor()) {
    ++g_stats.accepted;
    ++g_stats.open;
  }
  ~Conn() {
    --g_stats.open;
    if (holding) --g_stats.held;
  }

  tcp::socket sock;
  asio::steady_timer timer;
  std::string buf;
  std::string path;
  std::string query;
  bool holding = false;
  char scratch[256];

  void close() {
    asio::error_code ec;
    timer.cancel();
    sock.shutdown(tcp::socket::shutdown_both, ec);
    sock.close(ec);
  }

  void reset_close() {  // RST instead of FIN
    asio::error_code ec;
    sock.set_option(asio::socket_base::linger(true, 0), ec);
    sock.close(ec);
  }

  void write(std::string data, std::function<void()> next) {
    auto payload = std::make_shared<std::string>(std::move(data));
    asio::async_write(sock, asio::buffer(*payload),
                      [self = shared_from_this(), payload, next = std::move(next)](const asio::error_code& ec, std::size_t) {
                        if (ec) return self->close();
                        if (next) next();
                      });
  }

  void hold() {  // keep the connection open and silent until the client closes it
    if (!holding) {
      holding = true;
      ++g_stats.held;
    }
    sock.async_read_some(asio::buffer(scratch), [self = shared_from_this()](const asio::error_code& ec, std::size_t) {
      if (ec) return self->close();
      self->hold();
    });
  }

  void read_request() {
    asio::async_read_until(sock, asio::dynamic_buffer(buf), "\r\n\r\n",
                           [self = shared_from_this()](const asio::error_code& ec, std::size_t n) {
                             if (ec) return self->close();
                             self->on_head(n);
                           });
  }

  void on_head(std::size_t head_len) {
    const std::string head = buf.substr(0, head_len);
    buf.erase(0, head_len);
    std::istringstream in(head);
    std::string method, target, version;
    in >> method >> target >> version;
    const std::size_t q = target.find('?');
    path = target.substr(0, q);
    query = q == std::string::npos ? "" : target.substr(q + 1);
    std::size_t content_length = 0;
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
      const std::size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      if (lower(line.substr(0, colon)) == "content-length") content_length = std::strtoul(line.c_str() + colon + 1, nullptr, 10);
    }
    if (buf.size() >= content_length) return on_request(content_length);
    asio::async_read(sock, asio::dynamic_buffer(buf), asio::transfer_exactly(content_length - buf.size()),
                     [self = shared_from_this(), content_length](const asio::error_code& ec, std::size_t) {
                       if (ec) return self->close();
                       self->on_request(content_length);
                     });
  }

  void on_request(std::size_t body_len) {
    buf.erase(0, body_len);
    ++g_stats.requests;
    ++g_stats.by_path[path];
    dispatch(body_len);
  }

  static std::string simple(int status, const std::string& reason, const std::string& body,
                            const std::string& extra = "") {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << reason << "\r\nContent-Length: " << body.size() << "\r\n" << extra
        << "\r\n" << body;
    return out.str();
  }

  void keepalive_next() { read_request(); }

  void dispatch(std::size_t body_len) {
    auto self = shared_from_this();
    if (path == "/ok") {
      ++g_stats.ok;
      return write(simple(200, "OK", "ok"), [self] { self->keepalive_next(); });
    }
    if (path == "/echo-len") {
      return write(simple(200, "OK", std::to_string(body_len)), [self] { self->keepalive_next(); });
    }
    if (path == "/status500") {
      return write(simple(500, "Internal Server Error", "boom"), [self] { self->keepalive_next(); });
    }
    if (path == "/die") {
      ++g_stats.die;
      return close();  // no response bytes at all
    }
    if (path == "/stall-header") {
      ++g_stats.stall_header;
      return hold();
    }
    if (path == "/hold") {
      return write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n" +
                       chunk("data: hello\n\n"),
                   [self] { self->hold(); });
    }
    if (path == "/sse") {
      ++g_stats.sse_started;
      const int n = std::atoi(param(query, "n", "3").c_str());
      const int gap = std::atoi(param(query, "gap", "5").c_str());
      return write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n",
                   [self, n, gap] { self->sse_next(0, n, gap); });
    }
    if (path == "/flood") {
      const long total = std::atol(param(query, "total", "1048576").c_str());
      return write("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(total) + "\r\n\r\n",
                   [self, total] { self->flood_next(total); });
    }
    if (path == "/truncate-chunked") {
      return write("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + chunk("data: x\n\n"), [self] { self->close(); });
    }
    if (path == "/truncate-length") {
      return write("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n0123456789", [self] { self->close(); });
    }
    if (path == "/close-delimited") {
      return write("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nhello", [self] { self->close(); });
    }
    if (path == "/reset-mid-body") {
      return write("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n0123456789", [self] { self->reset_close(); });
    }
    if (path == "/bighead") {  // many ordinary header lines whose sum exceeds the transport's cap
      std::string extra;
      for (int i = 0; i < 32; ++i) extra += "X-Pad-" + std::to_string(i) + ": " + std::string(4096, 'a') + "\r\n";
      return write(simple(200, "OK", "x", extra), [self] { self->close(); });
    }
    if (path == "/hugeheader") {  // one header line beyond libcurl's own per-line limit
      return write(simple(200, "OK", "x", "X-Big: " + std::string(128 * 1024, 'a') + "\r\n"), [self] { self->close(); });
    }
    if (path == "/te-gzip-cl") {  // a transfer coding overrides Content-Length: close-delimited
      static const char gzip_hello[] =  // gzip("hello"), 25 bytes, contains NUL
          "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\x03\xcb\x48\xcd\xc9\xc9\x07\x00\x86\xa6\x10\x36\x05\x00\x00\x00";
      std::string response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\nContent-Length: 100\r\n\r\n";
      response.append(gzip_hello, 25);
      return write(std::move(response), [self] { self->close(); });
    }
    if (path == "/cl-overflow") {  // an unusable Content-Length: no framing, close-delimited
      return write("HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551616\r\n\r\n0123456789", [self] { self->close(); });
    }
    if (path == "/folded-cl") {  // obs-fold: the continuation is part of X-Note, not a Content-Length field
      return write("HTTP/1.1 200 OK\r\nX-Note: first\r\n Content-Length: 5\r\n\r\nhello", [self] { self->close(); });
    }
    if (path == "/continue-then-hold") {  // a complete 1xx block, then silence before the final response
      return write("HTTP/1.1 100 Continue\r\n\r\n", [self] { self->hold(); });
    }
    if (path == "/__stats") {
      std::ostringstream out;
      out << "accepted " << g_stats.accepted << "\nopen " << g_stats.open << "\nrequests " << g_stats.requests
          << "\nheld " << g_stats.held << "\ndie " << g_stats.die << "\nflood_written " << g_stats.flood_written
          << "\nflood_done " << g_stats.flood_done << "\nsse_started " << g_stats.sse_started
          << "\nsilent_accepted " << g_stats.silent_accepted << "\nstall_header " << g_stats.stall_header
          << "\nok " << g_stats.ok << "\n";
      for (const auto& [p, c] : g_stats.by_path) out << "path" << p << ' ' << c << "\n";
      return write(simple(200, "OK", out.str(), "Connection: close\r\n"), [self] { self->close(); });
    }
    write(simple(404, "Not Found", "no such script"), [self] { self->keepalive_next(); });
  }

  void sse_next(int i, int n, int gap) {
    auto self = shared_from_this();
    if (i >= n) {
      return write("0\r\n\r\n", [self] { self->keepalive_next(); });
    }
    write(chunk("data: " + std::to_string(i) + "\n\n"), [self, i, n, gap] {
      self->timer.expires_after(std::chrono::milliseconds(gap));
      self->timer.async_wait([self, i, n, gap](const asio::error_code& ec) {
        if (!ec) self->sse_next(i + 1, n, gap);
      });
    });
  }

  void flood_next(long remaining) {
    if (remaining <= 0) {
      ++g_stats.flood_done;
      return keepalive_next();
    }
    const std::size_t n = static_cast<std::size_t>(std::min<long>(remaining, 64 * 1024));
    auto block = std::make_shared<std::string>(n, 'z');
    asio::async_write(sock, asio::buffer(*block),
                      [self = shared_from_this(), block, remaining](const asio::error_code& ec, std::size_t written) {
                        if (ec) return self->close();
                        g_stats.flood_written += static_cast<long>(written);
                        self->flood_next(remaining - static_cast<long>(written));
                      });
  }
};

void accept_loop(tcp::acceptor& acceptor, bool silent, std::vector<std::shared_ptr<tcp::socket>>& parked) {
  acceptor.async_accept([&acceptor, silent, &parked](const asio::error_code& ec, tcp::socket socket) {
    if (!ec) {
      if (silent) {
        ++g_stats.silent_accepted;
        parked.push_back(std::make_shared<tcp::socket>(std::move(socket)));  // never read, never answer
      } else {
        std::make_shared<Conn>(std::move(socket))->read_request();
      }
    }
    accept_loop(acceptor, silent, parked);
  });
}

// A listener whose accept queue is already full and whose SYNs are therefore dropped: connect()
// stalls without any server participation. Fillers stay open for the process lifetime.
unsigned short make_blackhole(std::vector<int>& keep) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
  ::listen(fd, 0);
  socklen_t len = sizeof addr;
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  keep.push_back(fd);
  for (int i = 0; i < 8; ++i) {
    const int c = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    ::connect(c, reinterpret_cast<sockaddr*>(&addr), sizeof addr);  // EINPROGRESS; some stay in SYN_SENT
    keep.push_back(c);
  }
  return ntohs(addr.sin_port);
}

}  // namespace

int main() {
  asio::io_context ctx(1);
  tcp::acceptor http(ctx, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  tcp::acceptor silent(ctx, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  std::vector<std::shared_ptr<tcp::socket>> parked;
  std::vector<int> blackhole_fds;
  const unsigned short blackhole_port = make_blackhole(blackhole_fds);

  accept_loop(http, false, parked);
  accept_loop(silent, true, parked);

  asio::posix::stream_descriptor in(ctx, ::dup(STDIN_FILENO));
  char byte;
  std::function<void()> watch_stdin = [&] {
    in.async_read_some(asio::buffer(&byte, 1), [&](const asio::error_code& ec, std::size_t) {
      if (ec) return ctx.stop();  // parent closed the pipe
      watch_stdin();
    });
  };
  watch_stdin();

  std::printf("PORTS http=%u silent=%u blackhole=%u\n", http.local_endpoint().port(),
              silent.local_endpoint().port(), blackhole_port);
  std::fflush(stdout);
  ctx.run();
  return 0;
}
