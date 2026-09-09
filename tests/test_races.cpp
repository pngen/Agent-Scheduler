// Agent Scheduler — deterministic race coverage using barriers, never sleeps.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>

#include "scheduler_fixture.hpp"
#include "test_support.hpp"

using namespace agent_scheduler;
using namespace as_fixture;

namespace {

/// Blocks the first thread that reaches an armed interlock point until the test
/// releases it. Later arrivals pass straight through, so the racing thread can
/// complete its own operation.
class LatchInterlock final : public ScheduleInterlock {
 public:
  void arm(InterlockPoint point) {
    const std::lock_guard<std::mutex> guard(mutex_);
    point_ = point;
    armed_ = true;
    consumed_ = false;
    released_ = false;
  }
  void at(InterlockPoint point) override {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!armed_ || point_ != point || consumed_) {
      return;
    }
    consumed_ = true;
    arrived_ = true;
    arrived_cv_.notify_all();
    proceed_cv_.wait(lock, [this]() { return released_; });
  }
  /// Waits until the armed point is reached or the racing operation already finished.
  /// Deterministic: no timeout is involved.
  void wait_until_arrived(const std::atomic<bool>* completed = nullptr) {
    std::unique_lock<std::mutex> lock(mutex_);
    arrived_cv_.wait(lock, [this, completed]() {
      return arrived_ || !armed_ || (completed != nullptr && completed->load());
    });
  }
  void release() {
    const std::lock_guard<std::mutex> guard(mutex_);
    released_ = true;
    armed_ = false;
    proceed_cv_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable arrived_cv_;
  std::condition_variable proceed_cv_;
  InterlockPoint point_{InterlockPoint::BeforeScheduleCommit};
  bool armed_{false};
  bool consumed_{false};
  bool arrived_{false};
  bool released_{false};
};

struct Rig {
  std::shared_ptr<LatchInterlock> interlock = std::make_shared<LatchInterlock>();
  std::unique_ptr<AgentScheduler> scheduler;
  AgentDescriptor agent;
};

[[nodiscard]] Rig make_rig(std::uint32_t concurrency = 2) {
  SchedulerOptions options = test_options();
  options.interlock = std::make_shared<LatchInterlock>();
  Rig rig{std::static_pointer_cast<LatchInterlock>(options.interlock),
          std::make_unique<AgentScheduler>(options), make_agent(1, 1, concurrency)};
  AS_CHECK(register_full(*rig.scheduler, rig.agent, {{"coding", 900}}).ok());
  AS_CHECK(rig.scheduler->submit_work(make_work(100, {"coding"})).ok());
  return rig;
}

}  // namespace

AS_TEST(races, two_schedulers_racing_for_the_same_exclusive_work) {
  Rig rig = make_rig(4);
  rig.interlock->arm(InterlockPoint::BeforeScheduleCommit);
  ScheduleDecision first;
  ScheduleDecision second;
  std::thread blocked([&]() { first = rig.scheduler->schedule(WorkId{100}, WorkGeneration{1}); });
  rig.interlock->wait_until_arrived();
  second = rig.scheduler->schedule(WorkId{100}, WorkGeneration{1});
  rig.interlock->release();
  blocked.join();

  const bool first_assigned = first.outcome == ScheduleOutcome::Assigned;
  const bool second_assigned = second.outcome == ScheduleOutcome::Assigned;
  AS_CHECK(first_assigned != second_assigned);
  AS_CHECK(rig.scheduler->assignments_for_work(WorkId{100}).size() == 1);
  AS_CHECK(rig.scheduler->check_invariants().ok());
}

AS_TEST(races, agent_death_racing_assignment_commit) {
  Rig rig = make_rig(4);
  rig.interlock->arm(InterlockPoint::BeforeScheduleCommit);
  ScheduleDecision decision;
  std::thread blocked([&]() { decision = rig.scheduler->schedule(WorkId{100}, WorkGeneration{1}); });
  rig.interlock->wait_until_arrived();
  AS_CHECK(rig.scheduler->declare_agent_lost(rig.agent.id, rig.agent.boot, "killed during planning").ok());
  rig.interlock->release();
  blocked.join();

  AS_CHECK(decision.outcome != ScheduleOutcome::Assigned);
  AS_CHECK(decision.outcome == ScheduleOutcome::RejectStaleAgentBoot ||
           decision.outcome == ScheduleOutcome::RejectFenced ||
           decision.outcome == ScheduleOutcome::RejectLifecycle ||
           decision.outcome == ScheduleOutcome::RejectStaleAgentGeneration);
  AS_CHECK(rig.scheduler->assignments_for_work(WorkId{100}).empty());
  AS_CHECK(rig.scheduler->check_invariants().ok());
}

AS_TEST(races, reincarnation_racing_stale_publication) {
  Rig rig = make_rig();
  AgentDescriptor replacement = rig.agent;
  replacement.generation = AgentGeneration{2};
  replacement.boot = AgentBootId{derive(777, 1)};
  AS_CHECK(rig.scheduler->register_agent(replacement).ok());
  const MutationResult stale = rig.scheduler->publish_capabilities(
      rig.agent.id, rig.agent.boot, make_profile(rig.agent, {{"coding", 900}}));
  AS_CHECK(!stale.ok());
  AS_CHECK(stale.error->outcome == ScheduleOutcome::RejectStaleAgentBoot);
  AS_CHECK(rig.scheduler->agent_status(rig.agent.id)->boot == replacement.boot);
}

AS_TEST(races, cancellation_racing_assignment) {
  Rig rig = make_rig(4);
  rig.interlock->arm(InterlockPoint::BeforeScheduleCommit);
  ScheduleDecision decision;
  std::thread blocked([&]() { decision = rig.scheduler->schedule(WorkId{100}, WorkGeneration{1}); });
  rig.interlock->wait_until_arrived();
  AS_CHECK(rig.scheduler->cancel_work(WorkId{100}, WorkGeneration{1}, "racing cancel").ok());
  rig.interlock->release();
  blocked.join();

  AS_CHECK(decision.outcome == ScheduleOutcome::RejectCancelled);
  AS_CHECK(rig.scheduler->assignments_for_work(WorkId{100}).empty());
  AS_CHECK(rig.scheduler->work_status(WorkId{100})->lifecycle == WorkLifecycle::Cancelled);
}

AS_TEST(races, supersession_racing_dispatch) {
  Rig rig = make_rig(4);
  const ScheduleDecision planned = rig.scheduler->schedule(WorkId{100}, WorkGeneration{1});
  AS_REQUIRE(planned.assignment.has_value());
  rig.interlock->arm(InterlockPoint::BeforeDispatchHandoff);
  DispatchResult dispatch;
  std::thread blocked([&]() {
    dispatch = rig.scheduler->dispatch(planned.assignment->id, planned.assignment->generation);
  });
  rig.interlock->wait_until_arrived();
  AS_CHECK(rig.scheduler->
               supersede_work(WorkId{100}, WorkGeneration{1},
                               make_work(100, {"coding"}, 500, QueueId{1}, WorkGeneration{2}))
               .ok());
  rig.interlock->release();
  blocked.join();

  AS_CHECK(!dispatch.dispatched());
  AS_CHECK(dispatch.outcome == ScheduleOutcome::RejectStaleAssignment ||
           dispatch.outcome == ScheduleOutcome::RejectSuperseded);
  AS_CHECK(rig.scheduler->check_invariants().ok());
}

AS_TEST(races, policy_change_racing_dispatch) {
  Rig rig = make_rig(4);
  const ScheduleDecision planned = rig.scheduler->schedule(WorkId{100}, WorkGeneration{1});
  AS_REQUIRE(planned.assignment.has_value());
  rig.interlock->arm(InterlockPoint::BeforeDispatchHandoff);
  DispatchResult dispatch;
  std::thread blocked([&]() {
    dispatch = rig.scheduler->dispatch(planned.assignment->id, planned.assignment->generation);
  });
  rig.interlock->wait_until_arrived();
  PolicySnapshot policy = rig.scheduler->policy();
  policy.generation = PolicyGeneration{2};
  AS_CHECK(rig.scheduler->set_policy(policy).ok());
  rig.interlock->release();
  blocked.join();

  AS_CHECK(!dispatch.dispatched());
  AS_CHECK(dispatch.outcome == ScheduleOutcome::RejectStalePolicy ||
           dispatch.outcome == ScheduleOutcome::RejectStaleAssignment);
  AS_CHECK(rig.scheduler->check_invariants().ok());
}

AS_TEST(races, capability_generation_change_racing_dispatch) {
  Rig rig = make_rig(4);
  const ScheduleDecision planned = rig.scheduler->schedule(WorkId{100}, WorkGeneration{1});
  AS_REQUIRE(planned.assignment.has_value());
  rig.interlock->arm(InterlockPoint::BeforeDispatchHandoff);
  DispatchResult dispatch;
  std::thread blocked([&]() {
    dispatch = rig.scheduler->dispatch(planned.assignment->id, planned.assignment->generation);
  });
  rig.interlock->wait_until_arrived();
  AS_CHECK(rig.scheduler->
               publish_capabilities(rig.agent.id, rig.agent.boot,
                                     make_profile(rig.agent, {{"coding", 900}}, 900,
                                                  AgentCapabilityGeneration{2}))
               .ok());
  rig.interlock->release();
  blocked.join();

  AS_CHECK(!dispatch.dispatched());
  AS_CHECK(dispatch.outcome == ScheduleOutcome::RejectStaleCapability ||
           dispatch.outcome == ScheduleOutcome::RejectStaleAssignment ||
           dispatch.outcome == ScheduleOutcome::RevalidationRequired);
  AS_CHECK(rig.scheduler->check_invariants().ok());
  const RevalidationReport revalidated = rig.scheduler->revalidate_assignments();
  AS_CHECK(revalidated.invalidated == 1);
  AS_CHECK(rig.scheduler->check_invariants().ok());
}

AS_TEST(races, feasibility_change_racing_dispatch) {
  auto provider = std::make_shared<ScriptedFeasibilityProvider>();
  provider->set_resource(ResourceGeneration{9}, FeasibilityVerdict::Feasible);
  SchedulerOptions options = test_options();
  options.feasibility = provider;
  options.interlock = std::make_shared<LatchInterlock>();
  AgentScheduler scheduler(options);
  const auto interlock = std::static_pointer_cast<LatchInterlock>(options.interlock);
  const AgentDescriptor agent = make_agent(1, 1, 4);
  AS_CHECK(register_full(scheduler, agent, {{"coding", 900}}).ok());
  WorkRequest request = make_work(100, {"coding"});
  request.requirements.require_resource_feasibility = true;
  request.requirements.resource_generation = ResourceGeneration{9};
  AS_CHECK(scheduler.submit_work(request).ok());
  const ScheduleDecision planned = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_REQUIRE(planned.assignment.has_value());

  interlock->arm(InterlockPoint::BeforeDispatchHandoff);
  DispatchResult dispatch;
  std::atomic<bool> completed{false};
  std::thread blocked([&]() {
    dispatch = scheduler.dispatch(planned.assignment->id, planned.assignment->generation);
    completed.store(true);
  });
  interlock->wait_until_arrived(&completed);
  provider->set_resource(ResourceGeneration{9}, FeasibilityVerdict::Infeasible);
  interlock->release();
  blocked.join();

  AS_CHECK(!dispatch.dispatched());
  AS_CHECK(dispatch.outcome == ScheduleOutcome::RejectResourceFeasibility ||
           dispatch.outcome == ScheduleOutcome::RejectStaleAssignment);
  AS_CHECK(scheduler.check_invariants().ok());
}

AS_TEST(races, shutdown_racing_admission) {
  Rig rig = make_rig();
  rig.interlock->arm(InterlockPoint::BeforeWorkAdmissionCommit);
  MutationResult admitted;
  std::thread blocked([&]() { admitted = rig.scheduler->submit_work(make_work(101, {"coding"})); });
  rig.interlock->wait_until_arrived();
  AS_CHECK(rig.scheduler->shutdown().ok());
  rig.interlock->release();
  blocked.join();

  AS_CHECK(!admitted.ok());
  AS_CHECK(admitted.error->outcome == ScheduleOutcome::ShuttingDown);
  AS_CHECK(!rig.scheduler->work_status(WorkId{101}).has_value());
  AS_CHECK(rig.scheduler->check_invariants().ok());
}

AS_TEST(races, drain_racing_new_assignment) {
  Rig rig = make_rig(4);
  rig.interlock->arm(InterlockPoint::BeforeScheduleCommit);
  ScheduleDecision decision;
  std::thread blocked([&]() { decision = rig.scheduler->schedule(WorkId{100}, WorkGeneration{1}); });
  rig.interlock->wait_until_arrived();
  AS_CHECK(rig.scheduler->drain_agent(rig.agent.id, rig.agent.boot, false).ok());
  rig.interlock->release();
  blocked.join();

  AS_CHECK(!decision.assigned());
  AS_CHECK(decision.outcome == ScheduleOutcome::RejectDrain ||
           decision.outcome == ScheduleOutcome::RejectLifecycle ||
           decision.outcome == ScheduleOutcome::NoEligibleAgent);
  AS_CHECK(rig.scheduler->assignments_for_work(WorkId{100}).empty());
}

AS_TEST(races, lease_expiry_racing_acknowledgement) {
  auto clock = std::make_shared<ManualClock>(1000);
  SchedulerOptions options = test_options(clock);
  AgentScheduler scheduler(options);
  const AgentDescriptor agent = make_agent(1, 1, 4);
  AS_CHECK(register_full(scheduler, agent, {{"coding", 900}}).ok());
  AS_CHECK(scheduler.submit_work(make_work(100, {"coding"})).ok());
  const ScheduleDecision planned = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_REQUIRE(planned.assignment.has_value());
  const DispatchResult dispatch = scheduler.dispatch(planned.assignment->id, planned.assignment->generation);
  AS_REQUIRE(dispatch.handoff.has_value());
  const AssignmentBinding handoff = *dispatch.handoff;

  std::mutex mutex;
  std::condition_variable cv;
  int ready = 0;
  bool go = false;
  MutationResult ack_result;
  MutationResult expire_result;
  std::thread acknowledger([&]() {
    {
      std::unique_lock<std::mutex> lock(mutex);
      ++ready;
      cv.notify_all();
      cv.wait(lock, [&]() { return go; });
    }
    ack_result = scheduler.acknowledge(handoff.id, handoff.generation, handoff.dispatch,
                                       handoff.dispatch_generation, handoff.boot);
  });
  std::thread expirer([&]() {
    {
      std::unique_lock<std::mutex> lock(mutex);
      ++ready;
      cv.notify_all();
      cv.wait(lock, [&]() { return go; });
    }
    clock->set(1000 + scheduler.policy().assignment_lease_ttl_ms + 1);
    expire_result = scheduler.expire_leases();
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&]() { return ready == 2; });
    go = true;
    cv.notify_all();
  }
  acknowledger.join();
  expirer.join();
  AS_CHECK(expire_result.ok());

  const auto stored = scheduler.assignment(handoff.id);
  AS_REQUIRE(stored.has_value());
  // Two legal deterministic outcomes: the acknowledgement won the race and the
  // assignment is acknowledged, or the lease expired first and the assignment is
  // terminal. Either way it can never be dispatched again.
  AS_CHECK(stored->state == AssignmentState::Acknowledged ||
           stored->state == AssignmentState::Executing || stored->state == AssignmentState::Expired);
  if (stored->state == AssignmentState::Expired) {
    AS_CHECK(!stored->lease_current);
    const DispatchResult after =
        scheduler.dispatch(handoff.id, handoff.generation);
    AS_CHECK(!after.dispatched());
  } else {
    AS_CHECK(ack_result.ok());
    AS_CHECK(stored->lease_current);
  }
  AS_CHECK(scheduler.check_invariants().ok());
}

AS_TEST(races, completion_racing_restart) {
  ScratchDirectory scratch("race_restart");
  const std::filesystem::path path = scratch.file("race.asstate");
  SchedulerOptions options = test_options();
  AgentScheduler scheduler(options);
  const AgentDescriptor agent = make_agent(1, 1, 4);
  AS_CHECK(register_full(scheduler, agent, {{"coding", 900}}).ok());
  AS_CHECK(scheduler.submit_work(make_work(100, {"coding"})).ok());
  const ScheduleDecision planned = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_REQUIRE(planned.assignment.has_value());
  const AssignmentBinding binding = *planned.assignment;

  std::mutex mutex;
  std::condition_variable cv;
  int ready = 0;
  bool go = false;
  MutationResult completion;
  std::thread completer([&]() {
    {
      std::unique_lock<std::mutex> lock(mutex);
      ++ready;
      cv.notify_all();
      cv.wait(lock, [&]() { return go; });
    }
    completion = scheduler.complete_assignment(binding.id, binding.generation, binding.boot);
  });
  std::thread restarter([&]() {
    {
      std::unique_lock<std::mutex> lock(mutex);
      ++ready;
      cv.notify_all();
      cv.wait(lock, [&]() { return go; });
    }
    (void)scheduler.save(path);
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&]() { return ready == 2; });
    go = true;
    cv.notify_all();
  }
  completer.join();
  restarter.join();

  AS_CHECK(completion.ok());
  AS_CHECK(scheduler.save(path).ok());
  SchedulerOptions reload_options = test_options();
  reload_options.auto_start = false;
  AgentScheduler recovered(reload_options);
  AS_CHECK(recovered.load(path).ok());
  std::uint32_t active = 0;
  for (const Assignment& assignment : recovered.assignments_for_work(WorkId{100})) {
    if (assignment_state_is_active(assignment.state)) {
      ++active;
    }
  }
  AS_CHECK(active <= 1);
  AS_CHECK(recovered.check_invariants().ok());
}
