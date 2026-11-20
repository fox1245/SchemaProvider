#pragma once
// The facts a prepared request carries (core/request_facts.h), read from each family's typed request.
#include "codecs/batches_internal.h"
#include "codecs/chat.h"
#include "codecs/gemini_request.h"
#include "codecs/interactions_request.h"
#include "codecs/messages_request.h"
#include "codecs/responses_request.h"
#include "core/request_facts.h"
#include "descriptor/descriptor.h"

namespace sp::runtime::detail {
RequestFacts request_facts(const descriptor::ValidatedDescriptor&, const chat::Request&);
RequestFacts request_facts(const descriptor::ValidatedDescriptor&, const messages::Request&);
RequestFacts request_facts(const descriptor::ValidatedDescriptor&, const responses::Request&);
RequestFacts request_facts(const descriptor::ValidatedDescriptor&, const gemini::Request&);
RequestFacts request_facts(const descriptor::ValidatedDescriptor&, const interactions::Request&);
// A chat request a batch carries, as the bound reads it (CarriedRequest): its custom id and model, the output cap and
// model invocations its family encoder read (`caps`), and its facts, whose service tier is the catalogue tier the
// request names (DescriptorPolicy::billing_tier: empty for a standard tier of the family).
CarriedRequest carried_request(const descriptor::ValidatedDescriptor& family, const FamilyRequest& request, const batches::detail::ItemCaps& caps);
} // namespace sp::runtime::detail
