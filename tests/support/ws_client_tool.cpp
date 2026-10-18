// WebSocket client driver for conformance and live checks of the transport (not a CTest test).
//
//   sp_ws_client_tool autobahn <ws://host:port> [agent]
//     Runs every case of an Autobahn|Testsuite fuzzing server: each case echoes the server's
//     messages, then the server writes its reports (tests/autobahn/README.md).
//   sp_ws_client_tool script <url> <until> [header ...] -- [message-file ...]
//     Sends each file as one text message after the handshake, prints what arrives, and closes
//     once a message contains <until>. A header is "Name: value"; "${VAR}" in a value is taken from
//     the environment, so credentials never appear on the command line or in the output.
#include "transport/ws_transport.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using namespace sp::transport;

namespace {

// Lets callbacks reach the session handle, which exists only once open_websocket returned.
struct Echo {
  std::mutex mu;
  WsSession* session = nullptr;
  std::vector<std::pair<WsMessageKind, std::string>> early;
  std::string first_text;

  void reply(WsMessageKind kind, std::string_view payload) {
    std::lock_guard lock(mu);
    if (!session) {
      early.emplace_back(kind, std::string(payload));
      return;
    }
    if (kind == WsMessageKind::Text) session->send_text(std::string(payload));
    else session->send_binary(std::string(payload));
  }
  void attach(WsSession& s) {
    std::lock_guard lock(mu);
    session = &s;
    for (auto& [kind, payload] : early) {
      if (kind == WsMessageKind::Text) s.send_text(std::move(payload));
      else s.send_binary(std::move(payload));
    }
    early.clear();
  }
};

WsResult run_one(Transport& transport, const std::string& url, bool echo, std::string* first_text) {
  Echo state;
  WsRequest request;
  request.url = url;
  request.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
  request.limits.max_message_bytes = 64u * 1024u * 1024u;
  WsCallbacks callbacks;
  callbacks.on_message = [&](WsMessageKind kind, std::string_view payload) {
    if (first_text && first_text->empty() && kind == WsMessageKind::Text) *first_text = std::string(payload);
    if (echo) state.reply(kind, payload);
  };
  callbacks.on_done = [](const WsResult&) {};
  WsSession session = open_websocket(transport, std::move(request), std::move(callbacks));
  state.attach(session);
  return session.join();
}

std::string expand(std::string value) {
  for (std::size_t start; (start = value.find("${")) != std::string::npos;) {
    const auto end = value.find('}', start);
    if (end == std::string::npos) break;
    const char* found = std::getenv(value.substr(start + 2, end - start - 2).c_str());
    value.replace(start, end - start + 1, found ? found : "");
  }
  return value;
}

const char* status_name(Status status) {
  switch (status) {
    case Status::Completed: return "Completed";
    case Status::Cancelled: return "Cancelled";
    case Status::DeadlineExceeded: return "DeadlineExceeded";
    case Status::Failed: return "Failed";
  }
  return "?";
}

void print_result(const WsResult& r) {
  std::printf("result status=%s failure=%d http=%d close=%u clean=%d by_peer=%d detail=%s\n",
              status_name(r.transport.status), static_cast<int>(r.transport.failure), r.http_status,
              static_cast<unsigned>(r.close_code), r.clean ? 1 : 0, r.closed_by_peer ? 1 : 0,
              r.transport.detail.c_str());
}

int autobahn(const std::string& base, const std::string& agent) {
  Transport transport;
  std::string count_text;
  run_one(transport, base + "/getCaseCount", false, &count_text);
  const int cases = std::atoi(count_text.c_str());
  std::printf("cases %d\n", cases);
  for (int i = 1; i <= cases; ++i) {
    const auto r = run_one(transport, base + "/runCase?case=" + std::to_string(i) + "&agent=" + agent, true, nullptr);
    if (r.transport.status != Status::Completed) {
      std::printf("case %d ended: ", i);
      print_result(r);
    }
  }
  run_one(transport, base + "/updateReports?agent=" + agent, false, nullptr);
  return cases > 0 ? 0 : 1;
}

int script(int argc, char** argv) {
  const std::string url = argv[2], until = argv[3];
  WsRequest request;
  request.url = url;
  request.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
  request.connect_timeout = std::chrono::seconds(15);
  request.handshake_timeout = std::chrono::seconds(20);
  int index = 4;
  for (; index < argc && std::string(argv[index]) != "--"; ++index) {
    const std::string spec = argv[index];
    const auto colon = spec.find(':');
    if (colon == std::string::npos) return 2;
    std::string value = spec.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') value.erase(0, 1);
    request.headers.push_back(Header{spec.substr(0, colon), expand(value)});
  }
  std::vector<std::string> messages;
  for (++index; index < argc; ++index) {
    std::ifstream file(argv[index], std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    messages.push_back(text.str());
  }
  std::mutex mu;
  WsSession* handle = nullptr;
  bool reached = false;
  std::size_t received = 0, bytes = 0;
  WsCallbacks callbacks;
  callbacks.on_open = [](const ResponseHead& head) { std::printf("open status=%d\n", head.status); };
  callbacks.on_message = [&](WsMessageKind kind, std::string_view payload) {
    ++received;
    bytes += payload.size();
    std::string shown(payload.substr(0, 200));
    for (char& c : shown)
      if (c == '\n' || c == '\r') c = ' ';
    std::printf("message %zu %s %zu bytes: %s\n", received, kind == WsMessageKind::Text ? "text" : "binary",
                payload.size(), shown.c_str());
    if (!reached && payload.find(until) != std::string_view::npos) {
      reached = true;
      std::lock_guard lock(mu);
      if (handle) handle->close(ws_close::Normal, "done");
    }
  };
  callbacks.on_close = [](std::uint16_t code, std::string_view reason) {
    std::printf("server close %u %.*s\n", static_cast<unsigned>(code), static_cast<int>(reason.size()), reason.data());
  };
  callbacks.on_done = [](const WsResult&) {};
  Transport transport;
  WsSession session = open_websocket(transport, std::move(request), std::move(callbacks));
  {
    std::lock_guard lock(mu);
    handle = &session;
  }
  for (auto& message : messages) session.send_text(std::move(message));
  const auto r = session.join();
  std::printf("received %zu messages, %zu bytes, until=%s\n", received, bytes, reached ? "seen" : "not seen");
  print_result(r);
  return reached && r.transport.status == Status::Completed ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "autobahn") return autobahn(argv[2], argc > 3 ? argv[3] : "schemaprovider");
  if (argc >= 4 && std::string(argv[1]) == "script") return script(argc, argv);
  std::fprintf(stderr, "usage: sp_ws_client_tool autobahn <ws://host:port> [agent] | script <url> <until> [header ...] -- [file ...]\n");
  return 2;
}
