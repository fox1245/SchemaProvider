#pragma once
// The facts a prepared request carries (core/request_facts.h), read from each family's typed request.
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
} // namespace sp::runtime::detail
