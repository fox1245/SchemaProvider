#include "transport/sse_framer.h"

namespace sp::transport {

SseFramer::SseFramer(SseLimits limits) : limits_(limits) {}

bool SseFramer::fail(SseError error) {
  if (error_ == SseError::None) error_ = error;
  stopped_ = true;
  return false;
}

bool SseFramer::fits_event(std::size_t data, std::size_t type,
                           std::size_t id) const {
  auto available = limits_.max_event_bytes;
  if (data > available) return false;
  available -= data;
  if (type > available) return false;
  available -= type;
  return id <= available;
}

bool SseFramer::feed(std::string_view bytes, const Sink& sink) {
  if (feeding_) return fail(SseError::Misuse);
  if (stopped_) return false;
  if (!sink) return fail(SseError::Misuse);
  feeding_ = true;
  struct FeedingGuard {
    bool& flag;
    ~FeedingGuard() { flag = false; }
  } guard{feeding_};

  for (unsigned char byte : bytes) {
    // Check incrementally, not per feed: callbacks before the limit are
    // identical whether an oversized suffix arrives in this or another feed.
    if (total_bytes_ == limits_.max_total_bytes) {
      return fail(SseError::ResourceLimit);
    }
    ++total_bytes_;
    if (!consume_byte(byte, sink)) return false;
  }
  return true;
}

bool SseFramer::consume_byte(unsigned char byte, const Sink& sink) {
  if (remaining_ == 0) {
    if (byte <= 0x7f) {
      scalar_[0] = static_cast<char>(byte);
      return consume_scalar(std::string_view(scalar_.data(), 1), sink);
    }
    scalar_size_ = 1;
    scalar_[0] = static_cast<char>(byte);
    continuation_min_ = 0x80;
    continuation_max_ = 0xbf;
    if (byte >= 0xc2 && byte <= 0xdf) {
      remaining_ = 1;
    } else if (byte >= 0xe0 && byte <= 0xef) {
      remaining_ = 2;
      if (byte == 0xe0) continuation_min_ = 0xa0;
      if (byte == 0xed) continuation_max_ = 0x9f;
    } else if (byte >= 0xf0 && byte <= 0xf4) {
      remaining_ = 3;
      if (byte == 0xf0) continuation_min_ = 0x90;
      if (byte == 0xf4) continuation_max_ = 0x8f;
    } else {
      return fail(SseError::InvalidUtf8);
    }
    return true;
  }

  if (byte < continuation_min_ || byte > continuation_max_) {
    return fail(SseError::InvalidUtf8);
  }
  scalar_[scalar_size_++] = static_cast<char>(byte);
  continuation_min_ = 0x80;
  continuation_max_ = 0xbf;
  if (--remaining_ != 0) return true;
  const auto size = scalar_size_;
  scalar_size_ = 0;
  return consume_scalar(std::string_view(scalar_.data(), size), sink);
}

bool SseFramer::consume_scalar(std::string_view scalar, const Sink& sink) {
  if (at_start_) {
    at_start_ = false;
    if (scalar == "\xef\xbb\xbf") return true;
  }
  if (after_cr_) {
    after_cr_ = false;
    if (scalar == "\n") return true;
  }
  if (scalar == "\r" || scalar == "\n") {
    after_cr_ = scalar == "\r";
    const bool accepted = process_line(sink);
    line_.clear();
    return accepted;
  }
  if (scalar.size() > limits_.max_line_bytes - line_.size()) {
    return fail(SseError::ResourceLimit);
  }
  line_.append(scalar);
  return true;
}

bool SseFramer::process_line(const Sink& sink) {
  if (line_.empty()) {
    if (!has_data_) {
      event_.clear();
      return true;
    }
    const SseFrame frame{event_.empty() ? std::string_view("message")
                                       : std::string_view(event_),
                         data_, id_};
    bool accepted;
    try {
      accepted = sink(frame);
    } catch (...) {
      stopped_ = true;
      throw;
    }
    data_.clear();
    event_.clear();
    has_data_ = false;
    if (!accepted) stopped_ = true;
    return !stopped_;
  }
  if (line_.front() == ':') return true;

  const std::string_view line(line_);
  const auto colon = line.find(':');
  const auto field = line.substr(0, colon);
  auto value = colon == std::string_view::npos ? std::string_view{}
                                              : line.substr(colon + 1);
  if (!value.empty() && value.front() == ' ') value.remove_prefix(1);

  if (field == "data") {
    // Store only actual joined data, not the spec's removable trailing LF.
    // A separate flag distinguishes no data field from one empty data field.
    auto available = limits_.max_event_bytes - data_.size();
    if (has_data_) {
      if (available == 0) return fail(SseError::ResourceLimit);
      --available;
    }
    if (value.size() > available) return fail(SseError::ResourceLimit);
    const auto new_size = data_.size() + (has_data_ ? 1 : 0) + value.size();
    if (!fits_event(new_size, event_.size(), id_.size())) {
      return fail(SseError::ResourceLimit);
    }
    if (has_data_) data_.push_back('\n');
    data_.append(value);
    has_data_ = true;
  } else if (field == "event") {
    if (!fits_event(data_.size(), value.size(), id_.size())) {
      return fail(SseError::ResourceLimit);
    }
    event_.assign(value);
  } else if (field == "id" && value.find('\0') == std::string_view::npos) {
    if (!fits_event(data_.size(), event_.size(), value.size())) {
      return fail(SseError::ResourceLimit);
    }
    id_.assign(value);
  }
  // Comments, unknown fields and retry do not drive reconnection or delivery.
  return true;
}

void SseFramer::finish() {
  if (feeding_) {
    fail(SseError::Misuse);
    return;
  }
  if (stopped_) return;
  if (remaining_ != 0) fail(SseError::InvalidUtf8);
  stopped_ = true;
  line_.clear();
  data_.clear();
  event_.clear();
  has_data_ = false;
}

}  // namespace sp::transport
