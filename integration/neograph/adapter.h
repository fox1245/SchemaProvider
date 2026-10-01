#pragma once

#include "runtime/client.h"
#include <neograph/completion_provider.h>
#include <stdexcept>

namespace sp::neograph_integration {

enum class Family { Chat, Messages };

// No raw server error or opaque native bytes appear in what(). Rich evidence is
// owned separately, including partial output, nullable usage and native seals.
class BoundaryError final : public std::runtime_error {
 public:
  BoundaryError(std::vector<std::string> gaps, runtime::Result evidence = {});
  const std::vector<std::string>& gaps() const noexcept { return gaps_; }
  const runtime::Result& evidence() const noexcept { return evidence_; }
 private:
  std::vector<std::string> gaps_;
  runtime::Result evidence_;
};

// Executable lossless-cutover restriction, not a drop-in success adapter. Every
// existing Provider result entry throws BoundaryError with the real runtime
// outcome because ChatCompletion cannot carry even usage provenance. The owned
// entry is usable; it never fabricates zero counters or upgrades editable JSON
// into native replay authority. Messages tool history needs seals unavailable
// in CompletionParams and is therefore rejected before dispatch.
class RuntimeProvider final : public neograph::CompletionProvider {
 public:
  RuntimeProvider(std::shared_ptr<runtime::Client>, Family);
  std::string get_name() const override { return "schemaprovider-runtime"; }
  // As with the legacy awaitable API, keep the provider alive until this
  // coroutine starts. After its first suspension it owns all operation state.
  asio::awaitable<runtime::Result> invoke_owned(neograph::CompletionRequest);
  static neograph::ChatCompletion project(runtime::Result);
 protected:
  asio::awaitable<neograph::ChatCompletion> do_invoke(neograph::CompletionRequest) override;
 private:
  std::shared_ptr<runtime::Client> client_;
  Family family_;
};

} // namespace sp::neograph_integration
