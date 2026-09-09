// Agent Scheduler — authoritative assignment and lease records.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "agent_scheduler/ids.hpp"

namespace agent_scheduler {

/// Assignment state. Active states occupy the work item's assignment slot; terminal
/// states never regain authority.
enum class AssignmentState : std::uint8_t {
  Unknown = 0,
  Assigned = 1,
  Dispatched = 2,
  Acknowledged = 3,
  Executing = 4,
  Releasing = 5,
  RevalidationRequired = 6,
  Completed = 7,
  Failed = 8,
  Released = 9,
  Invalidated = 10,
  Superseded = 11,
  Expired = 12,
  Lost = 13,
  Cancelled = 14,
};

[[nodiscard]] const char* to_string(AssignmentState value) noexcept;
/// True while the assignment occupies the work item's authoritative assignment slot.
[[nodiscard]] bool assignment_state_is_active(AssignmentState value) noexcept;
/// True when the assignment is terminal and can never be revived.
[[nodiscard]] bool assignment_state_is_terminal(AssignmentState value) noexcept;
/// True when pre-dispatch revalidation has completed successfully and handoff may occur.
[[nodiscard]] bool assignment_state_is_dispatchable(AssignmentState value) noexcept;

enum class InvalidationReason : std::uint8_t {
  None = 0,
  AgentLost = 1,
  AgentRetired = 2,
  AgentDrained = 3,
  AgentFenced = 4,
  LeaseExpired = 5,
  WorkCancelled = 6,
  WorkSuperseded = 7,
  PolicyChanged = 8,
  CapabilityChanged = 9,
  ResourceChanged = 10,
  BudgetChanged = 11,
  SloChanged = 12,
  SchedulerRestart = 13,
  CoordinatorEpochChanged = 14,
  ExclusiveConflict = 15,
  Replaced = 16,
  Shutdown = 17,
  Rejected = 18,
  AssignmentAcknowledgedElsewhere = 19,
  WorkCompleted = 20,
  WorkFailed = 21,
  LeaseReleased = 22,
};

[[nodiscard]] const char* to_string(InvalidationReason value) noexcept;

/// The complete authority a scheduler assignment binds. Dispatch is legal only while
/// every one of these still matches current authoritative state.
struct AssignmentBinding {
  AssignmentId id{};
  AssignmentGeneration generation{};
  WorkId work{};
  WorkGeneration work_generation{};
  AgentId agent{};
  AgentGeneration agent_generation{};
  AgentBootId boot{};
  SchedulerId scheduler{};
  SchedulerEpoch scheduler_epoch{};
  CoordinatorEpoch coordinator_epoch{};
  PolicyId policy{};
  PolicyGeneration policy_generation{};
  CapabilityProfileId capability_profile{};
  AgentCapabilityGeneration capability_generation{};
  ResourceGeneration resource_generation{};
  BudgetGeneration budget_generation{};
  SloGeneration slo_generation{};
  ReservationGeneration reservation_generation{};
  QueueId queue{};
  QueueGeneration queue_generation{};
  LeaseId lease{};
  LeaseGeneration lease_generation{};
  DispatchId dispatch{};
  DispatchGeneration dispatch_generation{};
  PlacementDomainId placement_domain{};
  TopologyEpoch topology_epoch{};
  std::uint64_t created_at_ms{0};
  std::uint64_t lease_expires_at_ms{0};
  /// Zero-based replica index within an explicitly parallel work item.
  std::uint32_t replica_index{0};
  /// True when the work item is exclusive: at most one active assignment may exist.
  bool exclusive{true};
};

/// Deterministically derives an assignment identity from its authority content.
/// Identical authoritative state therefore yields identical assignment identity.
[[nodiscard]] AssignmentId derive_assignment_id(const AssignmentBinding& binding) noexcept;
[[nodiscard]] LeaseId derive_lease_id(const AssignmentBinding& binding) noexcept;
[[nodiscard]] DispatchId derive_dispatch_id(const AssignmentBinding& binding) noexcept;

struct Assignment {
  AssignmentBinding binding;
  AssignmentState state{AssignmentState::Unknown};
  InvalidationReason invalidation{InvalidationReason::None};
  bool lease_current{false};
  std::uint64_t dispatched_at_ms{0};
  std::uint64_t acknowledged_at_ms{0};
  std::uint64_t closed_at_ms{0};
  std::string reason;
};

/// A bounded lease over an assignment. Lease expiry never implies process death; it
/// only removes dispatch authority until the assignment is revalidated.
struct Lease {
  LeaseId id{};
  LeaseGeneration generation{};
  AssignmentId assignment{};
  AssignmentGeneration assignment_generation{};
  AgentId agent{};
  AgentBootId boot{};
  std::uint64_t granted_at_ms{0};
  std::uint64_t expires_at_ms{0};
  bool current{false};
};

/// A permanently fenced incarnation. Fencing is per (AgentId, AgentBootId) and never
/// applies globally to other agents.
struct FencedBoot {
  AgentId agent{};
  AgentBootId boot{};
  AgentGeneration generation{};
  std::uint64_t fenced_at_ms{0};
  std::string reason;
};

}  // namespace agent_scheduler
