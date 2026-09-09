// Agent Scheduler — public API: lifecycle, agents, work, policy, and assignment authority.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/scheduler.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "internal/scheduler_impl.hpp"
#include "internal/util.hpp"

namespace agent_scheduler {
namespace {

using internal::Impl;

[[nodiscard]] MutationResult rejected(ErrorCode code,
                                      std::string operation,
                                      std::string subject,
                                      std::string detail,
                                      ScheduleOutcome outcome) {
  return MutationResult::failure(
      make_error(code, std::move(operation), std::move(subject), std::move(detail), outcome));
}

[[nodiscard]] bool stale_boot(const Impl& impl, const AgentRecord& record, AgentBootId boot) {
  return record.descriptor.boot != boot || internal::boot_is_fenced(impl, boot);
}

[[nodiscard]] std::uint32_t effective_lease_ttl(const Impl& impl, const AgentDescriptor& descriptor) {
  const std::uint64_t ttl = descriptor.lease_ttl_ms != 0 ? descriptor.lease_ttl_ms
                                                         : impl.policy.registration_lease_ttl_ms;
  const std::uint64_t capped = std::min<std::uint64_t>(ttl, 24ull * 60 * 60 * 1000);
  return static_cast<std::uint32_t>(capped == 0 ? 1 : capped);
}

/// Refreshes derived accounting and indexes for one agent.
void refresh_agent(Impl& impl, AgentRecord& record) {
  internal::deindex_agent(impl, record.descriptor.id);
  internal::refresh_agent_accounting(impl, record);
  internal::index_agent(impl, record);
}

/// Creates or refreshes the queue record for a work request. Queue records are derived
/// bookkeeping; they never authorize anything on their own.
[[nodiscard]] MutationResult ensure_queue(Impl& impl, const WorkRequest& request) {
  const auto found = impl.queues.find(request.queue);
  if (found == impl.queues.end()) {
    if (impl.queues.size() >= impl.limits.max_queues) {
      return rejected(ErrorCode::LimitExceeded, "submit_work", request.id.to_string(),
                      "queue count exceeds max_queues", ScheduleOutcome::RejectLimitExceeded);
    }
    QueueRecord record;
    record.id = request.queue;
    record.generation = request.queue_generation;
    record.fairness_class = request.fairness_class;
    record.weight = fairness_class_weight(impl.policy, request.fairness_class);
    impl.queues.emplace(request.queue, std::move(record));
    return MutationResult::success();
  }
  if (request.queue_generation < found->second.generation) {
    return rejected(ErrorCode::StaleGeneration, "submit_work", request.id.to_string(),
                    "queue generation is stale", ScheduleOutcome::RejectStaleQueueGeneration);
  }
  if (request.queue_generation > found->second.generation) {
    found->second.generation = request.queue_generation;
    found->second.fairness_class = request.fairness_class;
    found->second.weight = fairness_class_weight(impl.policy, request.fairness_class);
  }
  return MutationResult::success();
}

[[nodiscard]] MutationResult ensure_tenant(Impl& impl, TenantId tenant) {
  if (tenant.is_zero()) {
    return MutationResult::success();
  }
  const auto found = impl.tenants.find(tenant);
  if (found != impl.tenants.end()) {
    return MutationResult::success();
  }
  if (impl.tenants.size() >= impl.limits.max_tenants) {
    return rejected(ErrorCode::LimitExceeded, "submit_work", tenant.to_string(),
                    "tenant count exceeds max_tenants", ScheduleOutcome::RejectLimitExceeded);
  }
  TenantRecord record;
  record.id = tenant;
  impl.tenants.emplace(tenant, record);
  return MutationResult::success();
}

void close_all_active(Impl& impl,
                      AgentBootId boot,
                      AssignmentState state,
                      InvalidationReason reason,
                      std::string detail) {
  for (auto& entry : impl.assignments) {
    AssignmentRecord& record = entry.second;
    if (record.binding.boot != boot || !assignment_state_is_active(record.state)) {
      continue;
    }
    internal::close_assignment(impl, record, state, reason, detail);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// construction and lifecycle
// ---------------------------------------------------------------------------

AgentScheduler::AgentScheduler(SchedulerOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {
  if (const auto problem = impl_->limits.validate()) {
    throw std::invalid_argument(std::string("Agent Scheduler: ") + std::string(*problem));
  }
  if (const auto problem = validate(impl_->policy, impl_->limits); !problem.ok()) {
    throw std::invalid_argument(std::string("Agent Scheduler: invalid policy: ") +
                                problem.error->describe());
  }
  if (impl_->options.auto_start) {
    (void)start();
  }
}

AgentScheduler::~AgentScheduler() = default;

SchedulerId AgentScheduler::id() const noexcept { return impl_->scheduler_id; }

SchedulerEpoch AgentScheduler::scheduler_epoch() const noexcept {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->scheduler_epoch;
}

CoordinatorEpoch AgentScheduler::coordinator_epoch() const noexcept {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->coordinator_epoch;
}

bool AgentScheduler::running() const noexcept {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->running;
}

const ResourceLimits& AgentScheduler::limits() const noexcept { return impl_->limits; }

void AgentScheduler::set_shutdown_notifier(std::function<void()> notifier) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->shutdown_notifier = std::move(notifier);
}

MutationResult AgentScheduler::start() {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->running) {
    return MutationResult::success();
  }
  // Process authority advances on every start of a process instance.
  impl_->coordinator_epoch = CoordinatorEpoch{impl_->coordinator_epoch.value() + 1};
  impl_->running = true;
  impl_->admission_open = true;
  impl_->shutting_down = false;
  impl_->shutdown_at_ms = 0;
  ++impl_->state_revision;
  return MutationResult::success();
}

MutationResult AgentScheduler::shutdown() {
  std::function<void()> notifier;
  {
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    if (impl_->shutting_down) {
      return MutationResult::success();
    }
    impl_->shutting_down = true;
    impl_->admission_open = false;
    impl_->running = false;
    impl_->shutdown_at_ms = impl_->now_ms();
    if (impl_->policy.close_dispatch_authority_on_shutdown) {
      for (auto& entry : impl_->assignments) {
        AssignmentRecord& record = entry.second;
        if (record.state == AssignmentState::Assigned) {
          internal::close_assignment(*impl_, record, AssignmentState::Invalidated,
                                     InvalidationReason::Shutdown,
                                     "dispatch authority closed by shutdown");
        }
      }
    }
    ++impl_->state_revision;
    notifier = impl_->shutdown_notifier;
  }
  if (notifier) {
    notifier();
  }
  return MutationResult::success();
}

// ---------------------------------------------------------------------------
// policy and queues
// ---------------------------------------------------------------------------

MutationResult AgentScheduler::set_policy(const PolicySnapshot& policy) {
  if (const auto problem = validate(policy, impl_->limits); !problem.ok()) {
    return problem;
  }
  {
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    if (policy.generation < impl_->policy.generation) {
      return rejected(ErrorCode::StaleGeneration, "set_policy", policy.id.to_string(),
                      "policy generation is stale", ScheduleOutcome::RejectStalePolicy);
    }
    if (policy.generation == impl_->policy.generation) {
      if (policy == impl_->policy) {
        return MutationResult::success();
      }
      return rejected(ErrorCode::Conflict, "set_policy", policy.id.to_string(),
                      "conflicting policy publication for the current generation",
                      ScheduleOutcome::RejectConflict);
    }
    impl_->policy = policy;
    ++impl_->state_revision;
    for (auto& entry : impl_->assignments) {
      AssignmentRecord& record = entry.second;
      if (!assignment_state_is_active(record.state)) {
        continue;
      }
      if (record.binding.policy_generation != policy.generation) {
        internal::close_assignment(*impl_, record, AssignmentState::Invalidated,
                                   InvalidationReason::PolicyChanged,
                                   "policy generation moved under an active assignment");
      }
    }
  }
  impl_->interlock->at(InterlockPoint::BeforePolicyChangeCommit);
  return MutationResult::success();
}

PolicySnapshot AgentScheduler::policy() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->policy;
}

MutationResult AgentScheduler::register_queue(QueueId queue,
                                              QueueGeneration generation,
                                              std::string fairness_class,
                                              std::uint32_t weight) {
  if (queue.is_zero() || generation.is_zero()) {
    return rejected(ErrorCode::InvalidArgument, "register_queue", queue.to_string(),
                    "queue id and generation must be non-zero", ScheduleOutcome::RejectInvalidRequest);
  }
  if (!internal::is_identifier(fairness_class, impl_->limits.max_identifier_size)) {
    return rejected(ErrorCode::InvalidArgument, "register_queue", queue.to_string(),
                    "fairness_class is not a valid identifier", ScheduleOutcome::RejectInvalidRequest);
  }
  if (weight == 0 || weight > 1000) {
    return rejected(ErrorCode::InvalidArgument, "register_queue", queue.to_string(),
                    "queue weight must be 1..1000", ScheduleOutcome::RejectInvalidRequest);
  }
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->queues.find(queue);
  if (found == impl_->queues.end()) {
    if (impl_->queues.size() >= impl_->limits.max_queues) {
      return rejected(ErrorCode::LimitExceeded, "register_queue", queue.to_string(),
                      "queue count exceeds max_queues", ScheduleOutcome::RejectLimitExceeded);
    }
    QueueRecord record;
    record.id = queue;
    record.generation = generation;
    record.fairness_class = std::move(fairness_class);
    record.weight = weight;
    impl_->queues.emplace(queue, std::move(record));
    ++impl_->state_revision;
    return MutationResult::success();
  }
  if (generation < found->second.generation) {
    return rejected(ErrorCode::StaleGeneration, "register_queue", queue.to_string(),
                    "queue generation is stale", ScheduleOutcome::RejectStaleQueueGeneration);
  }
  if (generation == found->second.generation) {
    if (found->second.fairness_class == fairness_class && found->second.weight == weight) {
      return MutationResult::success();
    }
    return rejected(ErrorCode::Conflict, "register_queue", queue.to_string(),
                    "conflicting queue publication for the current generation",
                    ScheduleOutcome::RejectConflict);
  }
  found->second.generation = generation;
  found->second.fairness_class = std::move(fairness_class);
  found->second.weight = weight;
  ++impl_->state_revision;
  return MutationResult::success();
}

// ---------------------------------------------------------------------------
// agent lifecycle
// ---------------------------------------------------------------------------

MutationResult AgentScheduler::register_agent(const AgentDescriptor& descriptor) {
  if (const auto problem = validate(descriptor, impl_->limits); !problem.ok()) {
    return problem;
  }
  const AgentId agent = descriptor.id;
  {
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    const auto found = impl_->agents.find(agent);
    if (found != impl_->agents.end()) {
      const AgentRecord& existing = found->second;
      if (descriptor.generation < existing.descriptor.generation) {
        return rejected(ErrorCode::StaleGeneration, "register_agent", agent.to_string(),
                        "agent generation is stale", ScheduleOutcome::RejectStaleAgentGeneration);
      }
      if (descriptor.generation == existing.descriptor.generation &&
          descriptor.boot != existing.descriptor.boot) {
        return rejected(ErrorCode::StaleGeneration, "register_agent", agent.to_string(),
                        "a new incarnation requires a higher agent generation",
                        ScheduleOutcome::RejectStaleAgentGeneration);
      }
      if (existing.lifecycle == AgentLifecycle::Retired && descriptor.generation == existing.descriptor.generation) {
        return rejected(ErrorCode::Conflict, "register_agent", agent.to_string(),
                        "retired agent cannot re-register at the same generation",
                        ScheduleOutcome::RejectLifecycle);
      }
      if (internal::boot_is_fenced(*impl_, descriptor.boot)) {
        return rejected(ErrorCode::Fenced, "register_agent", agent.to_string(),
                        "incarnation is permanently fenced", ScheduleOutcome::RejectFenced);
      }
    } else if (impl_->agents.size() >= impl_->limits.max_agents) {
      return rejected(ErrorCode::LimitExceeded, "register_agent", agent.to_string(),
                      "agent count exceeds max_agents", ScheduleOutcome::RejectLimitExceeded);
    }
  }

  impl_->interlock->at(InterlockPoint::BeforeAgentRegistrationCommit);

  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->agents.find(agent);
  if (found != impl_->agents.end()) {
    AgentRecord& existing = found->second;
    if (descriptor.generation < existing.descriptor.generation) {
      return rejected(ErrorCode::StaleGeneration, "register_agent", agent.to_string(),
                      "agent generation is stale", ScheduleOutcome::RejectStaleAgentGeneration);
    }
    if (descriptor.generation == existing.descriptor.generation &&
        descriptor.boot != existing.descriptor.boot) {
      return rejected(ErrorCode::StaleGeneration, "register_agent", agent.to_string(),
                      "a new incarnation requires a higher agent generation",
                      ScheduleOutcome::RejectStaleAgentGeneration);
    }
    if (internal::boot_is_fenced(*impl_, descriptor.boot)) {
      return rejected(ErrorCode::Fenced, "register_agent", agent.to_string(),
                      "incarnation is permanently fenced", ScheduleOutcome::RejectFenced);
    }
    if (descriptor.generation > existing.descriptor.generation) {
      const AgentBootId previous_boot = existing.descriptor.boot;
      const AgentGeneration previous_generation = existing.descriptor.generation;
      if (!previous_boot.is_zero()) {
        internal::fence_boot(*impl_, agent, previous_boot, previous_generation, "replaced by reincarnation");
        close_all_active(*impl_, previous_boot, AssignmentState::Lost, InvalidationReason::AgentFenced,
                         "agent incarnation replaced by a newer generation");
      }
      AgentRecord fresh;
      fresh.descriptor = descriptor;
      fresh.descriptor.policy_labels = descriptor.policy_labels;
      internal::canonicalize(fresh.descriptor.policy_labels);
      fresh.registered_at_ms = impl_->now_ms();
      fresh.lease_expires_at_ms = fresh.registered_at_ms + effective_lease_ttl(*impl_, descriptor);
      fresh.last_heartbeat_ms = fresh.registered_at_ms;
      fresh.reason = "reincarnated";
      internal::recompute_lifecycle(fresh);
      internal::deindex_agent(*impl_, agent);
      existing = std::move(fresh);
      internal::refresh_agent_accounting(*impl_, existing);
      internal::index_agent(*impl_, existing);
      ++impl_->state_revision;
      return MutationResult::success();
    }
    // Same incarnation: re-registration refreshes the session lease and identity metadata.
    existing.descriptor = descriptor;
    internal::canonicalize(existing.descriptor.policy_labels);
    existing.descriptor.registration_generation = descriptor.registration_generation;
    existing.registered_at_ms = impl_->now_ms();
    existing.last_heartbeat_ms = existing.registered_at_ms;
    existing.lease_expires_at_ms = existing.registered_at_ms + effective_lease_ttl(*impl_, descriptor);
    if (existing.lifecycle == AgentLifecycle::Lost) {
      existing.lifecycle = AgentLifecycle::RevalidationRequired;
    }
    existing.reason = "re-registered";
    refresh_agent(*impl_, existing);
    ++impl_->state_revision;
    return MutationResult::success();
  }

  if (impl_->agents.size() >= impl_->limits.max_agents) {
    return rejected(ErrorCode::LimitExceeded, "register_agent", agent.to_string(),
                    "agent count exceeds max_agents", ScheduleOutcome::RejectLimitExceeded);
  }
  AgentRecord record;
  record.descriptor = descriptor;
  internal::canonicalize(record.descriptor.policy_labels);
  record.registered_at_ms = impl_->now_ms();
  record.lease_expires_at_ms = record.registered_at_ms + effective_lease_ttl(*impl_, descriptor);
  record.last_heartbeat_ms = record.registered_at_ms;
  record.reason = "registered";
  internal::recompute_lifecycle(record);
  auto inserted = impl_->agents.emplace(agent, std::move(record));
  internal::index_agent(*impl_, inserted.first->second);
  ++impl_->state_revision;
  return MutationResult::success();
}

MutationResult AgentScheduler::publish_capabilities(AgentId agent,
                                                    AgentBootId boot,
                                                    const CapabilityProfile& profile) {
  CapabilityProfile canonical = profile;
  if (const auto problem = canonicalize(canonical, impl_->limits); !problem.ok()) {
    return problem;
  }
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->agents.find(agent);
  if (found == impl_->agents.end()) {
    return rejected(ErrorCode::NotFound, "publish_capabilities", agent.to_string(), "agent is not known",
                    ScheduleOutcome::RejectStaleAgentGeneration);
  }
  AgentRecord& record = found->second;
  if (stale_boot(*impl_, record, boot)) {
    return rejected(ErrorCode::StaleGeneration, "publish_capabilities", agent.to_string(),
                    "capability evidence names a fenced or replaced incarnation",
                    ScheduleOutcome::RejectStaleAgentBoot);
  }
  if (canonical.boot != boot) {
    return rejected(ErrorCode::InvalidArgument, "publish_capabilities", agent.to_string(),
                    "profile boot does not match the publishing incarnation",
                    ScheduleOutcome::RejectStaleAgentBoot);
  }
  if (canonical.generation < record.profile.generation) {
    return rejected(ErrorCode::StaleGeneration, "publish_capabilities", agent.to_string(),
                    "capability generation is stale", ScheduleOutcome::RejectStaleCapability);
  }
  if (canonical.generation == record.profile.generation && record.capability_current) {
    if (canonical.capabilities == record.profile.capabilities &&
        canonical.profile_id == record.profile.profile_id) {
      return MutationResult::success();
    }
    return rejected(ErrorCode::Conflict, "publish_capabilities", agent.to_string(),
                    "conflicting capability publication for the current generation",
                    ScheduleOutcome::RejectConflict);
  }
  const std::uint64_t now = impl_->now_ms();
  canonical.boot = boot;
  for (CapabilityEvidence& evidence : canonical.capabilities) {
    if (!evidence.boot.is_zero() && evidence.boot != boot) {
      return rejected(ErrorCode::StaleGeneration, "publish_capabilities", evidence.name,
                      "capability evidence names a different incarnation",
                      ScheduleOutcome::RejectStaleAgentBoot);
    }
    evidence.boot = boot;
    evidence.generation = canonical.generation;
    if (evidence.observed_at_ms == 0) {
      evidence.observed_at_ms = now;
    }
    if (evidence.observed_at_ms > now) {
      return rejected(ErrorCode::InvalidArgument, "publish_capabilities", evidence.name,
                      "capability observation time is in the future",
                      ScheduleOutcome::RejectInvalidRequest);
    }
  }
  internal::deindex_agent(*impl_, agent);
  record.profile = std::move(canonical);
  record.capability_current = true;
  record.recovered = false;
  // Existing assignments bound the superseded evidence. They keep their slot but lose
  // dispatch authority until revalidation decides their fate.
  for (auto& entry : impl_->assignments) {
    AssignmentRecord& assignment = entry.second;
    if (assignment.binding.agent != agent || !assignment_state_is_active(assignment.state)) {
      continue;
    }
    if (assignment.binding.capability_generation != record.profile.generation ||
        assignment.binding.capability_profile != record.profile.profile_id) {
      assignment.state = AssignmentState::RevalidationRequired;
      assignment.lease_current = false;
      assignment.reason = "capability evidence generation moved under the assignment";
      if (const auto lease_entry = impl_->leases.find(assignment.binding.lease);
          lease_entry != impl_->leases.end()) {
        lease_entry->second.lease.current = false;
      }
    }
  }
  internal::refresh_agent_accounting(*impl_, record);
  internal::index_agent(*impl_, record);
  ++impl_->state_revision;
  return MutationResult::success();
}

MutationResult AgentScheduler::publish_health(AgentId agent,
                                              AgentBootId boot,
                                              const HealthObservation& observation) {
  if (const auto problem = validate(observation, impl_->limits); !problem.ok()) {
    return problem;
  }
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->agents.find(agent);
  if (found == impl_->agents.end()) {
    return rejected(ErrorCode::NotFound, "publish_health", agent.to_string(), "agent is not known",
                    ScheduleOutcome::RejectStaleAgentGeneration);
  }
  AgentRecord& record = found->second;
  if (stale_boot(*impl_, record, boot)) {
    return rejected(ErrorCode::StaleGeneration, "publish_health", agent.to_string(),
                    "health evidence names a fenced or replaced incarnation",
                    ScheduleOutcome::RejectStaleAgentBoot);
  }
  if (observation.generation < record.health_generation) {
    return rejected(ErrorCode::StaleGeneration, "publish_health", agent.to_string(),
                    "health generation is stale", ScheduleOutcome::RejectStaleCapability);
  }
  const std::uint64_t now = impl_->now_ms();
  const std::uint64_t observed = observation.observed_at_ms == 0 ? now : observation.observed_at_ms;
  if (observed > now) {
    return rejected(ErrorCode::InvalidArgument, "publish_health", agent.to_string(),
                    "health observation time is in the future", ScheduleOutcome::RejectInvalidRequest);
  }
  record.health_generation = observation.generation;
  record.health_observation = observation;
  record.health_observation.observed_at_ms = observed;
  record.health = observation.health;
  record.health_current = true;
  record.recovered = false;
  refresh_agent(*impl_, record);
  ++impl_->state_revision;
  return MutationResult::success();
}

MutationResult AgentScheduler::publish_availability(AgentId agent,
                                                    AgentBootId boot,
                                                    const AvailabilityObservation& observation) {
  if (const auto problem = validate(observation, impl_->limits); !problem.ok()) {
    return problem;
  }
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->agents.find(agent);
  if (found == impl_->agents.end()) {
    return rejected(ErrorCode::NotFound, "publish_availability", agent.to_string(), "agent is not known",
                    ScheduleOutcome::RejectStaleAgentGeneration);
  }
  AgentRecord& record = found->second;
  if (stale_boot(*impl_, record, boot)) {
    return rejected(ErrorCode::StaleGeneration, "publish_availability", agent.to_string(),
                    "availability evidence names a fenced or replaced incarnation",
                    ScheduleOutcome::RejectStaleAgentBoot);
  }
  if (observation.generation < record.availability_generation) {
    return rejected(ErrorCode::StaleGeneration, "publish_availability", agent.to_string(),
                    "availability generation is stale", ScheduleOutcome::RejectStaleCapability);
  }
  const std::uint64_t now = impl_->now_ms();
  const std::uint64_t observed = observation.observed_at_ms == 0 ? now : observation.observed_at_ms;
  if (observed > now) {
    return rejected(ErrorCode::InvalidArgument, "publish_availability", agent.to_string(),
                    "availability observation time is in the future",
                    ScheduleOutcome::RejectInvalidRequest);
  }
  record.availability_generation = observation.generation;
  record.availability_observation = observation;
  record.availability_observation.observed_at_ms = observed;
  record.availability = observation.availability;
  record.readiness = observation.readiness;
  record.reachability = observation.reachability;
  record.availability_current = true;
  record.recovered = false;
  refresh_agent(*impl_, record);
  ++impl_->state_revision;
  return MutationResult::success();
}

MutationResult AgentScheduler::publish_load(AgentId agent,
                                            AgentBootId boot,
                                            const LoadObservation& observation) {
  if (const auto problem = validate(observation, impl_->limits); !problem.ok()) {
    return problem;
  }
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->agents.find(agent);
  if (found == impl_->agents.end()) {
    return rejected(ErrorCode::NotFound, "publish_load", agent.to_string(), "agent is not known",
                    ScheduleOutcome::RejectStaleAgentGeneration);
  }
  AgentRecord& record = found->second;
  if (stale_boot(*impl_, record, boot)) {
    return rejected(ErrorCode::StaleGeneration, "publish_load", agent.to_string(),
                    "load evidence names a fenced or replaced incarnation",
                    ScheduleOutcome::RejectStaleAgentBoot);
  }
  if (observation.generation < record.load_generation) {
    return rejected(ErrorCode::StaleGeneration, "publish_load", agent.to_string(),
                    "load generation is stale", ScheduleOutcome::RejectStaleCapability);
  }
  const std::uint64_t now = impl_->now_ms();
  const std::uint64_t observed = observation.observed_at_ms == 0 ? now : observation.observed_at_ms;
  if (observed > now) {
    return rejected(ErrorCode::InvalidArgument, "publish_load", agent.to_string(),
                    "load observation time is in the future", ScheduleOutcome::RejectInvalidRequest);
  }
  record.load_generation = observation.generation;
  record.load_observation = observation;
  record.load_observation.observed_at_ms = observed;
  internal::canonicalize(record.load_observation.warm_state_keys);
  record.load_current = true;
  ++impl_->state_revision;
  return MutationResult::success();
}

MutationResult AgentScheduler::heartbeat(AgentId agent,
                                         AgentBootId boot,
                                         AgentRegistrationGeneration registration_generation) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->agents.find(agent);
  if (found == impl_->agents.end()) {
    return rejected(ErrorCode::NotFound, "heartbeat", agent.to_string(), "agent is not known",
                    ScheduleOutcome::RejectStaleAgentGeneration);
  }
  AgentRecord& record = found->second;
  if (stale_boot(*impl_, record, boot)) {
    return rejected(ErrorCode::StaleGeneration, "heartbeat", agent.to_string(),
                    "heartbeat names a fenced or replaced incarnation",
                    ScheduleOutcome::RejectStaleAgentBoot);
  }
  if (registration_generation != record.descriptor.registration_generation) {
    return rejected(ErrorCode::StaleGeneration, "heartbeat", agent.to_string(),
                    "registration generation moved", ScheduleOutcome::RejectStaleAgentGeneration);
  }
  const std::uint64_t now = impl_->now_ms();
  record.last_heartbeat_ms = now;
  record.lease_expires_at_ms = now + effective_lease_ttl(*impl_, record.descriptor);
  ++impl_->state_revision;
  return MutationResult::success();
}

MutationResult AgentScheduler::drain_agent(AgentId agent, AgentBootId boot, bool force) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->agents.find(agent);
  if (found == impl_->agents.end()) {
    return rejected(ErrorCode::NotFound, "drain_agent", agent.to_string(), "agent is not known",
                    ScheduleOutcome::RejectStaleAgentGeneration);
  }
  AgentRecord& record = found->second;
  if (stale_boot(*impl_, record, boot)) {
    return rejected(ErrorCode::StaleGeneration, "drain_agent", agent.to_string(),
                    "drain names a fenced or replaced incarnation", ScheduleOutcome::RejectStaleAgentBoot);
  }
  if (record.lifecycle == AgentLifecycle::Retired) {
    return rejected(ErrorCode::Conflict, "drain_agent", agent.to_string(), "agent is retired",
                    ScheduleOutcome::RejectLifecycle);
  }
  record.lifecycle = AgentLifecycle::Draining;
  record.reason = "draining";
  if (force) {
    close_all_active(*impl_, boot, AssignmentState::Invalidated, InvalidationReason::AgentDrained,
                     "forced drain invalidated the assignment");
  }
  refresh_agent(*impl_, record);
  ++impl_->state_revision;
  return MutationResult::success();
}

MutationResult AgentScheduler::deregister_agent(AgentId agent, AgentBootId boot, bool force) {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->agents.find(agent);
  if (found == impl_->agents.end()) {
    return rejected(ErrorCode::NotFound, "deregister_agent", agent.to_string(), "agent is not known",
                    ScheduleOutcome::RejectStaleAgentGeneration);
  }
  AgentRecord& record = found->second;
  if (stale_boot(*impl_, record, boot)) {
    return rejected(ErrorCode::StaleGeneration, "deregister_agent", agent.to_string(),
                    "deregistration names a fenced or replaced incarnation",
                    ScheduleOutcome::RejectStaleAgentBoot);
  }
  const std::uint32_t active = internal::active_assignment_count(*impl_, agent);
  if (active > 0 && !force) {
    return rejected(ErrorCode::DrainBarrier, "deregister_agent", agent.to_string(),
                    "agent still owns active scheduler assignments",
                    ScheduleOutcome::RejectDrain);
  }
  if (active > 0) {
    close_all_active(*impl_, boot, AssignmentState::Invalidated, InvalidationReason::AgentRetired,
                     "forced deregistration invalidated the assignment");
  }
  internal::fence_boot(*impl_, agent, boot, record.descriptor.generation, "deregistered");
  internal::invalidate_dynamic_evidence(record);
  record.lifecycle = AgentLifecycle::Retired;
  record.reason = "deregistered";
  refresh_agent(*impl_, record);
  ++impl_->state_revision;
  return MutationResult::success();
}

MutationResult AgentScheduler::declare_agent_lost(AgentId agent, AgentBootId boot, std::string reason) {
  {
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    const auto found = impl_->agents.find(agent);
    if (found != impl_->agents.end() && found->second.descriptor.boot == boot &&
        found->second.lifecycle == AgentLifecycle::Lost) {
      return MutationResult::success();
    }
  }
  impl_->interlock->at(InterlockPoint::BeforeLossCommit);

  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->agents.find(agent);
  if (found == impl_->agents.end()) {
    internal::fence_boot(*impl_, agent, boot, AgentGeneration{}, std::move(reason));
    return MutationResult::success();
  }
  AgentRecord& record = found->second;
  if (record.descriptor.boot != boot) {
    // A stale loss report for an already-replaced incarnation only fences that boot.
    internal::fence_boot(*impl_, agent, boot, AgentGeneration{}, std::move(reason));
    return MutationResult::success();
  }
  internal::fence_boot(*impl_, agent, boot, record.descriptor.generation, reason);
  close_all_active(*impl_, boot, AssignmentState::Lost, InvalidationReason::AgentLost,
                   "agent incarnation was declared lost");
  internal::invalidate_dynamic_evidence(record);
  record.lifecycle = AgentLifecycle::Lost;
  record.reason = std::move(reason);
  refresh_agent(*impl_, record);
  ++impl_->state_revision;
  return MutationResult::success();
}

// ---------------------------------------------------------------------------
// work lifecycle
// ---------------------------------------------------------------------------

MutationResult AgentScheduler::submit_work(const WorkRequest& request) {
  WorkRequest canonical = request;
  canonicalize(canonical);
  if (const auto problem = validate(canonical, impl_->limits); !problem.ok()) {
    return problem;
  }
  const WorkId work_id = canonical.id;
  {
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    if (!impl_->admission_open) {
      return rejected(ErrorCode::AdmissionClosed, "submit_work", work_id.to_string(),
                      "admission is closed", ScheduleOutcome::ShuttingDown);
    }
    const auto found = impl_->work.find(work_id);
    if (found != impl_->work.end()) {
      const WorkRecord& existing = found->second;
      if (canonical.generation < existing.request.generation) {
        return rejected(ErrorCode::StaleGeneration, "submit_work", work_id.to_string(),
                        "work generation is stale", ScheduleOutcome::RejectStaleWorkGeneration);
      }
      if (canonical.generation == existing.request.generation) {
        if (existing.lifecycle == WorkLifecycle::Cancelled) {
          return rejected(ErrorCode::Cancelled, "submit_work", work_id.to_string(),
                          "work is cancelled and cannot be re-admitted at the same generation",
                          ScheduleOutcome::RejectCancelled);
        }
        if (existing.lifecycle == WorkLifecycle::Superseded) {
          return rejected(ErrorCode::Superseded, "submit_work", work_id.to_string(),
                          "work is superseded", ScheduleOutcome::RejectSuperseded);
        }
        if (existing.request == canonical) {
          return MutationResult::success();
        }
        return rejected(ErrorCode::Conflict, "submit_work", work_id.to_string(),
                        "conflicting work publication for the current generation",
                        ScheduleOutcome::RejectConflict);
      }
    } else if (impl_->work.size() >= impl_->limits.max_work_items) {
      return rejected(ErrorCode::LimitExceeded, "submit_work", work_id.to_string(),
                      "work item count exceeds max_work_items", ScheduleOutcome::RejectLimitExceeded);
    }
    const auto queue_entry = impl_->queues.find(canonical.queue);
    if (queue_entry != impl_->queues.end() && canonical.queue_generation < queue_entry->second.generation) {
      return rejected(ErrorCode::StaleGeneration, "submit_work", work_id.to_string(),
                      "queue generation is stale", ScheduleOutcome::RejectStaleQueueGeneration);
    }
  }

  impl_->interlock->at(InterlockPoint::BeforeWorkAdmissionCommit);

  const std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!impl_->admission_open) {
    return rejected(ErrorCode::AdmissionClosed, "submit_work", work_id.to_string(), "admission is closed",
                    ScheduleOutcome::ShuttingDown);
  }
  auto found = impl_->work.find(work_id);
  if (found != impl_->work.end()) {
    WorkRecord& existing = found->second;
    if (canonical.generation < existing.request.generation) {
      return rejected(ErrorCode::StaleGeneration, "submit_work", work_id.to_string(),
                      "work generation is stale", ScheduleOutcome::RejectStaleWorkGeneration);
    }
    if (canonical.generation == existing.request.generation) {
      if (existing.lifecycle == WorkLifecycle::Cancelled) {
        return rejected(ErrorCode::Cancelled, "submit_work", work_id.to_string(),
                        "work is cancelled", ScheduleOutcome::RejectCancelled);
      }
      if (existing.lifecycle == WorkLifecycle::Superseded) {
        return rejected(ErrorCode::Superseded, "submit_work", work_id.to_string(), "work is superseded",
                        ScheduleOutcome::RejectSuperseded);
      }
      if (existing.request == canonical) {
        return MutationResult::success();
      }
      return rejected(ErrorCode::Conflict, "submit_work", work_id.to_string(),
                      "conflicting work publication for the current generation",
                      ScheduleOutcome::RejectConflict);
    }
    for (auto& entry : impl_->assignments) {
      if (entry.second.binding.work == work_id && assignment_state_is_active(entry.second.state)) {
        internal::close_assignment(*impl_, entry.second, AssignmentState::Superseded,
                                   InvalidationReason::WorkSuperseded,
                                   "work generation was superseded");
      }
    }
    internal::deindex_work(*impl_, work_id);
    existing.request = canonical;
    existing.lifecycle = WorkLifecycle::Admitted;
    existing.admitted_sequence = impl_->next_admission_sequence++;
    existing.admitted_at_ms = impl_->now_ms();
    existing.bypass_count = 0;
    existing.starvation_rounds = 0;
    existing.current_assignments = 0;
    existing.reason = "re-admitted at a higher generation";
    internal::index_work(*impl_, existing);
    if (const auto queue_result = ensure_queue(*impl_, canonical); !queue_result.ok()) {
      return queue_result;
    }
    if (const auto tenant_result = ensure_tenant(*impl_, canonical.requirements.tenant); !tenant_result.ok()) {
      return tenant_result;
    }
    if (const auto queue_entry = impl_->queues.find(canonical.queue); queue_entry != impl_->queues.end()) {
      ++queue_entry->second.admitted_count;
    }
    if (const auto tenant_entry = impl_->tenants.find(canonical.requirements.tenant);
        tenant_entry != impl_->tenants.end()) {
      ++tenant_entry->second.admitted_count;
    }
    ++impl_->state_revision;
    return MutationResult::success();
  }
  if (impl_->work.size() >= impl_->limits.max_work_items) {
    return rejected(ErrorCode::LimitExceeded, "submit_work", work_id.to_string(),
                    "work item count exceeds max_work_items", ScheduleOutcome::RejectLimitExceeded);
  }
  WorkRecord record;
  record.request = canonical;
  record.lifecycle = WorkLifecycle::Admitted;
  record.admitted_sequence = impl_->next_admission_sequence++;
  record.admitted_at_ms = impl_->now_ms();
  record.reason = "admitted";
  if (const auto queue_result = ensure_queue(*impl_, canonical); !queue_result.ok()) {
    return queue_result;
  }
  if (const auto tenant_result = ensure_tenant(*impl_, canonical.requirements.tenant); !tenant_result.ok()) {
    return tenant_result;
  }
  auto inserted = impl_->work.emplace(work_id, std::move(record));
  internal::index_work(*impl_, inserted.first->second);
  if (const auto queue_entry = impl_->queues.find(canonical.queue); queue_entry != impl_->queues.end()) {
    ++queue_entry->second.admitted_count;
  }
  if (const auto tenant_entry = impl_->tenants.find(canonical.requirements.tenant);
      tenant_entry != impl_->tenants.end()) {
    ++tenant_entry->second.admitted_count;
  }
  ++impl_->state_revision;
  return MutationResult::success();
}

MutationResult AgentScheduler::cancel_work(WorkId work, WorkGeneration generation, std::string reason) {
  {
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    const auto found = impl_->work.find(work);
    if (found == impl_->work.end()) {
      return rejected(ErrorCode::NotFound, "cancel_work", work.to_string(), "work is not known",
                      ScheduleOutcome::RejectInvalidRequest);
    }
    if (found->second.request.generation != generation) {
      return rejected(ErrorCode::StaleGeneration, "cancel_work", work.to_string(),
                      "work generation moved", ScheduleOutcome::RejectStaleWorkGeneration);
    }
    if (found->second.lifecycle == WorkLifecycle::Cancelled) {
      return MutationResult::success();
    }
  }
  impl_->interlock->at(InterlockPoint::BeforeCancellationCommit);
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->work.find(work);
  if (found == impl_->work.end()) {
    return rejected(ErrorCode::NotFound, "cancel_work", work.to_string(), "work is not known",
                    ScheduleOutcome::RejectInvalidRequest);
  }
  WorkRecord& record = found->second;
  if (record.request.generation != generation) {
    return rejected(ErrorCode::StaleGeneration, "cancel_work", work.to_string(), "work generation moved",
                    ScheduleOutcome::RejectStaleWorkGeneration);
  }
  if (record.lifecycle == WorkLifecycle::Cancelled) {
    return MutationResult::success();
  }
  if (work_is_terminal(record.lifecycle)) {
    return rejected(ErrorCode::Conflict, "cancel_work", work.to_string(),
                    "work is already terminal", ScheduleOutcome::RejectConflict);
  }
  for (auto& entry : impl_->assignments) {
    if (entry.second.binding.work == work && assignment_state_is_active(entry.second.state)) {
      internal::close_assignment(*impl_, entry.second, AssignmentState::Cancelled,
                                 InvalidationReason::WorkCancelled, reason);
    }
  }
  record.lifecycle = WorkLifecycle::Cancelled;
  internal::sync_work_lifecycle_index(*impl_, work, record.lifecycle);
  record.reason = reason.empty() ? std::string("cancelled") : std::move(reason);
  ++impl_->state_revision;
  return MutationResult::success();
}

MutationResult AgentScheduler::supersede_work(WorkId work,
                                              WorkGeneration expected_generation,
                                              const WorkRequest& replacement) {
  if (replacement.id != work) {
    return rejected(ErrorCode::InvalidArgument, "supersede_work", work.to_string(),
                    "replacement work id does not match the target",
                    ScheduleOutcome::RejectInvalidRequest);
  }
  WorkRequest canonical = replacement;
  canonicalize(canonical);
  if (const auto problem = validate(canonical, impl_->limits); !problem.ok()) {
    return problem;
  }
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->work.find(work);
  if (found == impl_->work.end()) {
    return rejected(ErrorCode::NotFound, "supersede_work", work.to_string(), "work is not known",
                    ScheduleOutcome::RejectInvalidRequest);
  }
  WorkRecord& record = found->second;
  if (record.request.generation != expected_generation) {
    return rejected(ErrorCode::StaleGeneration, "supersede_work", work.to_string(),
                    "work generation moved", ScheduleOutcome::RejectStaleWorkGeneration);
  }
  if (canonical.generation <= expected_generation) {
    return rejected(ErrorCode::StaleGeneration, "supersede_work", work.to_string(),
                    "replacement generation must be higher than the superseded generation",
                    ScheduleOutcome::RejectStaleWorkGeneration);
  }
  if (record.lifecycle == WorkLifecycle::Cancelled) {
    return rejected(ErrorCode::Cancelled, "supersede_work", work.to_string(), "work is cancelled",
                    ScheduleOutcome::RejectCancelled);
  }
  for (auto& entry : impl_->assignments) {
    if (entry.second.binding.work == work && assignment_state_is_active(entry.second.state)) {
      internal::close_assignment(*impl_, entry.second, AssignmentState::Superseded,
                                 InvalidationReason::WorkSuperseded,
                                 "work generation was superseded");
    }
  }
  internal::deindex_work(*impl_, work);
  record.request = canonical;
  record.lifecycle = WorkLifecycle::Admitted;
  record.admitted_sequence = impl_->next_admission_sequence++;
  record.admitted_at_ms = impl_->now_ms();
  record.bypass_count = 0;
  record.starvation_rounds = 0;
  record.current_assignments = 0;
  record.reason = "superseded";
  internal::index_work(*impl_, record);
  if (const auto queue_result = ensure_queue(*impl_, canonical); !queue_result.ok()) {
    return queue_result;
  }
  if (const auto tenant_result = ensure_tenant(*impl_, canonical.requirements.tenant); !tenant_result.ok()) {
    return tenant_result;
  }
  ++impl_->state_revision;
  return MutationResult::success();
}

MutationResult AgentScheduler::expire_deadlines() {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const std::uint64_t now = impl_->now_ms();
  bool changed = false;
  for (auto& entry : impl_->work) {
    WorkRecord& record = entry.second;
    if (work_is_terminal(record.lifecycle)) {
      continue;
    }
    const std::uint64_t deadline = record.request.requirements.deadline_ms;
    if (deadline == 0 || now <= deadline) {
      continue;
    }
    for (auto& assignment_entry : impl_->assignments) {
      if (assignment_entry.second.binding.work == entry.first &&
          assignment_state_is_active(assignment_entry.second.state)) {
        internal::close_assignment(*impl_, assignment_entry.second, AssignmentState::Expired,
                                   InvalidationReason::Rejected, "work deadline passed");
      }
    }
    record.lifecycle = WorkLifecycle::Expired;
    internal::sync_work_lifecycle_index(*impl_, entry.first, record.lifecycle);
    record.reason = "deadline passed";
    changed = true;
  }
  if (changed) {
    ++impl_->state_revision;
  }
  return MutationResult::success();
}

}  // namespace agent_scheduler
