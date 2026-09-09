// Agent Scheduler — deterministic synthetic laboratory.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "scheduler_fixture.hpp"
#include "test_support.hpp"

using namespace agent_scheduler;
using namespace as_fixture;

AS_TEST(synthetic, population_generation_is_deterministic) {
  SyntheticConfig config;
  config.seed = 4242;
  config.agent_count = 32;
  config.work_count = 200;
  config.queue_count = 4;
  config.tenant_count = 3;
  config.capability_density = 5;
  config.placement_domains = 3;
  const SyntheticLaboratory first(config);
  const SyntheticLaboratory second(config);
  const SyntheticPopulation left = first.generate();
  const SyntheticPopulation right = second.generate();
  AS_CHECK(left.agents.size() == right.agents.size());
  AS_CHECK(left.work.size() == right.work.size());
  for (std::size_t index = 0; index < left.agents.size(); ++index) {
    AS_CHECK(left.agents[index].id == right.agents[index].id);
    AS_CHECK(left.agents[index].boot == right.agents[index].boot);
    AS_CHECK(left.agents[index].max_concurrency == right.agents[index].max_concurrency);
    AS_CHECK(left.agent_capabilities[index] == right.agent_capabilities[index]);
  }
  for (std::size_t index = 0; index < left.work.size(); ++index) {
    AS_CHECK(left.work[index] == right.work[index]);
  }
  SyntheticConfig other = config;
  other.seed = 4243;
  AS_CHECK(SyntheticLaboratory(other).generate().work.front() != left.work.front());
}

AS_TEST(synthetic, runs_are_reproducible_and_invariants_hold) {
  SyntheticConfig config;
  config.seed = 90210;
  config.agent_count = 24;
  config.work_count = 120;
  config.queue_count = 4;
  config.tenant_count = 3;
  config.capability_density = 4;
  config.min_concurrency = 1;
  config.max_concurrency = 4;
  config.priority_levels = 6;
  config.placement_domains = 2;
  config.agent_death_percent = 10;
  config.reincarnation_percent = 15;
  config.capability_churn_percent = 10;
  config.health_degradation_percent = 10;
  config.availability_change_percent = 10;
  config.affinity_percent = 10;
  config.anti_affinity_percent = 5;
  const SyntheticLaboratory laboratory(config);
  AgentScheduler first(test_options());
  const SyntheticLaboratory::RunReport left = laboratory.run(first, 16);
  AgentScheduler second(test_options());
  const SyntheticLaboratory::RunReport right = laboratory.run(second, 16);
  AS_CHECK(left.assignments == right.assignments);
  AS_CHECK(left.rejected == right.rejected);
  AS_CHECK(left.deferred == right.deferred);
  AS_CHECK(left.agent_deaths == right.agent_deaths);
  AS_CHECK(left.reincarnations == right.reincarnations);
  AS_CHECK(left.final_state_digest == right.final_state_digest);
  AS_CHECK(left.invariants.ok());
  AS_CHECK(first.check_invariants().ok());
}

AS_TEST(synthetic, scale_population_generation_and_scheduling) {
  SyntheticConfig config;
  config.seed = 777;
  config.agent_count = 1000;
  config.work_count = 4000;
  config.queue_count = 8;
  config.tenant_count = 4;
  config.capability_density = 6;
  config.capability_pool = 24;
  config.placement_domains = 4;
  config.min_concurrency = 1;
  config.max_concurrency = 4;
  const SyntheticLaboratory laboratory(config);
  const SyntheticPopulation population = laboratory.generate();
  AS_CHECK(population.agents.size() == 1000);
  AS_CHECK(population.work.size() == 4000);
  AgentScheduler scheduler(test_options());
  const SyntheticLaboratory::RunReport report = laboratory.run(scheduler, 3);
  AS_CHECK(report.agents_registered == 1000);
  AS_CHECK(report.work_admitted == 4000);
  AS_CHECK(report.assignments > 0);
  AS_CHECK(report.invariants.ok());
  const SchedulerSummary summary = scheduler.summary();
  AS_CHECK(summary.agent_count == 1000);
  AS_CHECK(summary.used_capacity <= summary.total_capacity);
}

AS_TEST(synthetic, infeasible_work_is_never_assigned) {
  SyntheticConfig config;
  config.seed = 5150;
  config.agent_count = 16;
  config.work_count = 64;
  config.capability_density = 4;
  config.require_external_feasibility = true;
  config.infeasible_resource_percent = 50;
  config.infeasible_budget_percent = 50;
  config.infeasible_slo_percent = 50;
  const SyntheticLaboratory laboratory(config);
  AgentScheduler scheduler(test_options());
  const SyntheticLaboratory::RunReport report = laboratory.run(scheduler, 6);
  AS_CHECK(report.invariants.ok());
  for (const Assignment& assignment : scheduler.all_assignments()) {
    if (!assignment_state_is_active(assignment.state)) {
      continue;
    }
    const auto work = scheduler.work_status(assignment.binding.work);
    AS_REQUIRE(work.has_value());
    AS_CHECK(work->lifecycle == WorkLifecycle::Assigned || work->lifecycle == WorkLifecycle::Executing);
  }
}
