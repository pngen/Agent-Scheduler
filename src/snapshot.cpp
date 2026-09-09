// Agent Scheduler — snapshots, summaries, and inspection queries.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "agent_scheduler/scheduler.hpp"
#include "internal/scheduler_impl.hpp"

namespace agent_scheduler {
namespace {

using internal::Impl;

[[nodiscard]] AgentStatus make_agent_status(const Impl& impl, const AgentRecord& record) {
  AgentStatus status;
  status.id = record.descriptor.id;
  status.generation = record.descriptor.generation;
  status.boot = record.descriptor.boot;
  status.registration_generation = record.descriptor.registration_generation;
  status.lifecycle = record.lifecycle;
  status.health = record.health;
  status.readiness = record.readiness;
  status.reachability = record.reachability;
  status.availability = record.availability;
  status.health_generation = record.health_generation;
  status.availability_generation = record.availability_generation;
  status.load_generation = record.load_generation;
  status.capability_generation = record.profile.generation;
  status.tenant = record.descriptor.tenant;
  status.name_space = record.descriptor.name_space;
  status.display_name = record.descriptor.display_name;
  status.policy_labels = record.descriptor.policy_labels;
  status.placement_domain = record.descriptor.placement_domain;
  status.topology_epoch = record.descriptor.topology_epoch;
  status.max_concurrency = record.descriptor.max_concurrency;
  status.active_assignments = record.active_assignments;
  status.available_capacity =
      record.descriptor.max_concurrency > record.active_assignments
          ? record.descriptor.max_concurrency - record.active_assignments
          : 0;
  status.reported_active_assignments = record.load_observation.reported_active_assignments;
  status.lease_expires_at_ms = record.lease_expires_at_ms;
  status.lease_current = internal::lease_current(impl, record);
  status.evidence_current = internal::capability_fresh(impl, record) && internal::health_fresh(impl, record) &&
                            internal::availability_fresh(impl, record);
  status.last_heartbeat_ms = record.last_heartbeat_ms;
  status.registered_at_ms = record.registered_at_ms;
  status.capabilities = record.profile.capabilities;
  status.reason = record.reason;
  return status;
}

[[nodiscard]] WorkStatus make_work_status(const WorkRecord& record) {
  WorkStatus status;
  status.id = record.request.id;
  status.generation = record.request.generation;
  status.queue = record.request.queue;
  status.queue_generation = record.request.queue_generation;
  status.lifecycle = record.lifecycle;
  status.priority = record.request.priority;
  status.fairness_class = record.request.fairness_class;
  status.mode = record.request.mode;
  status.current_assignments = record.current_assignments;
  status.max_parallel = record.request.requirements.max_parallel;
  status.last_assignment_generation = record.last_assignment_generation;
  status.admitted_at_ms = record.admitted_at_ms;
  status.admitted_sequence = record.admitted_sequence;
  status.bypass_count = record.bypass_count;
  status.starvation_rounds = record.starvation_rounds;
  status.deadline_ms = record.request.requirements.deadline_ms;
  status.kind = record.request.kind;
  status.reason = record.reason;
  for (const CapabilityRequirement& requirement : record.request.requirements.capabilities) {
    if (requirement.mode == CapabilityRequirementMode::Required) {
      ++status.required_capabilities;
    } else {
      ++status.preferred_capabilities;
    }
  }
  return status;
}

}  // namespace

const char* to_string(SnapshotStatus value) noexcept {
  switch (value) {
    case SnapshotStatus::Current: return "CURRENT";
    case SnapshotStatus::Stale: return "STALE";
    case SnapshotStatus::Reconstructed: return "RECONSTRUCTED";
    case SnapshotStatus::RevalidationRequired: return "REVALIDATION_REQUIRED";
  }
  return "CURRENT";
}

SchedulerSummary AgentScheduler::summary() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  SchedulerSummary summary;
  summary.scheduler_id = impl_->scheduler_id;
  summary.scheduler_epoch = impl_->scheduler_epoch;
  summary.coordinator_epoch = impl_->coordinator_epoch;
  summary.policy = impl_->policy.id;
  summary.policy_generation = impl_->policy.generation;
  summary.running = impl_->running;
  summary.admission_open = impl_->admission_open;
  summary.shutting_down = impl_->shutting_down;
  summary.agent_count = impl_->agents.size();
  for (const auto& entry : impl_->agents) {
    const AgentRecord& record = entry.second;
    switch (record.lifecycle) {
      case AgentLifecycle::Ready: ++summary.agents_ready; break;
      case AgentLifecycle::Busy: ++summary.agents_busy; break;
      case AgentLifecycle::Draining: ++summary.agents_draining; break;
      case AgentLifecycle::Lost: ++summary.agents_lost; break;
      case AgentLifecycle::Retired: ++summary.agents_retired; break;
      case AgentLifecycle::RevalidationRequired: ++summary.agents_revalidation_required; break;
      default: break;
    }
    summary.total_capacity += record.descriptor.max_concurrency;
    summary.used_capacity += record.active_assignments;
  }
  summary.queue_count = impl_->queues.size();
  for (const auto& entry : impl_->work) {
    switch (entry.second.lifecycle) {
      case WorkLifecycle::Admitted: ++summary.work_admitted; break;
      case WorkLifecycle::Assigned: ++summary.work_assigned; break;
      case WorkLifecycle::Executing: ++summary.work_executing; break;
      case WorkLifecycle::Completed: ++summary.work_completed; break;
      case WorkLifecycle::Cancelled: ++summary.work_cancelled; break;
      case WorkLifecycle::Superseded: ++summary.work_superseded; break;
      case WorkLifecycle::Expired: ++summary.work_expired; break;
      default: break;
    }
  }
  for (const auto& entry : impl_->assignments) {
    const AssignmentRecord& record = entry.second;
    if (assignment_state_is_active(record.state)) {
      ++summary.active_assignments;
      if (assignment_state_is_dispatchable(record.state)) {
        ++summary.dispatchable_assignments;
      }
      if (record.state == AssignmentState::RevalidationRequired) {
        ++summary.revalidation_required_assignments;
      }
    } else {
      ++summary.historical_assignments;
    }
  }
  summary.current_leases = impl_->leases.size();
  summary.fenced_boots = impl_->fenced_boots.size();
  summary.scheduling_rounds = impl_->scheduling_rounds;
  summary.state_revision = impl_->state_revision;
  summary.assignments_created = impl_->assignments_created;
  summary.assignments_invalidated = impl_->assignments_invalidated;
  summary.state_digest = internal::compute_state_digest(*impl_);
  return summary;
}

SchedulerSnapshot AgentScheduler::snapshot() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  SchedulerSnapshot snapshot;
  snapshot.status = impl_->recovered ? SnapshotStatus::Reconstructed : SnapshotStatus::Current;
  snapshot.scheduler_id = impl_->scheduler_id;
  snapshot.scheduler_epoch = impl_->scheduler_epoch;
  snapshot.coordinator_epoch = impl_->coordinator_epoch;
  snapshot.policy = impl_->policy.id;
  snapshot.policy_generation = impl_->policy.generation;
  snapshot.queue_generation = impl_->queue_generation;
  snapshot.state_revision = impl_->state_revision;
  snapshot.created_at_ms = impl_->now_ms();
  for (const auto& entry : impl_->agents) {
    if (entry.second.descriptor.generation > snapshot.max_agent_generation) {
      snapshot.max_agent_generation = entry.second.descriptor.generation;
    }
    snapshot.agents.push_back(make_agent_status(*impl_, entry.second));
  }
  for (const auto& entry : impl_->work) {
    if (entry.second.request.generation > snapshot.max_work_generation) {
      snapshot.max_work_generation = entry.second.request.generation;
    }
    snapshot.work.push_back(make_work_status(entry.second));
  }
  for (const auto& entry : impl_->assignments) {
    if (entry.second.binding.generation > snapshot.max_assignment_generation) {
      snapshot.max_assignment_generation = entry.second.binding.generation;
    }
    Assignment item;
    item.binding = entry.second.binding;
    item.state = entry.second.state;
    item.invalidation = entry.second.invalidation;
    item.lease_current = entry.second.lease_current;
    item.dispatched_at_ms = entry.second.dispatched_at_ms;
    item.acknowledged_at_ms = entry.second.acknowledged_at_ms;
    item.closed_at_ms = entry.second.closed_at_ms;
    item.reason = entry.second.reason;
    snapshot.assignments.push_back(std::move(item));
  }
  for (const auto& entry : impl_->leases) {
    snapshot.leases.push_back(entry.second.lease);
  }
  for (const auto& entry : impl_->fenced_boots) {
    snapshot.fenced_boots.push_back(entry.second);
  }
  snapshot.state_digest = internal::compute_state_digest(*impl_);
  snapshot.agent_generation_digest = internal::compute_agent_generation_digest(*impl_);
  snapshot.summary = [&]() {
    // Summary is derived from the same locked state; recompute inline to avoid a second lock.
    SchedulerSummary summary;
    summary.scheduler_id = impl_->scheduler_id;
    summary.scheduler_epoch = impl_->scheduler_epoch;
    summary.coordinator_epoch = impl_->coordinator_epoch;
    summary.policy = impl_->policy.id;
    summary.policy_generation = impl_->policy.generation;
    summary.running = impl_->running;
    summary.admission_open = impl_->admission_open;
    summary.shutting_down = impl_->shutting_down;
    summary.agent_count = impl_->agents.size();
    summary.queue_count = impl_->queues.size();
    summary.state_revision = impl_->state_revision;
    summary.state_digest = snapshot.state_digest;
    return summary;
  }();
  {
    // Fairness snapshot from the same locked state.
    FairnessSnapshot fairness_snapshot;
    fairness_snapshot.aging_rounds_per_step = impl_->policy.aging_rounds_per_step;
    fairness_snapshot.max_priority_bypass = impl_->policy.max_priority_bypass;
    fairness_snapshot.bounded_starvation = !impl_->policy.allow_unbounded_starvation;
    fairness_snapshot.scheduling_rounds = impl_->scheduling_rounds;
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
        ++fairness_snapshot.starved_count;
      }
      if (fairness_entry.must_run) {
        ++fairness_snapshot.must_run_count;
      }
      fairness_snapshot.max_bypass_observed =
          std::max(fairness_snapshot.max_bypass_observed, record.bypass_count);
      fairness_snapshot.entries.push_back(std::move(fairness_entry));
    }
    snapshot.fairness = std::move(fairness_snapshot);
  }
  return snapshot;
}

SnapshotStatus SchedulerSnapshot::evaluate(const AgentScheduler& scheduler) const {
  return binds_current_generations(scheduler) ? status : SnapshotStatus::Stale;
}

bool SchedulerSnapshot::binds_current_generations(const AgentScheduler& scheduler) const {
  const SchedulerSummary current = scheduler.summary();
  if (current.scheduler_id != scheduler_id) {
    return false;
  }
  if (current.scheduler_epoch != scheduler_epoch) {
    return false;
  }
  if (current.coordinator_epoch != coordinator_epoch) {
    return false;
  }
  if (current.policy != policy || current.policy_generation != policy_generation) {
    return false;
  }
  if (current.state_revision != state_revision) {
    return false;
  }
  return true;
}

std::optional<AgentStatus> AgentScheduler::agent_status(AgentId agent) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->agents.find(agent);
  if (found == impl_->agents.end()) {
    return std::nullopt;
  }
  return make_agent_status(*impl_, found->second);
}

std::vector<AgentStatus> AgentScheduler::all_agents() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  std::vector<AgentStatus> out;
  out.reserve(impl_->agents.size());
  for (const auto& entry : impl_->agents) {
    out.push_back(make_agent_status(*impl_, entry.second));
  }
  return out;
}

std::optional<WorkStatus> AgentScheduler::work_status(WorkId work) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->work.find(work);
  if (found == impl_->work.end()) {
    return std::nullopt;
  }
  return make_work_status(found->second);
}

std::vector<WorkStatus> AgentScheduler::all_work() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  std::vector<WorkStatus> out;
  out.reserve(impl_->work.size());
  for (const auto& entry : impl_->work) {
    out.push_back(make_work_status(entry.second));
  }
  return out;
}

std::optional<Assignment> AgentScheduler::assignment(AssignmentId id) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->assignments.find(id);
  if (found == impl_->assignments.end()) {
    return std::nullopt;
  }
  const AssignmentRecord& record = found->second;
  Assignment out;
  out.binding = record.binding;
  out.state = record.state;
  out.invalidation = record.invalidation;
  out.lease_current = record.lease_current;
  out.dispatched_at_ms = record.dispatched_at_ms;
  out.acknowledged_at_ms = record.acknowledged_at_ms;
  out.closed_at_ms = record.closed_at_ms;
  out.reason = record.reason;
  return out;
}

std::vector<Assignment> AgentScheduler::assignments_for_work(WorkId work) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  std::vector<Assignment> out;
  for (const auto& entry : impl_->assignments) {
    if (entry.second.binding.work != work) {
      continue;
    }
    Assignment item;
    item.binding = entry.second.binding;
    item.state = entry.second.state;
    item.invalidation = entry.second.invalidation;
    item.lease_current = entry.second.lease_current;
    item.dispatched_at_ms = entry.second.dispatched_at_ms;
    item.acknowledged_at_ms = entry.second.acknowledged_at_ms;
    item.closed_at_ms = entry.second.closed_at_ms;
    item.reason = entry.second.reason;
    out.push_back(std::move(item));
  }
  return out;
}

std::vector<Assignment> AgentScheduler::assignments_for_agent(AgentId agent) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  std::vector<Assignment> out;
  for (const auto& entry : impl_->assignments) {
    if (entry.second.binding.agent != agent) {
      continue;
    }
    Assignment item;
    item.binding = entry.second.binding;
    item.state = entry.second.state;
    item.invalidation = entry.second.invalidation;
    item.lease_current = entry.second.lease_current;
    item.dispatched_at_ms = entry.second.dispatched_at_ms;
    item.acknowledged_at_ms = entry.second.acknowledged_at_ms;
    item.closed_at_ms = entry.second.closed_at_ms;
    item.reason = entry.second.reason;
    out.push_back(std::move(item));
  }
  return out;
}

std::vector<Assignment> AgentScheduler::all_assignments() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  std::vector<Assignment> out;
  out.reserve(impl_->assignments.size());
  for (const auto& entry : impl_->assignments) {
    Assignment item;
    item.binding = entry.second.binding;
    item.state = entry.second.state;
    item.invalidation = entry.second.invalidation;
    item.lease_current = entry.second.lease_current;
    item.dispatched_at_ms = entry.second.dispatched_at_ms;
    item.acknowledged_at_ms = entry.second.acknowledged_at_ms;
    item.closed_at_ms = entry.second.closed_at_ms;
    item.reason = entry.second.reason;
    out.push_back(std::move(item));
  }
  return out;
}

std::optional<Lease> AgentScheduler::lease(LeaseId lease_id) const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->leases.find(lease_id);
  if (found == impl_->leases.end()) {
    return std::nullopt;
  }
  return found->second.lease;
}

std::vector<Lease> AgentScheduler::leases() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  std::vector<Lease> out;
  out.reserve(impl_->leases.size());
  for (const auto& entry : impl_->leases) {
    out.push_back(entry.second.lease);
  }
  return out;
}

std::vector<FencedBoot> AgentScheduler::fenced_boots() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  std::vector<FencedBoot> out;
  out.reserve(impl_->fenced_boots.size());
  for (const auto& entry : impl_->fenced_boots) {
    out.push_back(entry.second);
  }
  return out;
}

Digest256 AgentScheduler::state_digest() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return internal::compute_state_digest(*impl_);
}

Digest256 AgentScheduler::agent_generation_digest() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return internal::compute_agent_generation_digest(*impl_);
}

}  // namespace agent_scheduler
