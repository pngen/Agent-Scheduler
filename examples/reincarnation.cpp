// Agent Scheduler — example: agent death, reincarnation, and stale-assignment rejection.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include <cstdio>

#include "agent_scheduler/scheduler.hpp"

using namespace agent_scheduler;

namespace {

Hash128 boot_of(std::uint64_t seed, std::uint64_t salt) {
  Hasher256 hasher;
  hasher.update_field_tag(0xE3);
  hasher.update(seed);
  hasher.update(salt);
  return hash128_of(hasher.final());
}

void publish(AgentScheduler& scheduler, const AgentDescriptor& descriptor) {
  CapabilityProfile profile;
  profile.profile_id = CapabilityProfileId{descriptor.id.value()};
  profile.generation = AgentCapabilityGeneration{1};
  profile.boot = descriptor.boot;
  CapabilityEvidence evidence;
  evidence.name = "coding";
  evidence.state = CapabilityState::Observed;
  evidence.quality = 900;
  evidence.generation = profile.generation;
  evidence.boot = descriptor.boot;
  profile.capabilities.push_back(evidence);
  (void)scheduler.publish_capabilities(descriptor.id, descriptor.boot, profile);
  HealthObservation health;
  health.generation = AgentHealthGeneration{1};
  health.health = AgentHealth::Healthy;
  health.health_quality = 900;
  (void)scheduler.publish_health(descriptor.id, descriptor.boot, health);
  AvailabilityObservation availability;
  availability.generation = AgentAvailabilityGeneration{1};
  availability.availability = AgentAvailability::Available;
  availability.reachability = AgentReachability::Reachable;
  availability.readiness = AgentReadiness::Ready;
  (void)scheduler.publish_availability(descriptor.id, descriptor.boot, availability);
}

WorkRequest make_work(std::uint64_t id) {
  WorkRequest request;
  request.id = WorkId{id};
  request.generation = WorkGeneration{1};
  request.queue = QueueId{1};
  request.queue_generation = QueueGeneration{1};
  request.kind = "example";
  request.requirements.tenant = TenantId{1};
  request.requirements.name_space = NamespaceId{1};
  request.requirements.required_policy_labels = {"example"};
  CapabilityRequirement requirement;
  requirement.name = "coding";
  requirement.mode = CapabilityRequirementMode::Required;
  requirement.minimum_state = CapabilityState::Observed;
  request.requirements.capabilities.push_back(requirement);
  canonicalize(request);
  return request;
}

}  // namespace

int main() {
  SchedulerOptions options;
  options.scheduler_id = SchedulerId{11};
  options.clock = std::make_shared<ManualClock>(1000);
  AgentScheduler scheduler(options);

  AgentDescriptor original;
  original.id = AgentId{1};
  original.generation = AgentGeneration{1};
  original.boot = AgentBootId{boot_of(1, 1)};
  original.registration_generation = AgentRegistrationGeneration{1};
  original.tenant = TenantId{1};
  original.name_space = NamespaceId{1};
  original.policy_labels = {"example"};
  original.placement_domain = PlacementDomainId{1};
  original.max_concurrency = 2;
  (void)scheduler.register_agent(original);
  publish(scheduler, original);

  (void)scheduler.submit_work(make_work(1));
  const ScheduleDecision first = scheduler.schedule(WorkId{1}, WorkGeneration{1});
  if (!first.assignment.has_value()) {
    std::printf("first assignment failed: %s\n", to_string(first.outcome));
    return 1;
  }
  const DispatchResult dispatched = scheduler.dispatch(first.assignment->id, first.assignment->generation);
  std::printf("first assignment %s to boot %s dispatched=%s\n",
              first.assignment->id.to_string().c_str(), first.assignment->boot.to_string().c_str(),
              dispatched.dispatched() ? "yes" : "no");

  std::printf("-- agent process dies --\n");
  (void)scheduler.declare_agent_lost(original.id, original.boot, "process exited");

  const DispatchResult after_death =
      scheduler.dispatch(first.assignment->id, first.assignment->generation);
  std::printf("stale dispatch outcome=%s (rejected=%s)\n", to_string(after_death.outcome),
              after_death.dispatched() ? "no" : "yes");
  std::printf("stale acknowledgement outcome=%s\n",
              to_string(scheduler
                            .acknowledge(first.assignment->id, first.assignment->generation,
                                         first.assignment->dispatch, first.assignment->dispatch_generation,
                                         original.boot)
                            .error->outcome));

  AgentDescriptor replacement = original;
  replacement.generation = AgentGeneration{2};
  replacement.boot = AgentBootId{boot_of(1, 2)};
  (void)scheduler.register_agent(replacement);
  publish(scheduler, replacement);

  const ScheduleDecision second = scheduler.schedule(WorkId{1}, WorkGeneration{1});
  std::printf("second assignment=%s generation=%llu boot=%s\n",
              second.assignment.has_value() ? "created" : "missing",
              second.assignment.has_value()
                  ? static_cast<unsigned long long>(second.assignment->generation.value())
                  : 0ull,
              second.assignment.has_value() ? second.assignment->boot.to_string().c_str() : "-");
  std::printf("fenced incarnations=%zu (old boot permanently fenced)\n", scheduler.fenced_boots().size());
  const InvariantReport report = scheduler.check_invariants();
  std::printf("invariants=%s\n", report.ok() ? "OK" : report.describe().c_str());
  return report.ok() ? 0 : 1;
}
