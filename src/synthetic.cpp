// Agent Scheduler — deterministic synthetic scheduler laboratory (SYNTHETIC).
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/synthetic.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace agent_scheduler {
namespace {

[[nodiscard]] Hash128 derive(std::uint64_t seed, std::uint64_t kind, std::uint64_t index) {
  Hasher256 hasher;
  hasher.update_field_tag(static_cast<std::uint8_t>(kind & 0xFFu));
  hasher.update(seed);
  hasher.update(index);
  return hash128_of(hasher.final());
}

[[nodiscard]] FeasibilityVerdict verdict_from(SyntheticRandom& random,
                                             std::uint32_t infeasible_percent,
                                             bool required) {
  if (!required) {
    return FeasibilityVerdict::Unknown;
  }
  return random.next_percent(infeasible_percent) ? FeasibilityVerdict::Infeasible
                                                 : FeasibilityVerdict::Feasible;
}

}  // namespace

std::uint64_t SyntheticRandom::next() noexcept {
  state_ += 0x9E3779B97F4A7C15ull;
  std::uint64_t value = state_;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31);
}

std::uint32_t SyntheticRandom::next_bounded(std::uint32_t bound) noexcept {
  if (bound == 0) {
    return 0;
  }
  return static_cast<std::uint32_t>(next() % bound);
}

bool SyntheticRandom::next_percent(std::uint32_t percent) noexcept {
  if (percent == 0) {
    return false;
  }
  if (percent >= 100) {
    return true;
  }
  return next_bounded(100) < percent;
}

double SyntheticRandom::next_unit() noexcept {
  return static_cast<double>(next() >> 11) / static_cast<double>(1ull << 53);
}

std::string SyntheticLaboratory::capability_name(std::uint32_t index) {
  return "cap:" + std::to_string(index);
}

SyntheticLaboratory::SyntheticLaboratory(SyntheticConfig config) noexcept : config_(config) {
  policy_.id = PolicyId{1};
  policy_.generation = PolicyGeneration{1};
  policy_.ranking_weights = default_ranking_weights();
  policy_.fairness_class_weights = default_fairness_class_weights();
  policy_.max_priority_bypass = 8;
  policy_.aging_rounds_per_step = 1;
  policy_.starvation_alert_rounds = 32;
}

SyntheticPopulation SyntheticLaboratory::generate() const {
  SyntheticPopulation population;
  population.config = config_;
  population.policy = policy_;
  SyntheticRandom random(config_.seed);

  const std::uint32_t queue_count = std::max<std::uint32_t>(1, config_.queue_count);
  const std::uint32_t tenant_count = std::max<std::uint32_t>(1, config_.tenant_count);
  const std::uint32_t pool = std::max<std::uint32_t>(1, config_.capability_pool);
  for (std::uint32_t index = 0; index < queue_count; ++index) {
    population.queues.push_back(QueueId{index + 1});
  }
  for (std::uint32_t index = 0; index < tenant_count; ++index) {
    population.tenants.push_back(TenantId{index + 1});
  }

  const std::uint32_t min_concurrency = std::max<std::uint32_t>(1, config_.min_concurrency);
  const std::uint32_t max_concurrency = std::max(min_concurrency, config_.max_concurrency);

  for (std::uint32_t index = 0; index < config_.agent_count; ++index) {
    AgentDescriptor descriptor;
    descriptor.id = AgentId{index + 1};
    descriptor.generation = AgentGeneration{1};
    descriptor.boot = AgentBootId{derive(config_.seed, 1, index)};
    descriptor.registration_generation = AgentRegistrationGeneration{1};
    descriptor.tenant = population.tenants[index % population.tenants.size()];
    descriptor.name_space = NamespaceId{1};
    descriptor.display_name = "synthetic-agent-" + std::to_string(index);
    descriptor.policy_labels = {"synthetic"};
    if (config_.placement_domains > 0) {
      descriptor.placement_domain =
          PlacementDomainId{1 + (index % std::max<std::uint32_t>(1, config_.placement_domains))};
      descriptor.topology_epoch = TopologyEpoch{1};
    }
    descriptor.max_concurrency =
        min_concurrency + random.next_bounded(max_concurrency - min_concurrency + 1);
    descriptor.lease_ttl_ms = 600000;
    descriptor.provenance = "synthetic-laboratory";
    population.agents.push_back(descriptor);

    CapabilityProfile profile;
    profile.profile_id = CapabilityProfileId{index + 1};
    profile.generation = AgentCapabilityGeneration{1};
    profile.boot = descriptor.boot;
    std::vector<std::string> names;
    const std::uint32_t density = std::max<std::uint32_t>(1, config_.capability_density);
    for (std::uint32_t slot = 0; slot < density; ++slot) {
      names.push_back(capability_name(random.next_bounded(pool)));
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    for (const std::string& name : names) {
      CapabilityEvidence evidence;
      evidence.name = name;
      evidence.state = CapabilityState::Observed;
      evidence.quality = 400 + random.next_bounded(601);
      evidence.generation = profile.generation;
      evidence.boot = descriptor.boot;
      evidence.observed_at_ms = 0;
      evidence.provenance = "synthetic";
      profile.capabilities.push_back(std::move(evidence));
    }
    population.agent_capabilities.push_back(names);
    population.profiles.push_back(std::move(profile));
  }

  const std::uint32_t priority_levels = std::max<std::uint32_t>(1, config_.priority_levels);
  const char* kClasses[] = {"interactive", "default", "batch"};
  for (std::uint32_t index = 0; index < config_.work_count; ++index) {
    WorkRequest request;
    request.id = WorkId{index + 1};
    request.generation = WorkGeneration{1};
    request.queue = population.queues[index % population.queues.size()];
    request.queue_generation = QueueGeneration{1};
    request.kind = "synthetic";
    request.priority = 1000u * (priority_levels - 1 - random.next_bounded(priority_levels)) /
                       (priority_levels == 1 ? 1 : priority_levels - 1);
    request.fairness_class = kClasses[random.next_bounded(3)];
    request.mode = SchedulingMode::Exclusive;
    request.requirements.tenant = population.tenants[index % population.tenants.size()];
    request.requirements.name_space = NamespaceId{1};
    request.requirements.required_policy_labels = {"synthetic"};
    request.requirements.require_healthy = true;
    request.requirements.require_reachable = true;
    request.requirements.require_ready = true;
    request.requirements.require_current_lease = true;

    const std::uint32_t required = 1 + random.next_bounded(std::max<std::uint32_t>(1, config_.capability_density));
    std::vector<std::string> required_names;
    for (std::uint32_t slot = 0; slot < required; ++slot) {
      required_names.push_back(capability_name(random.next_bounded(pool)));
    }
    std::sort(required_names.begin(), required_names.end());
    required_names.erase(std::unique(required_names.begin(), required_names.end()), required_names.end());
    for (const std::string& name : required_names) {
      CapabilityRequirement requirement;
      requirement.name = name;
      requirement.mode = CapabilityRequirementMode::Required;
      requirement.minimum_state = CapabilityState::Observed;
      requirement.minimum_quality = 0;
      request.requirements.capabilities.push_back(std::move(requirement));
    }
    if (config_.affinity_percent > 0 && random.next_percent(config_.affinity_percent) &&
        !population.agents.empty()) {
      request.requirements.affinity.push_back(
          population.agents[random.next_bounded(static_cast<std::uint32_t>(population.agents.size()))].id);
    }
    if (config_.anti_affinity_percent > 0 && random.next_percent(config_.anti_affinity_percent) &&
        !population.agents.empty()) {
      const AgentId candidate =
          population.agents[random.next_bounded(static_cast<std::uint32_t>(population.agents.size()))].id;
      if (std::find(request.requirements.affinity.begin(), request.requirements.affinity.end(), candidate) ==
          request.requirements.affinity.end()) {
        request.requirements.anti_affinity.push_back(candidate);
      }
    }
    if (config_.placement_domains > 1 && random.next_percent(10)) {
      request.requirements.restrict_placement_domain = true;
      request.requirements.placement_domain =
          PlacementDomainId{1 + random.next_bounded(config_.placement_domains)};
      request.requirements.topology_epoch = TopologyEpoch{1};
    }
    if (config_.require_external_feasibility) {
      request.requirements.require_resource_feasibility = true;
      request.requirements.resource_generation = ResourceGeneration{1};
      request.requirements.resource_feasibility =
          verdict_from(random, config_.infeasible_resource_percent, true);
      request.requirements.require_budget_feasibility = true;
      request.requirements.budget_generation = BudgetGeneration{1};
      request.requirements.budget_feasibility =
          verdict_from(random, config_.infeasible_budget_percent, true);
      request.requirements.require_slo_feasibility = true;
      request.requirements.slo_generation = SloGeneration{1};
      request.requirements.slo_feasibility = verdict_from(random, config_.infeasible_slo_percent, true);
    }
    canonicalize(request);
    population.work.push_back(std::move(request));
  }
  return population;
}

SyntheticLaboratory::RunReport SyntheticLaboratory::run(AgentScheduler& scheduler,
                                                        std::uint32_t rounds) const {
  RunReport report;
  const SyntheticPopulation population = generate();
  SyntheticRandom random(config_.seed ^ 0xA5A5A5A5A5A5A5A5ull);
  (void)scheduler.set_policy(population.policy);

  for (std::size_t index = 0; index < population.agents.size(); ++index) {
    const AgentDescriptor& descriptor = population.agents[index];
    if (scheduler.register_agent(descriptor).ok()) {
      ++report.agents_registered;
    }
    (void)scheduler.publish_capabilities(descriptor.id, descriptor.boot, population.profiles[index]);
    HealthObservation health;
    health.generation = AgentHealthGeneration{1};
    health.health = AgentHealth::Healthy;
    health.health_quality = 900;
    health.observed_at_ms = 0;
    (void)scheduler.publish_health(descriptor.id, descriptor.boot, health);
    AvailabilityObservation availability;
    availability.generation = AgentAvailabilityGeneration{1};
    availability.availability = AgentAvailability::Available;
    availability.reachability = AgentReachability::Reachable;
    availability.readiness = AgentReadiness::Ready;
    availability.observed_at_ms = 0;
    (void)scheduler.publish_availability(descriptor.id, descriptor.boot, availability);
    LoadObservation load;
    load.generation = AgentLoadGeneration{1};
    load.max_concurrency = descriptor.max_concurrency;
    load.estimated_dispatch_latency_ms = 1 + random.next_bounded(500);
    load.cost_index = random.next_unit();
    load.slo_headroom_fraction = random.next_unit();
    load.observed_at_ms = 0;
    (void)scheduler.publish_load(descriptor.id, descriptor.boot, load);
  }

  for (const WorkRequest& request : population.work) {
    if (scheduler.submit_work(request).ok()) {
      ++report.work_admitted;
    }
  }

  for (std::uint32_t round = 0; round < rounds; ++round) {
    ScheduleRequest request;
    request.max_assignments = 16;
    const BatchDecision decision = scheduler.schedule_batch(request);
    report.assignments += decision.assigned;
    report.deferred += decision.deferred;
    report.rejected += decision.rejected;
    ++report.rounds;

    if (config_.agent_death_percent > 0 && random.next_percent(config_.agent_death_percent) &&
        !population.agents.empty()) {
      const AgentDescriptor& victim =
          population.agents[random.next_bounded(static_cast<std::uint32_t>(population.agents.size()))];
      if (scheduler.declare_agent_lost(victim.id, victim.boot, "synthetic loss").ok()) {
        ++report.agent_deaths;
      }
    }
    if (config_.reincarnation_percent > 0 && random.next_percent(config_.reincarnation_percent) &&
        !population.agents.empty()) {
      const AgentDescriptor& base =
          population.agents[random.next_bounded(static_cast<std::uint32_t>(population.agents.size()))];
      const auto status = scheduler.agent_status(base.id);
      if (status.has_value()) {
        AgentDescriptor replacement = base;
        replacement.generation = AgentGeneration{status->generation.value() + 1};
        replacement.boot = AgentBootId{derive(config_.seed, 2, round * 1000 + base.id.value())};
        replacement.registration_generation = AgentRegistrationGeneration{1};
        if (scheduler.register_agent(replacement).ok()) {
          CapabilityProfile profile;
          profile.profile_id = CapabilityProfileId{base.id.value()};
          profile.generation = AgentCapabilityGeneration{1};
          profile.boot = replacement.boot;
          for (const std::string& name : population.agent_capabilities[base.id.value() - 1]) {
            CapabilityEvidence evidence;
            evidence.name = name;
            evidence.state = CapabilityState::Observed;
            evidence.quality = 500;
            evidence.generation = profile.generation;
            evidence.boot = replacement.boot;
            evidence.observed_at_ms = 0;
            profile.capabilities.push_back(std::move(evidence));
          }
          (void)scheduler.publish_capabilities(base.id, replacement.boot, profile);
          HealthObservation health;
          health.generation = AgentHealthGeneration{1};
          health.health = AgentHealth::Healthy;
          health.health_quality = 900;
          (void)scheduler.publish_health(base.id, replacement.boot, health);
          AvailabilityObservation availability;
          availability.generation = AgentAvailabilityGeneration{1};
          availability.availability = AgentAvailability::Available;
          availability.reachability = AgentReachability::Reachable;
          availability.readiness = AgentReadiness::Ready;
          (void)scheduler.publish_availability(base.id, replacement.boot, availability);
          ++report.reincarnations;
        }
      }
    }
    if (config_.health_degradation_percent > 0 && random.next_percent(config_.health_degradation_percent) &&
        !population.agents.empty()) {
      const AgentDescriptor& victim =
          population.agents[random.next_bounded(static_cast<std::uint32_t>(population.agents.size()))];
      const auto status = scheduler.agent_status(victim.id);
      if (status.has_value()) {
        HealthObservation health;
        health.generation = AgentHealthGeneration{status->health_generation.value() + 1};
        health.health = AgentHealth::Degraded;
        health.health_quality = 400;
        (void)scheduler.publish_health(victim.id, status->boot, health);
      }
    }
    if (config_.availability_change_percent > 0 &&
        random.next_percent(config_.availability_change_percent) && !population.agents.empty()) {
      const AgentDescriptor& victim =
          population.agents[random.next_bounded(static_cast<std::uint32_t>(population.agents.size()))];
      const auto status = scheduler.agent_status(victim.id);
      if (status.has_value()) {
        AvailabilityObservation availability;
        availability.generation = AgentAvailabilityGeneration{status->availability_generation.value() + 1};
        availability.availability = random.next_percent(50) ? AgentAvailability::Available
                                                            : AgentAvailability::Unavailable;
        availability.reachability = AgentReachability::Reachable;
        availability.readiness =
            availability.availability == AgentAvailability::Available ? AgentReadiness::Ready
                                                                      : AgentReadiness::NotReady;
        (void)scheduler.publish_availability(victim.id, status->boot, availability);
      }
    }
  }

  report.starved = scheduler.fairness().starved_count;
  report.final_state_digest = scheduler.state_digest();
  report.invariants = scheduler.check_invariants();
  return report;
}

}  // namespace agent_scheduler
