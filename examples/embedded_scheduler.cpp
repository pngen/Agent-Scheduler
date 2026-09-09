// Agent Scheduler — example: embedded scheduler, register, submit, schedule, inspect.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include <cstdio>

#include "agent_scheduler/scheduler.hpp"

using namespace agent_scheduler;

namespace {

AgentDescriptor make_agent(std::uint64_t id, AgentBootId boot, std::uint32_t concurrency) {
  AgentDescriptor descriptor;
  descriptor.id = AgentId{id};
  descriptor.generation = AgentGeneration{1};
  descriptor.boot = boot;
  descriptor.registration_generation = AgentRegistrationGeneration{1};
  descriptor.tenant = TenantId{1};
  descriptor.name_space = NamespaceId{1};
  descriptor.display_name = "example-agent-" + std::to_string(id);
  descriptor.policy_labels = {"example"};
  descriptor.placement_domain = PlacementDomainId{1};
  descriptor.topology_epoch = TopologyEpoch{1};
  descriptor.max_concurrency = concurrency;
  descriptor.lease_ttl_ms = 600000;
  return descriptor;
}

}  // namespace

int main() {
  SchedulerOptions options;
  options.scheduler_id = SchedulerId{1};
  options.clock = std::make_shared<ManualClock>(1000);
  AgentScheduler scheduler(options);

  Hash128 boot_a{0x1111111111111111ull, 0x2222222222222222ull};
  Hash128 boot_b{0x3333333333333333ull, 0x4444444444444444ull};
  const AgentDescriptor agent_a = make_agent(1, AgentBootId{boot_a}, 2);
  const AgentDescriptor agent_b = make_agent(2, AgentBootId{boot_b}, 1);

  for (const AgentDescriptor& descriptor : {agent_a, agent_b}) {
    if (!scheduler.register_agent(descriptor).ok()) {
      std::printf("registration failed\n");
      return 1;
    }
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
    LoadObservation load;
    load.generation = AgentLoadGeneration{1};
    load.estimated_dispatch_latency_ms = 5;
    load.cost_index = 0.25;
    load.slo_headroom_fraction = 0.9;
    (void)scheduler.publish_load(descriptor.id, descriptor.boot, load);
  }

  WorkRequest request;
  request.id = WorkId{1};
  request.generation = WorkGeneration{1};
  request.queue = QueueId{1};
  request.queue_generation = QueueGeneration{1};
  request.kind = "example";
  request.priority = 500;
  request.fairness_class = "default";
  request.requirements.tenant = TenantId{1};
  request.requirements.name_space = NamespaceId{1};
  request.requirements.required_policy_labels = {"example"};
  CapabilityRequirement requirement;
  requirement.name = "coding";
  requirement.mode = CapabilityRequirementMode::Required;
  requirement.minimum_state = CapabilityState::Observed;
  request.requirements.capabilities.push_back(requirement);
  canonicalize(request);
  if (!scheduler.submit_work(request).ok()) {
    std::printf("admission failed\n");
    return 1;
  }

  const ScheduleDecision decision = scheduler.schedule(WorkId{1}, WorkGeneration{1});
  std::printf("outcome=%s candidates=%zu eligible=%zu selected=%s tie_break=%s\n",
              to_string(decision.outcome), decision.candidate_count, decision.eligible_count,
              decision.selected_agent.to_string().c_str(), decision.tie_break_reason.c_str());
  for (const CandidateEvaluation& candidate : decision.candidates) {
    std::printf("  candidate agent=%s eligible=%s", candidate.agent.to_string().c_str(),
                candidate.eligible ? "yes" : "no");
    if (candidate.eligible) {
      std::printf(" rank=%u score=%lld factors=%zu", candidate.rank,
                  static_cast<long long>(candidate.score), candidate.factors.size());
    } else {
      std::printf(" rejection=%s detail=%s", to_string(candidate.rejection),
                  candidate.rejection_detail.c_str());
    }
    std::printf("\n");
  }
  if (!decision.assignment.has_value()) {
    std::printf("no assignment\n");
    return 1;
  }
  const DispatchResult dispatch =
      scheduler.dispatch(decision.assignment->id, decision.assignment->generation);
  std::printf("dispatch=%s state=%s\n", dispatch.dispatched() ? "handed off" : to_string(dispatch.outcome),
              to_string(scheduler.assignment(decision.assignment->id)->state));
  const InvariantReport report = scheduler.check_invariants();
  std::printf("invariants=%s state_digest=%s\n", report.ok() ? "OK" : "VIOLATED",
              scheduler.state_digest().to_string().c_str());
  return report.ok() ? 0 : 1;
}
