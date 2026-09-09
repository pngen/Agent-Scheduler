// Agent Scheduler — deterministic shutdown and lifecycle.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include <atomic>
#include <thread>
#include <vector>

#include "scheduler_fixture.hpp"
#include "test_support.hpp"

using namespace agent_scheduler;
using namespace as_fixture;

AS_TEST(shutdown, shutdown_is_idempotent_and_reopenable) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor descriptor = make_agent(1, 1, 2);
  AS_CHECK(register_full(scheduler, descriptor, {{"coding", 900}}).ok());
  AS_CHECK(scheduler.shutdown().ok());
  AS_CHECK(scheduler.shutdown().ok());
  AS_CHECK(!scheduler.running());
  AS_CHECK(scheduler.start().ok());
  AS_CHECK(scheduler.running());
  AS_CHECK(scheduler.start().ok());
  AS_CHECK(scheduler.coordinator_epoch().value() == 2);
  for (int cycle = 0; cycle < 5; ++cycle) {
    AS_CHECK(scheduler.shutdown().ok());
    AS_CHECK(scheduler.start().ok());
  }
  AS_CHECK(scheduler.coordinator_epoch().value() == 7);
  AS_CHECK(scheduler.check_invariants().ok());
}

AS_TEST(shutdown, committed_assignments_can_still_be_released) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor descriptor = make_agent(1, 1, 2);
  AS_CHECK(register_full(scheduler, descriptor, {{"coding", 900}}).ok());
  AS_CHECK(scheduler.submit_work(make_work(100, {"coding"})).ok());
  const ScheduleDecision planned = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_REQUIRE(planned.assignment.has_value());
  const DispatchResult dispatch = scheduler.dispatch(planned.assignment->id, planned.assignment->generation);
  AS_REQUIRE(dispatch.handoff.has_value());
  AS_CHECK(scheduler.shutdown().ok());
  AS_CHECK(scheduler
               .release(planned.assignment->id, planned.assignment->generation, descriptor.boot, "post-shutdown")
               .ok());
  AS_CHECK(scheduler.assignment(planned.assignment->id)->state == AssignmentState::Released);
  AS_CHECK(scheduler.check_invariants().ok());
}

AS_TEST(shutdown, shutdown_notifier_is_invoked_without_locks_held) {
  AgentScheduler scheduler(test_options());
  std::atomic<int> calls{0};
  scheduler.set_shutdown_notifier([&]() {
    // Re-entering the scheduler from the notifier must not deadlock.
    (void)scheduler.summary();
    calls.fetch_add(1);
  });
  AS_CHECK(scheduler.shutdown().ok());
  AS_CHECK(calls.load() == 1);
  AS_CHECK(scheduler.shutdown().ok());
  AS_CHECK(calls.load() == 1);
}

AS_TEST(shutdown, concurrent_mutation_and_query_are_safe) {
  AgentScheduler scheduler(test_options());
  for (std::uint64_t index = 1; index <= 4; ++index) {
    AS_CHECK(register_full(scheduler, make_agent(index, index * 5, 2), {{"coding", 900}}).ok());
  }
  for (std::uint64_t index = 1; index <= 32; ++index) {
    AS_CHECK(scheduler.submit_work(make_work(700 + index, {"coding"}, static_cast<std::uint32_t>(index))).ok());
  }
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> queries{0};
  std::vector<std::thread> readers;
  for (int index = 0; index < 4; ++index) {
    readers.emplace_back([&]() {
      while (!stop.load()) {
        (void)scheduler.summary();
        (void)scheduler.snapshot();
        (void)scheduler.fairness();
        queries.fetch_add(1);
      }
    });
  }
  std::vector<std::thread> writers;
  for (int index = 0; index < 2; ++index) {
    writers.emplace_back([&]() {
      for (int round = 0; round < 40; ++round) {
        (void)scheduler.schedule_batch(ScheduleRequest{});
        for (const Assignment& assignment : scheduler.all_assignments()) {
          if (assignment_state_is_active(assignment.state)) {
            (void)scheduler.release(assignment.binding.id, assignment.binding.generation, assignment.binding.boot,
                                    "churn");
          }
        }
      }
    });
  }
  for (std::thread& writer : writers) {
    writer.join();
  }
  stop.store(true);
  for (std::thread& reader : readers) {
    reader.join();
  }
  AS_CHECK(queries.load() > 0);
  AS_CHECK(scheduler.check_invariants().ok());
}
