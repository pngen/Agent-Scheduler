// Agent Scheduler — schedulable work model.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/work.hpp"

#include <algorithm>
#include <utility>

#include "internal/util.hpp"

namespace agent_scheduler {

const char* to_string(WorkLifecycle value) noexcept {
  switch (value) {
    case WorkLifecycle::Unknown: return "UNKNOWN";
    case WorkLifecycle::Admitted: return "ADMITTED";
    case WorkLifecycle::Assigned: return "ASSIGNED";
    case WorkLifecycle::Executing: return "EXECUTING";
    case WorkLifecycle::Completed: return "COMPLETED";
    case WorkLifecycle::Failed: return "FAILED";
    case WorkLifecycle::Cancelled: return "CANCELLED";
    case WorkLifecycle::Superseded: return "SUPERSEDED";
    case WorkLifecycle::Expired: return "EXPIRED";
  }
  return "UNKNOWN";
}

bool work_accepts_assignment(WorkLifecycle value) noexcept {
  return value == WorkLifecycle::Admitted;
}

bool work_is_terminal(WorkLifecycle value) noexcept {
  switch (value) {
    case WorkLifecycle::Completed:
    case WorkLifecycle::Failed:
    case WorkLifecycle::Cancelled:
    case WorkLifecycle::Superseded:
    case WorkLifecycle::Expired:
      return true;
    default:
      return false;
  }
}

const char* to_string(FeasibilityVerdict value) noexcept {
  switch (value) {
    case FeasibilityVerdict::Unknown: return "UNKNOWN";
    case FeasibilityVerdict::Feasible: return "FEASIBLE";
    case FeasibilityVerdict::Infeasible: return "INFEASIBLE";
  }
  return "UNKNOWN";
}

namespace {

MutationResult fail(ErrorCode code, std::string detail, ScheduleOutcome outcome, std::string subject = {}) {
  return MutationResult::failure(
      make_error(code, "submit_work", std::move(subject), std::move(detail), outcome));
}

}  // namespace

MutationResult validate(const WorkRequest& request, const ResourceLimits& limits) {
  if (request.id.is_zero()) {
    return fail(ErrorCode::InvalidArgument, "work id must be non-zero", ScheduleOutcome::RejectInvalidRequest);
  }
  if (request.generation.is_zero()) {
    return fail(ErrorCode::InvalidArgument, "work generation must be non-zero",
                ScheduleOutcome::RejectInvalidRequest, request.id.to_string());
  }
  if (request.queue.is_zero()) {
    return fail(ErrorCode::InvalidArgument, "queue id must be non-zero",
                ScheduleOutcome::RejectInvalidRequest, request.id.to_string());
  }
  if (request.queue_generation.is_zero()) {
    return fail(ErrorCode::InvalidArgument, "queue generation must be non-zero",
                ScheduleOutcome::RejectInvalidRequest, request.id.to_string());
  }
  if (request.priority > 1000) {
    return fail(ErrorCode::InvalidArgument, "priority must be 0..1000", ScheduleOutcome::RejectInvalidRequest,
                request.id.to_string());
  }
  if (request.mode == SchedulingMode::Parallel) {
    if (request.requirements.max_parallel < 2) {
      return fail(ErrorCode::InvalidArgument, "parallel work requires max_parallel >= 2",
                  ScheduleOutcome::RejectInvalidRequest, request.id.to_string());
    }
  } else if (request.requirements.max_parallel != 1) {
    return fail(ErrorCode::InvalidArgument, "exclusive work requires max_parallel == 1",
                ScheduleOutcome::RejectInvalidRequest, request.id.to_string());
  }
  if (request.requirements.max_parallel > limits.max_batch_assignments) {
    return fail(ErrorCode::LimitExceeded, "max_parallel exceeds max_batch_assignments",
                ScheduleOutcome::RejectLimitExceeded, request.id.to_string());
  }
  if (request.requirements.max_assignments_per_agent == 0 ||
      request.requirements.max_assignments_per_agent > request.requirements.max_parallel) {
    return fail(ErrorCode::InvalidArgument,
                "max_assignments_per_agent must be within 1..max_parallel",
                ScheduleOutcome::RejectInvalidRequest, request.id.to_string());
  }
  if (const auto problem = internal::identifier_problem(request.fairness_class, limits.max_identifier_size,
                                                        "fairness_class")) {
    return fail(ErrorCode::InvalidArgument, *problem, ScheduleOutcome::RejectInvalidRequest,
                request.id.to_string());
  }
  if (const auto problem = internal::text_problem(request.kind, limits.max_identifier_size, "kind")) {
    return fail(ErrorCode::InvalidArgument, *problem, ScheduleOutcome::RejectInvalidRequest,
                request.id.to_string());
  }
  if (const auto problem = internal::text_problem(request.payload_ref, limits.max_string_size, "payload_ref")) {
    return fail(ErrorCode::InvalidArgument, *problem, ScheduleOutcome::RejectInvalidRequest,
                request.id.to_string());
  }
  if (request.requirements.capabilities.size() > limits.max_capability_requirements) {
    return fail(ErrorCode::LimitExceeded, "capability requirement count exceeds max_capability_requirements",
                ScheduleOutcome::RejectLimitExceeded, request.id.to_string());
  }
  std::vector<std::string> names;
  names.reserve(request.requirements.capabilities.size());
  for (const CapabilityRequirement& requirement : request.requirements.capabilities) {
    if (const auto problem = internal::identifier_problem(requirement.name, limits.max_identifier_size,
                                                          "capability requirement name")) {
      return fail(ErrorCode::InvalidArgument, *problem, ScheduleOutcome::RejectInvalidRequest,
                  request.id.to_string());
    }
    if (requirement.minimum_quality > 1000) {
      return fail(ErrorCode::InvalidArgument, "minimum_quality must be 0..1000",
                  ScheduleOutcome::RejectInvalidRequest, request.id.to_string());
    }
    if (!capability_state_is_usable(requirement.minimum_state)) {
      return fail(ErrorCode::InvalidArgument,
                  "minimum_state must be a usable capability state",
                  ScheduleOutcome::RejectInvalidRequest, request.id.to_string());
    }
    names.push_back(requirement.name);
  }
  if (!internal::canonicalize_unique(names)) {
    return fail(ErrorCode::Duplicate, "duplicate capability requirement name",
                ScheduleOutcome::RejectDuplicate, request.id.to_string());
  }
  if (request.requirements.required_policy_labels.size() > limits.max_policy_labels) {
    return fail(ErrorCode::LimitExceeded, "policy label count exceeds max_policy_labels",
                ScheduleOutcome::RejectLimitExceeded, request.id.to_string());
  }
  for (const std::string& label : request.requirements.required_policy_labels) {
    if (const auto problem = internal::identifier_problem(label, limits.max_identifier_size, "policy label")) {
      return fail(ErrorCode::InvalidArgument, *problem, ScheduleOutcome::RejectInvalidRequest,
                  request.id.to_string());
    }
  }
  if (request.requirements.affinity.size() > limits.max_affinity_entries ||
      request.requirements.anti_affinity.size() > limits.max_affinity_entries) {
    return fail(ErrorCode::LimitExceeded, "affinity set exceeds max_affinity_entries",
                ScheduleOutcome::RejectLimitExceeded, request.id.to_string());
  }
  for (const AgentId agent : request.requirements.anti_affinity) {
    if (std::find(request.requirements.affinity.begin(), request.requirements.affinity.end(), agent) !=
        request.requirements.affinity.end()) {
      return fail(ErrorCode::Conflict, "agent appears in both affinity and anti-affinity",
                  ScheduleOutcome::RejectAffinity, request.id.to_string());
    }
  }
  if (request.requirements.warm_state_keys.size() > limits.max_warm_state_keys) {
    return fail(ErrorCode::LimitExceeded, "warm_state_keys exceeds max_warm_state_keys",
                ScheduleOutcome::RejectLimitExceeded, request.id.to_string());
  }
  for (const std::string& key : request.requirements.warm_state_keys) {
    if (const auto problem = internal::identifier_problem(key, limits.max_identifier_size, "warm state key")) {
      return fail(ErrorCode::InvalidArgument, *problem, ScheduleOutcome::RejectInvalidRequest,
                  request.id.to_string());
    }
  }
  return MutationResult::success();
}

void canonicalize(WorkRequest& request) {
  std::sort(request.requirements.capabilities.begin(), request.requirements.capabilities.end(),
            [](const CapabilityRequirement& left, const CapabilityRequirement& right) {
              if (left.name != right.name) {
                return left.name < right.name;
              }
              return static_cast<int>(left.mode) < static_cast<int>(right.mode);
            });
  internal::canonicalize(request.requirements.required_policy_labels);
  internal::canonicalize(request.requirements.warm_state_keys);
  std::sort(request.requirements.affinity.begin(), request.requirements.affinity.end());
  request.requirements.affinity.erase(
      std::unique(request.requirements.affinity.begin(), request.requirements.affinity.end()),
      request.requirements.affinity.end());
  std::sort(request.requirements.anti_affinity.begin(), request.requirements.anti_affinity.end());
  request.requirements.anti_affinity.erase(
      std::unique(request.requirements.anti_affinity.begin(), request.requirements.anti_affinity.end()),
      request.requirements.anti_affinity.end());
}

}  // namespace agent_scheduler
