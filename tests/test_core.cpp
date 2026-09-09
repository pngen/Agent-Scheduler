// Agent Scheduler — core scheduling semantics.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "scheduler_fixture.hpp"
#include "test_support.hpp"

using namespace agent_scheduler;
using namespace as_fixture;

AS_TEST(core, readiness_must_be_earned) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor descriptor = make_agent(1, 7);
  AS_CHECK(scheduler.register_agent(descriptor).ok());
  const auto status = scheduler.agent_status(descriptor.id);
  AS_REQUIRE(status.has_value());
  AS_CHECK(status->lifecycle == AgentLifecycle::Registering);
  AS_CHECK(!status->evidence_current);

  AS_CHECK(scheduler.publish_capabilities(descriptor.id, descriptor.boot, make_profile(descriptor, {{"coding", 900}})).ok());
  AS_CHECK(scheduler.agent_status(descriptor.id)->lifecycle == AgentLifecycle::Registering);
  AS_CHECK(scheduler.publish_health(descriptor.id, descriptor.boot, healthy()).ok());
  AS_CHECK(scheduler.agent_status(descriptor.id)->lifecycle == AgentLifecycle::Registering);
  AS_CHECK(scheduler.publish_availability(descriptor.id, descriptor.boot, available()).ok());
  AS_CHECK(scheduler.agent_status(descriptor.id)->lifecycle == AgentLifecycle::Ready);
}

AS_TEST(core, registration_requires_generation_bump_for_new_incarnation) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor first = make_agent(1, 7);
  AS_CHECK(scheduler.register_agent(first).ok());

  AgentDescriptor same_generation_new_boot = first;
  same_generation_new_boot.boot = AgentBootId{derive(99, 1)};
  const MutationResult rejected = scheduler.register_agent(same_generation_new_boot);
  AS_CHECK(!rejected.ok());
  AS_CHECK(rejected.error->code == ErrorCode::StaleGeneration);
  AS_CHECK(rejected.error->outcome == ScheduleOutcome::RejectStaleAgentGeneration);

  AgentDescriptor stale = first;
  stale.generation = AgentGeneration{0};
  AS_CHECK(!scheduler.register_agent(stale).ok());

  AgentDescriptor reincarnated = first;
  reincarnated.generation = AgentGeneration{2};
  reincarnated.boot = AgentBootId{derive(99, 1)};
  AS_CHECK(scheduler.register_agent(reincarnated).ok());
  AS_CHECK(scheduler.agent_status(first.id)->generation == AgentGeneration{2});
  const std::vector<FencedBoot> fenced = scheduler.fenced_boots();
  AS_REQUIRE(fenced.size() == 1);
  AS_CHECK(fenced.front().boot == first.boot);
}

AS_TEST(core, unknown_required_capability_is_rejected_before_ranking) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor a = make_agent(1, 1);
  const AgentDescriptor b = make_agent(2, 2);
  AS_CHECK(register_full(scheduler, a, {{"coding", 1000}, {"tool:shell", 1000}}).ok());
  AS_CHECK(register_full(scheduler, b, {{"research", 1000}}).ok());

  AS_CHECK(scheduler.submit_work(make_work(100, {"coding", "tool:shell"})).ok());
  const ScheduleDecision decision = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_CHECK(decision.outcome == ScheduleOutcome::Assigned);
  AS_CHECK(decision.selected_agent == a.id);
  AS_REQUIRE(decision.candidates.size() == 2);
  bool saw_capability_rejection = false;
  for (const CandidateEvaluation& candidate : decision.candidates) {
    if (candidate.agent == b.id) {
      AS_CHECK(!candidate.eligible);
      AS_CHECK(candidate.rejection == ScheduleOutcome::RejectCapability);
      saw_capability_rejection = true;
    }
  }
  AS_CHECK(saw_capability_rejection);
}

AS_TEST(core, declared_capability_is_not_enough_by_default) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor a = make_agent(1, 1);
  AS_CHECK(scheduler.register_agent(a).ok());
  CapabilityProfile profile = make_profile(a, {{"coding", 1000}});
  profile.capabilities.front().state = CapabilityState::Declared;
  AS_CHECK(scheduler.publish_capabilities(a.id, a.boot, profile).ok());
  AS_CHECK(scheduler.publish_health(a.id, a.boot, healthy()).ok());
  AS_CHECK(scheduler.publish_availability(a.id, a.boot, available()).ok());
  AS_CHECK(scheduler.agent_status(a.id)->lifecycle == AgentLifecycle::Ready);

  AS_CHECK(scheduler.submit_work(make_work(100, {"coding"})).ok());
  const ScheduleDecision decision = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_CHECK(decision.outcome == ScheduleOutcome::NoEligibleAgent);
  AS_REQUIRE(decision.candidates.size() == 1);
  AS_CHECK(decision.candidates.front().rejection == ScheduleOutcome::RejectCapability);
}

AS_TEST(core, favorable_score_cannot_rescue_ineligibility) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor strong = make_agent(1, 1, 8);   // idle, high quality, no required capability
  const AgentDescriptor weak = make_agent(2, 2, 1);     // busy, low quality, has the capability
  AS_CHECK(register_full(scheduler, strong, {{"research", 1000}}, 1000).ok());
  AS_CHECK(register_full(scheduler, weak, {{"coding", 100}}, 100).ok());
  AS_CHECK(scheduler.submit_work(make_work(100, {"coding"})).ok());
  const ScheduleDecision decision = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_CHECK(decision.outcome == ScheduleOutcome::Assigned);
  AS_CHECK(decision.selected_agent == weak.id);
}

AS_TEST(core, ranking_prefers_capacity_then_is_deterministic) {
  const AgentDescriptor a = make_agent(1, 1, 4);
  const AgentDescriptor b = make_agent(2, 2, 2);
  const auto run_once = [&]() {
    AgentScheduler scheduler(test_options());
    AS_CHECK(register_full(scheduler, a, {{"coding", 500}}).ok());
    AS_CHECK(register_full(scheduler, b, {{"coding", 900}}).ok());
    AS_CHECK(scheduler.submit_work(make_work(100, {"coding"})).ok());
    return scheduler.schedule(WorkId{100}, WorkGeneration{1});
  };
  const ScheduleDecision first = run_once();
  AS_CHECK(first.outcome == ScheduleOutcome::Assigned);
  for (int repeat = 0; repeat < 20; ++repeat) {
    const ScheduleDecision again = run_once();
    AS_CHECK(again.outcome == first.outcome);
    AS_CHECK(again.selected_agent == first.selected_agent);
    AS_CHECK(again.selected_boot == first.selected_boot);
    AS_CHECK(again.assignment->id == first.assignment->id);
    AS_CHECK(again.ranking_order == first.ranking_order);
    AS_CHECK(again.rejection_summary == first.rejection_summary);
  }
}

AS_TEST(core, decision_is_independent_of_registration_order) {
  const AgentDescriptor a = make_agent(1, 11, 3);
  const AgentDescriptor b = make_agent(2, 22, 3);
  const AgentDescriptor c = make_agent(3, 33, 3);
  const auto run = [&](bool reverse) {
    AgentScheduler scheduler(test_options());
    const std::vector<AgentDescriptor> order =
        reverse ? std::vector<AgentDescriptor>{c, b, a} : std::vector<AgentDescriptor>{a, b, c};
    for (const AgentDescriptor& descriptor : order) {
      AS_CHECK(register_full(scheduler, descriptor, {{"coding", 700}}).ok());
    }
    AS_CHECK(scheduler.submit_work(make_work(100, {"coding"})).ok());
    const ScheduleDecision decision = scheduler.schedule(WorkId{100}, WorkGeneration{1});
    return std::make_pair(decision, scheduler.state_digest());
  };
  const auto forward = run(false);
  const auto backward = run(true);
  AS_CHECK(forward.first.selected_agent == backward.first.selected_agent);
  AS_CHECK(forward.first.assignment->id == backward.first.assignment->id);
  AS_CHECK(forward.second == backward.second);
}

AS_TEST(core, explanation_exposes_named_factors_and_rejections) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor a = make_agent(1, 1);
  const AgentDescriptor b = make_agent(2, 2);
  AS_CHECK(register_full(scheduler, a, {{"coding", 800}}).ok());
  AS_CHECK(register_full(scheduler, b, {{"research", 800}}).ok());
  AS_CHECK(scheduler.submit_work(make_work(100, {"coding"})).ok());
  const ScheduleDecision decision = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_CHECK(decision.assignment.has_value());
  AS_CHECK(decision.scheduler_epoch.value() != 0);
  AS_CHECK(decision.coordinator_epoch.value() != 0);
  AS_CHECK(decision.policy_generation.value() != 0);
  AS_REQUIRE(decision.rejection_summary.size() == 1);
  AS_CHECK(decision.rejection_summary.front().find("REJECT_CAPABILITY=1") != std::string::npos);
  bool saw_named_factor = false;
  bool saw_unknown_factor = false;
  for (const CandidateEvaluation& candidate : decision.candidates) {
    if (!candidate.eligible) {
      AS_CHECK(candidate.factors.empty());
      continue;
    }
    for (const FactorScore& factor : candidate.factors) {
      if (factor.name == ranking_factor::load_headroom && factor.status == FactorStatus::Known) {
        saw_named_factor = true;
      }
      if (factor.status == FactorStatus::Unknown) {
        AS_CHECK(factor.normalized_value == 0);
        saw_unknown_factor = true;
      }
    }
  }
  AS_CHECK(saw_named_factor);
  AS_CHECK(saw_unknown_factor);
}

AS_TEST(core, work_admission_is_idempotent_and_conflicts_are_rejected) {
  AgentScheduler scheduler(test_options());
  const WorkRequest request = make_work(100, {"coding"});
  AS_CHECK(scheduler.submit_work(request).ok());
  AS_CHECK(scheduler.submit_work(request).ok());

  WorkRequest conflicting = request;
  conflicting.priority = 900;
  const MutationResult conflict = scheduler.submit_work(conflicting);
  AS_CHECK(!conflict.ok());
  AS_CHECK(conflict.error->outcome == ScheduleOutcome::RejectConflict);

  WorkRequest stale = request;
  stale.generation = WorkGeneration{0};
  AS_CHECK(!scheduler.submit_work(stale).ok());

  AS_CHECK(scheduler.submit_work(make_work(100, {"coding"}, 500, QueueId{1}, WorkGeneration{2})).ok());
  const auto status = scheduler.work_status(WorkId{100});
  AS_REQUIRE(status.has_value());
  AS_CHECK(status->generation == WorkGeneration{2});
}

AS_TEST(core, cancelled_work_cannot_be_readmitted_at_the_same_generation) {
  AgentScheduler scheduler(test_options());
  const WorkRequest request = make_work(100, {"coding"});
  AS_CHECK(scheduler.submit_work(request).ok());
  AS_CHECK(scheduler.cancel_work(WorkId{100}, WorkGeneration{1}, "test").ok());
  const MutationResult again = scheduler.submit_work(request);
  AS_CHECK(!again.ok());
  AS_CHECK(again.error->outcome == ScheduleOutcome::RejectCancelled);
  AS_CHECK(scheduler.schedule(WorkId{100}, WorkGeneration{1}).outcome == ScheduleOutcome::RejectCancelled);
}

AS_TEST(core, supersession_replaces_generation_and_fences_old_assignment) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor a = make_agent(1, 1);
  AS_CHECK(register_full(scheduler, a, {{"coding", 900}}).ok());
  AS_CHECK(scheduler.submit_work(make_work(100, {"coding"})).ok());
  const ScheduleDecision first = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_REQUIRE(first.assignment.has_value());
  const AssignmentId old_assignment = first.assignment->id;

  AS_CHECK(scheduler.supersede_work(WorkId{100}, WorkGeneration{1}, make_work(100, {"coding"}, 500, QueueId{1}, WorkGeneration{2})).ok());
  const auto old = scheduler.assignment(old_assignment);
  AS_REQUIRE(old.has_value());
  AS_CHECK(old->state == AssignmentState::Superseded);
  AS_CHECK(assignment_state_is_terminal(old->state));
  AS_CHECK(!old->lease_current);

  const ScheduleDecision second = scheduler.schedule(WorkId{100}, WorkGeneration{2});
  AS_CHECK(second.outcome == ScheduleOutcome::Assigned);
  AS_CHECK(second.assignment->id != old_assignment);
  AS_CHECK(second.assignment->generation == AssignmentGeneration{2});
}

AS_TEST(core, policy_generation_change_invalidates_active_assignments) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor a = make_agent(1, 1);
  AS_CHECK(register_full(scheduler, a, {{"coding", 900}}).ok());
  AS_CHECK(scheduler.submit_work(make_work(100, {"coding"})).ok());
  const ScheduleDecision decision = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_REQUIRE(decision.assignment.has_value());

  PolicySnapshot policy = scheduler.policy();
  policy.generation = PolicyGeneration{2};
  policy.max_priority_bypass = 4;
  AS_CHECK(scheduler.set_policy(policy).ok());
  const auto stored = scheduler.assignment(decision.assignment->id);
  AS_REQUIRE(stored.has_value());
  AS_CHECK(stored->state == AssignmentState::Invalidated);
  AS_CHECK(stored->invalidation == InvalidationReason::PolicyChanged);

  PolicySnapshot stale = policy;
  stale.generation = PolicyGeneration{1};
  const MutationResult rejected = scheduler.set_policy(stale);
  AS_CHECK(!rejected.ok());
  AS_CHECK(rejected.error->outcome == ScheduleOutcome::RejectStalePolicy);
}

AS_TEST(core, locality_affinity_and_anti_affinity_are_hard) {
  AgentScheduler scheduler(test_options());
  AgentDescriptor a = make_agent(1, 1);
  AgentDescriptor b = make_agent(2, 2);
  a.placement_domain = PlacementDomainId{1};
  b.placement_domain = PlacementDomainId{2};
  AS_CHECK(register_full(scheduler, a, {{"coding", 900}}).ok());
  AS_CHECK(register_full(scheduler, b, {{"coding", 900}}).ok());

  WorkRequest pinned = make_work(100, {"coding"});
  pinned.requirements.restrict_placement_domain = true;
  pinned.requirements.placement_domain = PlacementDomainId{2};
  pinned.requirements.topology_epoch = TopologyEpoch{1};
  AS_CHECK(scheduler.submit_work(pinned).ok());
  const ScheduleDecision decision = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_CHECK(decision.selected_agent == b.id);

  WorkRequest anti = make_work(101, {"coding"});
  anti.requirements.anti_affinity.push_back(b.id);
  AS_CHECK(scheduler.submit_work(anti).ok());
  AS_CHECK(scheduler.schedule(WorkId{101}, WorkGeneration{1}).selected_agent == a.id);

  WorkRequest affinity = make_work(102, {"coding"});
  affinity.requirements.affinity.push_back(b.id);
  AS_CHECK(scheduler.submit_work(affinity).ok());
  AS_CHECK(scheduler.schedule(WorkId{102}, WorkGeneration{1}).selected_agent == b.id);
}

AS_TEST(core, external_feasibility_is_consumed_not_reimplemented) {
  auto provider = std::make_shared<ScriptedFeasibilityProvider>();
  provider->set_resource(ResourceGeneration{5}, FeasibilityVerdict::Infeasible);
  SchedulerOptions options = test_options();
  options.feasibility = provider;
  AgentScheduler scheduler(options);
  const AgentDescriptor a = make_agent(1, 1);
  AS_CHECK(register_full(scheduler, a, {{"coding", 900}}).ok());

  WorkRequest request = make_work(100, {"coding"});
  request.requirements.require_resource_feasibility = true;
  request.requirements.resource_generation = ResourceGeneration{5};
  AS_CHECK(scheduler.submit_work(request).ok());
  const ScheduleDecision decision = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_CHECK(decision.outcome == ScheduleOutcome::NoEligibleAgent);
  AS_REQUIRE(decision.candidates.size() == 1);
  AS_CHECK(decision.candidates.front().rejection == ScheduleOutcome::RejectResourceFeasibility);

  provider->set_resource(ResourceGeneration{5}, FeasibilityVerdict::Feasible);
  const ScheduleDecision allowed = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_CHECK(allowed.outcome == ScheduleOutcome::Assigned);
}

AS_TEST(core, unknown_feasibility_never_passes_a_hard_requirement) {
  SchedulerOptions options = test_options();
  options.feasibility = std::make_shared<NullFeasibilityProvider>();
  AgentScheduler scheduler(options);
  const AgentDescriptor a = make_agent(1, 1);
  AS_CHECK(register_full(scheduler, a, {{"coding", 900}}).ok());
  WorkRequest request = make_work(100, {"coding"});
  request.requirements.require_budget_feasibility = true;
  request.requirements.budget_generation = BudgetGeneration{3};
  AS_CHECK(scheduler.submit_work(request).ok());
  const ScheduleDecision decision = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_CHECK(decision.outcome == ScheduleOutcome::NoEligibleAgent);
  AS_CHECK(decision.candidates.front().rejection == ScheduleOutcome::RejectBudget);
}

AS_TEST(core, health_and_reachability_gate_eligibility) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor a = make_agent(1, 1);
  const AgentDescriptor b = make_agent(2, 2);
  AS_CHECK(register_full(scheduler, a, {{"coding", 900}}).ok());
  AS_CHECK(register_full(scheduler, b, {{"coding", 900}}).ok());
  HealthObservation unhealthy = healthy(AgentHealthGeneration{2}, 100);
  unhealthy.health = AgentHealth::Unhealthy;
  AS_CHECK(scheduler.publish_health(a.id, a.boot, unhealthy).ok());

  AS_CHECK(scheduler.submit_work(make_work(100, {"coding"})).ok());
  const ScheduleDecision decision = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_CHECK(decision.selected_agent == b.id);
  for (const CandidateEvaluation& candidate : decision.candidates) {
    if (candidate.agent == a.id) {
      AS_CHECK(candidate.rejection == ScheduleOutcome::RejectHealth);
    }
  }

  AvailabilityObservation unreachable = available(AgentAvailabilityGeneration{2});
  unreachable.reachability = AgentReachability::Unreachable;
  unreachable.readiness = AgentReadiness::NotReady;
  AS_CHECK(scheduler.publish_availability(b.id, b.boot, unreachable).ok());
  AS_CHECK(scheduler.agent_status(b.id)->lifecycle == AgentLifecycle::Unavailable);
  AS_CHECK(scheduler.submit_work(make_work(101, {"coding"})).ok());
  const ScheduleDecision none = scheduler.schedule(WorkId{101}, WorkGeneration{1});
  AS_CHECK(none.outcome == ScheduleOutcome::NoEligibleAgent);
  AS_CHECK(none.eligible_count == 0);
}

AS_TEST(core, deadline_expiry_and_cancellation_are_authoritative) {
  auto clock = std::make_shared<ManualClock>(1000);
  AgentScheduler scheduler(test_options(clock));
  const AgentDescriptor a = make_agent(1, 1);
  AS_CHECK(register_full(scheduler, a, {{"coding", 900}}).ok());
  WorkRequest request = make_work(100, {"coding"});
  request.requirements.deadline_ms = 1500;
  AS_CHECK(scheduler.submit_work(request).ok());
  AS_CHECK(scheduler.schedule(WorkId{100}, WorkGeneration{1}).outcome == ScheduleOutcome::Assigned);
  clock->set(2000);
  AS_CHECK(scheduler.expire_deadlines().ok());
  AS_CHECK(scheduler.work_status(WorkId{100})->lifecycle == WorkLifecycle::Expired);
  AS_CHECK(scheduler.schedule(WorkId{100}, WorkGeneration{1}).outcome == ScheduleOutcome::NoChange);
}

AS_TEST(core, drain_blocks_new_assignments_but_keeps_obligations) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor a = make_agent(1, 1);
  const AgentDescriptor b = make_agent(2, 2);
  AS_CHECK(register_full(scheduler, a, {{"coding", 900}}).ok());
  AS_CHECK(register_full(scheduler, b, {{"coding", 900}}).ok());
  AS_CHECK(scheduler.submit_work(make_work(100, {"coding"})).ok());
  const ScheduleDecision first = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_REQUIRE(first.assignment.has_value());
  const AgentId owner = first.selected_agent;
  const AgentId other = owner == a.id ? b.id : a.id;

  AS_CHECK(scheduler.drain_agent(owner, first.selected_boot, false).ok());
  AS_CHECK(scheduler.agent_status(owner)->lifecycle == AgentLifecycle::Draining);
  const auto kept = scheduler.assignment(first.assignment->id);
  AS_REQUIRE(kept.has_value());
  AS_CHECK(assignment_state_is_active(kept->state));

  AS_CHECK(scheduler.submit_work(make_work(101, {"coding"})).ok());
  AS_CHECK(scheduler.schedule(WorkId{101}, WorkGeneration{1}).selected_agent == other);

  const MutationResult cannot_deregister = scheduler.deregister_agent(owner, first.selected_boot, false);
  AS_CHECK(!cannot_deregister.ok());
  AS_CHECK(cannot_deregister.error->outcome == ScheduleOutcome::RejectDrain);
  AS_CHECK(scheduler.release(first.assignment->id, first.assignment->generation, first.selected_boot, "done").ok());
  AS_CHECK(scheduler.deregister_agent(owner, first.selected_boot, false).ok());
  AS_CHECK(scheduler.agent_status(owner)->lifecycle == AgentLifecycle::Retired);
  AS_CHECK(scheduler.submit_work(make_work(102, {"coding"})).ok());
  AS_CHECK(scheduler.schedule(WorkId{102}, WorkGeneration{1}).selected_agent == other);
}

AS_TEST(core, lease_expiry_removes_dispatch_authority) {
  auto clock = std::make_shared<ManualClock>(1000);
  AgentScheduler scheduler(test_options(clock));
  const AgentDescriptor a = make_agent(1, 1);
  AS_CHECK(register_full(scheduler, a, {{"coding", 900}}).ok());
  AS_CHECK(scheduler.submit_work(make_work(100, {"coding"})).ok());
  const ScheduleDecision decision = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_REQUIRE(decision.assignment.has_value());
  clock->set(1000 + scheduler.policy().assignment_lease_ttl_ms + 1);
  AS_CHECK(scheduler.expire_leases().ok());
  const auto stored = scheduler.assignment(decision.assignment->id);
  AS_REQUIRE(stored.has_value());
  AS_CHECK(stored->state == AssignmentState::Expired);
  AS_CHECK(stored->invalidation == InvalidationReason::LeaseExpired);
  AS_CHECK(scheduler.work_status(WorkId{100})->lifecycle == WorkLifecycle::Admitted);
}

AS_TEST(core, parallel_fanout_is_explicit_and_bounded) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor a = make_agent(1, 1, 2);
  const AgentDescriptor b = make_agent(2, 2, 2);
  AS_CHECK(register_full(scheduler, a, {{"coding", 900}}).ok());
  AS_CHECK(register_full(scheduler, b, {{"coding", 900}}).ok());
  WorkRequest request = make_work(100, {"coding"});
  request.mode = SchedulingMode::Parallel;
  request.requirements.max_parallel = 2;
  request.requirements.max_assignments_per_agent = 1;
  AS_CHECK(scheduler.submit_work(request).ok());
  ScheduleRequest pass;
  pass.max_assignments = 2;
  const BatchDecision batch = scheduler.schedule_batch(pass);
  AS_CHECK(batch.assigned == 2);
  const std::vector<Assignment> assignments = scheduler.assignments_for_work(WorkId{100});
  AS_CHECK(assignments.size() == 2);
  AS_CHECK(assignments[0].binding.agent != assignments[1].binding.agent);
  AS_CHECK(assignments[0].binding.replica_index != assignments[1].binding.replica_index);
  const BatchDecision again = scheduler.schedule_batch(ScheduleRequest{});
  AS_CHECK(again.assigned == 0);
}

AS_TEST(core, exclusive_work_never_receives_a_second_assignment) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor a = make_agent(1, 1, 4);
  AS_CHECK(register_full(scheduler, a, {{"coding", 900}}).ok());
  AS_CHECK(scheduler.submit_work(make_work(100, {"coding"})).ok());
  const ScheduleDecision first = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_CHECK(first.outcome == ScheduleOutcome::Assigned);
  const ScheduleDecision second = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_CHECK(second.outcome == ScheduleOutcome::NoChange);
  AS_CHECK(!second.assignment.has_value());
  AS_CHECK(scheduler.assignments_for_work(WorkId{100}).size() == 1);
}

AS_TEST(core, capacity_is_respected_across_concurrent_work) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor a = make_agent(1, 1, 1);
  AS_CHECK(register_full(scheduler, a, {{"coding", 900}}).ok());
  AS_CHECK(scheduler.submit_work(make_work(100, {"coding"})).ok());
  AS_CHECK(scheduler.submit_work(make_work(101, {"coding"})).ok());
  AS_CHECK(scheduler.schedule(WorkId{100}, WorkGeneration{1}).outcome == ScheduleOutcome::Assigned);
  const ScheduleDecision second = scheduler.schedule(WorkId{101}, WorkGeneration{1});
  AS_CHECK(second.outcome == ScheduleOutcome::NoEligibleAgent);
  AS_CHECK(second.candidates.front().rejection == ScheduleOutcome::RejectCapacity);
  AS_CHECK(scheduler.agent_status(a.id)->active_assignments == 1);
}
