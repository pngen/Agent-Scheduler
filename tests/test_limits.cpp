// Agent Scheduler — resource bounds, validation, and adversarial input.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include <limits>
#include <string>

#include "scheduler_fixture.hpp"
#include "test_support.hpp"

using namespace agent_scheduler;
using namespace as_fixture;

AS_TEST(limits, checked_arithmetic_never_wraps) {
  AS_CHECK(!checked_add(std::numeric_limits<std::uint64_t>::max(), std::uint64_t{1}).has_value());
  AS_CHECK(checked_add(std::uint64_t{2}, std::uint64_t{3}).value() == 5);
  AS_CHECK(!checked_mul(std::numeric_limits<std::uint64_t>::max(), std::uint64_t{2}).has_value());
  AS_CHECK(checked_mul(std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()).value() == 0);
  AS_CHECK(!checked_add(std::numeric_limits<std::int64_t>::max(), std::int64_t{1}).has_value());
  AS_CHECK(!checked_mul(std::numeric_limits<std::int64_t>::max(), std::int64_t{2}).has_value());
  AS_CHECK(ResourceLimits{}.validate() == std::nullopt);
  ResourceLimits broken;
  broken.max_agents = 0;
  AS_CHECK(broken.validate().has_value());
  ResourceLimits inconsistent;
  inconsistent.max_payload_size = inconsistent.max_frame_size + 1;
  AS_CHECK(inconsistent.validate().has_value());
}

AS_TEST(limits, agent_population_is_bounded) {
  SchedulerOptions options = test_options();
  options.limits.max_agents = 2;
  AgentScheduler scheduler(options);
  AS_CHECK(register_full(scheduler, make_agent(1, 1), {{"coding", 900}}).ok());
  AS_CHECK(register_full(scheduler, make_agent(2, 2), {{"coding", 900}}).ok());
  const MutationResult third = register_full(scheduler, make_agent(3, 3), {{"coding", 900}});
  AS_CHECK(!third.ok());
  AS_CHECK(third.error->code == ErrorCode::LimitExceeded);
  AS_CHECK(third.error->outcome == ScheduleOutcome::RejectLimitExceeded);
}

AS_TEST(limits, work_and_capability_collections_are_bounded) {
  SchedulerOptions options = test_options();
  options.limits.max_work_items = 2;
  options.limits.max_capabilities_per_agent = 3;
  AgentScheduler scheduler(options);
  const AgentDescriptor descriptor = make_agent(1, 1);
  AS_CHECK(register_full(scheduler, descriptor,
                         {{"a", 1}, {"b", 1}, {"c", 1}, {"d", 1}})
               .error.has_value());
  AS_CHECK(register_full(scheduler, descriptor, {{"a", 1}, {"b", 1}, {"c", 1}}).ok());
  AS_CHECK(scheduler.submit_work(make_work(1, {"a"})).ok());
  AS_CHECK(scheduler.submit_work(make_work(2, {"a"})).ok());
  const MutationResult third = scheduler.submit_work(make_work(3, {"a"}));
  AS_CHECK(!third.ok());
  AS_CHECK(third.error->code == ErrorCode::LimitExceeded);
}

AS_TEST(limits, absurd_and_invalid_numeric_state_is_rejected) {
  AgentScheduler scheduler(test_options());
  AgentDescriptor absurd = make_agent(1, 1);
  absurd.max_concurrency = 0xFFFFFFFFu;
  const MutationResult too_much_concurrency = scheduler.register_agent(absurd);
  AS_CHECK(!too_much_concurrency.ok());
  AS_CHECK(too_much_concurrency.error->code == ErrorCode::LimitExceeded);

  AgentDescriptor zero_concurrency = make_agent(2, 2);
  zero_concurrency.max_concurrency = 0;
  AS_CHECK(!scheduler.register_agent(zero_concurrency).ok());

  AS_CHECK(register_full(scheduler, make_agent(3, 3), {{"coding", 900}}).ok());

  WorkRequest bad_priority = make_work(100, {"coding"});
  bad_priority.priority = 5000;
  AS_CHECK(!scheduler.submit_work(bad_priority).ok());

  WorkRequest huge_parallel = make_work(101, {"coding"});
  huge_parallel.mode = SchedulingMode::Parallel;
  huge_parallel.requirements.max_parallel = 0xFFFFFFFFu;
  huge_parallel.requirements.max_assignments_per_agent = 1;
  AS_CHECK(!scheduler.submit_work(huge_parallel).ok());

  WorkRequest exclusive_parallel = make_work(102, {"coding"});
  exclusive_parallel.requirements.max_parallel = 4;
  AS_CHECK(!scheduler.submit_work(exclusive_parallel).ok());

  LoadObservation non_finite = load();
  non_finite.cost_index = std::numeric_limits<double>::infinity();
  AS_CHECK(!scheduler.publish_load(AgentId{3}, AgentBootId{derive(3, 3)}, non_finite).ok());
  non_finite.cost_index = std::numeric_limits<double>::quiet_NaN();
  AS_CHECK(!scheduler.publish_load(AgentId{3}, AgentBootId{derive(3, 3)}, non_finite).ok());
  non_finite.cost_index = -1.0;
  AS_CHECK(!scheduler.publish_load(AgentId{3}, AgentBootId{derive(3, 3)}, non_finite).ok());
  LoadObservation bad_fraction = load();
  bad_fraction.slo_headroom_fraction = 2.0;
  AS_CHECK(!scheduler.publish_load(AgentId{3}, AgentBootId{derive(3, 3)}, bad_fraction).ok());
  LoadObservation bad_pressure = load();
  bad_pressure.cpu_pressure_percent = 500;
  AS_CHECK(!scheduler.publish_load(AgentId{3}, AgentBootId{derive(3, 3)}, bad_pressure).ok());
}

AS_TEST(limits, hostile_text_is_rejected_everywhere) {
  AgentScheduler scheduler(test_options());
  AgentDescriptor descriptor = make_agent(1, 1);
  descriptor.display_name = std::string("bad\nline");
  AS_CHECK(!scheduler.register_agent(descriptor).ok());

  AgentDescriptor oversized = make_agent(2, 2);
  oversized.provenance = std::string(4000, 'p');
  AS_CHECK(!scheduler.register_agent(oversized).ok());

  AgentDescriptor bad_label = make_agent(3, 3);
  bad_label.policy_labels = {std::string("bad label")};
  AS_CHECK(!scheduler.register_agent(bad_label).ok());

  AS_CHECK(register_full(scheduler, make_agent(4, 4), {{"coding", 900}}).ok());
  WorkRequest control = make_work(200, {"coding"});
  control.kind = std::string("x\r\ny");
  AS_CHECK(!scheduler.submit_work(control).ok());

  WorkRequest bad_capability = make_work(201, {});
  CapabilityRequirement requirement;
  requirement.name = std::string("bad name");
  bad_capability.requirements.capabilities.push_back(requirement);
  AS_CHECK(!scheduler.submit_work(bad_capability).ok());

  WorkRequest duplicated = make_work(202, {"coding", "coding"});
  AS_CHECK(!scheduler.submit_work(duplicated).ok());

  WorkRequest overlapping = make_work(203, {"coding"});
  overlapping.requirements.affinity.push_back(AgentId{4});
  overlapping.requirements.anti_affinity.push_back(AgentId{4});
  AS_CHECK(!scheduler.submit_work(overlapping).ok());
}

AS_TEST(limits, scheduler_instances_are_independent) {
  AgentScheduler first(test_options());
  AgentScheduler second(test_options());
  AS_CHECK(register_full(first, make_agent(1, 1), {{"coding", 900}}).ok());
  AS_CHECK(first.summary().agent_count == 1);
  AS_CHECK(second.summary().agent_count == 0);
  AS_CHECK(first.state_digest() != second.state_digest());
  AS_CHECK(first.scheduler_epoch() == second.scheduler_epoch());
  AS_CHECK(register_full(second, make_agent(1, 2), {{"coding", 900}}).ok());
  AS_CHECK(second.summary().agent_count == 1);
  AS_CHECK(first.check_invariants().ok());
  AS_CHECK(second.check_invariants().ok());
}
