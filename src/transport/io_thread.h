#pragma once
// Private: marks transport I/O threads so runtime code can refuse blocking calls from them.
// Lives in sp_wire (no libcurl) because sp_runtime asks the question and sp_transport answers it.
namespace sp::transport::detail {
// Marks the calling thread as a transport I/O thread for the rest of its life.
void mark_io_thread() noexcept;
}  // namespace sp::transport::detail
