#pragma once

#include "core/value.h"
#include "transport/http_transport.h"
#include "configuration/runtime_policy.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace sp::json { class Value; }

namespace sp::runtime {

using SteadyTime = std::chrono::steady_clock::time_point;
using WallTime = std::chrono::system_clock::time_point;

struct RetryPolicy {
  bool enabled = config_defaults::defaults_retry_enabled;
  bool allow_duplicate_billing_risk = config_defaults::defaults_retry_allow_duplicate_billing_risk;
  std::uint32_t max_attempts = config_defaults::defaults_retry_max_attempts;
  std::chrono::milliseconds base_delay{config_defaults::defaults_retry_base_delay_ms};
  std::chrono::milliseconds max_delay{config_defaults::defaults_retry_max_delay_ms};
};

RetryPolicy default_retry_policy(const configuration::RuntimePolicy&) noexcept;

namespace detail {
struct ResponseInfo {
  ErrorKind kind = ErrorKind::RemoteFailure;
  RetryClass retry_class = RetryClass::Unknown;
  std::string vendor_code;
  std::optional<SteadyTime> retry_not_before;
};

// Only admitted family codes/headers survive. Caller bounds error_body before inspection.
ResponseInfo inspect_response(const configuration::RuntimePolicy&, std::string_view family, int status,
                              const std::vector<transport::Header>& headers,
                              std::string_view error_body,
                              SteadyTime received, WallTime wall_received);
ResponseInfo inspect_document_response(const configuration::RuntimePolicy&, std::string_view family, int status,
                                      const std::vector<transport::Header>& headers,
                                      json::Value error_document,
                                      SteadyTime received, WallTime wall_received);
Error classify_failure(const transport::Result&, const ResponseInfo&,
                       const std::optional<Error>& semantic_error,
                       bool semantic_output, std::uint32_t attempts, SteadyTime now);
std::string_view safe_message(ErrorKind) noexcept;
bool valid_retry_policy(const RetryPolicy&) noexcept;
// A stall bound is zero (disabled) or a positive duration within the admitted ceiling (milliseconds).
bool valid_stall_bound(std::chrono::milliseconds value, std::uint64_t ceiling_ms) noexcept;

// uniform01 is in [0,1]. No token is consumed by planning; recheck at dispatch.
std::optional<SteadyTime> retry_at(const Error&, const RetryPolicy&,
                                 std::uint32_t attempts, SteadyTime now,
                                 SteadyTime deadline, double uniform01);

// One bucket per immutable client credential-origin. Caller serializes access.
class TokenBucket {
 public:
  TokenBucket(std::size_t capacity, double refill_per_second, SteadyTime now);
  bool available(SteadyTime now);
  bool consume(SteadyTime now);
 private:
  void refill(SteadyTime now);
  double capacity_, tokens_, refill_per_second_;
  SteadyTime last_;
};
}  // namespace detail
}  // namespace sp::runtime
