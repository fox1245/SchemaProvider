#include "runtime/client.h"

#ifndef SP_MUTATION_CASE
#define SP_MUTATION_CASE 0
#endif

void result_contract(const sp::runtime::Result& result) {
#if SP_MUTATION_CASE == 0
  const auto& completion = std::get<sp::Completion>(*result);
  const auto& failure = std::get<sp::Failure>(*result);
  (void)completion.messages.size();
  (void)failure.partial.messages.size();
#elif SP_MUTATION_CASE == 1
  std::get<sp::Completion>(*result).messages.clear();
#elif SP_MUTATION_CASE == 2
  std::get<sp::Text>(std::get<sp::Completion>(*result).messages.front().parts.front()).value.clear();
#elif SP_MUTATION_CASE == 3
  std::get<sp::Failure>(*result).error.safe_message.clear();
#elif SP_MUTATION_CASE == 4
  std::get<sp::Failure>(*result).partial.messages.front().parts.clear();
#endif
}
