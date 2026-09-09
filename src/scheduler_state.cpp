// Agent Scheduler — internal state maintenance, accounting, and canonical digests.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "internal/scheduler_impl.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace agent_scheduler {

namespace {

void hash_text(Hasher256& hasher, std::string_view value) noexcept {
  hasher.update_length_prefixed(value);
}

void hash_double(Hasher256& hasher, double value) noexcept {
  std::uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value), "double must be 64-bit");
  std::memcpy(&bits, &value, sizeof(bits));
  hasher.update(bits);
}

void hash_labels(Hasher256& hasher, const std::vector<std::string>& labels) noexcept {
  hasher.update(static_cast<std::uint64_t>(labels.size()));
  for (const std::string& label : labels) {
    hash_text(hasher, label);
  }
}

void hash_policy(Hasher256& hasher, const PolicySnapshot& policy) noexcept {
  hasher.update_field_tag(0x30);
  hasher.update(policy.id.value());
  hasher.update(policy.generation.value());
  hasher.update(policy.aging_rounds_per_step);
  hasher.update(policy.max_priority_bypass);
  hasher.update(policy.starvation_alert_rounds);
  hasher.update(static_cast<std::uint8_t>(policy.allow_unbounded_starvation ? 1 : 0));
  hasher.update(policy.anti_concentration_window);
  hasher.update(policy.anti_concentration_penalty);
  hasher.update(policy.capability_staleness_ms);
  hasher.update(policy.health_staleness_ms);
  hasher.update(policy.load_staleness_ms);
  hasher.update(policy.availability_staleness_ms);
  hasher.update(policy.assignment_lease_ttl_ms);
  hasher.update(policy.registration_lease_ttl_ms);
  hasher.update(static_cast<std::uint8_t>(policy.close_dispatch_authority_on_shutdown ? 1 : 0));
  hasher.update(static_cast<std::uint64_t>(policy.ranking_weights.size()));
  for (const RankingWeight& weight : policy.ranking_weights) {
    hash_text(hasher, weight.factor);
    hasher.update(weight.weight);
  }
  hasher.update(static_cast<std::uint64_t>(policy.fairness_class_weights.size()));
  for (const QueueClassWeight& weight : policy.fairness_class_weights) {
    hash_text(hasher, weight.fairness_class);
    hasher.update(weight.weight);
  }
}

void hash_capability_profile(Hasher256& hasher, const CapabilityProfile& profile) noexcept {
  hasher.update_field_tag(0x31);
  hasher.update(profile.profile_id.value());
  hasher.update(profile.generation.value());
  hasher.update(profile.boot.value().high);
  hasher.update(profile.boot.value().low);
  hasher.update(static_cast<std::uint64_t>(profile.capabilities.size()));
  for (const CapabilityEvidence& evidence : profile.capabilities) {
    hash_text(hasher, evidence.name);
    hasher.update(static_cast<std::uint8_t>(evidence.state));
    hasher.update(evidence.quality);
    hasher.update(evidence.generation.value());
    hasher.update(evidence.boot.value().high);
    hasher.update(evidence.boot.value().low);
    hasher.update(evidence.observed_at_ms);
    hash_text(hasher, evidence.provenance);
  }
}

void hash_agent_record(Hasher256& hasher, const AgentRecord& record) noexcept {
  hasher.update_field_tag(0x40);
  hasher.update(record.descriptor.id.value());
  hasher.update(record.descriptor.generation.value());
  hasher.update(record.descriptor.boot.value().high);
  hasher.update(record.descriptor.boot.value().low);
  hasher.update(record.descriptor.registration_generation.value());
  hasher.update(record.descriptor.tenant.value());
  hasher.update(record.descriptor.name_space.value());
  hash_text(hasher, record.descriptor.display_name);
  hash_labels(hasher, record.descriptor.policy_labels);
  hasher.update(record.descriptor.placement_domain.value());
  hasher.update(record.descriptor.topology_epoch.value());
  hasher.update(record.descriptor.max_concurrency);
  hasher.update(record.descriptor.lease_ttl_ms);
  hash_text(hasher, record.descriptor.provenance);
  hasher.update(static_cast<std::uint8_t>(record.lifecycle));
  hasher.update(static_cast<std::uint8_t>(record.health));
  hasher.update(static_cast<std::uint8_t>(record.readiness));
  hasher.update(static_cast<std::uint8_t>(record.reachability));
  hasher.update(static_cast<std::uint8_t>(record.availability));
  hasher.update(record.health_generation.value());
  hasher.update(record.availability_generation.value());
  hasher.update(record.load_generation.value());
  hasher.update(record.health_observation.health_quality);
  hasher.update(record.health_observation.observed_at_ms);
  hasher.update(record.availability_observation.observed_at_ms);
  hasher.update(record.load_observation.reported_active_assignments);
  hasher.update(record.load_observation.queue_depth);
  hasher.update(record.load_observation.cpu_pressure_percent);
  hasher.update(record.load_observation.estimated_dispatch_latency_ms);
  hash_double(hasher, record.load_observation.cost_index);
  hash_double(hasher, record.load_observation.slo_headroom_fraction);
  hash_labels(hasher, record.load_observation.warm_state_keys);
  hasher.update(record.load_observation.observed_at_ms);
  hasher.update(record.lease_expires_at_ms);
  hasher.update(record.active_assignments);
  hasher.update(record.recent_assignments);
  hasher.update(record.last_heartbeat_ms);
  hasher.update(record.registered_at_ms);
  hasher.update(record.assignments_completed);
  hasher.update(record.assignments_failed);
  hasher.update(static_cast<std::uint8_t>(record.capability_current ? 1 : 0));
  hasher.update(static_cast<std::uint8_t>(record.health_current ? 1 : 0));
  hasher.update(static_cast<std::uint8_t>(record.availability_current ? 1 : 0));
  hasher.update(static_cast<std::uint8_t>(record.load_current ? 1 : 0));
  hasher.update(static_cast<std::uint8_t>(record.recovered ? 1 : 0));
  hash_text(hasher, record.reason);
  hash_capability_profile(hasher, record.profile);
}

void hash_work_record(Hasher256& hasher, const WorkRecord& record) noexcept {
  const WorkRequest& request = record.request;
  hasher.update_field_tag(0x41);
  hasher.update(request.id.value());
  hasher.update(request.generation.value());
  hasher.update(request.queue.value());
  hasher.update(request.queue_generation.value());
  hash_text(hasher, request.kind);
  hasher.update(request.priority);
  hash_text(hasher, request.fairness_class);
  hasher.update(static_cast<std::uint8_t>(request.mode));
  hash_text(hasher, request.payload_ref);
  hasher.update(request.earliest_start_ms);
  hasher.update(request.expected_duration_ms);
  const WorkRequirements& requirements = request.requirements;
  hasher.update(requirements.tenant.value());
  hasher.update(requirements.name_space.value());
  hasher.update(static_cast<std::uint64_t>(requirements.capabilities.size()));
  for (const CapabilityRequirement& capability : requirements.capabilities) {
    hash_text(hasher, capability.name);
    hasher.update(static_cast<std::uint8_t>(capability.mode));
    hasher.update(static_cast<std::uint8_t>(capability.minimum_state));
    hasher.update(capability.minimum_quality);
  }
  hash_labels(hasher, requirements.required_policy_labels);
  hasher.update(static_cast<std::uint64_t>(requirements.affinity.size()));
  for (const AgentId agent : requirements.affinity) {
    hasher.update(agent.value());
  }
  hasher.update(static_cast<std::uint64_t>(requirements.anti_affinity.size()));
  for (const AgentId agent : requirements.anti_affinity) {
    hasher.update(agent.value());
  }
  hasher.update(static_cast<std::uint8_t>(requirements.restrict_placement_domain ? 1 : 0));
  hasher.update(requirements.placement_domain.value());
  hasher.update(requirements.topology_epoch.value());
  hasher.update(static_cast<std::uint8_t>(requirements.require_healthy ? 1 : 0));
  hasher.update(static_cast<std::uint8_t>(requirements.require_reachable ? 1 : 0));
  hasher.update(static_cast<std::uint8_t>(requirements.require_ready ? 1 : 0));
  hasher.update(static_cast<std::uint8_t>(requirements.require_current_lease ? 1 : 0));
  hasher.update(static_cast<std::uint8_t>(requirements.require_resource_feasibility ? 1 : 0));
  hasher.update(requirements.resource_generation.value());
  hasher.update(static_cast<std::uint8_t>(requirements.resource_feasibility));
  hasher.update(static_cast<std::uint8_t>(requirements.require_budget_feasibility ? 1 : 0));
  hasher.update(requirements.budget_generation.value());
  hasher.update(static_cast<std::uint8_t>(requirements.budget_feasibility));
  hasher.update(static_cast<std::uint8_t>(requirements.require_slo_feasibility ? 1 : 0));
  hasher.update(requirements.slo_generation.value());
  hasher.update(static_cast<std::uint8_t>(requirements.slo_feasibility));
  hasher.update(static_cast<std::uint8_t>(requirements.require_reservation ? 1 : 0));
  hasher.update(requirements.reservation_generation.value());
  hasher.update(static_cast<std::uint8_t>(requirements.reservation_feasibility));
  hasher.update(static_cast<std::uint8_t>(requirements.require_warm_state ? 1 : 0));
  hash_labels(hasher, requirements.warm_state_keys);
  hasher.update(requirements.deadline_ms);
  hasher.update(requirements.max_parallel);
  hasher.update(requirements.max_assignments_per_agent);
  hasher.update(static_cast<std::uint8_t>(record.lifecycle));
  hasher.update(record.admitted_sequence);
  hasher.update(record.admitted_at_ms);
  hasher.update(record.bypass_count);
  hasher.update(record.starvation_rounds);
  hasher.update(record.current_assignments);
  hasher.update(record.last_assignment_generation.value());
  hash_text(hasher, record.reason);
}

void hash_assignment_record(Hasher256& hasher, const AssignmentRecord& record) noexcept {
  const AssignmentBinding& binding = record.binding;
  hasher.update_field_tag(0x42);
  hasher.update(binding.id.value().high);
  hasher.update(binding.id.value().low);
  hasher.update(binding.generation.value());
  hasher.update(binding.work.value());
  hasher.update(binding.work_generation.value());
  hasher.update(binding.agent.value());
  hasher.update(binding.agent_generation.value());
  hasher.update(binding.boot.value().high);
  hasher.update(binding.boot.value().low);
  hasher.update(binding.scheduler.value());
  hasher.update(binding.scheduler_epoch.value());
  hasher.update(binding.coordinator_epoch.value());
  hasher.update(binding.policy.value());
  hasher.update(binding.policy_generation.value());
  hasher.update(binding.capability_profile.value());
  hasher.update(binding.capability_generation.value());
  hasher.update(binding.resource_generation.value());
  hasher.update(binding.budget_generation.value());
  hasher.update(binding.slo_generation.value());
  hasher.update(binding.reservation_generation.value());
  hasher.update(binding.queue.value());
  hasher.update(binding.queue_generation.value());
  hasher.update(binding.lease.value().high);
  hasher.update(binding.lease.value().low);
  hasher.update(binding.lease_generation.value());
  hasher.update(binding.dispatch.value().high);
  hasher.update(binding.dispatch.value().low);
  hasher.update(binding.dispatch_generation.value());
  hasher.update(binding.placement_domain.value());
  hasher.update(binding.topology_epoch.value());
  hasher.update(binding.created_at_ms);
  hasher.update(binding.lease_expires_at_ms);
  hasher.update(binding.replica_index);
  hasher.update(static_cast<std::uint8_t>(binding.exclusive ? 1 : 0));
  hasher.update(static_cast<std::uint8_t>(record.state));
  hasher.update(static_cast<std::uint8_t>(record.invalidation));
  hasher.update(static_cast<std::uint8_t>(record.lease_current ? 1 : 0));
  hasher.update(record.dispatched_at_ms);
  hasher.update(record.acknowledged_at_ms);
  hasher.update(record.closed_at_ms);
  hash_text(hasher, record.reason);
}

}  // namespace

namespace internal {

bool boot_is_fenced(const Impl& impl, AgentBootId boot) {
  return impl.fenced_boots.find(boot) != impl.fenced_boots.end();
}

bool incarnation_current(const Impl& impl, const AgentRecord& record, AgentBootId boot) {
  if (record.descriptor.boot != boot) {
    return false;
  }
  return !boot_is_fenced(impl, boot);
}

void fence_boot(Impl& impl, AgentId agent, AgentBootId boot, AgentGeneration generation, std::string reason) {
  if (boot.is_zero() || boot_is_fenced(impl, boot)) {
    return;
  }
  FencedBoot record;
  record.agent = agent;
  record.boot = boot;
  record.generation = generation;
  record.fenced_at_ms = impl.now_ms();
  record.reason = std::move(reason);
  impl.fenced_boots.emplace(boot, record);
  impl.state_revision++;
}

void index_agent(Impl& impl, const AgentRecord& record) {
  impl.index_agents_by_lifecycle[record.lifecycle].insert(record.descriptor.id);
  impl.index_agents_by_tenant[record.descriptor.tenant].insert(record.descriptor.id);
  if (!record.descriptor.placement_domain.is_zero()) {
    impl.index_agents_by_domain[record.descriptor.placement_domain].insert(record.descriptor.id);
  }
  if (record.capability_current) {
    for (const CapabilityEvidence& evidence : record.profile.capabilities) {
      if (capability_state_is_usable(evidence.state)) {
        impl.index_agents_by_capability[evidence.name].insert(record.descriptor.id);
      }
    }
  }
}

void deindex_agent(Impl& impl, AgentId agent) {
  for (auto& entry : impl.index_agents_by_lifecycle) {
    entry.second.erase(agent);
  }
  for (auto& entry : impl.index_agents_by_tenant) {
    entry.second.erase(agent);
  }
  for (auto& entry : impl.index_agents_by_domain) {
    entry.second.erase(agent);
  }
  for (auto& entry : impl.index_agents_by_capability) {
    entry.second.erase(agent);
  }
}

void index_work(Impl& impl, const WorkRecord& record) {
  impl.index_work_by_queue[record.request.queue].insert(record.request.id);
  impl.index_work_by_lifecycle[record.lifecycle].insert(record.request.id);
}

void deindex_work(Impl& impl, WorkId work) {
  for (auto& entry : impl.index_work_by_queue) {
    entry.second.erase(work);
  }
  for (auto& entry : impl.index_work_by_lifecycle) {
    entry.second.erase(work);
  }
}

void index_assignment(Impl& impl, const AssignmentRecord& record) {
  if (!assignment_state_is_active(record.state)) {
    return;
  }
  impl.index_assignments_by_agent[record.binding.agent].insert(record.binding.id);
  impl.index_assignments_by_work[record.binding.work].insert(record.binding.id);
}

void deindex_assignment(Impl& impl, const AssignmentRecord& record) {
  const auto agent_entry = impl.index_assignments_by_agent.find(record.binding.agent);
  if (agent_entry != impl.index_assignments_by_agent.end()) {
    agent_entry->second.erase(record.binding.id);
    if (agent_entry->second.empty()) {
      impl.index_assignments_by_agent.erase(agent_entry);
    }
  }
  const auto work_entry = impl.index_assignments_by_work.find(record.binding.work);
  if (work_entry != impl.index_assignments_by_work.end()) {
    work_entry->second.erase(record.binding.id);
    if (work_entry->second.empty()) {
      impl.index_assignments_by_work.erase(work_entry);
    }
  }
}

void rebuild_indexes(Impl& impl) {
  impl.index_agents_by_capability.clear();
  impl.index_agents_by_lifecycle.clear();
  impl.index_agents_by_tenant.clear();
  impl.index_agents_by_domain.clear();
  impl.index_work_by_queue.clear();
  impl.index_work_by_lifecycle.clear();
  impl.index_assignments_by_agent.clear();
  impl.index_assignments_by_work.clear();
  for (const auto& entry : impl.agents) {
    index_agent(impl, entry.second);
  }
  for (const auto& entry : impl.work) {
    index_work(impl, entry.second);
  }
  for (const auto& entry : impl.assignments) {
    index_assignment(impl, entry.second);
  }
}

void recompute_lifecycle(AgentRecord& record) {
  if (record.lifecycle == AgentLifecycle::Lost || record.lifecycle == AgentLifecycle::Retired ||
      record.lifecycle == AgentLifecycle::Draining) {
    return;
  }
  if (!record.capability_current || !record.health_current || !record.availability_current) {
    record.lifecycle = record.recovered ? AgentLifecycle::RevalidationRequired : AgentLifecycle::Registering;
    return;
  }
  if (record.availability != AgentAvailability::Available || record.reachability != AgentReachability::Reachable ||
      record.readiness != AgentReadiness::Ready) {
    record.lifecycle = AgentLifecycle::Unavailable;
    return;
  }
  record.lifecycle = record.active_assignments > 0 ? AgentLifecycle::Busy : AgentLifecycle::Ready;
}

bool capability_fresh(const Impl& impl, const AgentRecord& record) {
  if (!record.capability_current) {
    return false;
  }
  if (record.profile.boot != record.descriptor.boot) {
    return false;
  }
  if (record.profile.capabilities.empty()) {
    return true;
  }
  const std::uint64_t observed = record.profile.capabilities.front().observed_at_ms;
  const std::uint64_t now = impl.now_ms();
  if (observed > now) {
    return false;
  }
  return (now - observed) <= impl.policy.capability_staleness_ms;
}

bool health_fresh(const Impl& impl, const AgentRecord& record) {
  if (!record.health_current) {
    return false;
  }
  const std::uint64_t observed = record.health_observation.observed_at_ms;
  const std::uint64_t now = impl.now_ms();
  if (observed > now) {
    return false;
  }
  return (now - observed) <= impl.policy.health_staleness_ms;
}

bool availability_fresh(const Impl& impl, const AgentRecord& record) {
  if (!record.availability_current) {
    return false;
  }
  const std::uint64_t observed = record.availability_observation.observed_at_ms;
  const std::uint64_t now = impl.now_ms();
  if (observed > now) {
    return false;
  }
  return (now - observed) <= impl.policy.availability_staleness_ms;
}

bool load_fresh(const Impl& impl, const AgentRecord& record) {
  if (!record.load_current) {
    return false;
  }
  const std::uint64_t observed = record.load_observation.observed_at_ms;
  const std::uint64_t now = impl.now_ms();
  if (observed > now) {
    return false;
  }
  return (now - observed) <= impl.policy.load_staleness_ms;
}

bool lease_current(const Impl& impl, const AgentRecord& record) {
  if (record.lease_expires_at_ms == 0) {
    return false;
  }
  return impl.now_ms() <= record.lease_expires_at_ms;
}

void invalidate_dynamic_evidence(AgentRecord& record) {
  record.capability_current = false;
  record.health_current = false;
  record.availability_current = false;
  record.load_current = false;
  record.health = AgentHealth::Unknown;
  record.readiness = AgentReadiness::Unknown;
  record.reachability = AgentReachability::Unknown;
  record.availability = AgentAvailability::Unknown;
  record.lease_expires_at_ms = 0;
  invalidate_evidence(record.profile);
}

std::uint32_t active_assignment_count(const Impl& impl, AgentId agent) {
  const auto entry = impl.index_assignments_by_agent.find(agent);
  if (entry == impl.index_assignments_by_agent.end()) {
    return 0;
  }
  std::uint32_t count = 0;
  for (const AssignmentId id : entry->second) {
    const auto found = impl.assignments.find(id);
    if (found != impl.assignments.end() && assignment_state_is_active(found->second.state)) {
      ++count;
    }
  }
  return count;
}

void sync_agent_lifecycle_index(Impl& impl, AgentId agent, AgentLifecycle lifecycle) {
  for (auto& entry : impl.index_agents_by_lifecycle) {
    entry.second.erase(agent);
  }
  impl.index_agents_by_lifecycle[lifecycle].insert(agent);
}

void sync_work_lifecycle_index(Impl& impl, WorkId work, WorkLifecycle lifecycle) {
  for (auto& entry : impl.index_work_by_lifecycle) {
    entry.second.erase(work);
  }
  impl.index_work_by_lifecycle[lifecycle].insert(work);
}

void refresh_agent_accounting(Impl& impl, AgentRecord& record) {
  record.active_assignments = active_assignment_count(impl, record.descriptor.id);
  recompute_lifecycle(record);
  sync_agent_lifecycle_index(impl, record.descriptor.id, record.lifecycle);
}

void refresh_work_accounting(Impl& impl, WorkRecord& record) {
  std::uint32_t active = 0;
  bool executing = false;
  const auto entry = impl.index_assignments_by_work.find(record.request.id);
  if (entry != impl.index_assignments_by_work.end()) {
    for (const AssignmentId id : entry->second) {
      const auto found = impl.assignments.find(id);
      if (found == impl.assignments.end() || !assignment_state_is_active(found->second.state)) {
        continue;
      }
      ++active;
      if (found->second.state == AssignmentState::Executing ||
          found->second.state == AssignmentState::Releasing) {
        executing = true;
      }
    }
  }
  record.current_assignments = active;
  if (work_is_terminal(record.lifecycle)) {
    sync_work_lifecycle_index(impl, record.request.id, record.lifecycle);
    return;
  }
  if (active == 0) {
    record.lifecycle = WorkLifecycle::Admitted;
  } else {
    record.lifecycle = executing ? WorkLifecycle::Executing : WorkLifecycle::Assigned;
  }
  sync_work_lifecycle_index(impl, record.request.id, record.lifecycle);
}

void close_assignment(Impl& impl,
                      AssignmentRecord& record,
                      AssignmentState state,
                      InvalidationReason reason,
                      std::string detail) {
  if (assignment_state_is_terminal(record.state)) {
    return;
  }
  record.state = state;
  record.invalidation = reason;
  record.lease_current = false;
  record.closed_at_ms = impl.now_ms();
  record.reason = std::move(detail);
  const auto lease_entry = impl.leases.find(record.binding.lease);
  if (lease_entry != impl.leases.end()) {
    lease_entry->second.lease.current = false;
  }
  deindex_assignment(impl, record);
  const auto agent_entry = impl.agents.find(record.binding.agent);
  if (agent_entry != impl.agents.end()) {
    if (agent_entry->second.recent_assignments > 0) {
      agent_entry->second.recent_assignments = (agent_entry->second.recent_assignments * 3u) / 4u;
    }
    refresh_agent_accounting(impl, agent_entry->second);
  }
  const auto work_entry = impl.work.find(record.binding.work);
  if (work_entry != impl.work.end()) {
    refresh_work_accounting(impl, work_entry->second);
  }
  ++impl.assignments_invalidated;
  ++impl.state_revision;
}

std::optional<AssignmentId> active_exclusive_assignment(const Impl& impl, WorkId work) {
  const auto entry = impl.index_assignments_by_work.find(work);
  if (entry == impl.index_assignments_by_work.end()) {
    return std::nullopt;
  }
  for (const AssignmentId id : entry->second) {
    const auto found = impl.assignments.find(id);
    if (found == impl.assignments.end()) {
      continue;
    }
    if (assignment_state_is_active(found->second.state) && found->second.binding.exclusive) {
      return id;
    }
  }
  return std::nullopt;
}

std::uint32_t active_assignments_on_agent(const Impl& impl, WorkId work, AgentId agent) {
  const auto entry = impl.index_assignments_by_work.find(work);
  if (entry == impl.index_assignments_by_work.end()) {
    return 0;
  }
  std::uint32_t count = 0;
  for (const AssignmentId id : entry->second) {
    const auto found = impl.assignments.find(id);
    if (found == impl.assignments.end()) {
      continue;
    }
    if (assignment_state_is_active(found->second.state) && found->second.binding.agent == agent) {
      ++count;
    }
  }
  return count;
}

AssignmentBinding build_binding(Impl& impl,
                                const WorkRecord& work_record,
                                const AgentRecord& agent_record,
                                AssignmentGeneration generation,
                                std::uint32_t replica_index) {
  AssignmentBinding binding;
  binding.generation = generation;
  binding.work = work_record.request.id;
  binding.work_generation = work_record.request.generation;
  binding.agent = agent_record.descriptor.id;
  binding.agent_generation = agent_record.descriptor.generation;
  binding.boot = agent_record.descriptor.boot;
  binding.scheduler = impl.scheduler_id;
  binding.scheduler_epoch = impl.scheduler_epoch;
  binding.coordinator_epoch = impl.coordinator_epoch;
  binding.policy = impl.policy.id;
  binding.policy_generation = impl.policy.generation;
  binding.capability_profile = agent_record.profile.profile_id;
  binding.capability_generation = agent_record.profile.generation;
  binding.resource_generation = work_record.request.requirements.resource_generation;
  binding.budget_generation = work_record.request.requirements.budget_generation;
  binding.slo_generation = work_record.request.requirements.slo_generation;
  binding.reservation_generation = work_record.request.requirements.reservation_generation;
  binding.queue = work_record.request.queue;
  binding.queue_generation = work_record.request.queue_generation;
  binding.placement_domain = agent_record.descriptor.placement_domain;
  binding.topology_epoch = agent_record.descriptor.topology_epoch;
  binding.replica_index = replica_index;
  binding.exclusive = work_record.request.mode == SchedulingMode::Exclusive;
  binding.created_at_ms = impl.now_ms();
  binding.lease_expires_at_ms = binding.created_at_ms + impl.policy.assignment_lease_ttl_ms;
  binding.lease_generation = LeaseGeneration{binding.generation.value()};
  binding.dispatch_generation = DispatchGeneration{binding.generation.value()};
  binding.id = derive_assignment_id(binding);
  binding.lease = derive_lease_id(binding);
  binding.dispatch = derive_dispatch_id(binding);
  return binding;
}

std::optional<ScheduleOutcome> revalidate_authority(const Impl& impl,
                                                    const AssignmentRecord& record,
                                                    std::string& detail) {
  const AssignmentBinding& binding = record.binding;
  if (impl.shutting_down && !assignment_state_is_dispatchable(record.state)) {
    detail = "scheduler is shutting down";
    return ScheduleOutcome::ShuttingDown;
  }
  if (binding.scheduler != impl.scheduler_id || binding.scheduler_epoch != impl.scheduler_epoch) {
    detail = "scheduler epoch moved";
    return ScheduleOutcome::RejectStaleSchedulerEpoch;
  }
  if (binding.coordinator_epoch != impl.coordinator_epoch) {
    detail = "coordinator epoch moved";
    return ScheduleOutcome::RejectStaleCoordinatorEpoch;
  }
  if (binding.policy != impl.policy.id || binding.policy_generation != impl.policy.generation) {
    detail = "policy generation moved";
    return ScheduleOutcome::RejectStalePolicy;
  }
  const auto work_entry = impl.work.find(binding.work);
  if (work_entry == impl.work.end()) {
    detail = "work item is not known";
    return ScheduleOutcome::RejectInvalidRequest;
  }
  const WorkRecord& work_record = work_entry->second;
  if (work_record.request.generation != binding.work_generation) {
    detail = "work generation moved";
    return ScheduleOutcome::RejectStaleWorkGeneration;
  }
  if (work_record.lifecycle == WorkLifecycle::Cancelled) {
    detail = "work is cancelled";
    return ScheduleOutcome::RejectCancelled;
  }
  if (work_record.lifecycle == WorkLifecycle::Superseded) {
    detail = "work is superseded";
    return ScheduleOutcome::RejectSuperseded;
  }
  if (work_is_terminal(work_record.lifecycle)) {
    detail = "work is terminal";
    return ScheduleOutcome::RejectExpired;
  }
  if (work_record.request.requirements.deadline_ms != 0 &&
      impl.now_ms() > work_record.request.requirements.deadline_ms) {
    detail = "work deadline passed";
    return ScheduleOutcome::RejectDeadline;
  }
  const auto agent_entry = impl.agents.find(binding.agent);
  if (agent_entry == impl.agents.end()) {
    detail = "agent is not known";
    return ScheduleOutcome::RejectStaleAgentGeneration;
  }
  const AgentRecord& agent_record = agent_entry->second;
  if (agent_record.descriptor.generation != binding.agent_generation) {
    detail = "agent generation moved";
    return ScheduleOutcome::RejectStaleAgentGeneration;
  }
  if (agent_record.descriptor.boot != binding.boot) {
    detail = "agent incarnation replaced";
    return ScheduleOutcome::RejectStaleAgentBoot;
  }
  if (boot_is_fenced(impl, binding.boot)) {
    detail = "agent incarnation is fenced";
    return ScheduleOutcome::RejectFenced;
  }
  if (agent_record.lifecycle == AgentLifecycle::Lost || agent_record.lifecycle == AgentLifecycle::Retired) {
    detail = "agent is lost or retired";
    return ScheduleOutcome::RejectLifecycle;
  }
  if (agent_record.lifecycle == AgentLifecycle::Draining) {
    detail = "agent is draining";
    return ScheduleOutcome::RejectDrain;
  }
  if (agent_record.profile.generation != binding.capability_generation ||
      agent_record.profile.profile_id != binding.capability_profile ||
      agent_record.profile.boot != binding.boot || !capability_fresh(impl, agent_record)) {
    detail = "capability evidence moved or is stale";
    return ScheduleOutcome::RejectStaleCapability;
  }
  const WorkRequirements& requirements = work_record.request.requirements;
  if (requirements.require_current_lease && !lease_current(impl, agent_record)) {
    detail = "registration lease is not current";
    return ScheduleOutcome::RejectLeaseExpired;
  }
  if (requirements.require_healthy &&
      (!health_fresh(impl, agent_record) || agent_record.health == AgentHealth::Unknown ||
       agent_record.health == AgentHealth::Unhealthy)) {
    detail = "health evidence is not current";
    return ScheduleOutcome::RejectHealth;
  }
  if (requirements.require_reachable &&
      (!availability_fresh(impl, agent_record) || agent_record.reachability != AgentReachability::Reachable)) {
    detail = "reachability evidence is not current";
    return ScheduleOutcome::RejectReachability;
  }
  const ExternalFeasibilityResult feasibility = evaluate_feasibility(impl, work_record);
  if (requirements.require_resource_feasibility && feasibility.resource != FeasibilityVerdict::Feasible) {
    detail = "resource feasibility is not current";
    return ScheduleOutcome::RejectResourceFeasibility;
  }
  if (requirements.require_budget_feasibility && feasibility.budget != FeasibilityVerdict::Feasible) {
    detail = "budget feasibility is not current";
    return ScheduleOutcome::RejectBudget;
  }
  if (requirements.require_slo_feasibility && feasibility.slo != FeasibilityVerdict::Feasible) {
    detail = "slo feasibility is not current";
    return ScheduleOutcome::RejectSlo;
  }
  if (requirements.require_reservation && feasibility.reservation != FeasibilityVerdict::Feasible) {
    detail = "reservation feasibility is not current";
    return ScheduleOutcome::RejectReservation;
  }
  return std::nullopt;
}

ExternalFeasibilityResult evaluate_feasibility(const Impl& impl, const WorkRecord& record) {
  if (!impl.feasibility) {
    ExternalFeasibilityResult result;
    result.resource = record.request.requirements.resource_feasibility;
    result.budget = record.request.requirements.budget_feasibility;
    result.slo = record.request.requirements.slo_feasibility;
    result.reservation = record.request.requirements.reservation_feasibility;
    return result;
  }
  ExternalFeasibilityRequest request;
  request.work = record.request.id;
  request.work_generation = record.request.generation;
  request.tenant = record.request.requirements.tenant;
  request.name_space = record.request.requirements.name_space;
  request.resource_generation = record.request.requirements.resource_generation;
  request.budget_generation = record.request.requirements.budget_generation;
  request.slo_generation = record.request.requirements.slo_generation;
  request.reservation_generation = record.request.requirements.reservation_generation;
  request.deadline_ms = record.request.requirements.deadline_ms;
  return impl.feasibility->evaluate(request);
}

Digest256 compute_state_digest(const Impl& impl) {
  Hasher256 hasher;
  hasher.update_field_tag(0x50);
  hasher.update(impl.scheduler_id.value());
  hasher.update(impl.scheduler_epoch.value());
  hasher.update(impl.coordinator_epoch.value());
  hasher.update(impl.queue_generation.value());
  hasher.update(static_cast<std::uint8_t>(impl.admission_open ? 1 : 0));
  hasher.update(static_cast<std::uint8_t>(impl.shutting_down ? 1 : 0));
  hasher.update(impl.next_admission_sequence);
  hasher.update(impl.scheduling_rounds);
  hasher.update(impl.assignments_created);
  hasher.update(impl.assignments_invalidated);
  hash_policy(hasher, impl.policy);
  hasher.update(static_cast<std::uint64_t>(impl.agents.size()));
  for (const auto& entry : impl.agents) {
    hash_agent_record(hasher, entry.second);
  }
  hasher.update(static_cast<std::uint64_t>(impl.work.size()));
  for (const auto& entry : impl.work) {
    hash_work_record(hasher, entry.second);
  }
  hasher.update(static_cast<std::uint64_t>(impl.assignments.size()));
  for (const auto& entry : impl.assignments) {
    hash_assignment_record(hasher, entry.second);
  }
  hasher.update(static_cast<std::uint64_t>(impl.leases.size()));
  for (const auto& entry : impl.leases) {
    hasher.update(entry.second.lease.id.value().high);
    hasher.update(entry.second.lease.id.value().low);
    hasher.update(entry.second.lease.generation.value());
    hasher.update(entry.second.lease.assignment.value().high);
    hasher.update(entry.second.lease.assignment.value().low);
    hasher.update(entry.second.lease.assignment_generation.value());
    hasher.update(entry.second.lease.granted_at_ms);
    hasher.update(entry.second.lease.expires_at_ms);
    hasher.update(static_cast<std::uint8_t>(entry.second.lease.current ? 1 : 0));
  }
  hasher.update(static_cast<std::uint64_t>(impl.fenced_boots.size()));
  for (const auto& entry : impl.fenced_boots) {
    hasher.update(entry.second.agent.value());
    hasher.update(entry.second.boot.value().high);
    hasher.update(entry.second.boot.value().low);
    hasher.update(entry.second.generation.value());
    hasher.update(entry.second.fenced_at_ms);
  }
  hasher.update(static_cast<std::uint64_t>(impl.queues.size()));
  for (const auto& entry : impl.queues) {
    hasher.update(entry.second.id.value());
    hasher.update(entry.second.generation.value());
    hash_text(hasher, entry.second.fairness_class);
    hasher.update(entry.second.weight);
    hasher.update(entry.second.admitted_count);
    hasher.update(entry.second.assigned_count);
    hasher.update(entry.second.completed_count);
    hasher.update(entry.second.cancelled_count);
  }
  hasher.update(static_cast<std::uint64_t>(impl.tenants.size()));
  for (const auto& entry : impl.tenants) {
    hasher.update(entry.second.id.value());
    hasher.update(entry.second.admitted_count);
    hasher.update(entry.second.active_assignments);
  }
  return hasher.final();
}

Digest256 compute_agent_generation_digest(const Impl& impl) {
  Hasher256 hasher;
  hasher.update_field_tag(0x51);
  for (const auto& entry : impl.agents) {
    hasher.update(entry.second.descriptor.id.value());
    hasher.update(entry.second.descriptor.generation.value());
    hasher.update(entry.second.descriptor.boot.value().high);
    hasher.update(entry.second.descriptor.boot.value().low);
    hasher.update(entry.second.descriptor.registration_generation.value());
  }
  return hasher.final();
}

FairnessKey fairness_key(const Impl& impl, const WorkRecord& record) {
  FairnessKey key;
  key.bypass_count = record.bypass_count;
  key.bypass_ceiling = bypass_ceiling(impl.policy, record.request.fairness_class);
  key.admitted_sequence = record.admitted_sequence;
  key.admitted_at_ms = record.admitted_at_ms;
  const std::uint64_t aging_step = static_cast<std::uint64_t>(std::max<std::uint32_t>(1u, impl.policy.aging_rounds_per_step));
  std::uint64_t credit = static_cast<std::uint64_t>(record.bypass_count) / aging_step;
  const std::uint64_t cap = std::max<std::uint32_t>(1u, impl.policy.max_priority_bypass);
  if (credit > cap) {
    credit = cap;
  }
  key.effective_priority = static_cast<std::int64_t>(record.request.priority) + static_cast<std::int64_t>(credit);
  key.must_run = !impl.policy.allow_unbounded_starvation && record.bypass_count >= key.bypass_ceiling;
  return key;
}

bool fairness_before(const FairnessKey& left, const FairnessKey& right) noexcept {
  if (left.must_run != right.must_run) {
    return left.must_run;
  }
  if (left.must_run && right.must_run) {
    // Higher bypass pressure first, then oldest admission.
    const std::uint64_t left_ratio =
        left.bypass_ceiling == 0 ? left.bypass_count : (static_cast<std::uint64_t>(left.bypass_count) * 1024u) / left.bypass_ceiling;
    const std::uint64_t right_ratio = right.bypass_ceiling == 0
                                          ? right.bypass_count
                                          : (static_cast<std::uint64_t>(right.bypass_count) * 1024u) / right.bypass_ceiling;
    if (left_ratio != right_ratio) {
      return left_ratio > right_ratio;
    }
    return left.admitted_sequence < right.admitted_sequence;
  }
  if (left.effective_priority != right.effective_priority) {
    return left.effective_priority > right.effective_priority;
  }
  return left.admitted_sequence < right.admitted_sequence;
}

}  // namespace internal

AgentScheduler::Impl::Impl(SchedulerOptions options_value)
    : options(std::move(options_value)),
      limits(options.limits),
      clock(options.clock ? options.clock : std::make_shared<SystemClock>()),
      feasibility(options.feasibility),
      interlock(options.interlock ? options.interlock : std::make_shared<ScheduleInterlock>()),
      scheduler_id(options.scheduler_id),
      policy(options.policy) {
  if (scheduler_id.is_zero()) {
    Hasher256 hasher;
    hasher.update_field_tag(0x52);
    hasher.update(process_nonce());
    hasher.update(wall_clock_ms());
    scheduler_id = SchedulerId{hash128_of(hasher.final()).high};
  }
  if (policy.id.is_zero()) {
    policy.id = PolicyId{1};
  }
  if (policy.generation.is_zero()) {
    policy.generation = PolicyGeneration{1};
  }
  if (policy.ranking_weights.empty()) {
    policy.ranking_weights = default_ranking_weights();
  }
  if (policy.fairness_class_weights.empty()) {
    policy.fairness_class_weights = default_fairness_class_weights();
  }
}

}  // namespace agent_scheduler
