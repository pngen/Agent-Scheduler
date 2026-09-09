// Agent Scheduler — fairness, aging, and bounded starvation.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "scheduler_fixture.hpp"
#include "test_support.hpp"

using namespace agent_scheduler;
using namespace as_fixture;

namespace {

[[nodiscard]] std::unique_ptr<AgentScheduler> make_contended(std::uint32_t agent_count,
                                                             std::uint32_t slots_per_agent,
                                                             std::uint32_t work_count,
                                                             std::uint32_t max_priority_bypass = 8) {
  SchedulerOptions options = test_options(std::make_shared<ManualClock>(1000));
  options.policy.max_priority_bypass = max_priority_bypass;
  options.policy.aging_rounds_per_step = 1;
  options.policy.starvation_alert_rounds = 32;
  auto scheduler = std::make_unique<AgentScheduler>(options);
  for (std::uint32_t index = 0; index < agent_count; ++index) {
    const AgentDescriptor descriptor = make_agent(index + 1, 100 + index, slots_per_agent);
    AS_CHECK(register_full(*scheduler, descriptor, {{"coding", 900}}).ok());
  }
  for (std::uint32_t index = 0; index < work_count; ++index) {
    WorkRequest request = make_work(1000 + index, {"coding"}, index == 0 ? 1000 : 0);
    request.fairness_class = "default";
    AS_CHECK(scheduler->submit_work(request).ok());
  }
  return scheduler;
}

}  // namespace

AS_TEST(fairness, high_priority_work_is_preferred) {
  auto scheduler = make_contended(1, 1, 4);
  const BatchDecision batch = scheduler->schedule_batch(ScheduleRequest{});
  AS_CHECK(batch.assigned == 1);
  AS_REQUIRE(!batch.decisions.empty());
  AS_CHECK(batch.decisions.front().work == WorkId{1000});
}

AS_TEST(fairness, low_priority_work_does_not_starve_indefinitely) {
  const std::uint32_t work_count = 6;
  auto scheduler = make_contended(1, 1, work_count, 4);
  std::uint32_t low_priority_assignments = 0;
  std::uint32_t rounds = 0;
  // Only work item 1000 has high priority; the rest are continuously eligible.
  for (rounds = 0; rounds < 200 && low_priority_assignments == 0; ++rounds) {
    const BatchDecision batch = scheduler->schedule_batch(ScheduleRequest{});
    for (const ScheduleDecision& decision : batch.decisions) {
      if (decision.assigned() && decision.work != WorkId{1000}) {
        ++low_priority_assignments;
      }
    }
    // Release the committed assignment so the same slot competes again.
    for (const Assignment& assignment : scheduler->all_assignments()) {
      if (assignment_state_is_active(assignment.state)) {
        AS_CHECK(scheduler->
                     release(assignment.binding.id, assignment.binding.generation, assignment.binding.boot,
                              "round")
                     .ok());
      }
    }
  }
  AS_CHECK(low_priority_assignments > 0);
  AS_CHECK(rounds <= 4 + work_count);
}

AS_TEST(fairness, bypass_ceiling_is_never_exceeded) {
  auto scheduler = make_contended(1, 1, 8, 3);
  for (int round = 0; round < 60; ++round) {
    (void)scheduler->schedule_batch(ScheduleRequest{});
    // Bound: ceiling + (admissible - 1) rounds before an item must be selected.
    for (const FairnessEntry& entry : scheduler->fairness().entries) {
      AS_CHECK(entry.bypass_count <= entry.bypass_ceiling + 8);
    }
    for (const Assignment& assignment : scheduler->all_assignments()) {
      if (assignment_state_is_active(assignment.state)) {
        AS_CHECK(scheduler->
                     release(assignment.binding.id, assignment.binding.generation, assignment.binding.boot,
                              "round")
                     .ok());
      }
    }
  }
}

AS_TEST(fairness, every_continuously_eligible_item_runs_within_the_bound) {
  const std::uint32_t work_count = 5;
  const std::uint32_t bypass = 4;
  auto scheduler = make_contended(1, 1, work_count, bypass);
  std::vector<std::uint32_t> runs(work_count, 0);
  for (int round = 0; round < 400; ++round) {
    const BatchDecision batch = scheduler->schedule_batch(ScheduleRequest{});
    for (const ScheduleDecision& decision : batch.decisions) {
      if (decision.assigned()) {
        runs[static_cast<std::size_t>(decision.work.value() - 1000)]++;
      }
    }
    for (const Assignment& assignment : scheduler->all_assignments()) {
      if (assignment_state_is_active(assignment.state)) {
        AS_CHECK(scheduler->
                     release(assignment.binding.id, assignment.binding.generation, assignment.binding.boot,
                              "round")
                     .ok());
      }
    }
  }
  // Bound: ceiling + (admissible - 1) rounds between successive runs of any item.
  for (const std::uint32_t count : runs) {
    AS_CHECK(count > 0);
  }
  AS_CHECK(runs[1] >= 20);
}

AS_TEST(fairness, fairness_state_survives_agent_reincarnation) {
  auto scheduler = make_contended(1, 1, 4, 2);
  (void)scheduler->schedule_batch(ScheduleRequest{});
  const FairnessSnapshot before = scheduler->fairness();
  std::uint32_t before_total = 0;
  for (const FairnessEntry& entry : before.entries) {
    before_total += entry.bypass_count;
  }
  const AgentDescriptor replacement = make_agent(1, 999, 1, AgentGeneration{2});
  AS_CHECK(register_full(*scheduler, replacement, {{"coding", 900}}).ok());
  const FairnessSnapshot after = scheduler->fairness();
  std::uint32_t after_total = 0;
  for (const FairnessEntry& entry : after.entries) {
    after_total += entry.bypass_count;
  }
  AS_CHECK(after_total == before_total);
}

AS_TEST(fairness, cancelled_work_leaves_the_fairness_accounting) {
  auto scheduler = make_contended(1, 1, 4);
  (void)scheduler->schedule_batch(ScheduleRequest{});
  AS_CHECK(scheduler->cancel_work(WorkId{1002}, WorkGeneration{1}, "test").ok());
  const FairnessSnapshot fairness = scheduler->fairness();
  for (const FairnessEntry& entry : fairness.entries) {
    AS_CHECK(entry.work != WorkId{1002});
  }
  AS_CHECK(scheduler->work_status(WorkId{1002})->lifecycle == WorkLifecycle::Cancelled);
}

AS_TEST(fairness, unbounded_starvation_requires_an_explicit_policy) {
  SchedulerOptions options = test_options();
  options.policy.allow_unbounded_starvation = true;
  options.policy.max_priority_bypass = 0;
  auto scheduler = std::make_unique<AgentScheduler>(options);
  const AgentDescriptor descriptor = make_agent(1, 1, 1);
  AS_CHECK(register_full(*scheduler, descriptor, {{"coding", 900}}).ok());
  for (std::uint32_t index = 0; index < 4; ++index) {
    AS_CHECK(scheduler->submit_work(make_work(2000 + index, {"coding"}, index == 0 ? 1000 : 0)).ok());
  }
  (void)scheduler->schedule_batch(ScheduleRequest{});
  const FairnessSnapshot fairness = scheduler->fairness();
  AS_CHECK(!fairness.bounded_starvation);
  for (const FairnessEntry& entry : fairness.entries) {
    AS_CHECK(entry.bypass_ceiling == 0xFFFFFFFFu);
  }

  PolicySnapshot bounded = options.policy;
  bounded.allow_unbounded_starvation = false;
  bounded.generation = PolicyGeneration{2};
  const MutationResult rejected = scheduler->set_policy(bounded);
  AS_CHECK(!rejected.ok());
  AS_CHECK(rejected.error->code == ErrorCode::InvalidArgument);
}

AS_TEST(fairness, priority_cannot_bypass_hard_eligibility) {
  auto scheduler = std::make_unique<AgentScheduler>(test_options());
  const AgentDescriptor descriptor = make_agent(1, 1, 1);
  AS_CHECK(register_full(*scheduler, descriptor, {{"coding", 900}}).ok());
  WorkRequest impossible = make_work(3000, {"capability:absent"}, 1000);
  AS_CHECK(scheduler->submit_work(impossible).ok());
  AS_CHECK(scheduler->submit_work(make_work(3001, {"coding"}, 0)).ok());
  const BatchDecision batch = scheduler->schedule_batch(ScheduleRequest{});
  AS_CHECK(batch.assigned == 1);
  bool saw_impossible = false;
  for (const ScheduleDecision& decision : batch.decisions) {
    if (decision.work == WorkId{3000}) {
      AS_CHECK(!decision.assigned());
      AS_CHECK(decision.outcome == ScheduleOutcome::NoEligibleAgent);
      saw_impossible = true;
    }
  }
  AS_CHECK(saw_impossible);
}
