// Agent Scheduler — immutable snapshots and summaries.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "agent_scheduler/agent.hpp"
#include "agent_scheduler/assignment.hpp"
#include "agent_scheduler/digest.hpp"
#include "agent_scheduler/explanation.hpp"
#include "agent_scheduler/ids.hpp"
#include "agent_scheduler/work.hpp"

namespace agent_scheduler {

class AgentScheduler;

/// Aggregate counters. Every count is derived from canonical state at the moment of the
/// call; none is an independently maintained authority.
struct SchedulerSummary {
  SchedulerId scheduler_id{};
  SchedulerEpoch scheduler_epoch{};
  CoordinatorEpoch coordinator_epoch{};
  PolicyId policy{};
  PolicyGeneration policy_generation{};
  bool running{false};
  bool admission_open{false};
  bool shutting_down{false};
  std::size_t agent_count{0};
  std::size_t agents_ready{0};
  std::size_t agents_busy{0};
  std::size_t agents_draining{0};
  std::size_t agents_lost{0};
  std::size_t agents_retired{0};
  std::size_t agents_revalidation_required{0};
  std::size_t queue_count{0};
  std::size_t work_admitted{0};
  std::size_t work_assigned{0};
  std::size_t work_executing{0};
  std::size_t work_completed{0};
  std::size_t work_cancelled{0};
  std::size_t work_superseded{0};
  std::size_t work_expired{0};
  std::size_t active_assignments{0};
  std::size_t dispatchable_assignments{0};
  std::size_t revalidation_required_assignments{0};
  std::size_t historical_assignments{0};
  std::size_t current_leases{0};
  std::size_t fenced_boots{0};
  std::uint64_t total_capacity{0};
  std::uint64_t used_capacity{0};
  std::uint64_t scheduling_rounds{0};
  std::uint64_t state_revision{0};
  std::uint64_t assignments_created{0};
  std::uint64_t assignments_invalidated{0};
  Digest256 state_digest{};
};

/// Why a snapshot is not necessarily authoritative for dispatch.
enum class SnapshotStatus : std::uint8_t {
  Current = 0,
  Stale = 1,
  Reconstructed = 2,
  RevalidationRequired = 3,
};

[[nodiscard]] const char* to_string(SnapshotStatus value) noexcept;

/// Immutable view of scheduler state, bound to the generations that were current when
/// it was taken. A snapshot never authorizes dispatch on its own.
struct SchedulerSnapshot {
  SnapshotStatus status{SnapshotStatus::Current};
  SchedulerId scheduler_id{};
  SchedulerEpoch scheduler_epoch{};
  CoordinatorEpoch coordinator_epoch{};
  PolicyId policy{};
  PolicyGeneration policy_generation{};
  QueueGeneration queue_generation{};
  AgentGeneration max_agent_generation{};
  WorkGeneration max_work_generation{};
  AssignmentGeneration max_assignment_generation{};
  Digest256 state_digest{};
  Digest256 agent_generation_digest{};
  std::uint64_t state_revision{0};
  std::uint64_t created_at_ms{0};
  SchedulerSummary summary;
  std::vector<AgentStatus> agents;
  std::vector<WorkStatus> work;
  std::vector<Assignment> assignments;
  std::vector<Lease> leases;
  std::vector<FencedBoot> fenced_boots;
  FairnessSnapshot fairness;

  /// Recomputes whether this snapshot still binds current authority.
  [[nodiscard]] SnapshotStatus evaluate(const AgentScheduler& scheduler) const;
  /// True when the snapshot's generation bindings still match the scheduler.
  [[nodiscard]] bool binds_current_generations(const AgentScheduler& scheduler) const;
};

}  // namespace agent_scheduler
