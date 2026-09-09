// Agent Scheduler — scheduling policy, fairness contract, and ranking weights.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/policy.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "internal/util.hpp"

namespace agent_scheduler {
namespace {

constexpr std::array<std::string_view, 18> kKnownFactors = {
    ranking_factor::load_headroom,          ranking_factor::remaining_capacity,
    ranking_factor::capability_quality,     ranking_factor::capability_completeness,
    ranking_factor::warm_state_affinity,    ranking_factor::locality_match,
    ranking_factor::health_quality,         ranking_factor::readiness,
    ranking_factor::queue_age,              ranking_factor::fairness_deficit,
    ranking_factor::dispatch_latency,       ranking_factor::cost_evidence,
    ranking_factor::slo_headroom,           ranking_factor::resource_headroom,
    ranking_factor::reliability_history,    ranking_factor::anti_concentration,
    ranking_factor::failure_domain_diversity, ranking_factor::handoff_cost};

[[nodiscard]] bool is_known_factor(std::string_view name) noexcept {
  return std::find(kKnownFactors.begin(), kKnownFactors.end(), name) != kKnownFactors.end();
}

}  // namespace

std::vector<RankingWeight> default_ranking_weights() {
  return {
      {std::string(ranking_factor::load_headroom), 30},
      {std::string(ranking_factor::remaining_capacity), 20},
      {std::string(ranking_factor::capability_quality), 25},
      {std::string(ranking_factor::capability_completeness), 15},
      {std::string(ranking_factor::warm_state_affinity), 10},
      {std::string(ranking_factor::locality_match), 10},
      {std::string(ranking_factor::health_quality), 15},
      {std::string(ranking_factor::readiness), 10},
      {std::string(ranking_factor::queue_age), 8},
      {std::string(ranking_factor::fairness_deficit), 8},
      {std::string(ranking_factor::dispatch_latency), 6},
      {std::string(ranking_factor::cost_evidence), 6},
      {std::string(ranking_factor::slo_headroom), 6},
      {std::string(ranking_factor::resource_headroom), 6},
      {std::string(ranking_factor::reliability_history), 6},
      {std::string(ranking_factor::anti_concentration), 8},
      {std::string(ranking_factor::failure_domain_diversity), 6},
      {std::string(ranking_factor::handoff_cost), 6},
  };
}

std::vector<QueueClassWeight> default_fairness_class_weights() {
  return {{"interactive", 4}, {"default", 2}, {"batch", 1}};
}

MutationResult validate(const PolicySnapshot& policy, const ResourceLimits& limits) {
  if (policy.id.is_zero()) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "set_policy", "",
                                              "policy id must be non-zero",
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  if (policy.generation.is_zero()) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "set_policy",
                                              policy.id.to_string(), "policy generation must be non-zero",
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  if (policy.aging_rounds_per_step == 0) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "set_policy",
                                              policy.id.to_string(),
                                              "aging_rounds_per_step must be at least 1",
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  if (policy.max_priority_bypass == 0 && !policy.allow_unbounded_starvation) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "set_policy",
                                              policy.id.to_string(),
                                              "max_priority_bypass must be at least 1 when starvation is bounded",
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  if (policy.ranking_weights.size() > limits.max_explanation_factors) {
    return MutationResult::failure(make_error(ErrorCode::LimitExceeded, "set_policy",
                                              policy.id.to_string(),
                                              "ranking weight count exceeds max_explanation_factors",
                                              ScheduleOutcome::RejectLimitExceeded));
  }
  std::vector<std::string> factors;
  for (const RankingWeight& weight : policy.ranking_weights) {
    if (!is_known_factor(weight.factor)) {
      return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "set_policy", weight.factor,
                                                "unknown ranking factor",
                                                ScheduleOutcome::RejectInvalidRequest));
    }
    if (weight.weight > 1000000) {
      return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "set_policy", weight.factor,
                                                "ranking weight exceeds 1000000",
                                                ScheduleOutcome::RejectInvalidRequest));
    }
    factors.push_back(weight.factor);
  }
  if (!internal::canonicalize_unique(factors)) {
    return MutationResult::failure(make_error(ErrorCode::Duplicate, "set_policy", "",
                                              "duplicate ranking factor",
                                              ScheduleOutcome::RejectDuplicate));
  }
  if (policy.fairness_class_weights.size() > limits.max_queues) {
    return MutationResult::failure(make_error(ErrorCode::LimitExceeded, "set_policy",
                                              policy.id.to_string(),
                                              "fairness class count exceeds max_queues",
                                              ScheduleOutcome::RejectLimitExceeded));
  }
  std::vector<std::string> classes;
  for (const QueueClassWeight& weight : policy.fairness_class_weights) {
    if (const auto problem = internal::identifier_problem(weight.fairness_class,
                                                          limits.max_identifier_size,
                                                          "fairness class")) {
      return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "set_policy", "", *problem,
                                                ScheduleOutcome::RejectInvalidRequest));
    }
    if (weight.weight == 0 || weight.weight > 1000) {
      return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "set_policy",
                                                weight.fairness_class,
                                                "fairness class weight must be 1..1000",
                                                ScheduleOutcome::RejectInvalidRequest));
    }
    classes.push_back(weight.fairness_class);
  }
  if (!internal::canonicalize_unique(classes)) {
    return MutationResult::failure(make_error(ErrorCode::Duplicate, "set_policy", "",
                                              "duplicate fairness class",
                                              ScheduleOutcome::RejectDuplicate));
  }
  if (policy.anti_concentration_window > limits.max_retained_history) {
    return MutationResult::failure(make_error(ErrorCode::LimitExceeded, "set_policy",
                                              policy.id.to_string(),
                                              "anti_concentration_window exceeds max_retained_history",
                                              ScheduleOutcome::RejectLimitExceeded));
  }
  if (policy.assignment_lease_ttl_ms == 0 || policy.registration_lease_ttl_ms == 0) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "set_policy",
                                              policy.id.to_string(), "lease TTLs must be non-zero",
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  return MutationResult::success();
}

std::uint32_t fairness_class_weight(const PolicySnapshot& policy, std::string_view fairness_class) noexcept {
  for (const QueueClassWeight& weight : policy.fairness_class_weights) {
    if (weight.fairness_class == fairness_class) {
      return weight.weight;
    }
  }
  return 1;
}

std::uint32_t bypass_ceiling(const PolicySnapshot& policy, std::string_view fairness_class) noexcept {
  if (policy.allow_unbounded_starvation) {
    return 0xFFFFFFFFu;
  }
  const std::uint64_t weight = fairness_class_weight(policy, fairness_class);
  if (weight == 0) {
    return policy.max_priority_bypass;
  }
  const std::uint64_t ceiling = static_cast<std::uint64_t>(policy.max_priority_bypass) / weight;
  if (ceiling < 1) {
    return 1;
  }
  if (ceiling > 0xFFFFFFFFull) {
    return 0xFFFFFFFFu;
  }
  return static_cast<std::uint32_t>(ceiling);
}

}  // namespace agent_scheduler
