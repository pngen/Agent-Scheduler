// Agent Scheduler — versioned, integrity-checked durable persistence.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/persistence.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "agent_scheduler/codec.hpp"
#include "agent_scheduler/scheduler.hpp"
#include "internal/persistence_io.hpp"
#include "internal/scheduler_impl.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace agent_scheduler {
namespace {

using internal::Impl;

constexpr std::uint8_t kSectionScheduler = 1;
constexpr std::uint8_t kSectionPolicy = 2;
constexpr std::uint8_t kSectionQueues = 3;
constexpr std::uint8_t kSectionTenants = 4;
constexpr std::uint8_t kSectionAgents = 5;
constexpr std::uint8_t kSectionWork = 6;
constexpr std::uint8_t kSectionAssignments = 7;
constexpr std::uint8_t kSectionLeases = 8;
constexpr std::uint8_t kSectionFenced = 9;
constexpr std::uint32_t kSectionCount = 9;

constexpr std::uint8_t kLifecycleNormal = 0;
constexpr std::uint8_t kLifecycleRetired = 1;
constexpr std::uint8_t kLifecycleLost = 2;

std::atomic<std::uint64_t> g_temp_counter{0};

[[nodiscard]] MutationResult persistence_failure(ErrorCode code,
                                                 std::string operation,
                                                 std::string subject,
                                                 std::string detail) {
  return MutationResult::failure(make_error(code, std::move(operation), std::move(subject),
                                            std::move(detail), ScheduleOutcome::RejectInvalidRequest));
}

[[nodiscard]] std::uint8_t lifecycle_hint(AgentLifecycle lifecycle) noexcept {
  switch (lifecycle) {
    case AgentLifecycle::Retired: return kLifecycleRetired;
    case AgentLifecycle::Lost: return kLifecycleLost;
    default: return kLifecycleNormal;
  }
}

[[nodiscard]] AgentLifecycle lifecycle_from_hint(std::uint8_t hint) noexcept {
  switch (hint) {
    case kLifecycleRetired: return AgentLifecycle::Retired;
    case kLifecycleLost: return AgentLifecycle::Lost;
    default: return AgentLifecycle::RevalidationRequired;
  }
}

[[nodiscard]] std::uint8_t work_lifecycle_hint(WorkLifecycle lifecycle) noexcept {
  return static_cast<std::uint8_t>(lifecycle);
}

[[nodiscard]] WorkLifecycle work_lifecycle_from_hint(std::uint8_t hint) noexcept {
  switch (static_cast<WorkLifecycle>(hint)) {
    case WorkLifecycle::Admitted:
    case WorkLifecycle::Assigned:
    case WorkLifecycle::Executing:
    case WorkLifecycle::Completed:
    case WorkLifecycle::Failed:
    case WorkLifecycle::Cancelled:
    case WorkLifecycle::Superseded:
    case WorkLifecycle::Expired:
      return static_cast<WorkLifecycle>(hint);
    default:
      return WorkLifecycle::Unknown;
  }
}

// ---------------------------------------------------------------------------
// canonical encoding
// ---------------------------------------------------------------------------

void write_descriptor(ByteWriter& writer, const AgentDescriptor& descriptor, const ResourceLimits& limits) {
  writer.put_u64(descriptor.id.value());
  writer.put_u64(descriptor.generation.value());
  writer.put_u64(descriptor.boot.value().high);
  writer.put_u64(descriptor.boot.value().low);
  writer.put_u64(descriptor.registration_generation.value());
  writer.put_u64(descriptor.tenant.value());
  writer.put_u64(descriptor.name_space.value());
  writer.put_string(descriptor.display_name, limits.max_string_size);
  writer.put_u32(static_cast<std::uint32_t>(descriptor.policy_labels.size()));
  for (const std::string& label : descriptor.policy_labels) {
    writer.put_string(label, limits.max_identifier_size);
  }
  writer.put_u64(descriptor.placement_domain.value());
  writer.put_u64(descriptor.topology_epoch.value());
  writer.put_u32(descriptor.max_concurrency);
  writer.put_u64(descriptor.lease_ttl_ms);
  writer.put_string(descriptor.provenance, limits.max_string_size);
}

AgentDescriptor read_descriptor(ByteReader& reader, const ResourceLimits& limits) {
  AgentDescriptor descriptor;
  descriptor.id = AgentId{reader.get_u64()};
  descriptor.generation = AgentGeneration{reader.get_u64()};
  const std::uint64_t boot_high = reader.get_u64();
  const std::uint64_t boot_low = reader.get_u64();
  descriptor.boot = AgentBootId{Hash128{boot_high, boot_low}};
  descriptor.registration_generation = AgentRegistrationGeneration{reader.get_u64()};
  descriptor.tenant = TenantId{reader.get_u64()};
  descriptor.name_space = NamespaceId{reader.get_u64()};
  descriptor.display_name = reader.get_string(limits.max_string_size);
  const std::uint32_t label_count = reader.get_u32();
  if (label_count > limits.max_policy_labels) {
    throw DecodeError("policy label count exceeds the configured maximum");
  }
  descriptor.policy_labels.reserve(label_count);
  for (std::uint32_t index = 0; index < label_count; ++index) {
    descriptor.policy_labels.push_back(reader.get_string(limits.max_identifier_size));
  }
  descriptor.placement_domain = PlacementDomainId{reader.get_u64()};
  descriptor.topology_epoch = TopologyEpoch{reader.get_u64()};
  descriptor.max_concurrency = reader.get_u32();
  descriptor.lease_ttl_ms = reader.get_u64();
  descriptor.provenance = reader.get_string(limits.max_string_size);
  return descriptor;
}

void write_capabilities(ByteWriter& writer, const CapabilityProfile& profile, const ResourceLimits& limits) {
  writer.put_u64(profile.profile_id.value());
  writer.put_u64(profile.generation.value());
  writer.put_u64(profile.boot.value().high);
  writer.put_u64(profile.boot.value().low);
  writer.put_u32(static_cast<std::uint32_t>(profile.capabilities.size()));
  for (const CapabilityEvidence& evidence : profile.capabilities) {
    writer.put_string(evidence.name, limits.max_identifier_size);
    writer.put_u8(static_cast<std::uint8_t>(evidence.state));
    writer.put_u32(evidence.quality);
    writer.put_u64(evidence.generation.value());
    writer.put_u64(evidence.boot.value().high);
    writer.put_u64(evidence.boot.value().low);
    writer.put_u64(evidence.observed_at_ms);
    writer.put_string(evidence.provenance, limits.max_string_size);
  }
}

CapabilityProfile read_capabilities(ByteReader& reader, const ResourceLimits& limits) {
  CapabilityProfile profile;
  profile.profile_id = CapabilityProfileId{reader.get_u64()};
  profile.generation = AgentCapabilityGeneration{reader.get_u64()};
  const std::uint64_t boot_high = reader.get_u64();
  const std::uint64_t boot_low = reader.get_u64();
  profile.boot = AgentBootId{Hash128{boot_high, boot_low}};
  const std::uint32_t count = reader.get_u32();
  if (count > limits.max_capabilities_per_agent) {
    throw DecodeError("capability count exceeds the configured maximum");
  }
  profile.capabilities.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    CapabilityEvidence evidence;
    evidence.name = reader.get_string(limits.max_identifier_size);
    const std::uint8_t state = reader.get_u8();
    if (state > static_cast<std::uint8_t>(CapabilityState::Stale)) {
      throw DecodeError("capability state is out of range");
    }
    evidence.state = static_cast<CapabilityState>(state);
    evidence.quality = reader.get_u32();
    if (evidence.quality > 1000) {
      throw DecodeError("capability quality is out of range");
    }
    evidence.generation = AgentCapabilityGeneration{reader.get_u64()};
    const std::uint64_t evidence_high = reader.get_u64();
    const std::uint64_t evidence_low = reader.get_u64();
    evidence.boot = AgentBootId{Hash128{evidence_high, evidence_low}};
    evidence.observed_at_ms = reader.get_u64();
    evidence.provenance = reader.get_string(limits.max_string_size);
    profile.capabilities.push_back(std::move(evidence));
  }
  return profile;
}

void write_work_request(ByteWriter& writer, const WorkRequest& request, const ResourceLimits& limits) {
  writer.put_u64(request.id.value());
  writer.put_u64(request.generation.value());
  writer.put_u64(request.queue.value());
  writer.put_u64(request.queue_generation.value());
  writer.put_string(request.kind, limits.max_identifier_size);
  writer.put_u32(request.priority);
  writer.put_string(request.fairness_class, limits.max_identifier_size);
  writer.put_u8(static_cast<std::uint8_t>(request.mode));
  writer.put_string(request.payload_ref, limits.max_string_size);
  writer.put_u64(request.earliest_start_ms);
  writer.put_u64(request.expected_duration_ms);
  const WorkRequirements& requirements = request.requirements;
  writer.put_u32(static_cast<std::uint32_t>(requirements.capabilities.size()));
  for (const CapabilityRequirement& capability : requirements.capabilities) {
    writer.put_string(capability.name, limits.max_identifier_size);
    writer.put_u8(static_cast<std::uint8_t>(capability.mode));
    writer.put_u8(static_cast<std::uint8_t>(capability.minimum_state));
    writer.put_u32(capability.minimum_quality);
  }
  writer.put_u64(requirements.tenant.value());
  writer.put_u64(requirements.name_space.value());
  writer.put_u32(static_cast<std::uint32_t>(requirements.required_policy_labels.size()));
  for (const std::string& label : requirements.required_policy_labels) {
    writer.put_string(label, limits.max_identifier_size);
  }
  writer.put_u32(static_cast<std::uint32_t>(requirements.affinity.size()));
  for (const AgentId agent : requirements.affinity) {
    writer.put_u64(agent.value());
  }
  writer.put_u32(static_cast<std::uint32_t>(requirements.anti_affinity.size()));
  for (const AgentId agent : requirements.anti_affinity) {
    writer.put_u64(agent.value());
  }
  writer.put_bool(requirements.restrict_placement_domain);
  writer.put_u64(requirements.placement_domain.value());
  writer.put_u64(requirements.topology_epoch.value());
  writer.put_bool(requirements.require_healthy);
  writer.put_bool(requirements.require_reachable);
  writer.put_bool(requirements.require_ready);
  writer.put_bool(requirements.require_current_lease);
  writer.put_bool(requirements.require_resource_feasibility);
  writer.put_u64(requirements.resource_generation.value());
  writer.put_u8(static_cast<std::uint8_t>(requirements.resource_feasibility));
  writer.put_bool(requirements.require_budget_feasibility);
  writer.put_u64(requirements.budget_generation.value());
  writer.put_u8(static_cast<std::uint8_t>(requirements.budget_feasibility));
  writer.put_bool(requirements.require_slo_feasibility);
  writer.put_u64(requirements.slo_generation.value());
  writer.put_u8(static_cast<std::uint8_t>(requirements.slo_feasibility));
  writer.put_bool(requirements.require_reservation);
  writer.put_u64(requirements.reservation_generation.value());
  writer.put_u8(static_cast<std::uint8_t>(requirements.reservation_feasibility));
  writer.put_bool(requirements.require_warm_state);
  writer.put_u32(static_cast<std::uint32_t>(requirements.warm_state_keys.size()));
  for (const std::string& key : requirements.warm_state_keys) {
    writer.put_string(key, limits.max_identifier_size);
  }
  writer.put_u64(requirements.deadline_ms);
  writer.put_u32(requirements.max_parallel);
  writer.put_u32(requirements.max_assignments_per_agent);
}

WorkRequest read_work_request(ByteReader& reader, const ResourceLimits& limits) {
  WorkRequest request;
  request.id = WorkId{reader.get_u64()};
  request.generation = WorkGeneration{reader.get_u64()};
  request.queue = QueueId{reader.get_u64()};
  request.queue_generation = QueueGeneration{reader.get_u64()};
  request.kind = reader.get_string(limits.max_identifier_size);
  request.priority = reader.get_u32();
  request.fairness_class = reader.get_string(limits.max_identifier_size);
  const std::uint8_t mode = reader.get_u8();
  if (mode > static_cast<std::uint8_t>(SchedulingMode::Parallel)) {
    throw DecodeError("scheduling mode is out of range");
  }
  request.mode = static_cast<SchedulingMode>(mode);
  request.payload_ref = reader.get_string(limits.max_string_size);
  request.earliest_start_ms = reader.get_u64();
  request.expected_duration_ms = reader.get_u64();
  WorkRequirements& requirements = request.requirements;
  const std::uint32_t capability_count = reader.get_u32();
  if (capability_count > limits.max_capability_requirements) {
    throw DecodeError("capability requirement count exceeds the configured maximum");
  }
  requirements.capabilities.reserve(capability_count);
  for (std::uint32_t index = 0; index < capability_count; ++index) {
    CapabilityRequirement capability;
    capability.name = reader.get_string(limits.max_identifier_size);
    const std::uint8_t requirement_mode = reader.get_u8();
    if (requirement_mode > static_cast<std::uint8_t>(CapabilityRequirementMode::Preferred)) {
      throw DecodeError("capability requirement mode is out of range");
    }
    capability.mode = static_cast<CapabilityRequirementMode>(requirement_mode);
    const std::uint8_t minimum_state = reader.get_u8();
    if (minimum_state > static_cast<std::uint8_t>(CapabilityState::Stale)) {
      throw DecodeError("capability minimum state is out of range");
    }
    capability.minimum_state = static_cast<CapabilityState>(minimum_state);
    capability.minimum_quality = reader.get_u32();
    requirements.capabilities.push_back(std::move(capability));
  }
  requirements.tenant = TenantId{reader.get_u64()};
  requirements.name_space = NamespaceId{reader.get_u64()};
  const std::uint32_t label_count = reader.get_u32();
  if (label_count > limits.max_policy_labels) {
    throw DecodeError("policy label count exceeds the configured maximum");
  }
  for (std::uint32_t index = 0; index < label_count; ++index) {
    requirements.required_policy_labels.push_back(reader.get_string(limits.max_identifier_size));
  }
  const std::uint32_t affinity_count = reader.get_u32();
  if (affinity_count > limits.max_affinity_entries) {
    throw DecodeError("affinity count exceeds the configured maximum");
  }
  for (std::uint32_t index = 0; index < affinity_count; ++index) {
    requirements.affinity.push_back(AgentId{reader.get_u64()});
  }
  const std::uint32_t anti_count = reader.get_u32();
  if (anti_count > limits.max_affinity_entries) {
    throw DecodeError("anti-affinity count exceeds the configured maximum");
  }
  for (std::uint32_t index = 0; index < anti_count; ++index) {
    requirements.anti_affinity.push_back(AgentId{reader.get_u64()});
  }
  requirements.restrict_placement_domain = reader.get_bool();
  requirements.placement_domain = PlacementDomainId{reader.get_u64()};
  requirements.topology_epoch = TopologyEpoch{reader.get_u64()};
  requirements.require_healthy = reader.get_bool();
  requirements.require_reachable = reader.get_bool();
  requirements.require_ready = reader.get_bool();
  requirements.require_current_lease = reader.get_bool();
  requirements.require_resource_feasibility = reader.get_bool();
  requirements.resource_generation = ResourceGeneration{reader.get_u64()};
  requirements.resource_feasibility = static_cast<FeasibilityVerdict>(reader.get_u8());
  requirements.require_budget_feasibility = reader.get_bool();
  requirements.budget_generation = BudgetGeneration{reader.get_u64()};
  requirements.budget_feasibility = static_cast<FeasibilityVerdict>(reader.get_u8());
  requirements.require_slo_feasibility = reader.get_bool();
  requirements.slo_generation = SloGeneration{reader.get_u64()};
  requirements.slo_feasibility = static_cast<FeasibilityVerdict>(reader.get_u8());
  requirements.require_reservation = reader.get_bool();
  requirements.reservation_generation = ReservationGeneration{reader.get_u64()};
  requirements.reservation_feasibility = static_cast<FeasibilityVerdict>(reader.get_u8());
  requirements.require_warm_state = reader.get_bool();
  const std::uint32_t warm_count = reader.get_u32();
  if (warm_count > limits.max_warm_state_keys) {
    throw DecodeError("warm state key count exceeds the configured maximum");
  }
  for (std::uint32_t index = 0; index < warm_count; ++index) {
    requirements.warm_state_keys.push_back(reader.get_string(limits.max_identifier_size));
  }
  requirements.deadline_ms = reader.get_u64();
  requirements.max_parallel = reader.get_u32();
  requirements.max_assignments_per_agent = reader.get_u32();
  if (requirements.resource_feasibility > FeasibilityVerdict::Infeasible ||
      requirements.budget_feasibility > FeasibilityVerdict::Infeasible ||
      requirements.slo_feasibility > FeasibilityVerdict::Infeasible ||
      requirements.reservation_feasibility > FeasibilityVerdict::Infeasible) {
    throw DecodeError("feasibility verdict is out of range");
  }
  return request;
}

void write_binding(ByteWriter& writer, const AssignmentBinding& binding) {
  writer.put_u64(binding.id.value().high);
  writer.put_u64(binding.id.value().low);
  writer.put_u64(binding.generation.value());
  writer.put_u64(binding.work.value());
  writer.put_u64(binding.work_generation.value());
  writer.put_u64(binding.agent.value());
  writer.put_u64(binding.agent_generation.value());
  writer.put_u64(binding.boot.value().high);
  writer.put_u64(binding.boot.value().low);
  writer.put_u64(binding.scheduler.value());
  writer.put_u64(binding.scheduler_epoch.value());
  writer.put_u64(binding.coordinator_epoch.value());
  writer.put_u64(binding.policy.value());
  writer.put_u64(binding.policy_generation.value());
  writer.put_u64(binding.capability_profile.value());
  writer.put_u64(binding.capability_generation.value());
  writer.put_u64(binding.resource_generation.value());
  writer.put_u64(binding.budget_generation.value());
  writer.put_u64(binding.slo_generation.value());
  writer.put_u64(binding.reservation_generation.value());
  writer.put_u64(binding.queue.value());
  writer.put_u64(binding.queue_generation.value());
  writer.put_u64(binding.lease.value().high);
  writer.put_u64(binding.lease.value().low);
  writer.put_u64(binding.lease_generation.value());
  writer.put_u64(binding.dispatch.value().high);
  writer.put_u64(binding.dispatch.value().low);
  writer.put_u64(binding.dispatch_generation.value());
  writer.put_u64(binding.placement_domain.value());
  writer.put_u64(binding.topology_epoch.value());
  writer.put_u64(binding.created_at_ms);
  writer.put_u64(binding.lease_expires_at_ms);
  writer.put_u32(binding.replica_index);
  writer.put_bool(binding.exclusive);
}

AssignmentBinding read_binding(ByteReader& reader) {
  AssignmentBinding binding;
  const std::uint64_t id_high = reader.get_u64();
  const std::uint64_t id_low = reader.get_u64();
  binding.id = AssignmentId{Hash128{id_high, id_low}};
  binding.generation = AssignmentGeneration{reader.get_u64()};
  binding.work = WorkId{reader.get_u64()};
  binding.work_generation = WorkGeneration{reader.get_u64()};
  binding.agent = AgentId{reader.get_u64()};
  binding.agent_generation = AgentGeneration{reader.get_u64()};
  const std::uint64_t boot_high = reader.get_u64();
  const std::uint64_t boot_low = reader.get_u64();
  binding.boot = AgentBootId{Hash128{boot_high, boot_low}};
  binding.scheduler = SchedulerId{reader.get_u64()};
  binding.scheduler_epoch = SchedulerEpoch{reader.get_u64()};
  binding.coordinator_epoch = CoordinatorEpoch{reader.get_u64()};
  binding.policy = PolicyId{reader.get_u64()};
  binding.policy_generation = PolicyGeneration{reader.get_u64()};
  binding.capability_profile = CapabilityProfileId{reader.get_u64()};
  binding.capability_generation = AgentCapabilityGeneration{reader.get_u64()};
  binding.resource_generation = ResourceGeneration{reader.get_u64()};
  binding.budget_generation = BudgetGeneration{reader.get_u64()};
  binding.slo_generation = SloGeneration{reader.get_u64()};
  binding.reservation_generation = ReservationGeneration{reader.get_u64()};
  binding.queue = QueueId{reader.get_u64()};
  binding.queue_generation = QueueGeneration{reader.get_u64()};
  const std::uint64_t lease_high = reader.get_u64();
  const std::uint64_t lease_low = reader.get_u64();
  binding.lease = LeaseId{Hash128{lease_high, lease_low}};
  binding.lease_generation = LeaseGeneration{reader.get_u64()};
  const std::uint64_t dispatch_high = reader.get_u64();
  const std::uint64_t dispatch_low = reader.get_u64();
  binding.dispatch = DispatchId{Hash128{dispatch_high, dispatch_low}};
  binding.dispatch_generation = DispatchGeneration{reader.get_u64()};
  binding.placement_domain = PlacementDomainId{reader.get_u64()};
  binding.topology_epoch = TopologyEpoch{reader.get_u64()};
  binding.created_at_ms = reader.get_u64();
  binding.lease_expires_at_ms = reader.get_u64();
  binding.replica_index = reader.get_u32();
  binding.exclusive = reader.get_bool();
  return binding;
}

/// Decoded candidate state. Nothing here is applied until every check passes.
struct DecodedState {
  SchedulerId scheduler_id{};
  SchedulerEpoch scheduler_epoch{};
  CoordinatorEpoch coordinator_epoch{};
  QueueGeneration queue_generation{};
  std::uint64_t next_admission_sequence{1};
  std::uint64_t assignments_created{0};
  std::uint64_t assignments_invalidated{0};
  std::uint64_t scheduling_rounds{0};
  bool admission_open{false};
  bool shutting_down{false};
  PolicySnapshot policy;
  std::map<QueueId, QueueRecord> queues;
  std::map<TenantId, TenantRecord> tenants;
  std::map<AgentId, AgentRecord> agents;
  std::map<WorkId, WorkRecord> work;
  std::map<AssignmentId, AssignmentRecord> assignments;
  std::map<LeaseId, LeaseRecord> leases;
  std::map<AgentBootId, FencedBoot> fenced_boots;
};

void write_policy(ByteWriter& writer, const PolicySnapshot& policy, const ResourceLimits& limits) {
  writer.put_u64(policy.id.value());
  writer.put_u64(policy.generation.value());
  writer.put_u32(policy.aging_rounds_per_step);
  writer.put_u32(policy.max_priority_bypass);
  writer.put_u32(policy.starvation_alert_rounds);
  writer.put_bool(policy.allow_unbounded_starvation);
  writer.put_u32(policy.anti_concentration_window);
  writer.put_u32(policy.anti_concentration_penalty);
  writer.put_u64(policy.capability_staleness_ms);
  writer.put_u64(policy.health_staleness_ms);
  writer.put_u64(policy.load_staleness_ms);
  writer.put_u64(policy.availability_staleness_ms);
  writer.put_u64(policy.assignment_lease_ttl_ms);
  writer.put_u64(policy.registration_lease_ttl_ms);
  writer.put_bool(policy.close_dispatch_authority_on_shutdown);
  writer.put_u32(static_cast<std::uint32_t>(policy.ranking_weights.size()));
  for (const RankingWeight& weight : policy.ranking_weights) {
    writer.put_string(weight.factor, limits.max_identifier_size);
    writer.put_u32(weight.weight);
  }
  writer.put_u32(static_cast<std::uint32_t>(policy.fairness_class_weights.size()));
  for (const QueueClassWeight& weight : policy.fairness_class_weights) {
    writer.put_string(weight.fairness_class, limits.max_identifier_size);
    writer.put_u32(weight.weight);
  }
}

PolicySnapshot read_policy(ByteReader& reader, const ResourceLimits& limits) {
  PolicySnapshot policy;
  policy.id = PolicyId{reader.get_u64()};
  policy.generation = PolicyGeneration{reader.get_u64()};
  policy.aging_rounds_per_step = reader.get_u32();
  policy.max_priority_bypass = reader.get_u32();
  policy.starvation_alert_rounds = reader.get_u32();
  policy.allow_unbounded_starvation = reader.get_bool();
  policy.anti_concentration_window = reader.get_u32();
  policy.anti_concentration_penalty = reader.get_u32();
  policy.capability_staleness_ms = reader.get_u64();
  policy.health_staleness_ms = reader.get_u64();
  policy.load_staleness_ms = reader.get_u64();
  policy.availability_staleness_ms = reader.get_u64();
  policy.assignment_lease_ttl_ms = reader.get_u64();
  policy.registration_lease_ttl_ms = reader.get_u64();
  policy.close_dispatch_authority_on_shutdown = reader.get_bool();
  const std::uint32_t ranking_count = reader.get_u32();
  if (ranking_count > limits.max_explanation_factors) {
    throw DecodeError("ranking weight count exceeds the configured maximum");
  }
  for (std::uint32_t index = 0; index < ranking_count; ++index) {
    RankingWeight weight;
    weight.factor = reader.get_string(limits.max_identifier_size);
    weight.weight = reader.get_u32();
    policy.ranking_weights.push_back(std::move(weight));
  }
  const std::uint32_t class_count = reader.get_u32();
  if (class_count > limits.max_queues) {
    throw DecodeError("fairness class count exceeds the configured maximum");
  }
  for (std::uint32_t index = 0; index < class_count; ++index) {
    QueueClassWeight weight;
    weight.fairness_class = reader.get_string(limits.max_identifier_size);
    weight.weight = reader.get_u32();
    policy.fairness_class_weights.push_back(std::move(weight));
  }
  return policy;
}

}  // namespace
}  // namespace agent_scheduler

namespace agent_scheduler {

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

MutationResult AgentScheduler::save(const std::filesystem::path& path,
                                    const PersistOptions& options) const {
  std::vector<std::uint8_t> payload;
  std::uint64_t record_count = 0;
  {
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    ByteWriter writer;
    writer.put_u32(kSectionCount);

    writer.put_u8(kSectionScheduler);
    writer.put_u64(impl_->scheduler_id.value());
    writer.put_u64(impl_->scheduler_epoch.value());
    writer.put_u64(impl_->coordinator_epoch.value());
    writer.put_u64(impl_->queue_generation.value());
    writer.put_u64(impl_->next_admission_sequence);
    writer.put_u64(impl_->assignments_created);
    writer.put_u64(impl_->assignments_invalidated);
    writer.put_u64(impl_->scheduling_rounds);
    writer.put_bool(impl_->admission_open);
    writer.put_bool(impl_->shutting_down);

    writer.put_u8(kSectionPolicy);
    write_policy(writer, impl_->policy, impl_->limits);

    writer.put_u8(kSectionQueues);
    writer.put_u32(static_cast<std::uint32_t>(impl_->queues.size()));
    for (const auto& entry : impl_->queues) {
      writer.put_u64(entry.second.id.value());
      writer.put_u64(entry.second.generation.value());
      writer.put_string(entry.second.fairness_class, impl_->limits.max_identifier_size);
      writer.put_u32(entry.second.weight);
      writer.put_u64(entry.second.admitted_count);
      writer.put_u64(entry.second.assigned_count);
      writer.put_u64(entry.second.completed_count);
      writer.put_u64(entry.second.cancelled_count);
      ++record_count;
    }

    writer.put_u8(kSectionTenants);
    writer.put_u32(static_cast<std::uint32_t>(impl_->tenants.size()));
    for (const auto& entry : impl_->tenants) {
      writer.put_u64(entry.second.id.value());
      writer.put_u64(entry.second.admitted_count);
      writer.put_u64(entry.second.active_assignments);
      ++record_count;
    }

    writer.put_u8(kSectionAgents);
    writer.put_u32(static_cast<std::uint32_t>(impl_->agents.size()));
    for (const auto& entry : impl_->agents) {
      write_descriptor(writer, entry.second.descriptor, impl_->limits);
      writer.put_u8(lifecycle_hint(entry.second.lifecycle));
      writer.put_u64(entry.second.assignments_completed);
      writer.put_u64(entry.second.assignments_failed);
      writer.put_u64(entry.second.registered_at_ms);
      writer.put_u32(entry.second.recent_assignments);
      writer.put_string(entry.second.reason, impl_->limits.max_string_size);
      write_capabilities(writer, entry.second.profile, impl_->limits);
      ++record_count;
    }

    writer.put_u8(kSectionWork);
    writer.put_u32(static_cast<std::uint32_t>(impl_->work.size()));
    for (const auto& entry : impl_->work) {
      write_work_request(writer, entry.second.request, impl_->limits);
      writer.put_u8(work_lifecycle_hint(entry.second.lifecycle));
      writer.put_u64(entry.second.admitted_sequence);
      writer.put_u64(entry.second.admitted_at_ms);
      writer.put_u32(entry.second.bypass_count);
      writer.put_u32(entry.second.starvation_rounds);
      writer.put_u64(entry.second.last_assignment_generation.value());
      writer.put_string(entry.second.reason, impl_->limits.max_string_size);
      ++record_count;
    }

    writer.put_u8(kSectionAssignments);
    writer.put_u32(static_cast<std::uint32_t>(impl_->assignments.size()));
    for (const auto& entry : impl_->assignments) {
      write_binding(writer, entry.second.binding);
      writer.put_u8(static_cast<std::uint8_t>(entry.second.state));
      writer.put_u8(static_cast<std::uint8_t>(entry.second.invalidation));
      writer.put_u64(entry.second.dispatched_at_ms);
      writer.put_u64(entry.second.acknowledged_at_ms);
      writer.put_u64(entry.second.closed_at_ms);
      writer.put_string(entry.second.reason, impl_->limits.max_string_size);
      ++record_count;
    }

    writer.put_u8(kSectionLeases);
    writer.put_u32(static_cast<std::uint32_t>(impl_->leases.size()));
    for (const auto& entry : impl_->leases) {
      writer.put_u64(entry.second.lease.id.value().high);
      writer.put_u64(entry.second.lease.id.value().low);
      writer.put_u64(entry.second.lease.generation.value());
      writer.put_u64(entry.second.lease.assignment.value().high);
      writer.put_u64(entry.second.lease.assignment.value().low);
      writer.put_u64(entry.second.lease.assignment_generation.value());
      writer.put_u64(entry.second.lease.agent.value());
      writer.put_u64(entry.second.lease.boot.value().high);
      writer.put_u64(entry.second.lease.boot.value().low);
      writer.put_u64(entry.second.lease.granted_at_ms);
      writer.put_u64(entry.second.lease.expires_at_ms);
      ++record_count;
    }

    writer.put_u8(kSectionFenced);
    writer.put_u32(static_cast<std::uint32_t>(impl_->fenced_boots.size()));
    for (const auto& entry : impl_->fenced_boots) {
      writer.put_u64(entry.second.agent.value());
      writer.put_u64(entry.second.boot.value().high);
      writer.put_u64(entry.second.boot.value().low);
      writer.put_u64(entry.second.generation.value());
      writer.put_u64(entry.second.fenced_at_ms);
      writer.put_string(entry.second.reason, impl_->limits.max_string_size);
      ++record_count;
    }
    payload = writer.take();
  }

  const std::uint64_t max_bytes =
      options.max_bytes != 0 ? options.max_bytes : impl_->limits.max_persistence_bytes;
  if (payload.size() > max_bytes) {
    return persistence_failure(ErrorCode::LimitExceeded, "save", path.string(),
                               "encoded state exceeds the configured persistence byte limit");
  }
  const Digest256 semantic = sha256(payload);
  const std::uint32_t payload_crc = crc32(payload);

  std::vector<std::uint8_t> file;
  file.reserve(PersistenceFormat::header_bytes + payload.size() + PersistenceFormat::trailer_bytes);
  {
    ByteWriter header;
    header.put_raw(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(PersistenceFormat::magic.data()),
        PersistenceFormat::magic.size()));
    header.put_u32(PersistenceFormat::version);
    header.put_u32(0);
    header.put_u64(payload.size());
    header.put_u64(record_count);
    header.put_u32(payload_crc);
    header.put_u32(0);  // placeholder for header crc
    header.put_digest(semantic);
    std::vector<std::uint8_t> bytes = header.take();
    const std::uint32_t header_crc = crc32(bytes);
    std::memcpy(bytes.data() + 36, &header_crc, sizeof(header_crc));
    file = std::move(bytes);
  }
  file.insert(file.end(), payload.begin(), payload.end());
  {
    ByteWriter trailer;
    trailer.put_raw(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(PersistenceFormat::magic.data()),
        PersistenceFormat::magic.size()));
    trailer.put_u32(0);
    trailer.put_u32(0);
    std::vector<std::uint8_t> bytes = trailer.take();
    const std::uint32_t trailer_crc = crc32(file) ^ 0x5A5A5A5Au;
    std::memcpy(bytes.data() + 8, &trailer_crc, sizeof(trailer_crc));
    file.insert(file.end(), bytes.begin(), bytes.end());
  }

  return internal::write_atomically(path, file, options);
}

RecoveryResult AgentScheduler::load(const std::filesystem::path& path, const LoadOptions& options) {
  RecoveryResult result;
  {
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    if (impl_->running) {
      result.error = make_error(ErrorCode::AlreadyStarted, "load", path.string(),
                                "recovery requires a stopped scheduler", ScheduleOutcome::NoChange);
      return result;
    }
  }
  std::vector<std::uint8_t> file;
  if (const auto read_result = internal::read_all(path, impl_->limits.max_persistence_bytes, file);
      !read_result.ok()) {
    result.error = read_result.error;
    return result;
  }
  result.bytes_read = file.size();
  PersistenceHeaderInfo info;
  std::vector<std::uint8_t> payload;
  if (const auto verify_result = internal::verify_file(file, info, payload); !verify_result.ok()) {
    result.error = verify_result.error;
    return result;
  }
  result.format_version = info.format_version;
  result.stored_digest = info.semantic_digest;
  result.computed_digest = sha256(payload);
  if (options.require_semantic_digest && result.stored_digest != result.computed_digest) {
    result.error = make_error(ErrorCode::PersistenceIntegrity, "load", path.string(),
                              "semantic state digest does not match the stored digest",
                              ScheduleOutcome::RejectInvalidRequest);
    return result;
  }

  DecodedState decoded;
  try {
    ByteReader reader(payload);
    const std::uint32_t section_count = reader.get_u32();
    if (section_count != kSectionCount) {
      result.error = make_error(ErrorCode::PersistenceFormat, "load", path.string(),
                                "section count does not match the current schema",
                                ScheduleOutcome::RejectInvalidRequest);
      return result;
    }
    for (std::uint32_t section = 0; section < section_count; ++section) {
      const std::uint8_t tag = reader.get_u8();
      switch (tag) {
        case kSectionScheduler:
          decoded.scheduler_id = SchedulerId{reader.get_u64()};
          decoded.scheduler_epoch = SchedulerEpoch{reader.get_u64()};
          decoded.coordinator_epoch = CoordinatorEpoch{reader.get_u64()};
          decoded.queue_generation = QueueGeneration{reader.get_u64()};
          decoded.next_admission_sequence = reader.get_u64();
          decoded.assignments_created = reader.get_u64();
          decoded.assignments_invalidated = reader.get_u64();
          decoded.scheduling_rounds = reader.get_u64();
          decoded.admission_open = reader.get_bool();
          decoded.shutting_down = reader.get_bool();
          break;
        case kSectionPolicy:
          decoded.policy = read_policy(reader, impl_->limits);
          break;
        case kSectionQueues: {
          const std::uint32_t count = reader.get_u32();
          if (count > impl_->limits.max_queues) {
            throw DecodeError("queue count exceeds the configured maximum");
          }
          for (std::uint32_t index = 0; index < count; ++index) {
            QueueRecord record;
            record.id = QueueId{reader.get_u64()};
            record.generation = QueueGeneration{reader.get_u64()};
            record.fairness_class = reader.get_string(impl_->limits.max_identifier_size);
            record.weight = reader.get_u32();
            record.admitted_count = reader.get_u64();
            record.assigned_count = reader.get_u64();
            record.completed_count = reader.get_u64();
            record.cancelled_count = reader.get_u64();
            decoded.queues.emplace(record.id, std::move(record));
          }
          break;
        }
        case kSectionTenants: {
          const std::uint32_t count = reader.get_u32();
          if (count > impl_->limits.max_tenants) {
            throw DecodeError("tenant count exceeds the configured maximum");
          }
          for (std::uint32_t index = 0; index < count; ++index) {
            TenantRecord record;
            record.id = TenantId{reader.get_u64()};
            record.admitted_count = reader.get_u64();
            record.active_assignments = reader.get_u64();
            decoded.tenants.emplace(record.id, std::move(record));
          }
          break;
        }
        case kSectionAgents: {
          const std::uint32_t count = reader.get_u32();
          if (count > impl_->limits.max_agents) {
            throw DecodeError("agent count exceeds the configured maximum");
          }
          for (std::uint32_t index = 0; index < count; ++index) {
            AgentRecord record;
            record.descriptor = read_descriptor(reader, impl_->limits);
            const std::uint8_t hint = reader.get_u8();
            if (hint > kLifecycleLost) {
              throw DecodeError("agent lifecycle hint is out of range");
            }
            record.lifecycle = lifecycle_from_hint(hint);
            record.assignments_completed = reader.get_u64();
            record.assignments_failed = reader.get_u64();
            record.registered_at_ms = reader.get_u64();
            record.recent_assignments = reader.get_u32();
            record.reason = reader.get_string(impl_->limits.max_string_size);
            record.profile = read_capabilities(reader, impl_->limits);
            // Dynamic process-local evidence is never restored as current.
            record.capability_current = false;
            record.health_current = false;
            record.availability_current = false;
            record.load_current = false;
            record.health = AgentHealth::Unknown;
            record.readiness = AgentReadiness::Unknown;
            record.reachability = AgentReachability::Unknown;
            record.availability = AgentAvailability::Unknown;
            record.lease_expires_at_ms = 0;
            record.recovered = true;
            invalidate_evidence(record.profile);
            decoded.agents.emplace(record.descriptor.id, std::move(record));
          }
          break;
        }
        case kSectionWork: {
          const std::uint32_t count = reader.get_u32();
          if (count > impl_->limits.max_work_items) {
            throw DecodeError("work count exceeds the configured maximum");
          }
          for (std::uint32_t index = 0; index < count; ++index) {
            WorkRecord record;
            record.request = read_work_request(reader, impl_->limits);
            const std::uint8_t hint = reader.get_u8();
            record.lifecycle = work_lifecycle_from_hint(hint);
            if (record.lifecycle == WorkLifecycle::Unknown) {
              throw DecodeError("work lifecycle hint is out of range");
            }
            record.admitted_sequence = reader.get_u64();
            record.admitted_at_ms = reader.get_u64();
            record.bypass_count = reader.get_u32();
            record.starvation_rounds = reader.get_u32();
            record.last_assignment_generation = AssignmentGeneration{reader.get_u64()};
            record.reason = reader.get_string(impl_->limits.max_string_size);
            record.current_assignments = 0;
            if (record.lifecycle == WorkLifecycle::Assigned || record.lifecycle == WorkLifecycle::Executing) {
              record.lifecycle = WorkLifecycle::Admitted;
            }
            decoded.work.emplace(record.request.id, std::move(record));
          }
          break;
        }
        case kSectionAssignments: {
          const std::uint32_t count = reader.get_u32();
          if (count > impl_->limits.max_active_assignments + impl_->limits.max_historical_assignments) {
            throw DecodeError("assignment count exceeds the configured maximum");
          }
          for (std::uint32_t index = 0; index < count; ++index) {
            AssignmentRecord record;
            record.binding = read_binding(reader);
            const std::uint8_t state = reader.get_u8();
            if (state > static_cast<std::uint8_t>(AssignmentState::Cancelled)) {
              throw DecodeError("assignment state is out of range");
            }
            record.state = static_cast<AssignmentState>(state);
            const std::uint8_t invalidation = reader.get_u8();
            if (invalidation > static_cast<std::uint8_t>(InvalidationReason::LeaseReleased)) {
              throw DecodeError("assignment invalidation reason is out of range");
            }
            record.invalidation = static_cast<InvalidationReason>(invalidation);
            record.dispatched_at_ms = reader.get_u64();
            record.acknowledged_at_ms = reader.get_u64();
            record.closed_at_ms = reader.get_u64();
            record.reason = reader.get_string(impl_->limits.max_string_size);
            record.lease_current = false;
            if (assignment_state_is_active(record.state)) {
              // A recovered assignment never silently regains dispatch authority.
              record.state = AssignmentState::RevalidationRequired;
              record.binding.lease_expires_at_ms = 0;
              ++result.assignments_requiring_revalidation;
            }
            decoded.assignments.emplace(record.binding.id, std::move(record));
          }
          break;
        }
        case kSectionLeases: {
          const std::uint32_t count = reader.get_u32();
          if (count > impl_->limits.max_leases) {
            throw DecodeError("lease count exceeds the configured maximum");
          }
          for (std::uint32_t index = 0; index < count; ++index) {
            LeaseRecord record;
            const std::uint64_t id_high = reader.get_u64();
            const std::uint64_t id_low = reader.get_u64();
            record.lease.id = LeaseId{Hash128{id_high, id_low}};
            record.lease.generation = LeaseGeneration{reader.get_u64()};
            const std::uint64_t assignment_high = reader.get_u64();
            const std::uint64_t assignment_low = reader.get_u64();
            record.lease.assignment = AssignmentId{Hash128{assignment_high, assignment_low}};
            record.lease.assignment_generation = AssignmentGeneration{reader.get_u64()};
            record.lease.agent = AgentId{reader.get_u64()};
            const std::uint64_t boot_high = reader.get_u64();
            const std::uint64_t boot_low = reader.get_u64();
            record.lease.boot = AgentBootId{Hash128{boot_high, boot_low}};
            record.lease.granted_at_ms = reader.get_u64();
            record.lease.expires_at_ms = reader.get_u64();
            record.lease.current = false;
            decoded.leases.emplace(record.lease.id, std::move(record));
          }
          break;
        }
        case kSectionFenced: {
          const std::uint32_t count = reader.get_u32();
          if (count > impl_->limits.max_fenced_boots) {
            throw DecodeError("fenced boot count exceeds the configured maximum");
          }
          for (std::uint32_t index = 0; index < count; ++index) {
            FencedBoot record;
            record.agent = AgentId{reader.get_u64()};
            const std::uint64_t boot_high = reader.get_u64();
            const std::uint64_t boot_low = reader.get_u64();
            record.boot = AgentBootId{Hash128{boot_high, boot_low}};
            record.generation = AgentGeneration{reader.get_u64()};
            record.fenced_at_ms = reader.get_u64();
            record.reason = reader.get_string(impl_->limits.max_string_size);
            decoded.fenced_boots.emplace(record.boot, std::move(record));
          }
          break;
        }
        default:
          throw DecodeError("unknown persistence section tag");
      }
    }
    reader.require_exhausted();
  } catch (const DecodeError& error) {
    result.error = make_error(ErrorCode::PersistenceFormat, "load", path.string(),
                              std::string("candidate state decode failed: ") + error.what(),
                              ScheduleOutcome::RejectInvalidRequest);
    return result;
  } catch (const std::exception& error) {
    result.error = make_error(ErrorCode::PersistenceFormat, "load", path.string(),
                              std::string("candidate state decode failed: ") + error.what(),
                              ScheduleOutcome::RejectInvalidRequest);
    return result;
  }

  {
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    if (impl_->running) {
      result.error = make_error(ErrorCode::AlreadyStarted, "load", path.string(),
                                "recovery requires a stopped scheduler", ScheduleOutcome::NoChange);
      return result;
    }
    result.previous_scheduler_epoch = impl_->scheduler_epoch;
    result.previous_coordinator_epoch = impl_->coordinator_epoch;
    result.agents_loaded = decoded.agents.size();
    result.work_loaded = decoded.work.size();
    result.assignments_loaded = decoded.assignments.size();
    result.leases_invalidated = decoded.leases.size();
    result.fenced_boots_loaded = decoded.fenced_boots.size();
    result.queues_loaded = decoded.queues.size();
    result.dynamic_evidence_invalidated = decoded.agents.size();

    impl_->scheduler_id = decoded.scheduler_id.is_zero() ? impl_->scheduler_id : decoded.scheduler_id;
    impl_->scheduler_epoch = SchedulerEpoch{decoded.scheduler_epoch.value() + 1};
    impl_->coordinator_epoch = decoded.coordinator_epoch;
    impl_->queue_generation = decoded.queue_generation;
    impl_->next_admission_sequence = decoded.next_admission_sequence == 0 ? 1 : decoded.next_admission_sequence;
    impl_->assignments_created = decoded.assignments_created;
    impl_->assignments_invalidated = decoded.assignments_invalidated;
    impl_->scheduling_rounds = decoded.scheduling_rounds;
    impl_->policy = decoded.policy;
    impl_->queues = std::move(decoded.queues);
    impl_->tenants = std::move(decoded.tenants);
    impl_->agents = std::move(decoded.agents);
    impl_->work = std::move(decoded.work);
    impl_->assignments = std::move(decoded.assignments);
    impl_->leases = std::move(decoded.leases);
    impl_->fenced_boots = std::move(decoded.fenced_boots);
    impl_->running = false;
    impl_->admission_open = false;
    impl_->shutting_down = false;
    impl_->shutdown_at_ms = 0;
    impl_->recovered = true;
    internal::rebuild_indexes(*impl_);
    for (auto& entry : impl_->agents) {
      internal::refresh_agent_accounting(*impl_, entry.second);
    }
    for (auto& entry : impl_->work) {
      internal::refresh_work_accounting(*impl_, entry.second);
    }
    ++impl_->state_revision;
    result.new_scheduler_epoch = impl_->scheduler_epoch;
    result.new_coordinator_epoch = impl_->coordinator_epoch;
    result.detail = options.inspect_only ? "inspected" : "recovered conservatively";
  }
  return result;
}

std::string RecoveryResult::describe() const {
  if (error.has_value()) {
    return error->describe();
  }
  std::string out = "RECOVERED format=" + std::to_string(format_version);
  out += " scheduler_epoch=" + std::to_string(new_scheduler_epoch.value());
  out += " coordinator_epoch=" + std::to_string(new_coordinator_epoch.value());
  out += " agents=" + std::to_string(agents_loaded);
  out += " work=" + std::to_string(work_loaded);
  out += " assignments=" + std::to_string(assignments_loaded);
  out += " revalidation_required=" + std::to_string(assignments_requiring_revalidation);
  out += " leases_invalidated=" + std::to_string(leases_invalidated);
  out += " fenced=" + std::to_string(fenced_boots_loaded);
  out += " digest=" + computed_digest.to_string();
  return out;
}

}  // namespace agent_scheduler
