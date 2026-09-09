// Agent Scheduler — reference wire protocol (version 1).
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/net/protocol.hpp"

#include <string>
#include <utility>

namespace agent_scheduler::net {
namespace {

[[nodiscard]] std::uint8_t clamp_u8(std::uint32_t value) {
  return static_cast<std::uint8_t>(value & 0xFFu);
}

template <class Enum>
[[nodiscard]] Enum checked_enum(ByteReader& reader, std::uint32_t maximum, const char* field) {
  const std::uint8_t raw = reader.get_u8();
  if (raw > maximum) {
    throw DecodeError(std::string(field) + " is out of range");
  }
  return static_cast<Enum>(raw);
}

[[nodiscard]] bool is_finite_verdict(std::uint8_t value) {
  return value <= static_cast<std::uint8_t>(FeasibilityVerdict::Infeasible);
}

void encode_requirements(ByteWriter& writer, const WorkRequirements& requirements, const ResourceLimits& limits) {
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

void decode_requirements(ByteReader& reader, WorkRequirements& requirements, const ResourceLimits& limits) {
  const std::uint32_t capability_count = reader.get_u32();
  if (capability_count > limits.max_capability_requirements) {
    throw DecodeError("capability requirement count exceeds the configured maximum");
  }
  for (std::uint32_t index = 0; index < capability_count; ++index) {
    CapabilityRequirement capability;
    capability.name = reader.get_string(limits.max_identifier_size);
    capability.mode = checked_enum<CapabilityRequirementMode>(
        reader, static_cast<std::uint32_t>(CapabilityRequirementMode::Preferred),
        "capability requirement mode");
    capability.minimum_state = checked_enum<CapabilityState>(
        reader, static_cast<std::uint32_t>(CapabilityState::Stale), "capability minimum state");
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
  requirements.resource_feasibility = checked_enum<FeasibilityVerdict>(
      reader, static_cast<std::uint32_t>(FeasibilityVerdict::Infeasible), "resource feasibility");
  requirements.require_budget_feasibility = reader.get_bool();
  requirements.budget_generation = BudgetGeneration{reader.get_u64()};
  requirements.budget_feasibility = checked_enum<FeasibilityVerdict>(
      reader, static_cast<std::uint32_t>(FeasibilityVerdict::Infeasible), "budget feasibility");
  requirements.require_slo_feasibility = reader.get_bool();
  requirements.slo_generation = SloGeneration{reader.get_u64()};
  requirements.slo_feasibility = checked_enum<FeasibilityVerdict>(
      reader, static_cast<std::uint32_t>(FeasibilityVerdict::Infeasible), "slo feasibility");
  requirements.require_reservation = reader.get_bool();
  requirements.reservation_generation = ReservationGeneration{reader.get_u64()};
  requirements.reservation_feasibility = checked_enum<FeasibilityVerdict>(
      reader, static_cast<std::uint32_t>(FeasibilityVerdict::Infeasible), "reservation feasibility");
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
}

}  // namespace

const char* to_string(QueryKind value) noexcept {
  switch (value) {
    case QueryKind::Version: return "VERSION";
    case QueryKind::Summary: return "SUMMARY";
    case QueryKind::Agents: return "AGENTS";
    case QueryKind::Work: return "WORK";
    case QueryKind::Assignments: return "ASSIGNMENTS";
    case QueryKind::Leases: return "LEASES";
    case QueryKind::Fenced: return "FENCED";
    case QueryKind::Invariants: return "INVARIANTS";
    case QueryKind::Fairness: return "FAIRNESS";
    case QueryKind::Capabilities: return "CAPABILITIES";
    case QueryKind::Queues: return "QUEUES";
    case QueryKind::Policy: return "POLICY";
  }
  return "UNKNOWN";
}

bool is_known_query_kind(std::uint16_t value) noexcept {
  return value >= static_cast<std::uint16_t>(QueryKind::Version) &&
         value <= static_cast<std::uint16_t>(QueryKind::Policy);
}

void encode(ByteWriter& writer, const SchedulerError& error, const ResourceLimits& limits) {
  writer.put_u32(static_cast<std::uint32_t>(error.code));
  writer.put_u32(static_cast<std::uint32_t>(error.outcome));
  writer.put_string(error.operation, limits.max_identifier_size);
  writer.put_string(error.subject, limits.max_string_size);
  writer.put_u64(error.current_generation);
  writer.put_u64(error.expected_generation);
  writer.put_u32(static_cast<std::uint32_t>(error.factors.size()));
  for (const std::string& factor : error.factors) {
    writer.put_string(factor, limits.max_string_size);
  }
  writer.put_string(error.detail, limits.max_string_size);
}

void decode(ByteReader& reader, SchedulerError& error, const ResourceLimits& limits) {
  error.code = static_cast<ErrorCode>(reader.get_u32());
  error.outcome = static_cast<ScheduleOutcome>(reader.get_u32());
  error.operation = reader.get_string(limits.max_identifier_size);
  error.subject = reader.get_string(limits.max_string_size);
  error.current_generation = reader.get_u64();
  error.expected_generation = reader.get_u64();
  const std::uint32_t factor_count = reader.get_u32();
  if (factor_count > limits.max_explanation_factors) {
    throw DecodeError("error factor count exceeds the configured maximum");
  }
  for (std::uint32_t index = 0; index < factor_count; ++index) {
    error.factors.push_back(reader.get_string(limits.max_string_size));
  }
  error.detail = reader.get_string(limits.max_string_size);
}

void encode(ByteWriter& writer, const AgentDescriptor& descriptor, const ResourceLimits& limits) {
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

void decode(ByteReader& reader, AgentDescriptor& descriptor, const ResourceLimits& limits) {
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
  for (std::uint32_t index = 0; index < label_count; ++index) {
    descriptor.policy_labels.push_back(reader.get_string(limits.max_identifier_size));
  }
  descriptor.placement_domain = PlacementDomainId{reader.get_u64()};
  descriptor.topology_epoch = TopologyEpoch{reader.get_u64()};
  descriptor.max_concurrency = reader.get_u32();
  descriptor.lease_ttl_ms = reader.get_u64();
  descriptor.provenance = reader.get_string(limits.max_string_size);
}

void encode(ByteWriter& writer, const CapabilityProfile& profile, const ResourceLimits& limits) {
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

void decode(ByteReader& reader, CapabilityProfile& profile, const ResourceLimits& limits) {
  profile.profile_id = CapabilityProfileId{reader.get_u64()};
  profile.generation = AgentCapabilityGeneration{reader.get_u64()};
  const std::uint64_t boot_high = reader.get_u64();
  const std::uint64_t boot_low = reader.get_u64();
  profile.boot = AgentBootId{Hash128{boot_high, boot_low}};
  const std::uint32_t count = reader.get_u32();
  if (count > limits.max_capabilities_per_agent) {
    throw DecodeError("capability count exceeds the configured maximum");
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    CapabilityEvidence evidence;
    evidence.name = reader.get_string(limits.max_identifier_size);
    evidence.state = checked_enum<CapabilityState>(
        reader, static_cast<std::uint32_t>(CapabilityState::Stale), "capability state");
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
}

void encode(ByteWriter& writer, const HealthObservation& observation, const ResourceLimits& limits) {
  writer.put_u64(observation.generation.value());
  writer.put_u8(static_cast<std::uint8_t>(observation.health));
  writer.put_u32(observation.health_quality);
  writer.put_string(observation.detail, limits.max_string_size);
  writer.put_u64(observation.observed_at_ms);
}

void decode(ByteReader& reader, HealthObservation& observation, const ResourceLimits& limits) {
  observation.generation = AgentHealthGeneration{reader.get_u64()};
  observation.health = checked_enum<AgentHealth>(
      reader, static_cast<std::uint32_t>(AgentHealth::Unhealthy), "agent health");
  observation.health_quality = reader.get_u32();
  observation.detail = reader.get_string(limits.max_string_size);
  observation.observed_at_ms = reader.get_u64();
}

void encode(ByteWriter& writer, const AvailabilityObservation& observation, const ResourceLimits& limits) {
  (void)limits;
  writer.put_u64(observation.generation.value());
  writer.put_u8(static_cast<std::uint8_t>(observation.availability));
  writer.put_u8(static_cast<std::uint8_t>(observation.reachability));
  writer.put_u8(static_cast<std::uint8_t>(observation.readiness));
  writer.put_u64(observation.observed_at_ms);
}

void decode(ByteReader& reader, AvailabilityObservation& observation, const ResourceLimits& limits) {
  (void)limits;
  observation.generation = AgentAvailabilityGeneration{reader.get_u64()};
  observation.availability = checked_enum<AgentAvailability>(
      reader, static_cast<std::uint32_t>(AgentAvailability::Unavailable), "agent availability");
  observation.reachability = checked_enum<AgentReachability>(
      reader, static_cast<std::uint32_t>(AgentReachability::Unreachable), "agent reachability");
  observation.readiness = checked_enum<AgentReadiness>(
      reader, static_cast<std::uint32_t>(AgentReadiness::NotReady), "agent readiness");
  observation.observed_at_ms = reader.get_u64();
}

void encode(ByteWriter& writer, const LoadObservation& observation, const ResourceLimits& limits) {
  writer.put_u64(observation.generation.value());
  writer.put_u32(observation.reported_active_assignments);
  writer.put_u32(observation.max_concurrency);
  writer.put_u32(observation.queue_depth);
  writer.put_u32(observation.cpu_pressure_percent);
  writer.put_u64(observation.estimated_dispatch_latency_ms);
  writer.put_f64(observation.cost_index);
  writer.put_f64(observation.slo_headroom_fraction);
  writer.put_u32(static_cast<std::uint32_t>(observation.warm_state_keys.size()));
  for (const std::string& key : observation.warm_state_keys) {
    writer.put_string(key, limits.max_identifier_size);
  }
  writer.put_u64(observation.observed_at_ms);
}

void decode(ByteReader& reader, LoadObservation& observation, const ResourceLimits& limits) {
  observation.generation = AgentLoadGeneration{reader.get_u64()};
  observation.reported_active_assignments = reader.get_u32();
  observation.max_concurrency = reader.get_u32();
  observation.queue_depth = reader.get_u32();
  observation.cpu_pressure_percent = reader.get_u32();
  observation.estimated_dispatch_latency_ms = reader.get_u64();
  observation.cost_index = reader.get_f64();
  observation.slo_headroom_fraction = reader.get_f64();
  const std::uint32_t key_count = reader.get_u32();
  if (key_count > limits.max_warm_state_keys) {
    throw DecodeError("warm state key count exceeds the configured maximum");
  }
  for (std::uint32_t index = 0; index < key_count; ++index) {
    observation.warm_state_keys.push_back(reader.get_string(limits.max_identifier_size));
  }
  observation.observed_at_ms = reader.get_u64();
}

void encode(ByteWriter& writer, const WorkRequest& request, const ResourceLimits& limits) {
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
  encode_requirements(writer, request.requirements, limits);
}

void decode(ByteReader& reader, WorkRequest& request, const ResourceLimits& limits) {
  request.id = WorkId{reader.get_u64()};
  request.generation = WorkGeneration{reader.get_u64()};
  request.queue = QueueId{reader.get_u64()};
  request.queue_generation = QueueGeneration{reader.get_u64()};
  request.kind = reader.get_string(limits.max_identifier_size);
  request.priority = reader.get_u32();
  request.fairness_class = reader.get_string(limits.max_identifier_size);
  request.mode = checked_enum<SchedulingMode>(
      reader, static_cast<std::uint32_t>(SchedulingMode::Parallel), "scheduling mode");
  request.payload_ref = reader.get_string(limits.max_string_size);
  request.earliest_start_ms = reader.get_u64();
  request.expected_duration_ms = reader.get_u64();
  decode_requirements(reader, request.requirements, limits);
}

void encode(ByteWriter& writer, const AssignmentBinding& binding, const ResourceLimits& limits) {
  (void)limits;
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

void decode(ByteReader& reader, AssignmentBinding& binding, const ResourceLimits& limits) {
  (void)limits;
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
}

#define AGENT_SCHEDULER_ENCODE(name, ...)                                                  \
  void encode(ByteWriter& writer, const name& message, const ResourceLimits& limits) {    \
    (void)limits;                                                                         \
    __VA_ARGS__                                                                           \
  }
#define AGENT_SCHEDULER_DECODE(name, ...)                                                  \
  void decode(ByteReader& reader, name& message, const ResourceLimits& limits) {          \
    (void)limits;                                                                         \
    __VA_ARGS__                                                                           \
  }

AGENT_SCHEDULER_ENCODE(HelloMessage,
  (void)limits;
  writer.put_u32(message.protocol_version);
  writer.put_string(message.role, limits.max_identifier_size);
  writer.put_string(message.build, limits.max_string_size);
  writer.put_u64(message.nonce);)
AGENT_SCHEDULER_DECODE(HelloMessage,
  message.protocol_version = reader.get_u32();
  message.role = reader.get_string(limits.max_identifier_size);
  message.build = reader.get_string(limits.max_string_size);
  message.nonce = reader.get_u64();)

AGENT_SCHEDULER_ENCODE(RegisterAgentMessage,
  encode(writer, message.descriptor, limits);)
AGENT_SCHEDULER_DECODE(RegisterAgentMessage,
  decode(reader, message.descriptor, limits);)

AGENT_SCHEDULER_ENCODE(PublishCapabilitiesMessage,
  writer.put_u64(message.agent.value());
  writer.put_u64(message.boot.value().high);
  writer.put_u64(message.boot.value().low);
  encode(writer, message.profile, limits);)
AGENT_SCHEDULER_DECODE(PublishCapabilitiesMessage,
  message.agent = AgentId{reader.get_u64()};
  const std::uint64_t high = reader.get_u64();
  const std::uint64_t low = reader.get_u64();
  message.boot = AgentBootId{Hash128{high, low}};
  decode(reader, message.profile, limits);)

AGENT_SCHEDULER_ENCODE(PublishHealthMessage,
  writer.put_u64(message.agent.value());
  writer.put_u64(message.boot.value().high);
  writer.put_u64(message.boot.value().low);
  encode(writer, message.observation, limits);)
AGENT_SCHEDULER_DECODE(PublishHealthMessage,
  message.agent = AgentId{reader.get_u64()};
  const std::uint64_t high = reader.get_u64();
  const std::uint64_t low = reader.get_u64();
  message.boot = AgentBootId{Hash128{high, low}};
  decode(reader, message.observation, limits);)

AGENT_SCHEDULER_ENCODE(PublishAvailabilityMessage,
  writer.put_u64(message.agent.value());
  writer.put_u64(message.boot.value().high);
  writer.put_u64(message.boot.value().low);
  encode(writer, message.observation, limits);)
AGENT_SCHEDULER_DECODE(PublishAvailabilityMessage,
  message.agent = AgentId{reader.get_u64()};
  const std::uint64_t high = reader.get_u64();
  const std::uint64_t low = reader.get_u64();
  message.boot = AgentBootId{Hash128{high, low}};
  decode(reader, message.observation, limits);)

AGENT_SCHEDULER_ENCODE(PublishLoadMessage,
  writer.put_u64(message.agent.value());
  writer.put_u64(message.boot.value().high);
  writer.put_u64(message.boot.value().low);
  encode(writer, message.observation, limits);)
AGENT_SCHEDULER_DECODE(PublishLoadMessage,
  message.agent = AgentId{reader.get_u64()};
  const std::uint64_t high = reader.get_u64();
  const std::uint64_t low = reader.get_u64();
  message.boot = AgentBootId{Hash128{high, low}};
  decode(reader, message.observation, limits);)

AGENT_SCHEDULER_ENCODE(HeartbeatMessage,
  writer.put_u64(message.agent.value());
  writer.put_u64(message.boot.value().high);
  writer.put_u64(message.boot.value().low);
  writer.put_u64(message.registration_generation.value());)
AGENT_SCHEDULER_DECODE(HeartbeatMessage,
  message.agent = AgentId{reader.get_u64()};
  const std::uint64_t high = reader.get_u64();
  const std::uint64_t low = reader.get_u64();
  message.boot = AgentBootId{Hash128{high, low}};
  message.registration_generation = AgentRegistrationGeneration{reader.get_u64()};)

AGENT_SCHEDULER_ENCODE(SubmitWorkMessage,
  encode(writer, message.request, limits);)
AGENT_SCHEDULER_DECODE(SubmitWorkMessage,
  decode(reader, message.request, limits);)

AGENT_SCHEDULER_ENCODE(CancelWorkMessage,
  writer.put_u64(message.work.value());
  writer.put_u64(message.generation.value());
  writer.put_string(message.reason, limits.max_string_size);)
AGENT_SCHEDULER_DECODE(CancelWorkMessage,
  message.work = WorkId{reader.get_u64()};
  message.generation = WorkGeneration{reader.get_u64()};
  message.reason = reader.get_string(limits.max_string_size);)

AGENT_SCHEDULER_ENCODE(SupersedeWorkMessage,
  writer.put_u64(message.work.value());
  writer.put_u64(message.expected_generation.value());
  encode(writer, message.replacement, limits);)
AGENT_SCHEDULER_DECODE(SupersedeWorkMessage,
  message.work = WorkId{reader.get_u64()};
  message.expected_generation = WorkGeneration{reader.get_u64()};
  decode(reader, message.replacement, limits);)

AGENT_SCHEDULER_ENCODE(AssignWorkMessage,
  encode(writer, message.binding, limits);
  writer.put_string(message.kind, limits.max_identifier_size);
  writer.put_string(message.payload_ref, limits.max_string_size);)
AGENT_SCHEDULER_DECODE(AssignWorkMessage,
  decode(reader, message.binding, limits);
  message.kind = reader.get_string(limits.max_identifier_size);
  message.payload_ref = reader.get_string(limits.max_string_size);)

AGENT_SCHEDULER_ENCODE(AssignmentAckMessage,
  encode(writer, message.binding, limits);
  writer.put_bool(message.accepted);
  writer.put_string(message.reason, limits.max_string_size);)
AGENT_SCHEDULER_DECODE(AssignmentAckMessage,
  decode(reader, message.binding, limits);
  message.accepted = reader.get_bool();
  message.reason = reader.get_string(limits.max_string_size);)

AGENT_SCHEDULER_ENCODE(AssignmentObservationMessage,
  encode(writer, message.binding, limits);
  writer.put_string(message.reason, limits.max_string_size);)
AGENT_SCHEDULER_DECODE(AssignmentObservationMessage,
  decode(reader, message.binding, limits);
  message.reason = reader.get_string(limits.max_string_size);)

AGENT_SCHEDULER_ENCODE(DrainAgentMessage,
  writer.put_u64(message.agent.value());
  writer.put_u64(message.boot.value().high);
  writer.put_u64(message.boot.value().low);
  writer.put_bool(message.force);)
AGENT_SCHEDULER_DECODE(DrainAgentMessage,
  message.agent = AgentId{reader.get_u64()};
  const std::uint64_t high = reader.get_u64();
  const std::uint64_t low = reader.get_u64();
  message.boot = AgentBootId{Hash128{high, low}};
  message.force = reader.get_bool();)

AGENT_SCHEDULER_ENCODE(DeregisterAgentMessage,
  writer.put_u64(message.agent.value());
  writer.put_u64(message.boot.value().high);
  writer.put_u64(message.boot.value().low);
  writer.put_bool(message.force);)
AGENT_SCHEDULER_DECODE(DeregisterAgentMessage,
  message.agent = AgentId{reader.get_u64()};
  const std::uint64_t high = reader.get_u64();
  const std::uint64_t low = reader.get_u64();
  message.boot = AgentBootId{Hash128{high, low}};
  message.force = reader.get_bool();)

AGENT_SCHEDULER_ENCODE(DispatchAssignmentMessage,
  writer.put_u64(message.assignment.value().high);
  writer.put_u64(message.assignment.value().low);
  writer.put_u64(message.generation.value());)
AGENT_SCHEDULER_DECODE(DispatchAssignmentMessage,
  const std::uint64_t high = reader.get_u64();
  const std::uint64_t low = reader.get_u64();
  message.assignment = AssignmentId{Hash128{high, low}};
  message.generation = AssignmentGeneration{reader.get_u64()};)

AGENT_SCHEDULER_ENCODE(QueryMessage,
  writer.put_u16(static_cast<std::uint16_t>(message.kind));
  writer.put_u64(message.agent.value());
  writer.put_u64(message.boot.value().high);
  writer.put_u64(message.boot.value().low);
  writer.put_u64(message.work.value());
  writer.put_u64(message.assignment.value().high);
  writer.put_u64(message.assignment.value().low);)
AGENT_SCHEDULER_DECODE(QueryMessage,
  const std::uint16_t kind = reader.get_u16();
  if (!is_known_query_kind(kind)) {
    throw DecodeError("query kind is unknown");
  }
  message.kind = static_cast<QueryKind>(kind);
  message.agent = AgentId{reader.get_u64()};
  const std::uint64_t high = reader.get_u64();
  const std::uint64_t low = reader.get_u64();
  message.boot = AgentBootId{Hash128{high, low}};
  message.work = WorkId{reader.get_u64()};
  const std::uint64_t assignment_high = reader.get_u64();
  const std::uint64_t assignment_low = reader.get_u64();
  message.assignment = AssignmentId{Hash128{assignment_high, assignment_low}};)

AGENT_SCHEDULER_ENCODE(QueryResultMessage,
  writer.put_u16(static_cast<std::uint16_t>(message.kind));
  writer.put_string(message.json, limits.max_query_result_bytes);)
AGENT_SCHEDULER_DECODE(QueryResultMessage,
  const std::uint16_t kind = reader.get_u16();
  if (!is_known_query_kind(kind)) {
    throw DecodeError("query result kind is unknown");
  }
  message.kind = static_cast<QueryKind>(kind);
  message.json = reader.get_string(limits.max_query_result_bytes);)

AGENT_SCHEDULER_ENCODE(ErrorMessage,
  writer.put_u32(static_cast<std::uint32_t>(message.code));
  writer.put_string(message.operation, limits.max_identifier_size);
  writer.put_string(message.subject, limits.max_string_size);
  writer.put_string(message.detail, limits.max_string_size);)
AGENT_SCHEDULER_DECODE(ErrorMessage,
  message.code = static_cast<ErrorCode>(reader.get_u32());
  message.operation = reader.get_string(limits.max_identifier_size);
  message.subject = reader.get_string(limits.max_string_size);
  message.detail = reader.get_string(limits.max_string_size);)

AGENT_SCHEDULER_ENCODE(ResultMessage,
  writer.put_u16(static_cast<std::uint16_t>(message.request_type));
  writer.put_bool(message.ok);
  writer.put_u32(static_cast<std::uint32_t>(message.outcome));
  encode(writer, message.error, limits);)
AGENT_SCHEDULER_DECODE(ResultMessage,
  const std::uint16_t request_type = reader.get_u16();
  if (!is_known_message_type(request_type)) {
    throw DecodeError("result request type is unknown");
  }
  message.request_type = static_cast<MessageType>(request_type);
  message.ok = reader.get_bool();
  message.outcome = static_cast<ScheduleOutcome>(reader.get_u32());
  decode(reader, message.error, limits);)

#undef AGENT_SCHEDULER_ENCODE
#undef AGENT_SCHEDULER_DECODE

}  // namespace agent_scheduler::net
