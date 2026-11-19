// The tunables of realtime sessions and of the WebSocket transport, as the public option structs
// default-initialize them. The numbers are written out here on purpose: they are the values a host
// gets without setting anything, and they must not move.
#include "configuration/runtime_policy.h"
#include "sp/config_defaults.h"
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

// Each public struct reads the generated constant of its key, and so does the policy a host reads its defaults from.
void defaults_come_from_the_generated_constants() {
  using namespace sp::config_defaults;
  const sp::realtime::Options options;
  CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(options.max_duration).count() ==
        static_cast<std::int64_t>(defaults_realtime_max_duration_ms));
  CHECK(options.media_bytes == defaults_realtime_media_bytes);
  CHECK(options.lead.count() == static_cast<std::int64_t>(defaults_realtime_lead_ms));
  CHECK(options.inbound_media_bytes == defaults_realtime_inbound_media_bytes);
  CHECK(options.inbound_event_bytes == defaults_realtime_inbound_event_bytes);
  const sp::transport::WsRequest request;
  CHECK(request.close_timeout.count() == static_cast<std::int64_t>(defaults_ws_close_timeout_ms));
  CHECK(request.limits.max_message_bytes == defaults_ws_max_message_bytes);
  CHECK(sp::transport::WsLimits{}.max_message_bytes == defaults_ws_max_message_bytes);
  const auto policy = sp::configuration::builtin_runtime_policy();
  const auto& values = policy->defaults();
  CHECK(values.realtime_max_duration_ms == defaults_realtime_max_duration_ms);
  CHECK(values.realtime_media_bytes == defaults_realtime_media_bytes);
  CHECK(values.realtime_lead_ms == defaults_realtime_lead_ms);
  CHECK(values.realtime_inbound_media_bytes == defaults_realtime_inbound_media_bytes);
  CHECK(values.realtime_inbound_event_bytes == defaults_realtime_inbound_event_bytes);
  CHECK(values.realtime_pump_interval_ms == defaults_realtime_pump_interval_ms);
  CHECK(values.ws_max_message_bytes == defaults_ws_max_message_bytes);
  CHECK(values.ws_close_timeout_ms == defaults_ws_close_timeout_ms);
  const auto& ceilings = policy->admission();
  CHECK(ceilings.realtime_max_duration_ms == admission_realtime_max_duration_ms);
  CHECK(ceilings.realtime_media_bytes == admission_realtime_media_bytes);
  CHECK(ceilings.realtime_lead_ms == admission_realtime_lead_ms);
  CHECK(ceilings.realtime_inbound_media_bytes == admission_realtime_inbound_media_bytes);
  CHECK(ceilings.realtime_inbound_event_bytes == admission_realtime_inbound_event_bytes);
  CHECK(ceilings.realtime_pump_interval_ms == admission_realtime_pump_interval_ms);
  CHECK(ceilings.ws_max_message_bytes == admission_ws_max_message_bytes);
  CHECK(ceilings.ws_close_timeout_ms == admission_ws_close_timeout_ms);
  // The shipped values are the ones the structs always had.
  CHECK(defaults_realtime_max_duration_ms == 900000 && defaults_realtime_media_bytes == 64000);
  CHECK(defaults_realtime_lead_ms == 200 && defaults_realtime_pump_interval_ms == 20);
  CHECK(defaults_realtime_inbound_media_bytes == 33554432 && defaults_realtime_inbound_event_bytes == 67108864);
  CHECK(defaults_ws_max_message_bytes == 67108864 && defaults_ws_close_timeout_ms == 5000);
}
}  // namespace

int main() {
  recorded_values();
  defaults_come_from_the_generated_constants();
  if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
