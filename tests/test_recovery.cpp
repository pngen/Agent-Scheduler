// Agent Scheduler — scheduler restart and conservative recovery.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "scheduler_fixture.hpp"
#include "test_support.hpp"

using namespace agent_scheduler;
using namespace as_fixture;

namespace {

struct Snapshot {
  std::filesystem::path path;
  SchedulerSummary summary;
  std::vector<AgentDescriptor> agents;
  std::vector<WorkId> work;
  std::vector<Assignment> assignments;
  SchedulerEpoch scheduler_epoch;
  CoordinatorEpoch coordinator_epoch;
  PolicyGeneration policy_generation;
  Digest256 digest;
  std::size_t fenced_boots{0};
  FairnessSnapshot fairness;
};

[[nodiscard]] Snapshot build_and_save(ScratchDirectory& scratch) {
  Snapshot snapshot;
  snapshot.path = scratch.file("restart.asstate");
  AgentScheduler scheduler(test_options());
  for (std::uint64_t index = 1; index <= 3; ++index) {
    const AgentDescriptor descriptor = make_agent(index, index * 11, 2);
    snapshot.agents.push_back(descriptor);
    AS_CHECK(register_full(scheduler, descriptor, {{"coding", 900}}).ok());
  }
  for (std::uint64_t index = 1; index <= 5; ++index) {
    snapshot.work.push_back(WorkId{200 + index});
    AS_CHECK(scheduler.submit_work(make_work(200 + index, {"coding"}, static_cast<std::uint32_t>(index * 10))).ok());
  }
  const BatchDecision batch = scheduler.schedule_batch(ScheduleRequest{});
  AS_CHECK(batch.assigned >= 1);
  AS_CHECK(scheduler.declare_agent_lost(snapshot.agents[2].id, snapshot.agents[2].boot, "pre-restart loss").ok());
  AS_CHECK(scheduler.submit_work(make_work(300, {"coding"})).ok());
  (void)scheduler.schedule_batch(ScheduleRequest{});
  snapshot.summary = scheduler.summary();
  snapshot.assignments = scheduler.all_assignments();
  snapshot.scheduler_epoch = scheduler.scheduler_epoch();
  snapshot.coordinator_epoch = scheduler.coordinator_epoch();
  snapshot.policy_generation = scheduler.policy().generation;
  snapshot.digest = scheduler.state_digest();
  snapshot.fenced_boots = scheduler.fenced_boots().size();
  snapshot.fairness = scheduler.fairness();
  AS_CHECK(scheduler.save(snapshot.path).ok());
  return snapshot;
}

}  // namespace

AS_TEST(recovery, restart_advances_authority_and_never_restores_live_state) {
  ScratchDirectory scratch("recovery_restart");
  const Snapshot snapshot = build_and_save(scratch);

  SchedulerOptions options = test_options();
  options.auto_start = false;
  AgentScheduler recovered(options);
  const RecoveryResult result = recovered.load(snapshot.path);
  AS_CHECK(result.ok());
  AS_CHECK(result.assignments_requiring_revalidation > 0);
  AS_CHECK(result.new_scheduler_epoch.value() == snapshot.scheduler_epoch.value() + 1);
  AS_CHECK(result.previous_coordinator_epoch == snapshot.coordinator_epoch ||
           result.previous_coordinator_epoch.value() == 0);
  AS_CHECK(recovered.fenced_boots().size() == snapshot.fenced_boots);

  // Dynamic evidence must not be current after recovery.
  for (const AgentStatus& agent : recovered.all_agents()) {
    AS_CHECK(!agent.evidence_current);
    AS_CHECK(!agent.lease_current);
    AS_CHECK(agent.health == AgentHealth::Unknown);
    AS_CHECK(agent.lifecycle == AgentLifecycle::RevalidationRequired ||
             agent.lifecycle == AgentLifecycle::Lost);
    for (const CapabilityEvidence& evidence : agent.capabilities) {
      AS_CHECK(evidence.state == CapabilityState::Stale);
    }
  }
  // Recovered assignments are never dispatchable.
  for (const Assignment& assignment : recovered.all_assignments()) {
    AS_CHECK(!assignment.lease_current);
    if (assignment_state_is_active(assignment.state)) {
      AS_CHECK(assignment.state == AssignmentState::RevalidationRequired);
      const DispatchResult dispatch = recovered.dispatch(assignment.binding.id, assignment.binding.generation);
      AS_CHECK(!dispatch.dispatched());
    }
  }
  AS_CHECK(recovered.check_invariants().ok());

  AS_CHECK(recovered.start().ok());
  AS_CHECK(recovered.coordinator_epoch().value() == snapshot.coordinator_epoch.value() + 1);
  AS_CHECK(recovered.scheduler_epoch().value() == snapshot.scheduler_epoch.value() + 1);
  AS_CHECK(recovered.check_invariants().ok());
}

AS_TEST(recovery, durable_semantics_are_preserved) {
  ScratchDirectory scratch("recovery_durable");
  const Snapshot snapshot = build_and_save(scratch);
  SchedulerOptions options = test_options();
  options.auto_start = false;
  AgentScheduler recovered(options);
  AS_CHECK(recovered.load(snapshot.path).ok());
  AS_CHECK(recovered.summary().agent_count == snapshot.summary.agent_count);
  AS_CHECK(recovered.summary().queue_count == snapshot.summary.queue_count);
  AS_CHECK(recovered.summary().fenced_boots == snapshot.summary.fenced_boots);
  AS_CHECK(recovered.policy().generation == snapshot.policy_generation);
  for (const WorkId work : snapshot.work) {
    const auto status = recovered.work_status(work);
    AS_CHECK(status.has_value());
  }
  AS_CHECK(recovered.work_status(WorkId{300}).has_value());
  std::uint32_t bypass_before = 0;
  for (const FairnessEntry& entry : snapshot.fairness.entries) {
    bypass_before += entry.bypass_count;
  }
  std::uint32_t bypass_after = 0;
  for (const FairnessEntry& entry : recovered.fairness().entries) {
    bypass_after += entry.bypass_count;
  }
  AS_CHECK(bypass_before == bypass_after);
}

AS_TEST(recovery, old_coordinator_epoch_authority_is_rejected_and_not_duplicated) {
  ScratchDirectory scratch("recovery_epoch");
  const Snapshot snapshot = build_and_save(scratch);
  SchedulerOptions options = test_options();
  options.auto_start = false;
  AgentScheduler recovered(options);
  AS_CHECK(recovered.load(snapshot.path).ok());
  AS_CHECK(recovered.start().ok());
  const RevalidationReport report = recovered.revalidate_assignments();
  AS_CHECK(report.examined > 0);
  AS_CHECK(report.invalidated > 0);
  for (const std::string& detail : report.details) {
    AS_CHECK(detail.find("PROMOTED") == std::string::npos);
  }
  for (const Assignment& assignment : recovered.all_assignments()) {
    if (assignment.binding.coordinator_epoch != recovered.coordinator_epoch() &&
        assignment_state_is_active(assignment.state)) {
      AS_FAIL("an assignment bound to the previous coordinator epoch is still active");
    }
  }
  AS_CHECK(recovered.check_invariants().ok());

  // Re-register and republish, then prove scheduling resumes without duplicate ownership.
  for (const AgentDescriptor& descriptor : snapshot.agents) {
    if (recovered.agent_status(descriptor.id)->lifecycle == AgentLifecycle::Lost) {
      continue;
    }
    const MutationResult registered = recovered.register_agent(descriptor);
    AS_CHECK(registered.ok() || registered.error->code == ErrorCode::StaleGeneration);
    if (!registered.ok()) {
      continue;
    }
    AS_CHECK(recovered.publish_capabilities(descriptor.id, descriptor.boot,
                                            make_profile(descriptor, {{"coding", 900}}))
                 .ok());
    AS_CHECK(recovered.publish_health(descriptor.id, descriptor.boot, healthy()).ok());
    AS_CHECK(recovered.publish_availability(descriptor.id, descriptor.boot, available()).ok());
    AS_CHECK(recovered.publish_load(descriptor.id, descriptor.boot, load()).ok());
  }
  const BatchDecision batch = recovered.schedule_batch(ScheduleRequest{});
  AS_CHECK(batch.assigned > 0);
  for (const WorkId work : snapshot.work) {
    std::uint32_t active_exclusive = 0;
    for (const Assignment& assignment : recovered.assignments_for_work(work)) {
      if (assignment_state_is_active(assignment.state) && assignment.binding.exclusive) {
        ++active_exclusive;
      }
    }
    AS_CHECK(active_exclusive <= 1);
  }
  AS_CHECK(recovered.check_invariants().ok());
}

AS_TEST(recovery, repeated_recovery_is_stable) {
  ScratchDirectory scratch("recovery_repeated");
  const Snapshot snapshot = build_and_save(scratch);
  const std::filesystem::path second = scratch.file("second.asstate");
  {
    SchedulerOptions options = test_options();
    options.auto_start = false;
    AgentScheduler first(options);
    AS_CHECK(first.load(snapshot.path).ok());
    AS_CHECK(first.start().ok());
    AS_CHECK(first.save(second).ok());
  }
  SchedulerOptions options = test_options();
  options.auto_start = false;
  AgentScheduler second_recovery(options);
  const RecoveryResult result = second_recovery.load(second);
  AS_CHECK(result.ok());
  AS_CHECK(result.new_scheduler_epoch.value() == snapshot.scheduler_epoch.value() + 2);
  AS_CHECK(second_recovery.summary().agent_count == snapshot.summary.agent_count);
  AS_CHECK(second_recovery.check_invariants().ok());
}
