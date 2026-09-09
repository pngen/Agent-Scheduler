// Agent Scheduler — invariant checking and index/canonical agreement.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "scheduler_fixture.hpp"
#include "test_support.hpp"

using namespace agent_scheduler;
using namespace as_fixture;

AS_TEST(invariants, clean_state_passes_every_check) {
  AgentScheduler scheduler(test_options());
  for (std::uint64_t index = 1; index <= 4; ++index) {
    AS_CHECK(register_full(scheduler, make_agent(index, index * 3, 2), {{"coding", 900}}).ok());
  }
  for (std::uint64_t index = 1; index <= 8; ++index) {
    AS_CHECK(scheduler.submit_work(make_work(400 + index, {"coding"}, static_cast<std::uint32_t>(index))).ok());
  }
  (void)scheduler.schedule_batch(ScheduleRequest{});
  const InvariantReport report = scheduler.check_invariants();
  AS_CHECK(report.ok());
  AS_CHECK(report.checks_run > 10);
  AS_CHECK(report.describe().find("OK") == 0);
}

AS_TEST(invariants, index_and_canonical_state_agree_after_churn) {
  AgentScheduler scheduler(test_options());
  SyntheticConfig config;
  config.seed = 20260214;
  config.agent_count = 12;
  config.work_count = 60;
  config.queue_count = 3;
  config.tenant_count = 2;
  config.capability_density = 4;
  config.min_concurrency = 1;
  config.max_concurrency = 3;
  config.priority_levels = 4;
  config.placement_domains = 2;
  config.agent_death_percent = 15;
  config.reincarnation_percent = 20;
  config.health_degradation_percent = 10;
  config.availability_change_percent = 10;
  config.affinity_percent = 10;
  config.anti_affinity_percent = 5;
  const SyntheticLaboratory laboratory(config);
  const SyntheticLaboratory::RunReport report = laboratory.run(scheduler, 24);
  AS_CHECK(report.invariants.ok());
  AS_CHECK(report.agents_registered == config.agent_count);
  AS_CHECK(report.work_admitted == config.work_count);
  AS_CHECK(scheduler.check_invariants().ok());
}

AS_TEST(invariants, generation_monotonicity_is_observable) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor descriptor = make_agent(1, 1, 1);
  AS_CHECK(register_full(scheduler, descriptor, {{"coding", 900}}).ok());
  AS_CHECK(scheduler.submit_work(make_work(500, {"coding"})).ok());
  AssignmentGeneration previous{};
  for (std::uint32_t attempt = 0; attempt < 5; ++attempt) {
    const ScheduleDecision decision = scheduler.schedule(WorkId{500}, WorkGeneration{1});
    AS_REQUIRE(decision.assignment.has_value());
    AS_CHECK(decision.assignment->generation > previous);
    previous = decision.assignment->generation;
    AS_CHECK(scheduler
                 .release(decision.assignment->id, decision.assignment->generation, descriptor.boot, "cycle")
                 .ok());
  }
  AS_CHECK(scheduler.check_invariants().ok());
  AS_CHECK(scheduler.work_status(WorkId{500})->last_assignment_generation == previous);
}

AS_TEST(invariants, shutdown_closes_admission_and_dispatch_authority) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor descriptor = make_agent(1, 1, 2);
  AS_CHECK(register_full(scheduler, descriptor, {{"coding", 900}}).ok());
  AS_CHECK(scheduler.submit_work(make_work(600, {"coding"})).ok());
  const ScheduleDecision decision = scheduler.schedule(WorkId{600}, WorkGeneration{1});
  AS_REQUIRE(decision.assignment.has_value());
  AS_CHECK(scheduler.shutdown().ok());
  AS_CHECK(scheduler.check_invariants().ok());
  const auto stored = scheduler.assignment(decision.assignment->id);
  AS_REQUIRE(stored.has_value());
  AS_CHECK(stored->state == AssignmentState::Invalidated);
  AS_CHECK(stored->invalidation == InvalidationReason::Shutdown);
  const MutationResult admitted = scheduler.submit_work(make_work(601, {"coding"}));
  AS_CHECK(!admitted.ok());
  AS_CHECK(admitted.error->outcome == ScheduleOutcome::ShuttingDown);
  AS_CHECK(scheduler.schedule(WorkId{600}, WorkGeneration{1}).outcome == ScheduleOutcome::ShuttingDown ||
           scheduler.schedule(WorkId{600}, WorkGeneration{1}).outcome == ScheduleOutcome::NoChange);
  AS_CHECK(scheduler.start().ok());
  AS_CHECK(scheduler.submit_work(make_work(602, {"coding"})).ok());
  AS_CHECK(scheduler.check_invariants().ok());
}
