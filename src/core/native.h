#pragma once
#include "codecs/chat.h"
#include "codecs/messages_request.h"
#include "codecs/responses_request.h"
#include "codecs/gemini_request.h"
#include "codecs/interactions_request.h"
#include "descriptor/policy.h"
#include <array>

namespace sp {
class Accumulator;
class NativeArchive;
// Private in-process provenance, not issuer authentication or a persistence format.
// There is deliberately no constructor accepting caller-provided binding hashes.
class NativeContext final {
 public:
  NativeContext(const NativeContext&) = delete;
  NativeContext& operator=(const NativeContext&) = delete;
  std::string_view model() const { return model_; }
  std::string_view family() const { return family_name(family_); }
  bool matches_descriptor(const descriptor::ValidatedDescriptor&) const;
  std::optional<std::string_view> server_tool_name(std::string_view id) const;
  bool client_tool_declared(std::string_view name) const;
  // Drops issuing authority permanently while retaining typed decode scope.
  std::shared_ptr<const NativeContext> decoding_only() const;
  bool replay_eligible() const noexcept { return replay_eligible_; }
 private:
  enum class Family { Messages, Responses, Gemini, Interactions, Chat };
  static std::string_view family_name(Family);
  NativeContext(Family, std::string model, std::string route, size_t prefix_count, descriptor::PolicySnapshot);
  NativeContext(const descriptor::ValidatedDescriptor&, const messages::Request&, const descriptor::EffectiveChoices&, bool streaming);
  NativeContext(const descriptor::ValidatedDescriptor&, const responses::Request&, const descriptor::EffectiveChoices&, bool streaming);
  NativeContext(const descriptor::ValidatedDescriptor&, const gemini::Request&, const descriptor::EffectiveChoices&, bool streaming);
  NativeContext(const descriptor::ValidatedDescriptor&, const interactions::Request&, const descriptor::EffectiveChoices&, bool streaming);
  NativeContext(const descriptor::ValidatedDescriptor&, const chat::Request&, const descriptor::EffectiveChoices&, bool streaming);
  void bind_history(const std::vector<Message>&, bool portable_gemini = false);
  const Family family_;
  const std::string model_, route_;
  const size_t prefix_count_;
  const descriptor::PolicySnapshot policy_;
  std::array<unsigned char, 32> origin_{}, prefix_{};
  // In-process Responses cursor custody is separate from portable/native replay.
  // These fields are deliberately not part of archive format 3.
  std::array<unsigned char, 32> configuration_{};
  bool cursor_authority_ = false;
  bool valid_ = false;
  bool history_valid_ = true;
  bool replay_eligible_ = true;
  std::vector<std::pair<std::string, std::string>> pending_server_tools_;
  std::vector<std::string> client_tools_;
  friend messages::EncodeResult messages::encode(const descriptor::ValidatedDescriptor&, const messages::Request&, bool);
  friend responses::EncodeResult responses::encode(const descriptor::ValidatedDescriptor&, const responses::Request&, bool);
  friend gemini::EncodeResult gemini::encode(const descriptor::ValidatedDescriptor&, const gemini::Request&, bool);
  friend interactions::EncodeResult interactions::encode(const descriptor::ValidatedDescriptor&, const interactions::Request&, bool);
  friend chat::EncodeResult chat::encode(const descriptor::ValidatedDescriptor&, const chat::Request&, bool);
  friend class NativeReplay;
  friend class NativeArchive;
  friend class Accumulator;
};
class NativeReplay final {
 public:
  NativeReplay(const NativeReplay&) = delete;
  NativeReplay& operator=(const NativeReplay&) = delete;
  bool complete() const { return complete_; }
 private:
  NativeReplay(std::shared_ptr<const NativeContext>, const Message&, const StopReason*, bool complete);
  NativeReplay(std::shared_ptr<const NativeContext>, StopKind, std::array<unsigned char, 32>, bool);
  bool archive_valid(const Message&) const;
  const std::shared_ptr<const NativeContext> context_;
  const StopKind stop_;
  std::array<unsigned char, 32> content_{};
  bool complete_ = false;
  bool continuation_complete_ = false;
  friend class Accumulator;
  friend class NativeContext;
  friend class NativeArchive;
  friend messages::EncodeResult messages::encode(const descriptor::ValidatedDescriptor&, const messages::Request&, bool);
  friend responses::EncodeResult responses::encode(const descriptor::ValidatedDescriptor&, const responses::Request&, bool);
  friend gemini::EncodeResult gemini::encode(const descriptor::ValidatedDescriptor&, const gemini::Request&, bool);
  friend interactions::EncodeResult interactions::encode(const descriptor::ValidatedDescriptor&, const interactions::Request&, bool);
  friend chat::EncodeResult chat::encode(const descriptor::ValidatedDescriptor&, const chat::Request&, bool);
};
} // namespace sp
