#pragma once
// Frozen format identifiers for host-native archive custody (v1).
#include <cstdint>
#include <string_view>

namespace sp::custody_format {

inline constexpr std::uint64_t header_format_version = 1;
inline constexpr std::uint64_t record_format_version = 1;

inline constexpr std::string_view header_body_domain = "sp.native.archive.custody.header.v1";
inline constexpr std::string_view header_tag_domain = "sp.native.archive.custody.header-tag.v1";
inline constexpr std::string_view epoch_aad_domain = "sp.native.archive.custody.epoch-aad.v1";
inline constexpr std::string_view kek_domain = "sp.native.archive.custody.kek.v1";
inline constexpr std::string_view record_aad_domain = "sp.native.archive.custody.record-aad.v1";
inline constexpr std::string_view ref_domain = "sp.native.archive.custody.record-ref.v1";
inline constexpr std::string_view ref_prefix = "spna4:";

}  // namespace sp::custody_format
