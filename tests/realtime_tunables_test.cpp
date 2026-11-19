// The tunables of realtime sessions and of the WebSocket transport, as the public option structs
// default-initialize them. The numbers are written out here on purpose: they are the values a host
// gets without setting anything, and they must not move.
#include "transport/realtime_session.h"
#include "transport/ws_frame.h"
#include "transport/ws_transport.h"

#include <chrono>
#include <cstdio>

using namespace std::chrono_literals;

namespace {
int failures = 0;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      ++failures;                                                               \
      std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__,       \
                   #condition);                                                 \
    }                                                                           \
  } while (0)

void recorded_values() {
  const sp::realtime::Options options;
  CHECK(options.max_duration == 15min);
  CHECK(options.media_bytes == 64000u);
  CHECK(options.lead == 200ms);
  CHECK(options.inbound_media_bytes == std::size_t{33554432});
  CHECK(options.inbound_event_bytes == std::size_t{67108864});
  const sp::transport::WsRequest request;
  CHECK(request.close_timeout == 5000ms);
  CHECK(request.limits.max_message_bytes == std::size_t{67108864});
  const sp::transport::WsLimits limits;
  CHECK(limits.max_message_bytes == std::size_t{67108864});
}
}  // namespace

int main() {
  recorded_values();
  if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
