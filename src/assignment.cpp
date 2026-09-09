// Agent Scheduler — authoritative assignment and lease records.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/assignment.hpp"

#include "agent_scheduler/digest.hpp"

namespace agent_scheduler {
namespace {

/// Canonical binding encoding. Field tags keep structurally distinct bindings from
/// colliding by concatenation.
void hash_binding(Hasher256& hasher, const AssignmentBinding& binding) noexcept {
  hasher.update_field_tag(0x01);
  hasher.update(binding.generation.value());
  hasher.update_field_tag(0x02);
  hasher.update(binding.work.value());
  hasher.update(binding.work_generation.value());
  hasher.update_field_tag(0x03);
  hasher.update(binding.agent.value());
  hasher.update(binding.agent_generation.value());
  hasher.update(binding.boot.value().high);
  hasher.update(binding.boot.value().low);
  hasher.update_field_tag(0x04);
  hasher.update(binding.scheduler.value());
  hasher.update(binding.scheduler_epoch.value());
  hasher.update(binding.coordinator_epoch.value());
  hasher.update_field_tag(0x05);
  hasher.update(binding.policy.value());
  hasher.update(binding.policy_generation.value());
  hasher.update_field_tag(0x06);
  hasher.update(binding.capability_profile.value());
  hasher.update(binding.capability_generation.value());
  hasher.update_field_tag(0x07);
  hasher.update(binding.resource_generation.value());
  hasher.update(binding.budget_generation.value());
  hasher.update(binding.slo_generation.value());
  hasher.update(binding.reservation_generation.value());
  hasher.update_field_tag(0x08);
  hasher.update(binding.queue.value());
  hasher.update(binding.queue_generation.value());
  hasher.update_field_tag(0x09);
  hasher.update(binding.placement_domain.value());
  hasher.update(binding.topology_epoch.value());
  hasher.update_field_tag(0x0A);
  hasher.update(binding.replica_index);
  hasher.update(static_cast<std::uint8_t>(binding.exclusive ? 1 : 0));
}

}  // namespace

const char* to_string(AssignmentState value) noexcept {
  switch (value) {
    case AssignmentState::Unknown: return "UNKNOWN";
    case AssignmentState::Assigned: return "ASSIGNED";
    case AssignmentState::Dispatched: return "DISPATCHED";
    case AssignmentState::Acknowledged: return "ACKNOWLEDGED";
    case AssignmentState::Executing: return "EXECUTING";
    case AssignmentState::Releasing: return "RELEASING";
    case AssignmentState::RevalidationRequired: return "REVALIDATION_REQUIRED";
    case AssignmentState::Completed: return "COMPLETED";
    case AssignmentState::Failed: return "FAILED";
    case AssignmentState::Released: return "RELEASED";
    case AssignmentState::Invalidated: return "INVALIDATED";
    case AssignmentState::Superseded: return "SUPERSEDED";
    case AssignmentState::Expired: return "EXPIRED";
    case AssignmentState::Lost: return "LOST";
    case AssignmentState::Cancelled: return "CANCELLED";
  }
  return "UNKNOWN";
}

bool assignment_state_is_active(AssignmentState value) noexcept {
  switch (value) {
    case AssignmentState::Assigned:
    case AssignmentState::Dispatched:
    case AssignmentState::Acknowledged:
    case AssignmentState::Executing:
    case AssignmentState::Releasing:
    case AssignmentState::RevalidationRequired:
      return true;
    default:
      return false;
  }
}

bool assignment_state_is_terminal(AssignmentState value) noexcept {
  switch (value) {
    case AssignmentState::Completed:
    case AssignmentState::Failed:
    case AssignmentState::Released:
    case AssignmentState::Invalidated:
    case AssignmentState::Superseded:
    case AssignmentState::Expired:
    case AssignmentState::Lost:
    case AssignmentState::Cancelled:
      return true;
    default:
      return false;
  }
}

bool assignment_state_is_dispatchable(AssignmentState value) noexcept {
  return value == AssignmentState::Assigned;
}

const char* to_string(InvalidationReason value) noexcept {
  switch (value) {
    case InvalidationReason::None: return "NONE";
    case InvalidationReason::AgentLost: return "AGENT_LOST";
    case InvalidationReason::AgentRetired: return "AGENT_RETIRED";
    case InvalidationReason::AgentDrained: return "AGENT_DRAINED";
    case InvalidationReason::AgentFenced: return "AGENT_FENCED";
    case InvalidationReason::LeaseExpired: return "LEASE_EXPIRED";
    case InvalidationReason::WorkCancelled: return "WORK_CANCELLED";
    case InvalidationReason::WorkSuperseded: return "WORK_SUPERSEDED";
    case InvalidationReason::PolicyChanged: return "POLICY_CHANGED";
    case InvalidationReason::CapabilityChanged: return "CAPABILITY_CHANGED";
    case InvalidationReason::ResourceChanged: return "RESOURCE_CHANGED";
    case InvalidationReason::BudgetChanged: return "BUDGET_CHANGED";
    case InvalidationReason::SloChanged: return "SLO_CHANGED";
    case InvalidationReason::SchedulerRestart: return "SCHEDULER_RESTART";
    case InvalidationReason::CoordinatorEpochChanged: return "COORDINATOR_EPOCH_CHANGED";
    case InvalidationReason::ExclusiveConflict: return "EXCLUSIVE_CONFLICT";
    case InvalidationReason::Replaced: return "REPLACED";
    case InvalidationReason::Shutdown: return "SHUTDOWN";
    case InvalidationReason::Rejected: return "REJECTED";
    case InvalidationReason::AssignmentAcknowledgedElsewhere: return "ACKNOWLEDGED_ELSEWHERE";
    case InvalidationReason::WorkCompleted: return "WORK_COMPLETED";
    case InvalidationReason::WorkFailed: return "WORK_FAILED";
    case InvalidationReason::LeaseReleased: return "LEASE_RELEASED";
  }
  return "NONE";
}

AssignmentId derive_assignment_id(const AssignmentBinding& binding) noexcept {
  Hasher256 hasher;
  hash_binding(hasher, binding);
  return AssignmentId{hash128_of(hasher.final())};
}

LeaseId derive_lease_id(const AssignmentBinding& binding) noexcept {
  Hasher256 hasher;
  hasher.update_field_tag(0x11);
  hasher.update(binding.id.value().high);
  hasher.update(binding.id.value().low);
  hasher.update(binding.lease_generation.value());
  return LeaseId{hash128_of(hasher.final())};
}

DispatchId derive_dispatch_id(const AssignmentBinding& binding) noexcept {
  Hasher256 hasher;
  hasher.update_field_tag(0x12);
  hasher.update(binding.id.value().high);
  hasher.update(binding.id.value().low);
  hasher.update(binding.dispatch_generation.value());
  return DispatchId{hash128_of(hasher.final())};
}

}  // namespace agent_scheduler
