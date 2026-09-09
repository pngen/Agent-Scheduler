// Agent Scheduler — shared fixtures for the test suites.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agent_scheduler/scheduler.hpp"
#include "agent_scheduler/synthetic.hpp"

namespace as_fixture {

using namespace agent_scheduler;

[[nodiscard]] inline Hash128 derive(std::uint64_t seed, std::uint64_t index) {
  Hasher256 hasher;
  hasher.update_field_tag(0x77);
  hasher.update(seed);
  hasher.update(index);
  return hash128_of(hasher.final());
}

[[nodiscard]] inline AgentDescriptor make_agent(std::uint64_t id,
                                                std::uint64_t seed,
                                                std::uint32_t concurrency = 2,
                                                AgentGeneration generation = AgentGeneration{1}) {
  AgentDescriptor descriptor;
  descriptor.id = AgentId{id};
  descriptor.generation = generation;
  descriptor.boot = AgentBootId{derive(seed, id)};
  descriptor.registration_generation = AgentRegistrationGeneration{1};
  descriptor.tenant = TenantId{1};
  descriptor.name_space = NamespaceId{1};
  descriptor.display_name = "agent-" + std::to_string(id);
  descriptor.policy_labels = {"test"};
  descriptor.placement_domain = PlacementDomainId{1};
  descriptor.topology_epoch = TopologyEpoch{1};
  descriptor.max_concurrency = concurrency;
  descriptor.lease_ttl_ms = 600000;
  descriptor.provenance = "fixture";
  return descriptor;
}

[[nodiscard]] inline CapabilityProfile make_profile(const AgentDescriptor& descriptor,
                                                    const std::vector<std::pair<std::string, std::uint32_t>>& caps,
                                                    std::uint32_t quality = 800,
                                                    AgentCapabilityGeneration generation = AgentCapabilityGeneration{1}) {
  CapabilityProfile profile;
  profile.profile_id = CapabilityProfileId{descriptor.id.value()};
  profile.generation = generation;
  profile.boot = descriptor.boot;
  for (const auto& entry : caps) {
    CapabilityEvidence evidence;
    evidence.name = entry.first;
    evidence.state = CapabilityState::Observed;
    evidence.quality = entry.second == 0 ? quality : entry.second;
    evidence.generation = generation;
    evidence.boot = descriptor.boot;
    evidence.observed_at_ms = 0;
    evidence.provenance = "fixture";
    profile.capabilities.push_back(std::move(evidence));
  }
  return profile;
}

[[nodiscard]] inline HealthObservation healthy(AgentHealthGeneration generation = AgentHealthGeneration{1},
                                               std::uint32_t quality = 900) {
  HealthObservation observation;
  observation.generation = generation;
  observation.health = AgentHealth::Healthy;
  observation.health_quality = quality;
  observation.detail = "healthy";
  return observation;
}

[[nodiscard]] inline AvailabilityObservation available(AgentAvailabilityGeneration generation = AgentAvailabilityGeneration{1}) {
  AvailabilityObservation observation;
  observation.generation = generation;
  observation.availability = AgentAvailability::Available;
  observation.reachability = AgentReachability::Reachable;
  observation.readiness = AgentReadiness::Ready;
  return observation;
}

[[nodiscard]] inline LoadObservation load(AgentLoadGeneration generation = AgentLoadGeneration{1},
                                          std::uint64_t latency_ms = 5,
                                          double cost = 0.5,
                                          double slo_headroom = 0.9) {
  LoadObservation observation;
  observation.generation = generation;
  observation.estimated_dispatch_latency_ms = latency_ms;
  observation.cost_index = cost;
  observation.slo_headroom_fraction = slo_headroom;
  return observation;
}

/// Registers an agent and publishes complete current evidence so it becomes READY.
[[nodiscard]] inline MutationResult register_full(AgentScheduler& scheduler,
                                                  const AgentDescriptor& descriptor,
                                                  const std::vector<std::pair<std::string, std::uint32_t>>& caps,
                                                  std::uint32_t quality = 800,
                                                  AgentCapabilityGeneration capability_generation = AgentCapabilityGeneration{1}) {
  MutationResult result = scheduler.register_agent(descriptor);
  if (!result.ok()) {
    return result;
  }
  result = scheduler.publish_capabilities(descriptor.id, descriptor.boot,
                                          make_profile(descriptor, caps, quality, capability_generation));
  if (!result.ok()) {
    return result;
  }
  result = scheduler.publish_health(descriptor.id, descriptor.boot, healthy());
  if (!result.ok()) {
    return result;
  }
  result = scheduler.publish_availability(descriptor.id, descriptor.boot, available());
  if (!result.ok()) {
    return result;
  }
  return scheduler.publish_load(descriptor.id, descriptor.boot, load());
}

[[nodiscard]] inline WorkRequest make_work(std::uint64_t id,
                                           std::vector<std::string> required,
                                           std::uint32_t priority = 500,
                                           QueueId queue = QueueId{1},
                                           WorkGeneration generation = WorkGeneration{1}) {
  WorkRequest request;
  request.id = WorkId{id};
  request.generation = generation;
  request.queue = queue;
  request.queue_generation = QueueGeneration{1};
  request.kind = "unit-test";
  request.priority = priority;
  request.fairness_class = "default";
  request.mode = SchedulingMode::Exclusive;
  request.requirements.tenant = TenantId{1};
  request.requirements.name_space = NamespaceId{1};
  request.requirements.required_policy_labels = {"test"};
  for (std::string& name : required) {
    CapabilityRequirement requirement;
    requirement.name = name;
    requirement.mode = CapabilityRequirementMode::Required;
    requirement.minimum_state = CapabilityState::Observed;
    request.requirements.capabilities.push_back(std::move(requirement));
  }
  canonicalize(request);
  return request;
}

[[nodiscard]] inline SchedulerOptions test_options(std::shared_ptr<Clock> clock = nullptr,
                                                   std::shared_ptr<ScheduleInterlock> interlock = nullptr) {
  SchedulerOptions options;
  options.scheduler_id = SchedulerId{42};
  options.clock = clock ? clock : std::make_shared<ManualClock>(1000);
  options.interlock = std::move(interlock);
  options.auto_start = true;
  options.policy.id = PolicyId{1};
  options.policy.generation = PolicyGeneration{1};
  options.policy.ranking_weights = default_ranking_weights();
  options.policy.fairness_class_weights = default_fairness_class_weights();
  return options;
}

/// Temporary directory that removes itself.
class ScratchDirectory {
 public:
  explicit ScratchDirectory(const std::string& label) {
    path_ = std::filesystem::temp_directory_path() /
            ("agent_scheduler_test_" + label + "_" + std::to_string(derive(process_nonce(), 1).high));
    std::filesystem::create_directories(path_);
  }
  ~ScratchDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  ScratchDirectory(const ScratchDirectory&) = delete;
  ScratchDirectory& operator=(const ScratchDirectory&) = delete;
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }
  [[nodiscard]] std::filesystem::path file(const std::string& name) const { return path_ / name; }

 private:
  std::filesystem::path path_;
};

}  // namespace as_fixture
