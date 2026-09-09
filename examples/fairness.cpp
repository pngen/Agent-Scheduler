// Agent Scheduler — example: competing queues under sustained contention.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include <cstdio>

#include "agent_scheduler/scheduler.hpp"

using namespace agent_scheduler;

namespace {

Hash128 boot_of(std::uint64_t seed) {
  Hasher256 hasher;
  hasher.update_field_tag(0xE2);
  hasher.update(seed);
  return hash128_of(hasher.final());
}

}  // namespace

int main() {
  SchedulerOptions options;
  options.scheduler_id = SchedulerId{9};
  options.clock = std::make_shared<ManualClock>(1000);
  options.policy.max_priority_bypass = 4;
  options.policy.starvation_alert_rounds = 16;
  AgentScheduler scheduler(options);

  AgentDescriptor descriptor;
  descriptor.id = AgentId{1};
  descriptor.generation = AgentGeneration{1};
  descriptor.boot = AgentBootId{boot_of(1)};
  descriptor.registration_generation = AgentRegistrationGeneration{1};
  descriptor.tenant = TenantId{1};
  descriptor.name_space = NamespaceId{1};
  descriptor.policy_labels = {"example"};
  descriptor.placement_domain = PlacementDomainId{1};
  descriptor.max_concurrency = 1;
  (void)scheduler.register_agent(descriptor);
  CapabilityProfile profile;
  profile.profile_id = CapabilityProfileId{1};
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
  LoadObservation load;
  load.generation = AgentLoadGeneration{1};
  (void)scheduler.publish_load(descriptor.id, descriptor.boot, load);

  for (std::uint32_t index = 0; index < 6; ++index) {
    WorkRequest request;
    request.id = WorkId{100 + index};
    request.generation = WorkGeneration{1};
    request.queue = QueueId{static_cast<std::uint64_t>(index == 0 ? 1 : 2)};
    request.queue_generation = QueueGeneration{1};
    request.kind = "example";
    request.priority = index == 0 ? 1000 : 0;
    request.fairness_class = index == 0 ? "interactive" : "batch";
    request.requirements.tenant = TenantId{1};
    request.requirements.name_space = NamespaceId{1};
    request.requirements.required_policy_labels = {"example"};
    CapabilityRequirement requirement;
    requirement.name = "coding";
    requirement.mode = CapabilityRequirementMode::Required;
    requirement.minimum_state = CapabilityState::Observed;
    request.requirements.capabilities.push_back(requirement);
    canonicalize(request);
    (void)scheduler.submit_work(request);
  }

  for (int round = 0; round < 14; ++round) {
    const BatchDecision batch = scheduler.schedule_batch(ScheduleRequest{});
    std::printf("round %2d assigned=%zu deferred=%zu", round, batch.assigned, batch.deferred);
    for (const ScheduleDecision& decision : batch.decisions) {
      if (decision.assigned()) {
        std::printf(" [%s->%s]", decision.work.to_string().c_str(), decision.selected_agent.to_string().c_str());
      }
    }
    std::printf("\n");
    for (const Assignment& assignment : scheduler.all_assignments()) {
      if (assignment_state_is_active(assignment.state)) {
        (void)scheduler.release(assignment.binding.id, assignment.binding.generation, assignment.binding.boot,
                                "example release");
      }
    }
  }

  const FairnessSnapshot fairness = scheduler.fairness();
  std::printf("bounded_starvation=%s starved=%u must_run=%u max_bypass=%u\n",
              fairness.bounded_starvation ? "yes" : "no", fairness.starved_count, fairness.must_run_count,
              fairness.max_bypass_observed);
  for (const FairnessEntry& entry : fairness.entries) {
    std::printf("  work=%s priority=%u effective=%lld bypass=%u/%u starvation_rounds=%u\n",
                entry.work.to_string().c_str(), entry.priority,
                static_cast<long long>(entry.effective_priority), entry.bypass_count, entry.bypass_ceiling,
                entry.starvation_rounds);
  }
  const InvariantReport report = scheduler.check_invariants();
  std::printf("invariants=%s\n", report.ok() ? "OK" : report.describe().c_str());
  return report.ok() ? 0 : 1;
}
