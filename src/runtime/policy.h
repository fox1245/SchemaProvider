#pragma once

#include "core/value.h"
#include "transport/http_transport.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace sp::runtime {

using SteadyTime = std::chrono::steady_clock::time_point;
using WallTime = std::chrono::system_clock::time_point;

struct RetryPolicy {
  bool enabled = false;
  bool allow_duplicate_billing_risk = false;
  std::uint32_t max_attempts = 3;
  std::chrono::milliseconds base_delay{100};
  std::chrono::milliseconds max_delay{5000};
};

namespace detail {
struct ResponseInfo {
  ErrorKind kind = ErrorKind::RemoteFailure;
  RetryClass retry_class = RetryClass::Unknown;
  std::string vendor_code;
  std::optional<SteadyTime> retry_not_before;
};

// Only admitted family codes/headers survive. Caller bounds error_body before inspection.
ResponseInfo inspect_response(std::string_view family, int status,
                              const std::vector<transport::Header>& headers,
                              std::string_view error_body,
                              SteadyTime received, WallTime wall_received);
Error classify_failure(const transport::Result&, const ResponseInfo&,
                       const std::optional<Error>& semantic_error,
                       bool semantic_output, std::uint32_t attempts, SteadyTime now);
std::string_view safe_message(ErrorKind) noexcept;
bool valid_retry_policy(const RetryPolicy&) noexcept;

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
