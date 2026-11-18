#pragma once
// Test access to the digests a native context binds. This header is not installed and is not part of the
// interface: it exists so tests can pin the values that sealed history depends on.
#include "codecs/chat.h"
#include "descriptor/descriptor.h"
#include "descriptor/policy.h"
#include <array>
#include <optional>

namespace sp::native_testing {
// The native origin digest of `descriptor`, computed by the function every native context uses.
// Empty when the family has no native origin or a resource bound is exceeded.
std::optional<std::array<unsigned char, 32>> origin_digest_of(const descriptor::ValidatedDescriptor& descriptor);
// The Chat configuration digest of `request` under `choices`, computed by the function the Chat context uses.
// Empty when a resource bound is exceeded.
std::optional<std::array<unsigned char, 32>> chat_configuration_digest_of(const descriptor::ValidatedDescriptor& descriptor,
                                                                          const chat::Request& request,
                                                                          const descriptor::EffectiveChoices& choices);
} // namespace sp::native_testing
