// Agent Scheduler — example: heterogeneous capabilities, hard eligibility, ranking.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include <cstdio>
#include <string>
#include <vector>

#include "agent_scheduler/scheduler.hpp"

using namespace agent_scheduler;

namespace {

Hash128 boot_of(std::uint64_t seed) {
  Hasher256 hasher;
  hasher.update_field_tag(0xE1);
  hasher.update(seed);
  return hash128_of(hasher.final());
}

void publish(AgentScheduler& scheduler,
             const AgentDescriptor& descriptor,
             const std::vector<std::string>& capabilities,
             std::uint32_t quality,
             std::uint32_t concurrency) {
  (void)concurrency;
  CapabilityProfile profile;
  profile.profile_id = CapabilityProfileId{descriptor.id.value()};
  profile.generation = AgentCapabilityGeneration{1};
  profile.boot = descriptor.boot;
  for (const std::string& name : capabilities) {
    CapabilityEvidence evidence;
    evidence.name = name;
    evidence.state = CapabilityState::Observed;
    evidence.quality = quality;
    evidence.generation = profile.generation;
    evidence.boot = descriptor.boot;
    profile.capabilities.push_back(evidence);
  }
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
  load.estimated_dispatch_latency_ms = 4;
  load.cost_index = 0.2;
  load.slo_headroom_fraction = 0.8;
  (void)scheduler.publish_load(descriptor.id, descriptor.boot, load);
}

AgentDescriptor describe(std::uint64_t id, std::uint32_t concurrency, const std::string& label) {
  AgentDescriptor descriptor;
  descriptor.id = AgentId{id};
  descriptor.generation = AgentGeneration{1};
  descriptor.boot = AgentBootId{boot_of(id)};
  descriptor.registration_generation = AgentRegistrationGeneration{1};
  descriptor.tenant = TenantId{1};
  descriptor.name_space = NamespaceId{1};
  descriptor.display_name = label;
  descriptor.policy_labels = {"example"};
  descriptor.placement_domain = PlacementDomainId{1};
  descriptor.topology_epoch = TopologyEpoch{1};
  descriptor.max_concurrency = concurrency;
  return descriptor;
}

WorkRequest request_for(std::uint64_t id, const std::vector<std::string>& required,
                        const std::vector<std::string>& preferred) {
  WorkRequest request;
  request.id = WorkId{id};
  request.generation = WorkGeneration{1};
  request.queue = QueueId{1};
  request.queue_generation = QueueGeneration{1};
  request.kind = "example";
  request.requirements.tenant = TenantId{1};
  request.requirements.name_space = NamespaceId{1};
  request.requirements.required_policy_labels = {"example"};
  for (const std::string& name : required) {
    CapabilityRequirement requirement;
    requirement.name = name;
    requirement.mode = CapabilityRequirementMode::Required;
    requirement.minimum_state = CapabilityState::Observed;
    request.requirements.capabilities.push_back(requirement);
  }
  for (const std::string& name : preferred) {
    CapabilityRequirement requirement;
    requirement.name = name;
    requirement.mode = CapabilityRequirementMode::Preferred;
    requirement.minimum_state = CapabilityState::Observed;
    request.requirements.capabilities.push_back(requirement);
  }
  canonicalize(request);
  return request;
}

void run_case(AgentScheduler& scheduler, const WorkRequest& request) {
  (void)scheduler.submit_work(request);
  const ScheduleDecision decision = scheduler.schedule(request.id, request.generation);
  std::printf("work %s outcome=%s selected=%s\n", request.id.to_string().c_str(),
              to_string(decision.outcome), decision.selected_agent.to_string().c_str());
  for (const CandidateEvaluation& candidate : decision.candidates) {
    std::printf("  %s %s\n", candidate.agent.to_string().c_str(),
                candidate.eligible ? "ELIGIBLE" : to_string(candidate.rejection));
  }
}

}  // namespace

int main() {
  SchedulerOptions options;
  options.scheduler_id = SchedulerId{7};
  options.clock = std::make_shared<ManualClock>(1000);
  AgentScheduler scheduler(options);

  const AgentDescriptor coding = describe(1, 2, "coding + cpp + windows + repository");
  const AgentDescriptor research = describe(2, 2, "research + network + documents");
  const AgentDescriptor cuda = describe(3, 3, "coding + cuda + cpp + windows");

  (void)scheduler.register_agent(coding);
  publish(scheduler, coding, {"coding", "language:cpp", "platform:windows", "tool:repository"}, 900, 2);
  (void)scheduler.register_agent(research);
  publish(scheduler, research, {"research", "tool:network", "tool:document"}, 900, 2);
  (void)scheduler.register_agent(cuda);
  publish(scheduler, cuda, {"coding", "language:cpp", "platform:windows", "accelerator:cuda"}, 800, 3);

  run_case(scheduler, request_for(1, {"coding", "language:cpp", "platform:windows"}, {"accelerator:cuda"}));
  run_case(scheduler, request_for(2, {"research", "tool:network"}, {}));
  run_case(scheduler, request_for(3, {"capability:does-not-exist"}, {}));
  run_case(scheduler, request_for(4, {"accelerator:cuda"}, {"coding"}));

  const InvariantReport report = scheduler.check_invariants();
  std::printf("invariants=%s\n", report.ok() ? "OK" : report.describe().c_str());
  return report.ok() ? 0 : 1;
}
