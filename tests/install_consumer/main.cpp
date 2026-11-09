#include <codecs/accumulator.h>
#include <codecs/messages.h>
#include <codecs/messages_request.h>
#include <core/interface_contract.h>
#include <core/native.h>
#include <core/native_archive.h>
#include <core/native_archive_custody.h>
#include <core/value.h>
#include <descriptor/descriptor.h>

#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

constexpr std::string_view kDescriptorJson = R"json({
  "descriptor_version": 1,
  "revision": 1,
  "id": "install-consumer-messages",
  "family": "anthropic.messages",
  "connection": {
    "base_url": "http://127.0.0.1:18080",
    "paths": {
      "buffered": "/v1/messages",
      "streaming": "/v1/messages"
    }
  }
})json";

constexpr std::string_view kMessagesResponseJson =
    R"({"id":"msg_install_consumer","type":"message","role":"assistant","model":"fixture-model","content":[{"type":"thinking","thinking":"reason","signature":"synthetic-sensitive-signature"},{"type":"redacted_thinking","data":"synthetic-encrypted-state"},{"type":"text","text":"Custody round-trip verified."}],"stop_reason":"end_turn","stop_sequence":null,"usage":{"input_tokens":5,"output_tokens":4}})";

int fail(std::string_view step, std::string_view detail = {}) {
  std::cerr << "install_consumer failed at " << step;
  if (!detail.empty()) {
    std::cerr << ": " << detail;
  }
  std::cerr << '\n';
  return 1;
}

}  // namespace

int main() {
  const auto contract = sp::core_interface_contract();
  if (contract.revision != sp::EXPECTED_INTERFACE_REVISION) {
    return fail("core_interface_contract.revision");
  }
  // core_interface_contract() reports the core library's own bits; RequiredProvider also needs the runtime's, which
  // this consumer does not link. It checks only what it uses: the revision and host custody.
  if ((contract.capabilities & sp::capability::HostArchiveCustody) == 0) {
    return fail("core_interface_contract.HostArchiveCustody");
  }
  if (sp::codec_interface_revision() != sp::EXPECTED_INTERFACE_REVISION) {
    return fail("codec_interface_revision");
  }

  auto loaded = sp::descriptor::load(kDescriptorJson);
  if (const auto* cfg_err = std::get_if<sp::descriptor::ConfigError>(&loaded)) {
    return fail("descriptor::load", cfg_err->message);
  }
  const auto& descriptor = std::get<sp::descriptor::ValidatedDescriptor>(loaded);

  std::string secret(32, '\0');
  for (std::size_t i = 0; i < secret.size(); ++i) {
    secret[i] = static_cast<char>(0x30u + i);
  }

  auto secrets = std::make_shared<sp::PreResolvedSecretSource>("k1");
  if (secrets->put("k1", secret).has_value()) {
    return fail("PreResolvedSecretSource::put(create_header)");
  }

  auto key_source = sp::make_hkdf_key_source(secrets);
  const auto header_res = sp::create_header(*key_source, "owner-install-consumer", descriptor);
  if (const auto* err = std::get_if<sp::CustodyError>(&header_res)) {
    return fail("create_header", err->error.safe_message);
  }
  const std::string& header_bytes = std::get<std::string>(header_res);

  const auto parsed = sp::parse_header(header_bytes);
  if (const auto* err = std::get_if<sp::CustodyError>(&parsed)) {
    return fail("parse_header", err->error.safe_message);
  }
  const auto& parsed_header = std::get<sp::StoreHeader>(parsed);
  if (parsed_header.format_version != 1 || parsed_header.owner_scope != "owner-install-consumer" ||
      parsed_header.current_epoch != 1 || parsed_header.epochs.size() != 1 ||
      parsed_header.epochs[0].wrapping_key_id != "k1") {
    return fail("parse_header.fields");
  }

  secrets->clear();
  if (secrets->put("k1", secret).has_value()) {
    return fail("PreResolvedSecretSource::put(open_custody)");
  }

  const auto opened_archive = sp::NativeArchive::open_custody(
      header_bytes, *key_source, "owner-install-consumer", descriptor);
  if (const auto* err = std::get_if<sp::CustodyError>(&opened_archive)) {
    return fail("NativeArchive::open_custody", err->error.safe_message);
  }
  const auto& archive = std::get<std::shared_ptr<sp::NativeArchive>>(opened_archive);
  secrets->clear();

  sp::messages::Request req;
  req.model = "fixture-model";
  req.account_scope = "fixture-account";
  req.messages.push_back(sp::Message::user_text("Verify host archive custody."));
  const auto encoded_initial = sp::messages::encode(descriptor, req, false);
  if (const auto* err = std::get_if<sp::Error>(&encoded_initial)) {
    return fail("messages::encode(initial)", err->safe_message);
  }

  sp::Accumulator acc;
  sp::messages::Codec codec(descriptor,
                            sp::messages::Mode::Buffered,
                            acc,
                            std::get<sp::messages::EncodedRequest>(encoded_initial).context);
  if (!codec.buffered(kMessagesResponseJson, {true, sp::ErrorKind::Truncated})) {
    return fail("messages::Codec::buffered");
  }
  codec.finish();
  if (!acc.outcome() || !std::holds_alternative<sp::Completion>(*acc.outcome())) {
    return fail("messages::Codec::outcome");
  }
  const sp::Message assistant_msg = std::get<sp::Completion>(*acc.outcome()).messages.at(0);
  if (!assistant_msg.native || !assistant_msg.native->complete()) {
    return fail("assistant_msg.native");
  }

  req.messages.push_back(assistant_msg);
  req.messages.push_back(sp::Message::user_text("Continue."));
  const auto expected_encoded = sp::messages::encode(descriptor, req, false);
  if (const auto* err = std::get_if<sp::Error>(&expected_encoded)) {
    return fail("messages::encode(expected)", err->safe_message);
  }

  const auto sealed_res = archive->seal(req.messages, "install-consumer-binding");
  if (const auto* err = std::get_if<sp::CustodyError>(&sealed_res)) {
    return fail("NativeArchive::seal", err->error.safe_message);
  }
  const auto& sealed = std::get<sp::SealedRecord>(sealed_res);
  if (sealed.reference.rfind("spna4:", 0) != 0 || sealed.bytes.empty()) {
    return fail("sealed.shape");
  }

  const auto opened_res =
      archive->open(sealed.reference, sealed.bytes, "install-consumer-binding");
  if (const auto* err = std::get_if<sp::CustodyError>(&opened_res)) {
    return fail("NativeArchive::open", err->error.safe_message);
  }
  const auto& restored = std::get<std::vector<sp::Message>>(opened_res);
  if (restored.size() != 3 || restored[0].joined_text() != "Verify host archive custody." ||
      restored[1].joined_text() != "Custody round-trip verified." ||
      !restored[1].native || !restored[1].native->complete() ||
      restored[2].joined_text() != "Continue.") {
    return fail("restored.messages");
  }

  auto replay_req = req;
  replay_req.messages = restored;
  const auto actual_encoded = sp::messages::encode(descriptor, replay_req, false);
  if (const auto* err = std::get_if<sp::Error>(&actual_encoded)) {
    return fail("messages::encode(replay)", err->safe_message);
  }
  if (std::get<sp::messages::EncodedRequest>(actual_encoded).body !=
      std::get<sp::messages::EncodedRequest>(expected_encoded).body) {
    return fail("replay.body.mismatch");
  }

  const auto tampered_res = archive->open(sealed.reference, sealed.bytes, "wrong-binding");
  const auto* tampered_err = std::get_if<sp::CustodyError>(&tampered_res);
  if (tampered_err == nullptr || tampered_err->error.reason != "archive.custody.tampered") {
    return fail("tampered.binding.check");
  }

  return 0;
}
