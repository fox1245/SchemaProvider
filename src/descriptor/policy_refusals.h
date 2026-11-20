#pragma once
// The policy checks the request encoders run, each refusal naming the constant of the check (descriptor/reason.h).
// The public validate_choices, route_refusal and literal_route_refusal give the constant's text. Private: not
// installed.
#include "descriptor/policy.h"
#include "descriptor/reason.h"
#include <optional>
#include <string_view>

namespace sp::descriptor {
std::optional<reason::Reason> choices_refusal(const ValidatedDescriptor&, std::string_view model, const EffectiveChoices&);
std::optional<reason::Reason> route_refusal_reason(const ValidatedDescriptor&, std::string_view model);
std::optional<reason::Reason> literal_route_refusal_reason(const ValidatedDescriptor&, std::string_view model);
}  // namespace sp::descriptor
