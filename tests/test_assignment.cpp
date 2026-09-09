// Agent Scheduler — assignment authority, leases, dispatch, and revalidation.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "scheduler_fixture.hpp"
#include "test_support.hpp"

using namespace agent_scheduler;
using namespace as_fixture;

namespace {

struct Bound {
  std::unique_ptr<AgentScheduler> scheduler;
  AgentDescriptor agent;
  Assignment assignment;
};

[[nodiscard]] Bound bind_one(std::uint32_t bypass = 8) {
  SchedulerOptions options = test_options();
  options.policy.max_priority_bypass = bypass;
  auto scheduler = std::make_unique<AgentScheduler>(options);
  const AgentDescriptor descriptor = make_agent(1, 1, 2);
  AS_CHECK(register_full(*scheduler, descriptor, {{"coding", 900}}).ok());
  AS_CHECK(scheduler->submit_work(make_work(100, {"coding"})).ok());
  const ScheduleDecision decision = scheduler->schedule(WorkId{100}, WorkGeneration{1});
  AS_REQUIRE(decision.assignment.has_value());
  const auto stored = scheduler->assignment(decision.assignment->id);
  AS_REQUIRE(stored.has_value());
  return Bound{std::move(scheduler), descriptor, *stored};
}

}  // namespace

AS_TEST(assignment, dispatch_requires_revalidation_and_extends_the_lease) {
  Bound bound = bind_one();
  const DispatchResult dispatch =
      bound.scheduler->dispatch(bound.assignment.binding.id, bound.assignment.binding.generation);
  AS_CHECK(dispatch.dispatched());
  AS_REQUIRE(dispatch.handoff.has_value());
  AS_CHECK(dispatch.handoff->id == bound.assignment.binding.id);
  AS_CHECK(dispatch.handoff->boot == bound.agent.boot);
  const auto stored = bound.scheduler->assignment(bound.assignment.binding.id);
  AS_REQUIRE(stored.has_value());
  AS_CHECK(stored->state == AssignmentState::Dispatched);
  AS_CHECK(stored->lease_current);

  const DispatchResult repeat =
      bound.scheduler->dispatch(bound.assignment.binding.id, bound.assignment.binding.generation);
  AS_CHECK(!repeat.dispatched());
  AS_CHECK(repeat.outcome == ScheduleOutcome::NoChange);
}

AS_TEST(assignment, dispatch_rejects_every_stale_authority) {
  {
    Bound bound = bind_one();
    const DispatchResult result =
        bound.scheduler->dispatch(bound.assignment.binding.id, AssignmentGeneration{99});
    AS_CHECK(!result.dispatched());
    AS_CHECK(result.outcome == ScheduleOutcome::RejectStaleAssignment);
  }
  {
    Bound bound = bind_one();
    const DispatchResult result =
        bound.scheduler->dispatch(AssignmentId{Hash128{1, 2}}, AssignmentGeneration{1});
    AS_CHECK(result.outcome == ScheduleOutcome::RejectStaleAssignment);
  }
  {
    Bound bound = bind_one();
    AS_CHECK(bound.scheduler->cancel_work(WorkId{100}, WorkGeneration{1}, "cancel").ok());
    const DispatchResult result =
        bound.scheduler->dispatch(bound.assignment.binding.id, bound.assignment.binding.generation);
    AS_CHECK(!result.dispatched());
    AS_CHECK(result.outcome == ScheduleOutcome::RejectStaleAssignment ||
             result.outcome == ScheduleOutcome::RejectCancelled);
  }
  {
    Bound bound = bind_one();
    PolicySnapshot policy = bound.scheduler->policy();
    policy.generation = PolicyGeneration{2};
    AS_CHECK(bound.scheduler->set_policy(policy).ok());
    const DispatchResult result =
        bound.scheduler->dispatch(bound.assignment.binding.id, bound.assignment.binding.generation);
    AS_CHECK(!result.dispatched());
    AS_CHECK(result.outcome == ScheduleOutcome::RejectStaleAssignment ||
             result.outcome == ScheduleOutcome::RejectStalePolicy);
  }
  {
    Bound bound = bind_one();
    AS_CHECK(bound.scheduler->declare_agent_lost(bound.agent.id, bound.agent.boot, "lost").ok());
    const DispatchResult result =
        bound.scheduler->dispatch(bound.assignment.binding.id, bound.assignment.binding.generation);
    AS_CHECK(!result.dispatched());
    AS_CHECK(result.outcome == ScheduleOutcome::RejectStaleAgentBoot ||
             result.outcome == ScheduleOutcome::RejectStaleAssignment);
  }
  {
    Bound bound = bind_one();
    AS_CHECK(bound.scheduler->shutdown().ok());
    const DispatchResult result =
        bound.scheduler->dispatch(bound.assignment.binding.id, bound.assignment.binding.generation);
    AS_CHECK(!result.dispatched());
    AS_CHECK(result.outcome == ScheduleOutcome::ShuttingDown ||
             result.outcome == ScheduleOutcome::RejectStaleAssignment);
  }
}

AS_TEST(assignment, acknowledgement_requires_the_exact_dispatch_identity) {
  Bound bound = bind_one();
  const DispatchResult dispatch =
      bound.scheduler->dispatch(bound.assignment.binding.id, bound.assignment.binding.generation);
  AS_REQUIRE(dispatch.handoff.has_value());
  const AssignmentBinding& handoff = *dispatch.handoff;

  const MutationResult wrong_dispatch = bound.scheduler->acknowledge(
      handoff.id, handoff.generation, DispatchId{Hash128{7, 7}}, handoff.dispatch_generation, handoff.boot);
  AS_CHECK(!wrong_dispatch.ok());
  AS_CHECK(wrong_dispatch.error->outcome == ScheduleOutcome::RejectStaleAssignment);

  const MutationResult wrong_generation = bound.scheduler->acknowledge(
      handoff.id, handoff.generation, handoff.dispatch, DispatchGeneration{99}, handoff.boot);
  AS_CHECK(!wrong_generation.ok());

  const MutationResult wrong_boot = bound.scheduler->acknowledge(
      handoff.id, handoff.generation, handoff.dispatch, handoff.dispatch_generation,
      AgentBootId{derive(4242, 1)});
  AS_CHECK(!wrong_boot.ok());
  AS_CHECK(wrong_boot.error->outcome == ScheduleOutcome::RejectStaleAgentBoot);

  const MutationResult accepted = bound.scheduler->acknowledge(
      handoff.id, handoff.generation, handoff.dispatch, handoff.dispatch_generation, handoff.boot);
  AS_CHECK(accepted.ok());
  AS_CHECK(bound.scheduler->assignment(handoff.id)->state == AssignmentState::Acknowledged);
  AS_CHECK(bound.scheduler->
               acknowledge(handoff.id, handoff.generation, handoff.dispatch, handoff.dispatch_generation,
                            handoff.boot)
               .ok());
}

AS_TEST(assignment, observation_before_dispatch_is_a_conflict) {
  Bound bound = bind_one();
  const AssignmentBinding& binding = bound.assignment.binding;
  const MutationResult result = bound.scheduler->acknowledge(
      binding.id, binding.generation, binding.dispatch, binding.dispatch_generation, binding.boot);
  AS_CHECK(!result.ok());
  AS_CHECK(result.error->outcome == ScheduleOutcome::RejectConflict);
}

AS_TEST(assignment, terminal_states_are_idempotent_and_release_capacity) {
  Bound bound = bind_one();
  const AssignmentBinding& binding = bound.assignment.binding;
  AS_CHECK(bound.scheduler->complete_assignment(binding.id, binding.generation, binding.boot).ok());
  AS_CHECK(bound.scheduler->complete_assignment(binding.id, binding.generation, binding.boot).ok());
  const auto stored = bound.scheduler->assignment(binding.id);
  AS_REQUIRE(stored.has_value());
  AS_CHECK(stored->state == AssignmentState::Completed);
  AS_CHECK(!stored->lease_current);
  AS_CHECK(bound.scheduler->agent_status(bound.agent.id)->active_assignments == 0);
  AS_CHECK(bound.scheduler->work_status(WorkId{100})->lifecycle == WorkLifecycle::Completed);

  const MutationResult late_ack = bound.scheduler->acknowledge(
      binding.id, binding.generation, binding.dispatch, binding.dispatch_generation, binding.boot);
  AS_CHECK(!late_ack.ok());
  AS_CHECK(late_ack.error->outcome == ScheduleOutcome::RejectStaleAssignment);
}

AS_TEST(assignment, reject_returns_work_to_admission) {
  Bound bound = bind_one();
  const AssignmentBinding& binding = bound.assignment.binding;
  AS_CHECK(bound.scheduler->reject_assignment(binding.id, binding.generation, binding.boot, "not now").ok());
  AS_CHECK(bound.scheduler->work_status(WorkId{100})->lifecycle == WorkLifecycle::Admitted);
  AS_CHECK(bound.scheduler->agent_status(bound.agent.id)->active_assignments == 0);
  const ScheduleDecision again = bound.scheduler->schedule(WorkId{100}, WorkGeneration{1});
  AS_CHECK(again.outcome == ScheduleOutcome::Assigned);
  AS_CHECK(again.assignment->generation == AssignmentGeneration{2});
}

AS_TEST(assignment, execution_observation_requires_acknowledgement) {
  Bound bound = bind_one();
  const AssignmentBinding& binding = bound.assignment.binding;
  AS_CHECK(!bound.scheduler->mark_executing(binding.id, binding.generation, binding.boot).ok());
  const DispatchResult dispatch = bound.scheduler->dispatch(binding.id, binding.generation);
  AS_REQUIRE(dispatch.handoff.has_value());
  AS_CHECK(bound.scheduler->
               acknowledge(binding.id, binding.generation, binding.dispatch, binding.dispatch_generation,
                            binding.boot)
               .ok());
  AS_CHECK(bound.scheduler->mark_executing(binding.id, binding.generation, binding.boot).ok());
  AS_CHECK(bound.scheduler->assignment(binding.id)->state == AssignmentState::Executing);
  AS_CHECK(bound.scheduler->work_status(WorkId{100})->lifecycle == WorkLifecycle::Executing);
}

AS_TEST(assignment, revalidation_promotes_or_invalidates) {
  Bound bound = bind_one();
  AS_CHECK(bound.scheduler->declare_agent_lost(bound.agent.id, bound.agent.boot, "session lost").ok());
  AS_CHECK(bound.scheduler->assignment(bound.assignment.binding.id)->state == AssignmentState::Lost);

  // Reincarnate under a higher generation, then revalidate.
  AgentDescriptor replacement = bound.agent;
  replacement.generation = AgentGeneration{2};
  replacement.boot = AgentBootId{derive(555, 1)};
  AS_CHECK(register_full(*bound.scheduler, replacement, {{"coding", 900}}).ok());
  const RevalidationReport report = bound.scheduler->revalidate_assignments();
  AS_CHECK(report.examined == 0);
  AS_CHECK(bound.scheduler->assignment(bound.assignment.binding.id)->state == AssignmentState::Lost);

  // A fresh assignment is created under the new incarnation.
  const ScheduleDecision decision = bound.scheduler->schedule(WorkId{100}, WorkGeneration{1});
  AS_CHECK(decision.outcome == ScheduleOutcome::Assigned);
  AS_CHECK(decision.selected_agent == replacement.id);
  AS_CHECK(decision.assignment->boot == replacement.boot);
}

AS_TEST(assignment, revalidation_required_assignments_are_promoted_after_republication) {
  SchedulerOptions options = test_options();
  AgentScheduler scheduler(options);
  const AgentDescriptor descriptor = make_agent(1, 1, 2);
  AS_CHECK(register_full(scheduler, descriptor, {{"coding", 900}}).ok());
  AS_CHECK(scheduler.submit_work(make_work(100, {"coding"})).ok());
  const ScheduleDecision decision = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_REQUIRE(decision.assignment.has_value());

  // Simulate a recovered assignment: authority must be revalidated before dispatch.
  const MutationResult lost = scheduler.declare_agent_lost(descriptor.id, descriptor.boot, "restart");
  AS_CHECK(lost.ok());
  AS_CHECK(scheduler.assignment(decision.assignment->id)->state == AssignmentState::Lost);
  AS_CHECK(scheduler.revalidate_assignments().examined == 0);
}

AS_TEST(assignment, agent_loss_invalidates_only_that_incarnation) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor a = make_agent(1, 1, 1);
  const AgentDescriptor b = make_agent(2, 2, 1);
  AS_CHECK(register_full(scheduler, a, {{"coding", 900}}).ok());
  AS_CHECK(register_full(scheduler, b, {{"coding", 900}}).ok());
  AS_CHECK(scheduler.submit_work(make_work(100, {"coding"})).ok());
  AS_CHECK(scheduler.submit_work(make_work(101, {"coding"})).ok());
  const ScheduleDecision first = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  const ScheduleDecision second = scheduler.schedule(WorkId{101}, WorkGeneration{1});
  AS_REQUIRE(first.assignment.has_value());
  AS_REQUIRE(second.assignment.has_value());
  AS_CHECK(first.selected_agent != second.selected_agent);

  const AgentId survivor = first.selected_agent == a.id ? b.id : a.id;
  const AgentBootId survivor_boot = first.selected_agent == a.id ? b.boot : a.boot;
  const AgentId victim = first.selected_agent;
  const AgentBootId victim_boot = first.selected_boot;
  AS_CHECK(scheduler.declare_agent_lost(victim, victim_boot, "lost").ok());

  AS_CHECK(scheduler.agent_status(victim)->lifecycle == AgentLifecycle::Lost);
  AS_CHECK(scheduler.agent_status(survivor)->lifecycle == AgentLifecycle::Busy);
  const auto survivor_status = scheduler.agent_status(survivor);
  AS_REQUIRE(survivor_status.has_value());
  AS_CHECK(survivor_status->boot == survivor_boot);
  AS_CHECK(survivor_status->evidence_current);
  const auto survivor_assignment = scheduler.assignment(second.assignment->id);
  AS_REQUIRE(survivor_assignment.has_value());
  AS_CHECK(assignment_state_is_active(survivor_assignment->state));

  // Stale traffic from the dead incarnation is rejected before any mutation.
  const MutationResult stale_heartbeat = scheduler.heartbeat(victim, victim_boot, AgentRegistrationGeneration{1});
  AS_CHECK(!stale_heartbeat.ok());
  AS_CHECK(stale_heartbeat.error->outcome == ScheduleOutcome::RejectStaleAgentBoot);
  const MutationResult stale_publish =
      scheduler.publish_load(victim, victim_boot, load(AgentLoadGeneration{5}));
  AS_CHECK(!stale_publish.ok());
  AS_CHECK(stale_publish.error->outcome == ScheduleOutcome::RejectStaleAgentBoot);
  const MutationResult stale_register = scheduler.register_agent(
      AgentDescriptor{[&]() {
                        AgentDescriptor descriptor = make_agent(1, 1);
                        descriptor.boot = victim_boot;
                        return descriptor;
                      }()});
  AS_CHECK(!stale_register.ok());
}
