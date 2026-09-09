// Agent Scheduler — eligibility, ranking, assignment, dispatch, and revalidation.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agent_scheduler/scheduler.hpp"
#include "internal/scheduler_impl.hpp"

namespace agent_scheduler {
namespace {

using internal::Impl;

constexpr std::int64_t kQueueAgeCeilingMs = 300000;
constexpr std::int64_t kLatencyCeilingMs = 30000;

[[nodiscard]] std::int64_t ratio(std::uint64_t numerator, std::uint64_t denominator) noexcept {
  if (denominator == 0) {
    return 0;
  }
  if (numerator >= denominator) {
    return 1000;
  }
  return static_cast<std::int64_t>((numerator * 1000u) / denominator);
}

void add_factor(CandidateEvaluation& evaluation,
                std::string name,
                std::uint32_t weight,
                std::int64_t value,
                std::string note) {
  FactorScore factor;
  factor.name = std::move(name);
  factor.weight = weight;
  if (value < 0) {
    factor.status = FactorStatus::Unknown;
    factor.raw_value = 0;
    factor.normalized_value = 0;
  } else {
    factor.status = FactorStatus::Known;
    factor.raw_value = value;
    factor.normalized_value = std::clamp<std::int64_t>(value, 0, 1000);
  }
  factor.note = std::move(note);
  evaluation.factors.push_back(std::move(factor));
}

void reject(CandidateEvaluation& evaluation, ScheduleOutcome outcome, std::string detail) {
  evaluation.eligible = false;
  evaluation.rejection = outcome;
  evaluation.rejection_detail = std::move(detail);
  evaluation.factors.clear();
  evaluation.score = 0;
  evaluation.rank = 0;
}

[[nodiscard]] bool contains_id(const std::vector<AgentId>& values, AgentId agent) noexcept {
  return std::find(values.begin(), values.end(), agent) != values.end();
}

/// Hard eligibility. Executes before any ranking. UNKNOWN never passes.
[[nodiscard]] bool evaluate_eligibility(const Impl& impl,
                                        const WorkRecord& work_record,
                                        const AgentRecord& agent_record,
                                        const ExternalFeasibilityResult& feasibility,
                                        CandidateEvaluation& evaluation) {
  const WorkRequest& request = work_record.request;
  const WorkRequirements& requirements = request.requirements;
  evaluation.agent = agent_record.descriptor.id;
  evaluation.agent_generation = agent_record.descriptor.generation;
  evaluation.boot = agent_record.descriptor.boot;
  evaluation.active_assignments = agent_record.active_assignments;
  evaluation.available_capacity =
      agent_record.descriptor.max_concurrency > agent_record.active_assignments
          ? agent_record.descriptor.max_concurrency - agent_record.active_assignments
          : 0;

  if (internal::boot_is_fenced(impl, agent_record.descriptor.boot)) {
    reject(evaluation, ScheduleOutcome::RejectFenced, "incarnation is permanently fenced");
    return false;
  }
  switch (agent_record.lifecycle) {
    case AgentLifecycle::Lost:
    case AgentLifecycle::Retired:
      reject(evaluation, ScheduleOutcome::RejectLifecycle,
             std::string("agent lifecycle is ") + to_string(agent_record.lifecycle));
      return false;
    case AgentLifecycle::Draining:
      reject(evaluation, ScheduleOutcome::RejectDrain, "agent is draining and accepts no new work");
      return false;
    case AgentLifecycle::Registering:
      reject(evaluation, ScheduleOutcome::RejectReadiness, "agent has not published current evidence");
      return false;
    case AgentLifecycle::RevalidationRequired:
      reject(evaluation, ScheduleOutcome::RevalidationRequired,
             "agent incarnation requires revalidation");
      return false;
    case AgentLifecycle::Unavailable:
      reject(evaluation, ScheduleOutcome::RejectReadiness, "agent is unavailable");
      return false;
    case AgentLifecycle::Ready:
    case AgentLifecycle::Busy:
      break;
    case AgentLifecycle::Unregistered:
      reject(evaluation, ScheduleOutcome::RejectLifecycle, "agent is not registered");
      return false;
  }
  if (!requirements.tenant.is_zero() && requirements.tenant != agent_record.descriptor.tenant) {
    reject(evaluation, ScheduleOutcome::RejectTenant, "agent belongs to a different tenant");
    return false;
  }
  if (!requirements.name_space.is_zero() && requirements.name_space != agent_record.descriptor.name_space) {
    reject(evaluation, ScheduleOutcome::RejectTenant, "agent belongs to a different namespace");
    return false;
  }
  for (const std::string& label : requirements.required_policy_labels) {
    if (std::find(agent_record.descriptor.policy_labels.begin(), agent_record.descriptor.policy_labels.end(),
                  label) == agent_record.descriptor.policy_labels.end()) {
      reject(evaluation, ScheduleOutcome::RejectPolicy, std::string("agent lacks policy label ") + label);
      return false;
    }
  }
  if (requirements.require_ready && !lifecycle_accepts_new_work(agent_record.lifecycle)) {
    reject(evaluation, ScheduleOutcome::RejectReadiness, "agent lifecycle does not accept new work");
    return false;
  }
  if (requirements.require_healthy) {
    if (!internal::health_fresh(impl, agent_record)) {
      reject(evaluation, ScheduleOutcome::RejectHealth, "health evidence is not current");
      return false;
    }
    if (agent_record.health == AgentHealth::Unknown || agent_record.health == AgentHealth::Unhealthy) {
      reject(evaluation, ScheduleOutcome::RejectHealth,
             std::string("agent health is ") + to_string(agent_record.health));
      return false;
    }
  }
  if (requirements.require_reachable) {
    if (!internal::availability_fresh(impl, agent_record)) {
      reject(evaluation, ScheduleOutcome::RejectReachability, "reachability evidence is not current");
      return false;
    }
    if (agent_record.reachability != AgentReachability::Reachable) {
      reject(evaluation, ScheduleOutcome::RejectReachability, "agent is not reachable");
      return false;
    }
  }
  if (requirements.require_current_lease && !internal::lease_current(impl, agent_record)) {
    reject(evaluation, ScheduleOutcome::RejectLeaseExpired, "registration lease is not current");
    return false;
  }
  if (agent_record.active_assignments >= agent_record.descriptor.max_concurrency) {
    reject(evaluation, ScheduleOutcome::RejectCapacity, "agent is at declared concurrency capacity");
    return false;
  }
  if (work_record.current_assignments >= request.requirements.max_parallel) {
    reject(evaluation, ScheduleOutcome::RejectCapacity, "work fan-out is already saturated");
    return false;
  }
  if (internal::active_assignments_on_agent(impl, request.id, agent_record.descriptor.id) >=
      requirements.max_assignments_per_agent) {
    reject(evaluation, ScheduleOutcome::RejectCapacity,
           "agent already holds the maximum assignments for this work");
    return false;
  }
  if (requirements.restrict_placement_domain) {
    if (requirements.placement_domain.is_zero() ||
        agent_record.descriptor.placement_domain != requirements.placement_domain) {
      reject(evaluation, ScheduleOutcome::RejectLocality, "agent is outside the required placement domain");
      return false;
    }
    if (!requirements.topology_epoch.is_zero() &&
        agent_record.descriptor.topology_epoch != requirements.topology_epoch) {
      reject(evaluation, ScheduleOutcome::RejectLocality, "agent topology epoch is not the required epoch");
      return false;
    }
  }
  if (contains_id(requirements.anti_affinity, agent_record.descriptor.id)) {
    reject(evaluation, ScheduleOutcome::RejectAffinity, "agent is listed in anti-affinity");
    return false;
  }
  if (!requirements.affinity.empty() && !contains_id(requirements.affinity, agent_record.descriptor.id)) {
    reject(evaluation, ScheduleOutcome::RejectAffinity, "agent is not listed in affinity");
    return false;
  }
  if (requirements.require_warm_state) {
    if (!internal::load_fresh(impl, agent_record)) {
      reject(evaluation, ScheduleOutcome::RejectAffinity, "warm state evidence is not current");
      return false;
    }
    bool matched = false;
    for (const std::string& key : requirements.warm_state_keys) {
      if (std::find(agent_record.load_observation.warm_state_keys.begin(),
                    agent_record.load_observation.warm_state_keys.end(),
                    key) != agent_record.load_observation.warm_state_keys.end()) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      reject(evaluation, ScheduleOutcome::RejectAffinity, "agent reports no required warm state");
      return false;
    }
  }
  // Capability evidence. A missing, unknown, revoked, stale, or generation-mismatched
  // capability never satisfies a hard requirement.
  for (const CapabilityRequirement& requirement : request.requirements.capabilities) {
    if (requirement.mode != CapabilityRequirementMode::Required) {
      continue;
    }
    if (agent_record.profile.generation == AgentCapabilityGeneration{} ||
        agent_record.profile.boot != agent_record.descriptor.boot) {
      reject(evaluation, ScheduleOutcome::RejectStaleCapability,
             "agent has no capability profile for the current incarnation");
      return false;
    }
    if (!internal::capability_fresh(impl, agent_record)) {
      reject(evaluation, ScheduleOutcome::RejectStaleCapability, "capability evidence is not current");
      return false;
    }
    const CapabilityEvidence* evidence = find_capability(agent_record.profile, requirement.name);
    if (evidence == nullptr) {
      reject(evaluation, ScheduleOutcome::RejectCapability,
             std::string("required capability is unknown: ") + requirement.name);
      return false;
    }
    if (evidence->generation != agent_record.profile.generation ||
        evidence->boot != agent_record.descriptor.boot) {
      reject(evaluation, ScheduleOutcome::RejectStaleCapability,
             std::string("capability evidence generation is stale: ") + requirement.name);
      return false;
    }
    if (!capability_state_is_usable(evidence->state) ||
        capability_state_rank(evidence->state) < capability_state_rank(requirement.minimum_state)) {
      reject(evaluation, ScheduleOutcome::RejectCapability,
             std::string("capability evidence state is insufficient: ") + requirement.name);
      return false;
    }
    if (evidence->quality < requirement.minimum_quality) {
      reject(evaluation, ScheduleOutcome::RejectCapability,
             std::string("capability quality is insufficient: ") + requirement.name);
      return false;
    }
  }
  if (requirements.require_resource_feasibility &&
      feasibility.resource != FeasibilityVerdict::Feasible) {
    reject(evaluation, ScheduleOutcome::RejectResourceFeasibility,
           std::string("external resource feasibility is ") + to_string(feasibility.resource));
    return false;
  }
  if (requirements.require_budget_feasibility && feasibility.budget != FeasibilityVerdict::Feasible) {
    reject(evaluation, ScheduleOutcome::RejectBudget,
           std::string("external budget feasibility is ") + to_string(feasibility.budget));
    return false;
  }
  if (requirements.require_slo_feasibility && feasibility.slo != FeasibilityVerdict::Feasible) {
    reject(evaluation, ScheduleOutcome::RejectSlo,
           std::string("external slo feasibility is ") + to_string(feasibility.slo));
    return false;
  }
  if (requirements.require_reservation && feasibility.reservation != FeasibilityVerdict::Feasible) {
    reject(evaluation, ScheduleOutcome::RejectReservation,
           std::string("external reservation feasibility is ") + to_string(feasibility.reservation));
    return false;
  }
  evaluation.eligible = true;
  evaluation.rejection = ScheduleOutcome::NoChange;
  evaluation.rejection_detail.clear();
  return true;
}

[[nodiscard]] std::uint32_t weight_of(const PolicySnapshot& policy, std::string_view factor) noexcept {
  for (const RankingWeight& weight : policy.ranking_weights) {
    if (weight.factor == factor) {
      return weight.weight;
    }
  }
  return 0;
}

/// Deterministic named-factor ranking. Unknown factors contribute zero.
void score_candidate(const Impl& impl,
                     const WorkRecord& work_record,
                     const AgentRecord& agent_record,
                     const ExternalFeasibilityResult& feasibility,
                     CandidateEvaluation& evaluation) {
  const WorkRequest& request = work_record.request;
  const PolicySnapshot& policy = impl.policy;
  const std::uint64_t now = impl.now_ms();
  evaluation.factors.reserve(policy.ranking_weights.size());

  const std::uint32_t max_concurrency = agent_record.descriptor.max_concurrency;
  if (max_concurrency > 0) {
    add_factor(evaluation, std::string(ranking_factor::load_headroom),
               weight_of(policy, ranking_factor::load_headroom),
               ratio(max_concurrency - std::min(max_concurrency, agent_record.active_assignments),
                     max_concurrency),
               "canonical capacity minus active assignments");
    add_factor(evaluation, std::string(ranking_factor::remaining_capacity),
               weight_of(policy, ranking_factor::remaining_capacity), evaluation.available_capacity,
               "absolute remaining assignment slots");
  } else {
    add_factor(evaluation, std::string(ranking_factor::load_headroom),
               weight_of(policy, ranking_factor::load_headroom), -1, "no declared concurrency");
    add_factor(evaluation, std::string(ranking_factor::remaining_capacity),
               weight_of(policy, ranking_factor::remaining_capacity), -1, "no declared concurrency");
  }

  std::int64_t capability_quality = -1;
  std::uint32_t required_count = 0;
  for (const CapabilityRequirement& requirement : request.requirements.capabilities) {
    if (requirement.mode != CapabilityRequirementMode::Required) {
      continue;
    }
    ++required_count;
    const CapabilityEvidence* evidence = find_capability(agent_record.profile, requirement.name);
    if (evidence == nullptr) {
      capability_quality = -1;
      break;
    }
    const std::int64_t quality = static_cast<std::int64_t>(evidence->quality);
    capability_quality = capability_quality < 0 ? quality : std::min(capability_quality, quality);
  }
  if (required_count == 0) {
    capability_quality = 1000;
  }
  add_factor(evaluation, std::string(ranking_factor::capability_quality),
             weight_of(policy, ranking_factor::capability_quality), capability_quality,
             "minimum reported quality across required capabilities");

  std::uint32_t preferred_total = 0;
  std::uint32_t preferred_satisfied = 0;
  for (const CapabilityRequirement& requirement : request.requirements.capabilities) {
    if (requirement.mode != CapabilityRequirementMode::Preferred) {
      continue;
    }
    ++preferred_total;
    const CapabilityEvidence* evidence = find_capability(agent_record.profile, requirement.name);
    if (evidence != nullptr && capability_state_is_usable(evidence->state) &&
        capability_state_rank(evidence->state) >= capability_state_rank(requirement.minimum_state) &&
        evidence->quality >= requirement.minimum_quality) {
      ++preferred_satisfied;
    }
  }
  add_factor(evaluation, std::string(ranking_factor::capability_completeness),
             weight_of(policy, ranking_factor::capability_completeness),
             preferred_total == 0 ? 1000 : ratio(preferred_satisfied, preferred_total),
             "fraction of preferred capabilities satisfied");

  if (request.requirements.warm_state_keys.empty()) {
    add_factor(evaluation, std::string(ranking_factor::warm_state_affinity),
               weight_of(policy, ranking_factor::warm_state_affinity), -1, "no warm state requested");
    add_factor(evaluation, std::string(ranking_factor::handoff_cost),
               weight_of(policy, ranking_factor::handoff_cost), -1, "no warm state requested");
  } else {
    std::uint32_t matched = 0;
    for (const std::string& key : request.requirements.warm_state_keys) {
      if (std::find(agent_record.load_observation.warm_state_keys.begin(),
                    agent_record.load_observation.warm_state_keys.end(),
                    key) != agent_record.load_observation.warm_state_keys.end()) {
        ++matched;
      }
    }
    add_factor(evaluation, std::string(ranking_factor::warm_state_affinity),
               weight_of(policy, ranking_factor::warm_state_affinity),
               ratio(matched, static_cast<std::uint32_t>(request.requirements.warm_state_keys.size())),
               "warm state keys matched");
    add_factor(evaluation, std::string(ranking_factor::handoff_cost),
               weight_of(policy, ranking_factor::handoff_cost), matched > 0 ? 1000 : 0,
               "handoff cost implied by warm state reuse");
  }

  if (request.requirements.placement_domain.is_zero()) {
    add_factor(evaluation, std::string(ranking_factor::locality_match),
               weight_of(policy, ranking_factor::locality_match), -1, "no placement domain requested");
  } else {
    add_factor(evaluation, std::string(ranking_factor::locality_match),
               weight_of(policy, ranking_factor::locality_match),
               agent_record.descriptor.placement_domain == request.requirements.placement_domain ? 1000 : 0,
               "placement domain comparison");
  }

  add_factor(evaluation, std::string(ranking_factor::health_quality),
             weight_of(policy, ranking_factor::health_quality),
             internal::health_fresh(impl, agent_record)
                 ? static_cast<std::int64_t>(agent_record.health_observation.health_quality)
                 : -1,
             "reported health quality");
  add_factor(evaluation, std::string(ranking_factor::readiness), weight_of(policy, ranking_factor::readiness),
             internal::availability_fresh(impl, agent_record)
                 ? (agent_record.readiness == AgentReadiness::Ready ? 1000 : 0)
                 : -1,
             "reported readiness");

  const std::uint64_t age_ms = now > work_record.admitted_at_ms ? now - work_record.admitted_at_ms : 0;
  add_factor(evaluation, std::string(ranking_factor::queue_age),
             weight_of(policy, ranking_factor::queue_age),
             ratio(std::min<std::uint64_t>(age_ms, static_cast<std::uint64_t>(kQueueAgeCeilingMs)),
                   static_cast<std::uint64_t>(kQueueAgeCeilingMs)),
             "time since admission");

  const std::uint32_t ceiling = bypass_ceiling(policy, request.fairness_class);
  add_factor(evaluation, std::string(ranking_factor::fairness_deficit),
             weight_of(policy, ranking_factor::fairness_deficit),
             ceiling == 0 ? 0 : ratio(work_record.bypass_count, ceiling), "bypass pressure for this work");

  if (internal::load_fresh(impl, agent_record)) {
    const std::uint64_t latency = agent_record.load_observation.estimated_dispatch_latency_ms;
    add_factor(evaluation, std::string(ranking_factor::dispatch_latency),
               weight_of(policy, ranking_factor::dispatch_latency),
               1000 - ratio(std::min<std::uint64_t>(latency, static_cast<std::uint64_t>(kLatencyCeilingMs)),
                            static_cast<std::uint64_t>(kLatencyCeilingMs)),
               "reported dispatch latency");
    const double cost = agent_record.load_observation.cost_index;
    const std::int64_t cost_score =
        cost >= 0.0 ? static_cast<std::int64_t>(1000.0 / (1.0 + cost)) : -1;
    add_factor(evaluation, std::string(ranking_factor::cost_evidence),
               weight_of(policy, ranking_factor::cost_evidence), cost_score, "reported cost index");
    add_factor(evaluation, std::string(ranking_factor::slo_headroom),
               weight_of(policy, ranking_factor::slo_headroom),
               static_cast<std::int64_t>(agent_record.load_observation.slo_headroom_fraction * 1000.0),
               "reported SLO headroom");
  } else {
    add_factor(evaluation, std::string(ranking_factor::dispatch_latency),
               weight_of(policy, ranking_factor::dispatch_latency), -1, "no current load evidence");
    add_factor(evaluation, std::string(ranking_factor::cost_evidence),
               weight_of(policy, ranking_factor::cost_evidence), -1, "no current load evidence");
    add_factor(evaluation, std::string(ranking_factor::slo_headroom),
               weight_of(policy, ranking_factor::slo_headroom), -1, "no current load evidence");
  }

  add_factor(evaluation, std::string(ranking_factor::resource_headroom),
             weight_of(policy, ranking_factor::resource_headroom),
             feasibility.resource == FeasibilityVerdict::Feasible ? 1000 : -1,
             "external resource feasibility evidence");

  const std::uint64_t attempts = agent_record.assignments_completed + agent_record.assignments_failed;
  add_factor(evaluation, std::string(ranking_factor::reliability_history),
             weight_of(policy, ranking_factor::reliability_history),
             attempts == 0 ? -1 : ratio(agent_record.assignments_completed, attempts),
             "completed versus failed assignments");

  const std::uint32_t window = policy.anti_concentration_window;
  add_factor(evaluation, std::string(ranking_factor::anti_concentration),
             weight_of(policy, ranking_factor::anti_concentration),
             window == 0 ? -1
                         : 1000 - ratio(std::min(agent_record.recent_assignments, window), window),
             "recent assignment concentration");

  std::int64_t diversity = -1;
  if (!agent_record.descriptor.placement_domain.is_zero()) {
    std::uint64_t domain_total = 0;
    std::uint64_t domain_here = 0;
    for (const auto& entry : impl.assignments) {
      if (!assignment_state_is_active(entry.second.state)) {
        continue;
      }
      ++domain_total;
      if (entry.second.binding.placement_domain == agent_record.descriptor.placement_domain) {
        ++domain_here;
      }
    }
    if (domain_total > 0) {
      diversity = 1000 - ratio(domain_here, domain_total);
    }
  }
  add_factor(evaluation, std::string(ranking_factor::failure_domain_diversity),
             weight_of(policy, ranking_factor::failure_domain_diversity), diversity,
             "share of active assignments in this placement domain");

  std::int64_t weighted = 0;
  std::int64_t total_weight = 0;
  evaluation.known_weight = 0;
  for (const FactorScore& factor : evaluation.factors) {
    total_weight += static_cast<std::int64_t>(factor.weight);
    if (factor.status == FactorStatus::Known) {
      evaluation.known_weight += factor.weight;
      weighted += static_cast<std::int64_t>(factor.weight) * factor.normalized_value;
    }
  }
  evaluation.total_weight = static_cast<std::uint32_t>(std::min<std::int64_t>(total_weight, 0xFFFFFFFF));
  evaluation.score = total_weight == 0 ? 0 : weighted / total_weight;
}

[[nodiscard]] bool candidate_better(const CandidateEvaluation& left, const CandidateEvaluation& right) noexcept {
  if (left.score != right.score) {
    return left.score > right.score;
  }
  if (left.known_weight != right.known_weight) {
    return left.known_weight > right.known_weight;
  }
  return left.agent < right.agent;
}

struct PlanResult {
  ScheduleDecision decision;
  std::optional<AssignmentBinding> binding;
};

[[nodiscard]] std::string rejection_key(ScheduleOutcome outcome) { return to_string(outcome); }

PlanResult plan_locked(Impl& impl, WorkRecord& work_record) {
  PlanResult plan;
  ScheduleDecision& decision = plan.decision;
  decision.work = work_record.request.id;
  decision.work_generation = work_record.request.generation;
  decision.scheduler_epoch = impl.scheduler_epoch;
  decision.coordinator_epoch = impl.coordinator_epoch;
  decision.policy_generation = impl.policy.generation;

  if (!impl.running || !impl.admission_open) {
    decision.outcome = ScheduleOutcome::ShuttingDown;
    decision.detail = "admission is closed";
    return plan;
  }
  if (work_record.lifecycle == WorkLifecycle::Cancelled) {
    decision.outcome = ScheduleOutcome::RejectCancelled;
    decision.detail = "work is cancelled";
    return plan;
  }
  if (work_record.lifecycle == WorkLifecycle::Superseded) {
    decision.outcome = ScheduleOutcome::RejectSuperseded;
    decision.detail = "work is superseded";
    return plan;
  }
  if (work_is_terminal(work_record.lifecycle)) {
    decision.outcome = ScheduleOutcome::NoChange;
    decision.detail = "work is terminal";
    return plan;
  }
  if (!work_accepts_assignment(work_record.lifecycle) && work_record.current_assignments > 0) {
    const bool fanout_open = work_record.request.mode == SchedulingMode::Parallel &&
                             work_record.current_assignments <
                                 work_record.request.requirements.max_parallel;
    if (!fanout_open) {
      decision.outcome = ScheduleOutcome::NoChange;
      decision.detail = "work already holds an authoritative assignment";
      return plan;
    }
  }
  const std::uint64_t now = impl.now_ms();
  if (work_record.request.requirements.deadline_ms != 0 &&
      now > work_record.request.requirements.deadline_ms) {
    decision.outcome = ScheduleOutcome::RejectDeadline;
    decision.detail = "work deadline has passed";
    return plan;
  }
  if (work_record.request.earliest_start_ms != 0 && now < work_record.request.earliest_start_ms) {
    decision.outcome = ScheduleOutcome::Deferred;
    decision.detail = "work is not yet eligible to start";
    return plan;
  }
  if (work_record.request.mode == SchedulingMode::Exclusive) {
    if (const auto existing = internal::active_exclusive_assignment(impl, work_record.request.id)) {
      decision.outcome = ScheduleOutcome::NoChange;
      decision.detail = "work already has an authoritative exclusive assignment";
      decision.assignment = std::nullopt;
      (void)existing;
      return plan;
    }
  } else if (work_record.current_assignments >= work_record.request.requirements.max_parallel) {
    decision.outcome = ScheduleOutcome::NoChange;
    decision.detail = "explicit parallel fan-out is saturated";
    return plan;
  }

  const ExternalFeasibilityResult feasibility = internal::evaluate_feasibility(impl, work_record);
  std::vector<CandidateEvaluation> candidates;
  candidates.reserve(impl.agents.size());
  for (const auto& entry : impl.agents) {
    CandidateEvaluation evaluation;
    (void)evaluate_eligibility(impl, work_record, entry.second, feasibility, evaluation);
    if (evaluation.eligible) {
      score_candidate(impl, work_record, entry.second, feasibility, evaluation);
    }
    candidates.push_back(std::move(evaluation));
  }
  decision.candidate_count = candidates.size();

  std::vector<std::size_t> eligible;
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    if (candidates[index].eligible) {
      eligible.push_back(index);
    }
  }
  decision.eligible_count = eligible.size();

  std::vector<std::pair<ScheduleOutcome, std::size_t>> histogram;
  for (const CandidateEvaluation& evaluation : candidates) {
    if (evaluation.eligible) {
      continue;
    }
    bool found = false;
    for (auto& bucket : histogram) {
      if (bucket.first == evaluation.rejection) {
        ++bucket.second;
        found = true;
        break;
      }
    }
    if (!found) {
      histogram.emplace_back(evaluation.rejection, 1);
    }
  }
  std::sort(histogram.begin(), histogram.end(),
            [](const auto& left, const auto& right) { return left.first < right.first; });
  for (const auto& bucket : histogram) {
    decision.rejection_summary.push_back(rejection_key(bucket.first) + "=" + std::to_string(bucket.second));
  }

  std::sort(eligible.begin(), eligible.end(), [&candidates](std::size_t left, std::size_t right) {
    return candidate_better(candidates[left], candidates[right]);
  });
  decision.ranking_order = eligible;
  for (std::size_t rank = 0; rank < eligible.size(); ++rank) {
    candidates[eligible[rank]].rank = static_cast<std::uint32_t>(rank + 1);
  }
  decision.candidates = std::move(candidates);

  if (eligible.empty()) {
    decision.outcome = ScheduleOutcome::NoEligibleAgent;
    decision.detail = "no hard-eligible agent exists for this work item";
    return plan;
  }

  const std::size_t winner_index = eligible.front();
  CandidateEvaluation& winner = decision.candidates[winner_index];
  winner.selected = true;
  decision.selected_agent = winner.agent;
  decision.selected_agent_generation = winner.agent_generation;
  decision.selected_boot = winner.boot;
  if (eligible.size() > 1) {
    const CandidateEvaluation& runner_up = decision.candidates[eligible[1]];
    if (winner.score == runner_up.score && winner.known_weight == runner_up.known_weight) {
      winner.tie_break_decided = true;
      winner.tie_break_reason = "equal score and evidence coverage; lowest agent id wins";
      decision.tie_break_reason = winner.tie_break_reason;
    } else {
      decision.tie_break_reason = "score";
    }
  } else {
    decision.tie_break_reason = "sole eligible candidate";
  }

  const auto agent_entry = impl.agents.find(winner.agent);
  if (agent_entry == impl.agents.end()) {
    decision.outcome = ScheduleOutcome::RejectStaleAgentGeneration;
    decision.detail = "selected agent disappeared during planning";
    return plan;
  }
  const AssignmentGeneration generation =
      AssignmentGeneration{work_record.last_assignment_generation.value() + 1};
  std::uint32_t replica_index = 0;
  if (work_record.request.mode == SchedulingMode::Parallel) {
    replica_index = work_record.current_assignments;
  }
  plan.binding = internal::build_binding(impl, work_record, agent_entry->second, generation, replica_index);
  decision.assignment = plan.binding;
  decision.outcome = ScheduleOutcome::Assigned;
  decision.detail = "hard-eligible winner under current authority";
  return plan;
}

/// Revalidates every authority the plan bound, then commits. Returns std::nullopt on
/// success, otherwise the specific rejection that prevented the commit.
[[nodiscard]] std::optional<ScheduleOutcome> commit_locked(Impl& impl,
                                                           WorkId work_id,
                                                           WorkGeneration work_generation,
                                                           const AssignmentBinding& binding,
                                                           std::string& detail) {
  if (!impl.running || !impl.admission_open) {
    detail = "admission closed before commit";
    return ScheduleOutcome::ShuttingDown;
  }
  if (binding.scheduler != impl.scheduler_id || binding.scheduler_epoch != impl.scheduler_epoch) {
    detail = "scheduler epoch moved before commit";
    return ScheduleOutcome::RejectStaleSchedulerEpoch;
  }
  if (binding.coordinator_epoch != impl.coordinator_epoch) {
    detail = "coordinator epoch moved before commit";
    return ScheduleOutcome::RejectStaleCoordinatorEpoch;
  }
  if (binding.policy != impl.policy.id || binding.policy_generation != impl.policy.generation) {
    detail = "policy generation moved before commit";
    return ScheduleOutcome::RejectStalePolicy;
  }
  const auto work_entry = impl.work.find(work_id);
  if (work_entry == impl.work.end()) {
    detail = "work disappeared before commit";
    return ScheduleOutcome::RejectInvalidRequest;
  }
  WorkRecord& work_record = work_entry->second;
  if (work_record.request.generation != work_generation || work_generation != binding.work_generation) {
    detail = "work generation moved before commit";
    return ScheduleOutcome::RejectStaleWorkGeneration;
  }
  if (work_record.lifecycle == WorkLifecycle::Cancelled) {
    detail = "work was cancelled before commit";
    return ScheduleOutcome::RejectCancelled;
  }
  if (work_record.lifecycle == WorkLifecycle::Superseded) {
    detail = "work was superseded before commit";
    return ScheduleOutcome::RejectSuperseded;
  }
  if (!work_accepts_assignment(work_record.lifecycle) && work_record.current_assignments > 0) {
    const bool fanout_open = work_record.request.mode == SchedulingMode::Parallel &&
                             work_record.current_assignments <
                                 work_record.request.requirements.max_parallel;
    if (!fanout_open) {
      detail = "work acquired an assignment before commit";
      return ScheduleOutcome::RejectConflict;
    }
  }
  if (work_record.request.requirements.deadline_ms != 0 &&
      impl.now_ms() > work_record.request.requirements.deadline_ms) {
    detail = "work deadline passed before commit";
    return ScheduleOutcome::RejectDeadline;
  }
  if (binding.exclusive) {
    if (internal::active_exclusive_assignment(impl, work_id)) {
      detail = "exclusive assignment already exists";
      return ScheduleOutcome::RejectConflict;
    }
  } else if (work_record.current_assignments >= work_record.request.requirements.max_parallel) {
    detail = "parallel fan-out is saturated";
    return ScheduleOutcome::RejectCapacity;
  }
  const auto agent_entry = impl.agents.find(binding.agent);
  if (agent_entry == impl.agents.end()) {
    detail = "agent disappeared before commit";
    return ScheduleOutcome::RejectStaleAgentGeneration;
  }
  AgentRecord& agent_record = agent_entry->second;
  if (agent_record.descriptor.generation != binding.agent_generation) {
    detail = "agent generation moved before commit";
    return ScheduleOutcome::RejectStaleAgentGeneration;
  }
  if (agent_record.descriptor.boot != binding.boot) {
    detail = "agent incarnation replaced before commit";
    return ScheduleOutcome::RejectStaleAgentBoot;
  }
  if (internal::boot_is_fenced(impl, binding.boot)) {
    detail = "agent incarnation fenced before commit";
    return ScheduleOutcome::RejectFenced;
  }
  if (!lifecycle_accepts_new_work(agent_record.lifecycle)) {
    detail = std::string("agent lifecycle is ") + to_string(agent_record.lifecycle);
    return agent_record.lifecycle == AgentLifecycle::Draining ? ScheduleOutcome::RejectDrain
                                                              : ScheduleOutcome::RejectLifecycle;
  }
  if (agent_record.profile.generation != binding.capability_generation ||
      agent_record.profile.profile_id != binding.capability_profile ||
      !internal::capability_fresh(impl, agent_record)) {
    detail = "capability generation moved before commit";
    return ScheduleOutcome::RejectStaleCapability;
  }
  if (agent_record.active_assignments >= agent_record.descriptor.max_concurrency) {
    detail = "agent reached capacity before commit";
    return ScheduleOutcome::RejectCapacity;
  }
  if (internal::active_assignments_on_agent(impl, work_id, binding.agent) >=
      work_record.request.requirements.max_assignments_per_agent) {
    detail = "agent already holds the maximum assignments for this work";
    return ScheduleOutcome::RejectCapacity;
  }
  const ExternalFeasibilityResult feasibility = internal::evaluate_feasibility(impl, work_record);
  const WorkRequirements& requirements = work_record.request.requirements;
  if (requirements.require_resource_feasibility && feasibility.resource != FeasibilityVerdict::Feasible) {
    detail = "external resource feasibility changed before commit";
    return ScheduleOutcome::RejectResourceFeasibility;
  }
  if (requirements.require_budget_feasibility && feasibility.budget != FeasibilityVerdict::Feasible) {
    detail = "external budget feasibility changed before commit";
    return ScheduleOutcome::RejectBudget;
  }
  if (requirements.require_slo_feasibility && feasibility.slo != FeasibilityVerdict::Feasible) {
    detail = "external slo feasibility changed before commit";
    return ScheduleOutcome::RejectSlo;
  }
  if (requirements.require_reservation && feasibility.reservation != FeasibilityVerdict::Feasible) {
    detail = "external reservation feasibility changed before commit";
    return ScheduleOutcome::RejectReservation;
  }

  AssignmentRecord record;
  record.binding = binding;
  record.state = AssignmentState::Assigned;
  record.lease_current = true;
  record.reason = "assigned";
  auto inserted = impl.assignments.emplace(binding.id, std::move(record));
  LeaseRecord lease_record;
  lease_record.lease.id = binding.lease;
  lease_record.lease.generation = binding.lease_generation;
  lease_record.lease.assignment = binding.id;
  lease_record.lease.assignment_generation = binding.generation;
  lease_record.lease.agent = binding.agent;
  lease_record.lease.boot = binding.boot;
  lease_record.lease.granted_at_ms = binding.created_at_ms;
  lease_record.lease.expires_at_ms = binding.lease_expires_at_ms;
  lease_record.lease.current = true;
  impl.leases.emplace(binding.lease, std::move(lease_record));
  work_record.last_assignment_generation = binding.generation;
  internal::index_assignment(impl, inserted.first->second);
  internal::refresh_work_accounting(impl, work_record);
  internal::refresh_agent_accounting(impl, agent_record);
  ++impl.assignments_created;
  if (const auto queue_entry = impl.queues.find(binding.queue); queue_entry != impl.queues.end()) {
    ++queue_entry->second.assigned_count;
  }
  if (const auto tenant_entry = impl.tenants.find(work_record.request.requirements.tenant);
      tenant_entry != impl.tenants.end()) {
    ++tenant_entry->second.active_assignments;
  }
  ++impl.state_revision;
  return std::nullopt;
}

[[nodiscard]] MutationResult assignment_error(ErrorCode code,
                                              std::string operation,
                                              std::string subject,
                                              std::string detail,
                                              ScheduleOutcome outcome) {
  return MutationResult::failure(
      make_error(code, std::move(operation), std::move(subject), std::move(detail), outcome));
}

[[nodiscard]] ErrorCode code_for(ScheduleOutcome outcome) noexcept {
  switch (outcome) {
    case ScheduleOutcome::RejectStaleSchedulerEpoch:
    case ScheduleOutcome::RejectStaleCoordinatorEpoch:
    case ScheduleOutcome::RejectStaleAgentBoot:
    case ScheduleOutcome::RejectStaleAgentGeneration:
    case ScheduleOutcome::RejectStaleWorkGeneration:
    case ScheduleOutcome::RejectStaleAssignment:
    case ScheduleOutcome::RejectStalePolicy:
    case ScheduleOutcome::RejectStaleCapability:
    case ScheduleOutcome::RejectStaleQueueGeneration:
      return ErrorCode::StaleGeneration;
    case ScheduleOutcome::RejectConflict:
      return ErrorCode::Conflict;
    case ScheduleOutcome::RejectCancelled:
      return ErrorCode::Cancelled;
    case ScheduleOutcome::RejectSuperseded:
      return ErrorCode::Superseded;
    case ScheduleOutcome::RejectFenced:
      return ErrorCode::Fenced;
    case ScheduleOutcome::RejectLeaseExpired:
      return ErrorCode::LeaseExpired;
    case ScheduleOutcome::ShuttingDown:
      return ErrorCode::ShuttingDown;
    case ScheduleOutcome::RejectDrain:
      return ErrorCode::DrainBarrier;
    case ScheduleOutcome::RejectCapacity:
      return ErrorCode::NoCapacity;
    case ScheduleOutcome::RejectHealth:
      return ErrorCode::Unhealthy;
    case ScheduleOutcome::RejectReachability:
      return ErrorCode::Unreachable;
    case ScheduleOutcome::RejectReadiness:
    case ScheduleOutcome::RejectLifecycle:
      return ErrorCode::NotReady;
    case ScheduleOutcome::RejectPolicy:
    case ScheduleOutcome::RejectTenant:
    case ScheduleOutcome::RejectAffinity:
    case ScheduleOutcome::RejectLocality:
      return ErrorCode::PolicyRejected;
    case ScheduleOutcome::RejectCapability:
      return ErrorCode::CapabilityRejected;
    case ScheduleOutcome::RejectResourceFeasibility:
    case ScheduleOutcome::RejectBudget:
    case ScheduleOutcome::RejectSlo:
    case ScheduleOutcome::RejectReservation:
      return ErrorCode::FeasibilityRejected;
    case ScheduleOutcome::RejectDeadline:
    case ScheduleOutcome::RejectExpired:
      return ErrorCode::DeadlineExpired;
    case ScheduleOutcome::RevalidationRequired:
      return ErrorCode::NotEligible;
    case ScheduleOutcome::RejectLimitExceeded:
      return ErrorCode::LimitExceeded;
    case ScheduleOutcome::RejectInvalidRequest:
    case ScheduleOutcome::RejectDuplicate:
      return ErrorCode::InvalidArgument;
    default:
      return ErrorCode::NotEligible;
  }
}

[[nodiscard]] InvalidationReason reason_for(ScheduleOutcome outcome) noexcept {
  switch (outcome) {
    case ScheduleOutcome::RejectCancelled: return InvalidationReason::WorkCancelled;
    case ScheduleOutcome::RejectSuperseded: return InvalidationReason::WorkSuperseded;
    case ScheduleOutcome::RejectStalePolicy: return InvalidationReason::PolicyChanged;
    case ScheduleOutcome::RejectStaleCapability: return InvalidationReason::CapabilityChanged;
    case ScheduleOutcome::RejectResourceFeasibility: return InvalidationReason::ResourceChanged;
    case ScheduleOutcome::RejectBudget: return InvalidationReason::BudgetChanged;
    case ScheduleOutcome::RejectSlo: return InvalidationReason::SloChanged;
    case ScheduleOutcome::RejectStaleCoordinatorEpoch:
    case ScheduleOutcome::RejectStaleSchedulerEpoch:
      return InvalidationReason::CoordinatorEpochChanged;
    case ScheduleOutcome::RejectStaleAgentBoot:
    case ScheduleOutcome::RejectFenced: return InvalidationReason::AgentFenced;
    case ScheduleOutcome::RejectLeaseExpired: return InvalidationReason::LeaseExpired;
    case ScheduleOutcome::RejectLifecycle: return InvalidationReason::AgentLost;
    case ScheduleOutcome::RejectDrain: return InvalidationReason::AgentDrained;
    case ScheduleOutcome::RejectDeadline:
    case ScheduleOutcome::RejectExpired: return InvalidationReason::Rejected;
    default: return InvalidationReason::Rejected;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// scheduling
// ---------------------------------------------------------------------------

ScheduleDecision AgentScheduler::schedule(WorkId work, WorkGeneration generation) {
  PlanResult plan;
  {
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    const auto found = impl_->work.find(work);
    if (found == impl_->work.end()) {
      plan.decision.outcome = ScheduleOutcome::RejectInvalidRequest;
      plan.decision.work = work;
      plan.decision.work_generation = generation;
      plan.decision.detail = "work is not known";
      return plan.decision;
    }
    if (found->second.request.generation != generation) {
      plan.decision.outcome = ScheduleOutcome::RejectStaleWorkGeneration;
      plan.decision.work = work;
      plan.decision.work_generation = generation;
      plan.decision.detail = "work generation moved";
      return plan.decision;
    }
    plan = plan_locked(*impl_, found->second);
  }
  if (!plan.binding.has_value()) {
    return plan.decision;
  }
  // The interlock runs without the authoritative state lock: a racing mutation may
  // interleave here, and the commit below revalidates every bound authority.
  impl_->interlock->at(InterlockPoint::BeforeScheduleCommit);
  {
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    std::string detail;
    const auto failure = commit_locked(*impl_, work, generation, *plan.binding, detail);
    if (failure.has_value()) {
      plan.decision.outcome = *failure;
      plan.decision.detail = std::move(detail);
      plan.decision.assignment = std::nullopt;
      plan.decision.selected_agent = AgentId{};
      plan.decision.selected_agent_generation = AgentGeneration{};
      plan.decision.selected_boot = AgentBootId{};
      plan.decision.tie_break_reason.clear();
      for (CandidateEvaluation& evaluation : plan.decision.candidates) {
        evaluation.selected = false;
      }
      return plan.decision;
    }
    const auto stored = impl_->assignments.find(plan.binding->id);
    if (stored != impl_->assignments.end()) {
      plan.decision.assignment = stored->second.binding;
    }
  }
  return plan.decision;
}

BatchDecision AgentScheduler::schedule_batch(const ScheduleRequest& request) {
  BatchDecision batch;
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!impl_->running || !impl_->admission_open) {
    batch.outcome = ScheduleOutcome::ShuttingDown;
    return batch;
  }
  std::vector<WorkId> candidates;
  std::uint32_t max_fanout = 1;
  for (const auto& entry : impl_->work) {
    const WorkRecord& record = entry.second;
    const bool admitted = work_accepts_assignment(record.lifecycle);
    const bool fanout_open = record.request.mode == SchedulingMode::Parallel &&
                             record.current_assignments > 0 &&
                             record.current_assignments < record.request.requirements.max_parallel;
    if (!admitted && !fanout_open) {
      continue;
    }
    if (!request.queues.empty() &&
        std::find(request.queues.begin(), request.queues.end(), record.request.queue) ==
            request.queues.end()) {
      continue;
    }
    if (!request.work.empty() &&
        std::find(request.work.begin(), request.work.end(), entry.first) == request.work.end()) {
      continue;
    }
    candidates.push_back(entry.first);
    max_fanout = std::max(max_fanout, record.request.requirements.max_parallel);
  }
  if (candidates.size() > impl_->limits.max_candidates_examined) {
    batch.outcome = ScheduleOutcome::RejectLimitExceeded;
    return batch;
  }

  std::size_t assigned = 0;
  const std::size_t pass_limit =
      std::max<std::size_t>(1, std::min<std::size_t>(max_fanout, impl_->limits.max_batch_assignments));
  for (std::size_t pass = 0; pass < pass_limit && assigned < request.max_assignments; ++pass) {
    std::vector<std::pair<internal::FairnessKey, WorkId>> ordered;
    ordered.reserve(candidates.size());
    for (const WorkId id : candidates) {
      const WorkRecord& record = impl_->work.at(id);
      if (pass > 0) {
        // Later passes only continue explicitly parallel fan-out that is still open.
        const bool fanout_open = record.request.mode == SchedulingMode::Parallel &&
                                 record.current_assignments > 0 &&
                                 record.current_assignments < record.request.requirements.max_parallel;
        if (!fanout_open) {
          continue;
        }
      }
      ordered.emplace_back(internal::fairness_key(*impl_, record), id);
    }
    if (ordered.empty()) {
      break;
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
      return internal::fairness_before(left.first, right.first);
    });
    bool progress = false;
    for (const auto& entry : ordered) {
      WorkRecord& record = impl_->work.at(entry.second);
      if (pass == 0) {
        ++batch.examined;
      }
      if (assigned >= request.max_assignments) {
        if (pass == 0) {
          if (record.bypass_count != 0xFFFFFFFFu) {
            ++record.bypass_count;
          }
          if (record.starvation_rounds != 0xFFFFFFFFu) {
            ++record.starvation_rounds;
          }
          ++batch.deferred;
        }
        continue;
      }
      PlanResult plan = plan_locked(*impl_, record);
      if (!plan.binding.has_value()) {
        if (pass == 0) {
          if (plan.decision.outcome == ScheduleOutcome::Deferred ||
              plan.decision.outcome == ScheduleOutcome::NoChange) {
            ++batch.deferred;
          } else {
            if (record.bypass_count != 0xFFFFFFFFu) {
              ++record.bypass_count;
            }
            if (record.starvation_rounds != 0xFFFFFFFFu) {
              ++record.starvation_rounds;
            }
            ++batch.rejected;
          }
          batch.decisions.push_back(std::move(plan.decision));
        }
        continue;
      }
      if (request.dry_run) {
        if (pass == 0) {
          batch.decisions.push_back(std::move(plan.decision));
          ++batch.deferred;
        }
        continue;
      }
      std::string detail;
      const auto failure = commit_locked(*impl_, record.request.id, record.request.generation,
                                         *plan.binding, detail);
      if (failure.has_value()) {
        plan.decision.outcome = *failure;
        plan.decision.detail = std::move(detail);
        plan.decision.assignment = std::nullopt;
        if (pass == 0) {
          ++batch.rejected;
        }
      } else {
        record.bypass_count = 0;
        record.starvation_rounds = 0;
        ++assigned;
        ++batch.assigned;
        progress = true;
      }
      if (pass == 0) {
        batch.decisions.push_back(std::move(plan.decision));
      }
    }
    if (!progress) {
      break;
    }
  }
  ++impl_->scheduling_rounds;
  batch.outcome = batch.assigned > 0 ? ScheduleOutcome::Assigned : ScheduleOutcome::Deferred;
  return batch;
}

DispatchResult AgentScheduler::dispatch(AssignmentId assignment, AssignmentGeneration generation) {
  DispatchResult result;
  AssignmentBinding handoff;
  {
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    const auto found = impl_->assignments.find(assignment);
    if (found == impl_->assignments.end()) {
      result.outcome = ScheduleOutcome::RejectStaleAssignment;
      result.detail = "assignment is not known";
      return result;
    }
    if (found->second.binding.generation != generation) {
      result.outcome = ScheduleOutcome::RejectStaleAssignment;
      result.detail = "assignment generation moved";
      return result;
    }
    if (!impl_->running || impl_->shutting_down) {
      result.outcome = ScheduleOutcome::ShuttingDown;
      result.detail = "scheduler is shutting down";
      return result;
    }
    if (internal::boot_is_fenced(*impl_, found->second.binding.boot)) {
      result.outcome = ScheduleOutcome::RejectStaleAgentBoot;
      result.detail = "assignment incarnation is permanently fenced";
      return result;
    }
    if (found->second.state == AssignmentState::Dispatched) {
      result.outcome = ScheduleOutcome::NoChange;
      result.detail = "assignment is already dispatched";
      return result;
    }
    if (found->second.state == AssignmentState::RevalidationRequired) {
      result.outcome = ScheduleOutcome::RevalidationRequired;
      result.detail = "assignment requires revalidation before dispatch";
      return result;
    }
    if (!assignment_state_is_dispatchable(found->second.state)) {
      result.outcome = ScheduleOutcome::RejectStaleAssignment;
      result.detail = std::string("assignment state is ") + to_string(found->second.state);
      return result;
    }
    if (!found->second.lease_current ||
        found->second.binding.lease_expires_at_ms < impl_->now_ms()) {
      result.outcome = ScheduleOutcome::RejectLeaseExpired;
      result.detail = "assignment lease is not current";
      return result;
    }
    std::string detail;
    if (const auto failure = internal::revalidate_authority(*impl_, found->second, detail)) {
      result.outcome = *failure;
      result.detail = std::move(detail);
      return result;
    }
    handoff = found->second.binding;
  }
  impl_->interlock->at(InterlockPoint::BeforeDispatchHandoff);
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->assignments.find(assignment);
  if (found == impl_->assignments.end() || found->second.binding.generation != generation) {
    result.outcome = ScheduleOutcome::RejectStaleAssignment;
    result.detail = "assignment disappeared before handoff";
    return result;
  }
  if (found->second.state != AssignmentState::Assigned) {
    result.outcome = found->second.state == AssignmentState::Dispatched ? ScheduleOutcome::NoChange
                                                                       : ScheduleOutcome::RejectStaleAssignment;
    result.detail = "assignment state changed before handoff";
    return result;
  }
  std::string detail;
  if (const auto failure = internal::revalidate_authority(*impl_, found->second, detail)) {
    result.outcome = *failure;
    result.detail = std::move(detail);
    return result;
  }
  const std::uint64_t now = impl_->now_ms();
  found->second.state = AssignmentState::Dispatched;
  found->second.dispatched_at_ms = now;
  found->second.binding.lease_expires_at_ms = now + impl_->policy.assignment_lease_ttl_ms;
  found->second.lease_current = true;
  if (const auto lease_entry = impl_->leases.find(found->second.binding.lease);
      lease_entry != impl_->leases.end()) {
    lease_entry->second.lease.expires_at_ms = found->second.binding.lease_expires_at_ms;
    lease_entry->second.lease.current = true;
  }
  ++impl_->state_revision;
  result.outcome = ScheduleOutcome::Assigned;
  result.handoff = found->second.binding;
  result.detail = "revalidated and handed off";
  return result;
}

// ---------------------------------------------------------------------------
// assignment observation
// ---------------------------------------------------------------------------

namespace {

struct AssignmentLookup {
  AssignmentRecord* record{nullptr};
  std::string problem;
  ScheduleOutcome outcome{ScheduleOutcome::NoChange};
};

AssignmentLookup lookup_assignment(Impl& impl,
                                   AssignmentId id,
                                   AssignmentGeneration generation,
                                   AgentBootId boot,
                                   std::string_view operation) {
  AssignmentLookup result;
  const auto found = impl.assignments.find(id);
  if (found == impl.assignments.end()) {
    result.problem = "assignment is not known";
    result.outcome = ScheduleOutcome::RejectStaleAssignment;
    return result;
  }
  AssignmentRecord& record = found->second;
  if (record.binding.generation != generation) {
    result.problem = "assignment generation moved";
    result.outcome = ScheduleOutcome::RejectStaleAssignment;
    return result;
  }
  if (!boot.is_zero() && record.binding.boot != boot) {
    result.problem = "observation names a different incarnation";
    result.outcome = ScheduleOutcome::RejectStaleAgentBoot;
    return result;
  }
  if (internal::boot_is_fenced(impl, record.binding.boot)) {
    result.problem = "incarnation is fenced";
    result.outcome = ScheduleOutcome::RejectStaleAgentBoot;
    return result;
  }
  const auto agent_entry = impl.agents.find(record.binding.agent);
  if (agent_entry == impl.agents.end() ||
      agent_entry->second.descriptor.boot != record.binding.boot) {
    result.problem = "incarnation is no longer current";
    result.outcome = ScheduleOutcome::RejectStaleAgentBoot;
    return result;
  }
  (void)operation;
  result.record = &record;
  return result;
}

}  // namespace

MutationResult AgentScheduler::acknowledge(AssignmentId assignment,
                                           AssignmentGeneration generation,
                                           DispatchId dispatch_id,
                                           DispatchGeneration dispatch_generation,
                                           AgentBootId boot) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const AssignmentLookup lookup =
      lookup_assignment(*impl_, assignment, generation, boot, "acknowledge");
  if (lookup.record == nullptr) {
    return assignment_error(code_for(lookup.outcome), "acknowledge", assignment.to_string(),
                            lookup.problem, lookup.outcome);
  }
  AssignmentRecord& record = *lookup.record;
  if (record.binding.dispatch != dispatch_id ||
      record.binding.dispatch_generation != dispatch_generation) {
    return assignment_error(ErrorCode::StaleGeneration, "acknowledge", assignment.to_string(),
                            "dispatch identity or generation does not match the assignment",
                            ScheduleOutcome::RejectStaleAssignment);
  }
  if (record.state == AssignmentState::Acknowledged || record.state == AssignmentState::Executing) {
    return MutationResult::success();
  }
  if (assignment_state_is_terminal(record.state)) {
    return assignment_error(ErrorCode::StaleGeneration, "acknowledge", assignment.to_string(),
                            std::string("assignment is terminal: ") + to_string(record.state),
                            ScheduleOutcome::RejectStaleAssignment);
  }
  if (record.state != AssignmentState::Dispatched) {
    return assignment_error(ErrorCode::Conflict, "acknowledge", assignment.to_string(),
                            std::string("assignment is not awaiting acknowledgement: ") +
                                to_string(record.state),
                            ScheduleOutcome::RejectConflict);
  }
  if (!record.lease_current || record.binding.lease_expires_at_ms < impl_->now_ms()) {
    internal::close_assignment(*impl_, record, AssignmentState::Expired,
                               InvalidationReason::LeaseExpired, "lease expired before acknowledgement");
    return assignment_error(ErrorCode::LeaseExpired, "acknowledge", assignment.to_string(),
                            "assignment lease expired before acknowledgement",
                            ScheduleOutcome::RejectLeaseExpired);
  }
  record.state = AssignmentState::Acknowledged;
  record.acknowledged_at_ms = impl_->now_ms();
  ++impl_->state_revision;
  return MutationResult::success();
}

MutationResult AgentScheduler::mark_executing(AssignmentId assignment,
                                              AssignmentGeneration generation,
                                              AgentBootId boot) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const AssignmentLookup lookup = lookup_assignment(*impl_, assignment, generation, boot, "mark_executing");
  if (lookup.record == nullptr) {
    return assignment_error(code_for(lookup.outcome), "mark_executing", assignment.to_string(),
                            lookup.problem, lookup.outcome);
  }
  AssignmentRecord& record = *lookup.record;
  if (assignment_state_is_terminal(record.state)) {
    return assignment_error(ErrorCode::StaleGeneration, "mark_executing", assignment.to_string(),
                            "assignment is terminal", ScheduleOutcome::RejectStaleAssignment);
  }
  if (record.state != AssignmentState::Acknowledged && record.state != AssignmentState::Dispatched &&
      record.state != AssignmentState::Executing) {
    return assignment_error(ErrorCode::Conflict, "mark_executing", assignment.to_string(),
                            "assignment has not been acknowledged",
                            ScheduleOutcome::RejectConflict);
  }
  if (!record.lease_current || record.binding.lease_expires_at_ms < impl_->now_ms()) {
    internal::close_assignment(*impl_, record, AssignmentState::Expired,
                               InvalidationReason::LeaseExpired, "lease expired before execution");
    return assignment_error(ErrorCode::LeaseExpired, "mark_executing", assignment.to_string(),
                            "assignment lease expired before execution",
                            ScheduleOutcome::RejectLeaseExpired);
  }
  record.state = AssignmentState::Executing;
  const auto work_entry = impl_->work.find(record.binding.work);
  if (work_entry != impl_->work.end()) {
    internal::refresh_work_accounting(*impl_, work_entry->second);
  }
  ++impl_->state_revision;
  return MutationResult::success();
}

MutationResult AgentScheduler::release(AssignmentId assignment,
                                       AssignmentGeneration generation,
                                       AgentBootId boot,
                                       std::string reason) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const AssignmentLookup lookup = lookup_assignment(*impl_, assignment, generation, boot, "release");
  if (lookup.record == nullptr) {
    return assignment_error(code_for(lookup.outcome), "release", assignment.to_string(), lookup.problem,
                            lookup.outcome);
  }
  AssignmentRecord& record = *lookup.record;
  if (assignment_state_is_terminal(record.state)) {
    return MutationResult::success();
  }
  internal::close_assignment(*impl_, record, AssignmentState::Released,
                             InvalidationReason::LeaseReleased,
                             reason.empty() ? std::string("released") : std::move(reason));
  const auto agent_entry = impl_->agents.find(record.binding.agent);
  if (agent_entry != impl_->agents.end()) {
    internal::refresh_agent_accounting(*impl_, agent_entry->second);
  }
  return MutationResult::success();
}

MutationResult AgentScheduler::complete_assignment(AssignmentId assignment,
                                                   AssignmentGeneration generation,
                                                   AgentBootId boot) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const AssignmentLookup lookup =
      lookup_assignment(*impl_, assignment, generation, boot, "complete_assignment");
  if (lookup.record == nullptr) {
    return assignment_error(code_for(lookup.outcome), "complete_assignment", assignment.to_string(),
                            lookup.problem, lookup.outcome);
  }
  AssignmentRecord& record = *lookup.record;
  if (assignment_state_is_terminal(record.state)) {
    return MutationResult::success();
  }
  internal::close_assignment(*impl_, record, AssignmentState::Completed,
                             InvalidationReason::WorkCompleted, "assignment completed");
  if (const auto agent_entry = impl_->agents.find(record.binding.agent);
      agent_entry != impl_->agents.end()) {
    ++agent_entry->second.assignments_completed;
    internal::refresh_agent_accounting(*impl_, agent_entry->second);
  }
  const auto work_entry = impl_->work.find(record.binding.work);
  if (work_entry != impl_->work.end() && work_entry->second.current_assignments == 0) {
    work_entry->second.lifecycle = WorkLifecycle::Completed;
    internal::sync_work_lifecycle_index(*impl_, record.binding.work, WorkLifecycle::Completed);
    work_entry->second.reason = "completed";
  }
  if (const auto queue_entry = impl_->queues.find(record.binding.queue);
      queue_entry != impl_->queues.end()) {
    ++queue_entry->second.completed_count;
  }
  if (work_entry != impl_->work.end()) {
    if (const auto tenant_entry = impl_->tenants.find(work_entry->second.request.requirements.tenant);
        tenant_entry != impl_->tenants.end() && tenant_entry->second.active_assignments > 0) {
      --tenant_entry->second.active_assignments;
    }
  }
  ++impl_->state_revision;
  return MutationResult::success();
}

MutationResult AgentScheduler::fail_assignment(AssignmentId assignment,
                                               AssignmentGeneration generation,
                                               AgentBootId boot,
                                               std::string reason) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const AssignmentLookup lookup =
      lookup_assignment(*impl_, assignment, generation, boot, "fail_assignment");
  if (lookup.record == nullptr) {
    return assignment_error(code_for(lookup.outcome), "fail_assignment", assignment.to_string(),
                            lookup.problem, lookup.outcome);
  }
  AssignmentRecord& record = *lookup.record;
  if (assignment_state_is_terminal(record.state)) {
    return MutationResult::success();
  }
  internal::close_assignment(*impl_, record, AssignmentState::Failed, InvalidationReason::WorkFailed,
                             reason.empty() ? std::string("failed") : std::move(reason));
  if (const auto agent_entry = impl_->agents.find(record.binding.agent);
      agent_entry != impl_->agents.end()) {
    ++agent_entry->second.assignments_failed;
    internal::refresh_agent_accounting(*impl_, agent_entry->second);
  }
  const auto work_entry = impl_->work.find(record.binding.work);
  if (work_entry != impl_->work.end() && work_entry->second.current_assignments == 0) {
    work_entry->second.lifecycle = WorkLifecycle::Failed;
    internal::sync_work_lifecycle_index(*impl_, record.binding.work, WorkLifecycle::Failed);
    work_entry->second.reason = "failed";
  }
  if (const auto queue_entry = impl_->queues.find(record.binding.queue);
      queue_entry != impl_->queues.end()) {
    ++queue_entry->second.cancelled_count;
  }
  if (work_entry != impl_->work.end()) {
    if (const auto tenant_entry = impl_->tenants.find(work_entry->second.request.requirements.tenant);
        tenant_entry != impl_->tenants.end() && tenant_entry->second.active_assignments > 0) {
      --tenant_entry->second.active_assignments;
    }
  }
  ++impl_->state_revision;
  return MutationResult::success();
}

MutationResult AgentScheduler::reject_assignment(AssignmentId assignment,
                                                 AssignmentGeneration generation,
                                                 AgentBootId boot,
                                                 std::string reason) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const AssignmentLookup lookup =
      lookup_assignment(*impl_, assignment, generation, boot, "reject_assignment");
  if (lookup.record == nullptr) {
    return assignment_error(code_for(lookup.outcome), "reject_assignment", assignment.to_string(),
                            lookup.problem, lookup.outcome);
  }
  AssignmentRecord& record = *lookup.record;
  if (assignment_state_is_terminal(record.state)) {
    return MutationResult::success();
  }
  internal::close_assignment(*impl_, record, AssignmentState::Invalidated, InvalidationReason::Rejected,
                             reason.empty() ? std::string("rejected by agent") : std::move(reason));
  if (const auto queue_entry = impl_->queues.find(record.binding.queue);
      queue_entry != impl_->queues.end()) {
    ++queue_entry->second.cancelled_count;
  }
  const auto work_entry = impl_->work.find(record.binding.work);
  if (work_entry != impl_->work.end()) {
    if (const auto tenant_entry = impl_->tenants.find(work_entry->second.request.requirements.tenant);
        tenant_entry != impl_->tenants.end() && tenant_entry->second.active_assignments > 0) {
      --tenant_entry->second.active_assignments;
    }
  }
  ++impl_->state_revision;
  return MutationResult::success();
}

// ---------------------------------------------------------------------------
// revalidation and lease expiry
// ---------------------------------------------------------------------------

RevalidationReport AgentScheduler::revalidate_assignments() {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  RevalidationReport report;
  for (auto& entry : impl_->assignments) {
    AssignmentRecord& record = entry.second;
    if (record.state != AssignmentState::RevalidationRequired) {
      continue;
    }
    ++report.examined;
    std::string detail;
    if (const auto failure = internal::revalidate_authority(*impl_, record, detail)) {
      internal::close_assignment(*impl_, record, AssignmentState::Invalidated, reason_for(*failure),
                                 std::string(to_string(*failure)) + ": " + detail);
      ++report.invalidated;
      report.details.push_back(record.binding.id.to_string() + " " + to_string(*failure) + " " + detail);
      continue;
    }
    const auto agent_entry = impl_->agents.find(record.binding.agent);
    if (agent_entry == impl_->agents.end() ||
        !lifecycle_accepts_new_work(agent_entry->second.lifecycle)) {
      internal::close_assignment(*impl_, record, AssignmentState::Invalidated,
                                 InvalidationReason::AgentLost,
                                 "incarnation did not revalidate to an accepting lifecycle");
      ++report.invalidated;
      report.details.push_back(record.binding.id.to_string() + " REVALIDATION_REQUIRED lifecycle");
      continue;
    }
    if (agent_entry->second.active_assignments >= agent_entry->second.descriptor.max_concurrency) {
      internal::close_assignment(*impl_, record, AssignmentState::Invalidated,
                                 InvalidationReason::Rejected,
                                 "incarnation is at capacity during revalidation");
      ++report.invalidated;
      continue;
    }
    record.state = AssignmentState::Assigned;
    record.lease_current = true;
    record.binding.lease_expires_at_ms = impl_->now_ms() + impl_->policy.assignment_lease_ttl_ms;
    if (const auto lease_entry = impl_->leases.find(record.binding.lease);
        lease_entry != impl_->leases.end()) {
      lease_entry->second.lease.expires_at_ms = record.binding.lease_expires_at_ms;
      lease_entry->second.lease.current = true;
    }
    internal::index_assignment(*impl_, record);
    if (const auto work_entry = impl_->work.find(record.binding.work);
        work_entry != impl_->work.end()) {
      internal::refresh_work_accounting(*impl_, work_entry->second);
    }
    internal::refresh_agent_accounting(*impl_, agent_entry->second);
    ++report.promoted;
    report.details.push_back(record.binding.id.to_string() + " PROMOTED");
  }
  if (report.examined > 0) {
    ++impl_->state_revision;
  }
  return report;
}

RevalidationReport AgentScheduler::revalidate_agent_assignments(AgentId agent, AgentBootId boot) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  RevalidationReport report;
  const auto agent_entry = impl_->agents.find(agent);
  if (agent_entry == impl_->agents.end() || agent_entry->second.descriptor.boot != boot) {
    return report;
  }
  for (auto& entry : impl_->assignments) {
    AssignmentRecord& record = entry.second;
    if (record.binding.agent != agent || record.binding.boot != boot) {
      continue;
    }
    if (record.state != AssignmentState::RevalidationRequired) {
      continue;
    }
    ++report.examined;
    std::string detail;
    if (const auto failure = internal::revalidate_authority(*impl_, record, detail)) {
      internal::close_assignment(*impl_, record, AssignmentState::Invalidated, reason_for(*failure),
                                 std::string(to_string(*failure)) + ": " + detail);
      ++report.invalidated;
      report.details.push_back(record.binding.id.to_string() + " " + to_string(*failure));
      continue;
    }
    record.state = AssignmentState::Assigned;
    record.lease_current = true;
    record.binding.lease_expires_at_ms = impl_->now_ms() + impl_->policy.assignment_lease_ttl_ms;
    if (const auto lease_entry = impl_->leases.find(record.binding.lease);
        lease_entry != impl_->leases.end()) {
      lease_entry->second.lease.expires_at_ms = record.binding.lease_expires_at_ms;
      lease_entry->second.lease.current = true;
    }
    internal::index_assignment(*impl_, record);
    if (const auto work_entry = impl_->work.find(record.binding.work); work_entry != impl_->work.end()) {
      internal::refresh_work_accounting(*impl_, work_entry->second);
    }
    internal::refresh_agent_accounting(*impl_, agent_entry->second);
    ++report.promoted;
    report.details.push_back(record.binding.id.to_string() + " PROMOTED");
  }
  if (report.examined > 0) {
    ++impl_->state_revision;
  }
  return report;
}

MutationResult AgentScheduler::expire_leases() {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const std::uint64_t now = impl_->now_ms();
  bool changed = false;
  for (auto& entry : impl_->assignments) {
    AssignmentRecord& record = entry.second;
    if (!assignment_state_is_active(record.state)) {
      continue;
    }
    if (record.binding.lease_expires_at_ms >= now) {
      continue;
    }
    internal::close_assignment(*impl_, record, AssignmentState::Expired,
                               InvalidationReason::LeaseExpired, "assignment lease expired");
    changed = true;
  }
  (void)changed;
  return MutationResult::success();
}

FairnessSnapshot AgentScheduler::fairness() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  FairnessSnapshot snapshot;
  snapshot.aging_rounds_per_step = impl_->policy.aging_rounds_per_step;
  snapshot.max_priority_bypass = impl_->policy.max_priority_bypass;
  snapshot.bounded_starvation = !impl_->policy.allow_unbounded_starvation;
  snapshot.scheduling_rounds = impl_->scheduling_rounds;
  for (const auto& entry : impl_->work) {
    const WorkRecord& record = entry.second;
    if (work_is_terminal(record.lifecycle)) {
      continue;
    }
    const internal::FairnessKey key = internal::fairness_key(*impl_, record);
    FairnessEntry fairness_entry;
    fairness_entry.work = entry.first;
    fairness_entry.queue = record.request.queue;
    fairness_entry.tenant = record.request.requirements.tenant;
    fairness_entry.fairness_class = record.request.fairness_class;
    fairness_entry.priority = record.request.priority;
    fairness_entry.effective_priority = key.effective_priority;
    fairness_entry.bypass_count = record.bypass_count;
    fairness_entry.bypass_ceiling = key.bypass_ceiling;
    fairness_entry.starvation_rounds = record.starvation_rounds;
    fairness_entry.admitted_sequence = record.admitted_sequence;
    fairness_entry.starved = record.starvation_rounds >= impl_->policy.starvation_alert_rounds;
    fairness_entry.must_run = key.must_run;
    if (fairness_entry.starved) {
      ++snapshot.starved_count;
    }
    if (fairness_entry.must_run) {
      ++snapshot.must_run_count;
    }
    snapshot.max_bypass_observed = std::max(snapshot.max_bypass_observed, record.bypass_count);
    snapshot.entries.push_back(std::move(fairness_entry));
  }
  return snapshot;
}

}  // namespace agent_scheduler
