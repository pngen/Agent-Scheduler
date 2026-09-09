// Agent Scheduler — deterministic randomized property testing.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "scheduler_fixture.hpp"
#include "test_support.hpp"

using namespace agent_scheduler;
using namespace as_fixture;

namespace {

/// Independent reference implementation of the hard-eligibility predicate. It is
/// written from the public contract only and never consults scheduler internals.
[[nodiscard]] bool reference_eligible(const AgentStatus& agent, const WorkRequest& request) {
  if (agent.lifecycle != AgentLifecycle::Ready && agent.lifecycle != AgentLifecycle::Busy) {
    return false;
  }
  if (request.requirements.require_healthy &&
      (!agent.evidence_current || agent.health == AgentHealth::Unknown ||
       agent.health == AgentHealth::Unhealthy)) {
    return false;
  }
  if (request.requirements.require_reachable &&
      (!agent.evidence_current || agent.reachability != AgentReachability::Reachable)) {
    return false;
  }
  if (request.requirements.require_current_lease && !agent.lease_current) {
    return false;
  }
  if (agent.active_assignments >= agent.max_concurrency) {
    return false;
  }
  if (!request.requirements.tenant.is_zero() && request.requirements.tenant != agent.tenant) {
    return false;
  }
  for (const std::string& label : request.requirements.required_policy_labels) {
    if (std::find(agent.policy_labels.begin(), agent.policy_labels.end(), label) ==
        agent.policy_labels.end()) {
      return false;
    }
  }
  for (const CapabilityRequirement& requirement : request.requirements.capabilities) {
    if (requirement.mode != CapabilityRequirementMode::Required) {
      continue;
    }
    const auto found = std::find_if(
        agent.capabilities.begin(), agent.capabilities.end(),
        [&requirement](const CapabilityEvidence& evidence) { return evidence.name == requirement.name; });
    if (found == agent.capabilities.end()) {
      return false;
    }
    if (capability_state_rank(found->state) < capability_state_rank(requirement.minimum_state)) {
      return false;
    }
    if (found->quality < requirement.minimum_quality) {
      return false;
    }
    if (found->boot != agent.boot || found->generation != agent.capability_generation) {
      return false;
    }
  }
  return true;
}

struct Population {
  std::vector<AgentDescriptor> agents;
  std::vector<std::vector<std::string>> capabilities;
  std::vector<WorkRequest> work;
};

[[nodiscard]] Population generate(std::uint64_t seed, std::uint32_t agent_count, std::uint32_t work_count) {
  SyntheticRandom random(seed);
  Population population;
  const char* kPool[] = {"coding", "research", "tool:shell", "accelerator:cuda", "platform:windows",
                         "language:cpp"};
  for (std::uint32_t index = 0; index < agent_count; ++index) {
    AgentDescriptor descriptor = make_agent(index + 1, seed + index, 1 + random.next_bounded(3));
    descriptor.tenant = TenantId{1 + random.next_bounded(2)};
    population.agents.push_back(descriptor);
    std::vector<std::string> names;
    const std::uint32_t density = 1 + random.next_bounded(3);
    for (std::uint32_t slot = 0; slot < density; ++slot) {
      names.emplace_back(kPool[random.next_bounded(6)]);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    population.capabilities.push_back(names);
  }
  for (std::uint32_t index = 0; index < work_count; ++index) {
    std::vector<std::string> required;
    const std::uint32_t count = 1 + random.next_bounded(2);
    for (std::uint32_t slot = 0; slot < count; ++slot) {
      required.emplace_back(kPool[random.next_bounded(6)]);
    }
    std::sort(required.begin(), required.end());
    required.erase(std::unique(required.begin(), required.end()), required.end());
    WorkRequest request = make_work(9000 + index, required, random.next_bounded(1000));
    request.requirements.tenant = TenantId{1 + random.next_bounded(2)};
    canonicalize(request);
    population.work.push_back(request);
  }
  return population;
}

[[nodiscard]] std::set<std::uint64_t> eligible_from_decision(const ScheduleDecision& decision) {
  std::set<std::uint64_t> eligible;
  for (const CandidateEvaluation& candidate : decision.candidates) {
    if (candidate.eligible) {
      eligible.insert(candidate.agent.value());
    }
  }
  return eligible;
}

}  // namespace

AS_TEST(property, candidate_filtering_matches_an_independent_reference) {
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    const Population population = generate(seed, 6, 6);
    AgentScheduler scheduler(test_options());
    for (std::size_t index = 0; index < population.agents.size(); ++index) {
      const AgentDescriptor& descriptor = population.agents[index];
      AS_CHECK(register_full(scheduler, descriptor, {{"unused", 0}}, 0).ok());
      CapabilityProfile profile;
      profile.profile_id = CapabilityProfileId{descriptor.id.value()};
      profile.generation = AgentCapabilityGeneration{2};
      profile.boot = descriptor.boot;
      for (const std::string& name : population.capabilities[index]) {
        CapabilityEvidence evidence;
        evidence.name = name;
        evidence.state = CapabilityState::Observed;
        evidence.quality = 400 + static_cast<std::uint32_t>(seed % 600);
        evidence.generation = profile.generation;
        evidence.boot = descriptor.boot;
        profile.capabilities.push_back(std::move(evidence));
      }
      AS_CHECK(scheduler.publish_capabilities(descriptor.id, descriptor.boot, profile).ok());
    }
    for (const WorkRequest& request : population.work) {
      AS_CHECK(scheduler.submit_work(request).ok());
    }
    for (const WorkRequest& request : population.work) {
      // The reference view is captured from the same pre-state the scheduler sees.
      std::set<std::uint64_t> reference;
      for (const AgentStatus& agent : scheduler.all_agents()) {
        if (reference_eligible(agent, request)) {
          reference.insert(agent.id.value());
        }
      }
      const ScheduleDecision decision = scheduler.schedule(request.id, request.generation);
      const std::set<std::uint64_t> actual = eligible_from_decision(decision);
      if (reference != actual) {
        std::string detail = "seed " + std::to_string(seed) + " work " + request.id.to_string() +
                             " reference=" + std::to_string(reference.size()) +
                             " scheduler=" + std::to_string(actual.size());
        for (const AgentStatus& agent : scheduler.all_agents()) {
          const bool in_reference = reference.count(agent.id.value()) == 1;
          const bool in_actual = actual.count(agent.id.value()) == 1;
          if (in_reference == in_actual) {
            continue;
          }
          detail += " agent=" + agent.id.to_string();
          detail += in_reference ? " reference=yes scheduler=no" : " reference=no scheduler=yes";
          for (const CandidateEvaluation& candidate : decision.candidates) {
            if (candidate.agent == agent.id) {
              detail += std::string(" scheduler_rejection=") + to_string(candidate.rejection) + "(" +
                        candidate.rejection_detail + ")";
            }
          }
          detail += " lifecycle=" + std::string(to_string(agent.lifecycle));
          detail += " health=" + std::string(to_string(agent.health));
          detail += " reach=" + std::string(to_string(agent.reachability));
          detail += " lease=" + std::string(agent.lease_current ? "current" : "stale");
          detail += " evidence=" + std::string(agent.evidence_current ? "current" : "stale");
          detail += " caps=" + std::to_string(agent.capabilities.size());
          detail += " capacity=" + std::to_string(agent.max_concurrency) + "/" +
                    std::to_string(agent.active_assignments);
        }
        AS_FAIL(detail);
      }
      if (decision.assigned()) {
        AS_CHECK(reference.count(decision.selected_agent.value()) == 1);
      } else {
        AS_CHECK(reference.empty());
      }
      // Release so the next item sees a quiesced capacity picture.
      for (const Assignment& assignment : scheduler.all_assignments()) {
        if (assignment_state_is_active(assignment.state)) {
          (void)scheduler.release(assignment.binding.id, assignment.binding.generation, assignment.binding.boot,
                                  "property");
        }
      }
      AS_CHECK(scheduler.check_invariants().ok());
    }
  }
}

AS_TEST(property, random_operation_sequences_preserve_invariants) {
  for (std::uint64_t seed = 1; seed <= 12; ++seed) {
    SyntheticConfig config;
    config.seed = seed;
    config.agent_count = 10;
    config.work_count = 40;
    config.queue_count = 3;
    config.tenant_count = 2;
    config.capability_density = 3;
    config.min_concurrency = 1;
    config.max_concurrency = 3;
    config.priority_levels = 5;
    config.placement_domains = 2;
    config.agent_death_percent = 20;
    config.reincarnation_percent = 25;
    config.capability_churn_percent = 20;
    config.health_degradation_percent = 15;
    config.availability_change_percent = 15;
    config.affinity_percent = 10;
    config.anti_affinity_percent = 5;
    config.infeasible_resource_percent = 20;
    config.require_external_feasibility = true;
    AgentScheduler scheduler(test_options());
    const SyntheticLaboratory laboratory(config);
    const SyntheticLaboratory::RunReport report = laboratory.run(scheduler, 20);
    if (!report.invariants.ok()) {
      AS_FAIL("seed " + std::to_string(seed) + " invariants: " + report.invariants.describe());
    }
    AS_CHECK(scheduler.check_invariants().ok());
  }
}

AS_TEST(property, save_load_preserves_durable_semantics_for_random_state) {
  ScratchDirectory scratch("property_persistence");
  for (std::uint64_t seed = 1; seed <= 6; ++seed) {
    const std::filesystem::path path = scratch.file("state_" + std::to_string(seed) + ".asstate");
    SyntheticConfig config;
    config.seed = seed * 31;
    config.agent_count = 8;
    config.work_count = 30;
    config.queue_count = 2;
    config.capability_density = 3;
    config.agent_death_percent = 20;
    config.reincarnation_percent = 20;
    AgentScheduler scheduler(test_options());
    const SyntheticLaboratory laboratory(config);
    const SyntheticLaboratory::RunReport report = laboratory.run(scheduler, 12);
    if (!report.invariants.ok()) {
      AS_FAIL("seed " + std::to_string(seed) + " invariants before save: " + report.invariants.describe());
    }
    const SchedulerSummary before = scheduler.summary();
    AS_CHECK(scheduler.save(path).ok());

    SchedulerOptions options = test_options();
    options.auto_start = false;
    AgentScheduler recovered(options);
    const RecoveryResult recovery = recovered.load(path);
    if (!recovery.ok()) {
      AS_FAIL("seed " + std::to_string(seed) + " recovery: " + recovery.describe());
    }
    AS_CHECK(recovered.summary().agent_count == before.agent_count);
    AS_CHECK(recovered.summary().queue_count == before.queue_count);
    AS_CHECK(recovered.summary().fenced_boots == before.fenced_boots);
    for (const AgentStatus& agent : recovered.all_agents()) {
      AS_CHECK(!agent.evidence_current);
    }
    AS_CHECK(recovered.check_invariants().ok());
  }
}

AS_TEST(property, deterministic_state_digest_across_insertion_orders) {
  for (std::uint64_t seed = 1; seed <= 10; ++seed) {
    const Population population = generate(seed, 5, 5);
    const auto run = [&](bool reverse) {
      AgentScheduler scheduler(test_options());
      std::vector<std::size_t> order;
      for (std::size_t index = 0; index < population.agents.size(); ++index) {
        order.push_back(index);
      }
      if (reverse) {
        std::reverse(order.begin(), order.end());
      }
      for (const std::size_t index : order) {
        AS_CHECK(register_full(scheduler, population.agents[index], {{"coding", 900}}).ok());
      }
      for (const WorkRequest& request : population.work) {
        AS_CHECK(scheduler.submit_work(request).ok());
      }
      (void)scheduler.schedule_batch(ScheduleRequest{});
      return std::make_pair(scheduler.state_digest(), scheduler.summary());
    };
    const auto forward = run(false);
    const auto backward = run(true);
    AS_CHECK(forward.first == backward.first);
    AS_CHECK(forward.second.active_assignments == backward.second.active_assignments);
  }
}
