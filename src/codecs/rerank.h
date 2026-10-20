#pragma once
// Rerank (D6): OpenRouter `/api/v1/rerank` orders documents by their relevance to a query. A request is dispatched
// by `runtime::Client::prepare(rerank::Request)`; the policy's endpoint facts name the origins.
#include "codecs/endpoint.h"
#include "core/value.h"
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sp::rerank {
struct Request {
  std::string model, query;
  std::vector<std::string> documents;
  // How many of the best results to return; all when unset.
  std::optional<std::uint32_t> top{};
  // Members the endpoint's policy fact admits (`provider`, `session_id`), sent as given.
  std::shared_ptr<const json::Document> extensions{};
};
struct Ranked {
  // The document's position in the request.
  std::uint64_t index = 0;
  double relevance = 0;
};
struct Result {
  // Most relevant first; documents of equal relevance keep the provider's order. The response document in
  // `raw_events` keeps the provider's own order.
  std::vector<Ranked> ranked;
  std::string id, provider;
  Usage usage;
  ServedModel model;
  AttemptEvidence attempt{};
  std::vector<RawWire> raw_events;
};
using Outcome = std::variant<Result, Failure>;

using Encoded = endpoints::Encoded;
std::variant<Encoded, Error> encode(const descriptor::ValidatedDescriptor&, const Request&);

using Close = endpoints::Close;
// Checks one buffered response against the call: every result names a document of the request once, and there are as
// many as the call asked for (`top`, or every document; zero `documents`: unchecked).
class Codec final : public endpoints::Codec {
 public:
  Codec(const descriptor::ValidatedDescriptor&, Accumulator&, std::string requested, SemanticLimits = {}, std::uint64_t documents = 0,
        std::uint64_t top = 0);
  bool buffered(std::string_view body, Close close) override;
 private:
  bool fail(ErrorKind, std::string);
  bool emit(const Event&);
  const descriptor::ValidatedDescriptor& descriptor_;
  Accumulator& accumulator_;
  std::string requested_;
  SemanticLimits limits_;
  std::uint64_t documents_, top_;
};
Outcome typed(const sp::Outcome&);
}  // namespace sp::rerank
