// Agent Scheduler — internal authoritative state and derived indexes.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "agent_scheduler/scheduler.hpp"

namespace agent_scheduler {

/// Durable + dynamic record for one persistent agent identity.
struct AgentRecord {
  AgentDescriptor descriptor;
  AgentLifecycle lifecycle{AgentLifecycle::Unregistered};
  AgentHealth health{AgentHealth::Unknown};
  AgentReadiness readiness{AgentReadiness::Unknown};
  AgentReachability reachability{AgentReachability::Unknown};
  AgentAvailability availability{AgentAvailability::Unknown};
  CapabilityProfile profile;
  AgentHealthGeneration health_generation{};
  AgentAvailabilityGeneration availability_generation{};
  AgentLoadGeneration load_generation{};
  HealthObservation health_observation;
  AvailabilityObservation availability_observation;
  LoadObservation load_observation;
  std::uint64_t lease_expires_at_ms{0};
  std::uint32_t active_assignments{0};
  std::uint32_t recent_assignments{0};
  std::uint64_t last_heartbeat_ms{0};
  std::uint64_t registered_at_ms{0};
  std::uint64_t assignments_completed{0};
  std::uint64_t assignments_failed{0};
  bool capability_current{false};
  bool health_current{false};
  bool availability_current{false};
  bool load_current{false};
  bool recovered{false};
  std::string reason;
};

struct WorkRecord {
  WorkRequest request;
  WorkLifecycle lifecycle{WorkLifecycle::Unknown};
  std::uint64_t admitted_sequence{0};
  std::uint64_t admitted_at_ms{0};
  std::uint32_t bypass_count{0};
  std::uint32_t starvation_rounds{0};
  std::uint32_t current_assignments{0};
  AssignmentGeneration last_assignment_generation{};
  std::string reason;
};

struct AssignmentRecord {
  AssignmentBinding binding;
  AssignmentState state{AssignmentState::Unknown};
  InvalidationReason invalidation{InvalidationReason::None};
  bool lease_current{false};
  std::uint64_t dispatched_at_ms{0};
  std::uint64_t acknowledged_at_ms{0};
  std::uint64_t closed_at_ms{0};
  std::string reason;
};

struct LeaseRecord {
  Lease lease;
};

struct QueueRecord {
  QueueId id{};
  QueueGeneration generation{};
  std::string fairness_class{"default"};
  std::uint32_t weight{1};
  std::uint64_t admitted_count{0};
  std::uint64_t assigned_count{0};
  std::uint64_t completed_count{0};
  std::uint64_t cancelled_count{0};
};

struct TenantRecord {
  TenantId id{};
  std::uint64_t admitted_count{0};
  std::uint64_t active_assignments{0};
};

struct AgentScheduler::Impl {
  explicit Impl(SchedulerOptions options);

  SchedulerOptions options;
  ResourceLimits limits;
  std::shared_ptr<Clock> clock;
  std::shared_ptr<ExternalFeasibilityProvider> feasibility;
  std::shared_ptr<ScheduleInterlock> interlock;
  std::function<void()> shutdown_notifier;

  mutable std::mutex mutex;

  // ---- canonical authoritative state ----
  SchedulerId scheduler_id{};
  SchedulerEpoch scheduler_epoch{1};
  CoordinatorEpoch coordinator_epoch{0};
  QueueGeneration queue_generation{};
  PolicySnapshot policy;
  std::map<AgentId, AgentRecord> agents;
  std::map<WorkId, WorkRecord> work;
  std::map<AssignmentId, AssignmentRecord> assignments;
  std::map<LeaseId, LeaseRecord> leases;
  std::map<AgentBootId, FencedBoot> fenced_boots;
  std::map<QueueId, QueueRecord> queues;
  std::map<TenantId, TenantRecord> tenants;
  std::uint64_t next_admission_sequence{1};
  std::uint64_t state_revision{0};
  std::uint64_t scheduling_rounds{0};
  std::uint64_t assignments_created{0};
  std::uint64_t assignments_invalidated{0};
  bool running{false};
  bool admission_open{false};
  bool shutting_down{false};
  std::uint64_t shutdown_at_ms{0};
  bool recovered{false};

  // ---- derived acceleration indexes (never authoritative) ----
  std::map<std::string, std::set<AgentId>> index_agents_by_capability;
  std::map<AgentLifecycle, std::set<AgentId>> index_agents_by_lifecycle;
  std::map<TenantId, std::set<AgentId>> index_agents_by_tenant;
  std::map<PlacementDomainId, std::set<AgentId>> index_agents_by_domain;
  std::map<QueueId, std::set<WorkId>> index_work_by_queue;
  std::map<WorkLifecycle, std::set<WorkId>> index_work_by_lifecycle;
  std::map<AgentId, std::set<AssignmentId>> index_assignments_by_agent;
  std::map<WorkId, std::set<AssignmentId>> index_assignments_by_work;

  [[nodiscard]] std::uint64_t now_ms() const noexcept { return clock->now_ms(); }
};

namespace internal {

using Impl = AgentScheduler::Impl;

/// True when the incarnation is permanently fenced. Lock must be held.
[[nodiscard]] bool boot_is_fenced(const Impl& impl, AgentBootId boot);

/// True when the record still represents the named incarnation and it is not fenced.
[[nodiscard]] bool incarnation_current(const Impl& impl, const AgentRecord& record, AgentBootId boot);

/// Permanently fences an incarnation. Fencing is scoped to (agent, boot).
void fence_boot(Impl& impl, AgentId agent, AgentBootId boot, AgentGeneration generation, std::string reason);

/// Index maintenance. Lock must be held.
void index_agent(Impl& impl, const AgentRecord& record);
void deindex_agent(Impl& impl, AgentId agent);
void index_work(Impl& impl, const WorkRecord& record);
void deindex_work(Impl& impl, WorkId work);
void index_assignment(Impl& impl, const AssignmentRecord& record);
void deindex_assignment(Impl& impl, const AssignmentRecord& record);
void rebuild_indexes(Impl& impl);

/// Recomputes lifecycle from current evidence. Lost, Retired, and Draining are sticky.
void recompute_lifecycle(AgentRecord& record);

/// Freshness of dynamic evidence against the current policy.
[[nodiscard]] bool capability_fresh(const Impl& impl, const AgentRecord& record);
[[nodiscard]] bool health_fresh(const Impl& impl, const AgentRecord& record);
[[nodiscard]] bool availability_fresh(const Impl& impl, const AgentRecord& record);
[[nodiscard]] bool load_fresh(const Impl& impl, const AgentRecord& record);
[[nodiscard]] bool lease_current(const Impl& impl, const AgentRecord& record);

/// Clears process-local dynamic evidence without touching durable identity.
void invalidate_dynamic_evidence(AgentRecord& record);

/// Number of active assignments currently bound to the agent.
[[nodiscard]] std::uint32_t active_assignment_count(const Impl& impl, AgentId agent);

/// Terminal-state transition of an assignment, releasing the work slot and capacity.
void close_assignment(Impl& impl,
                      AssignmentRecord& record,
                      AssignmentState state,
                      InvalidationReason reason,
                      std::string detail);

/// Keeps the lifecycle acceleration index in step with canonical state.
void sync_agent_lifecycle_index(Impl& impl, AgentId agent, AgentLifecycle lifecycle);
void sync_work_lifecycle_index(Impl& impl, WorkId work, WorkLifecycle lifecycle);

/// Recomputes derived accounting for one agent from canonical assignment state.
void refresh_agent_accounting(Impl& impl, AgentRecord& record);

/// Recomputes derived accounting for one work item from canonical assignment state.
void refresh_work_accounting(Impl& impl, WorkRecord& record);

/// Finds the single active exclusive assignment for a work item, if any.
[[nodiscard]] std::optional<AssignmentId> active_exclusive_assignment(const Impl& impl, WorkId work);

/// Counts active assignments of a work item on a specific agent.
[[nodiscard]] std::uint32_t active_assignments_on_agent(const Impl& impl, WorkId work, AgentId agent);

/// Builds the authority binding for a proposed assignment.
[[nodiscard]] AssignmentBinding build_binding(Impl& impl,
                                              const WorkRecord& work_record,
                                              const AgentRecord& agent_record,
                                              AssignmentGeneration generation,
                                              std::uint32_t replica_index);

/// Verifies every authority a dispatchable assignment binds. Returns std::nullopt when
/// the assignment is still legal, otherwise the specific rejection.
[[nodiscard]] std::optional<ScheduleOutcome> revalidate_authority(const Impl& impl,
                                                                  const AssignmentRecord& record,
                                                                  std::string& detail);

/// Canonical digest of durable + dynamic authoritative state.
[[nodiscard]] Digest256 compute_state_digest(const Impl& impl);
[[nodiscard]] Digest256 compute_agent_generation_digest(const Impl& impl);

/// Applies external feasibility evidence for a work item.
[[nodiscard]] ExternalFeasibilityResult evaluate_feasibility(const Impl& impl, const WorkRecord& record);

/// Fairness ordering key for one admitted work item.
struct FairnessKey {
  std::int64_t effective_priority{0};
  std::uint32_t bypass_count{0};
  std::uint32_t bypass_ceiling{0};
  bool must_run{false};
  std::uint64_t admitted_sequence{0};
  std::uint64_t admitted_at_ms{0};
};

[[nodiscard]] FairnessKey fairness_key(const Impl& impl, const WorkRecord& record);
[[nodiscard]] bool fairness_before(const FairnessKey& left, const FairnessKey& right) noexcept;

}  // namespace internal
}  // namespace agent_scheduler
