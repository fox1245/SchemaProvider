#pragma once
// Internal to the library and not installed: what the batch encoders read from each item as its family encodes it, so
// the runtime can bound the requests a batch carries without encoding them a second time.
#include "codecs/batches.h"
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sp::batches::detail {
// One item as its family encoder read it: the custom id, the model the request names, the output cap and the model
// invocations one attempt may make (the family's EncodedRequest).
struct ItemCaps {
  std::string custom_id, model;
  std::optional<std::uint64_t> max_output_tokens;
  std::optional<std::uint64_t> model_invocation_limit;
};
// A submit as batches::encode encodes it, with the caps of each inline item in order; none for OpenAI, whose items
// are in an uploaded file.
struct EncodedSubmit {
  Encoded encoded;
  std::vector<ItemCaps> items;
};
std::variant<EncodedSubmit, Error> encode_submit(const descriptor::ValidatedDescriptor&, const Submit&);
// OpenAI's input file as batches::jsonl writes it, with the caps of each item in order.
struct EncodedLines {
  std::string text;
  std::vector<ItemCaps> items;
};
std::variant<EncodedLines, Error> encode_lines(const descriptor::ValidatedDescriptor& family, const std::vector<Item>&);
}  // namespace sp::batches::detail
