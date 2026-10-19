#pragma once
// A request of one of the chat families, for the endpoints that carry one: a batch item, a token count.
#include "codecs/chat.h"
#include "codecs/gemini_request.h"
#include "codecs/messages_request.h"
#include "codecs/responses_request.h"
#include <variant>

namespace sp {
using FamilyRequest = std::variant<chat::Request, messages::Request, responses::Request, gemini::Request>;
}  // namespace sp
