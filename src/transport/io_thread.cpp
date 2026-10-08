#include "transport/http_transport.h"
#include "transport/io_thread.h"

namespace sp::transport {
namespace {
thread_local bool tl_io_thread = false;
}  // namespace
bool on_io_thread() noexcept { return tl_io_thread; }
namespace detail {
void mark_io_thread() noexcept { tl_io_thread = true; }
}  // namespace detail
}  // namespace sp::transport
