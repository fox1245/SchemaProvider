#pragma once
#include "codecs/messages_request.h"
#include "codecs/responses_request.h"
#include <array>

namespace sp {
class Accumulator;
// Private in-process provenance, not issuer authentication or a persistence format.
// There is deliberately no constructor accepting caller-provided binding hashes.
class NativeContext final {
 public:
  NativeContext(const NativeContext&) = delete;
  NativeContext& operator=(const NativeContext&) = delete;
  std::string_view model() const { return model_; }
  bool matches_descriptor(const descriptor::ValidatedDescriptor&) const;
  std::optional<std::string_view> server_tool_name(std::string_view id) const;
 private:
  NativeContext(const descriptor::ValidatedDescriptor&, const messages::Request&, bool streaming);
  NativeContext(const descriptor::ValidatedDescriptor&, const responses::Request&, bool streaming);
  void bind_history(const std::vector<Message>&);
  const bool responses_;
  const std::string model_, route_;
  const size_t prefix_count_;
  std::array<unsigned char, 32> origin_{}, prefix_{};
  bool valid_ = false;
  bool history_valid_ = true;
  std::vector<std::pair<std::string, std::string>> pending_server_tools_;
  friend messages::EncodeResult messages::encode(const descriptor::ValidatedDescriptor&, const messages::Request&, bool);
  friend responses::EncodeResult responses::encode(const descriptor::ValidatedDescriptor&, const responses::Request&, bool);
  friend class NativeReplay;
};
class NativeReplay final {
 public:
  NativeReplay(const NativeReplay&) = delete;
  NativeReplay& operator=(const NativeReplay&) = delete;
  bool complete() const { return complete_; }
 private:
  NativeReplay(std::shared_ptr<const NativeContext>, const Message&, const StopReason*, bool complete);
  const std::shared_ptr<const NativeContext> context_;
  const StopKind stop_;
  std::array<unsigned char, 32> content_{};
  bool complete_ = false;
  friend class Accumulator;
  friend class NativeContext;
  friend messages::EncodeResult messages::encode(const descriptor::ValidatedDescriptor&, const messages::Request&, bool);
  friend responses::EncodeResult responses::encode(const descriptor::ValidatedDescriptor&, const responses::Request&, bool);
};
} // namespace sp
