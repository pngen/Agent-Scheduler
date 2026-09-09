// Agent Scheduler — narrow external evidence adapters.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/adapters.hpp"

#include <utility>

namespace agent_scheduler {
namespace {

[[nodiscard]] FeasibilityVerdict lookup(const std::map<std::uint64_t, FeasibilityVerdict>& table,
                                        std::uint64_t generation) {
  const auto found = table.find(generation);
  if (found == table.end()) {
    return FeasibilityVerdict::Unknown;
  }
  return found->second;
}

}  // namespace

ExternalFeasibilityResult NullFeasibilityProvider::evaluate(const ExternalFeasibilityRequest& request) {
  (void)request;
  return ExternalFeasibilityResult{};
}

void ScriptedFeasibilityProvider::set_resource(ResourceGeneration generation, FeasibilityVerdict verdict) {
  const std::lock_guard<std::mutex> guard(mutex_);
  resource_[generation.value()] = verdict;
}

void ScriptedFeasibilityProvider::set_budget(BudgetGeneration generation, FeasibilityVerdict verdict) {
  const std::lock_guard<std::mutex> guard(mutex_);
  budget_[generation.value()] = verdict;
}

void ScriptedFeasibilityProvider::set_slo(SloGeneration generation, FeasibilityVerdict verdict) {
  const std::lock_guard<std::mutex> guard(mutex_);
  slo_[generation.value()] = verdict;
}

void ScriptedFeasibilityProvider::set_reservation(ReservationGeneration generation,
                                                  FeasibilityVerdict verdict) {
  const std::lock_guard<std::mutex> guard(mutex_);
  reservation_[generation.value()] = verdict;
}

void ScriptedFeasibilityProvider::set_detail(std::string detail) {
  const std::lock_guard<std::mutex> guard(mutex_);
  detail_ = std::move(detail);
}

ExternalFeasibilityResult ScriptedFeasibilityProvider::evaluate(const ExternalFeasibilityRequest& request) {
  const std::lock_guard<std::mutex> guard(mutex_);
  ExternalFeasibilityResult result;
  result.resource = lookup(resource_, request.resource_generation.value());
  result.budget = lookup(budget_, request.budget_generation.value());
  result.slo = lookup(slo_, request.slo_generation.value());
  result.reservation = lookup(reservation_, request.reservation_generation.value());
  result.detail = detail_;
  return result;
}

}  // namespace agent_scheduler
