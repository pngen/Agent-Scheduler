// Agent Scheduler — regression coverage for authority, history, persistence, and session
// defects. Every test names the guarantee it protects.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "agent_scheduler/digest.hpp"
#include "agent_scheduler/net/agent_client.hpp"
#include "agent_scheduler/net/control_client.hpp"
#include "agent_scheduler/net/coordinator.hpp"
#include "agent_scheduler/net/frame.hpp"
#include "agent_scheduler/net/protocol.hpp"
#include "agent_scheduler/net/transport.hpp"
#include "agent_scheduler/persistence.hpp"
#include "agent_scheduler/version.hpp"
#include "scheduler_fixture.hpp"
#include "test_support.hpp"

using namespace agent_scheduler;
using namespace agent_scheduler::net;
using namespace as_fixture;

namespace {

/// Blocks the first thread that reaches an armed interlock point until released.
class LatchInterlock final : public ScheduleInterlock {
 public:
  void arm(InterlockPoint point) {
    const std::lock_guard<std::mutex> guard(mutex_);
    point_ = point;
    armed_ = true;
    consumed_ = false;
    released_ = false;
    arrived_ = false;
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
  void wait_until_arrived() {
    std::unique_lock<std::mutex> lock(mutex_);
    arrived_cv_.wait(lock, [this]() { return arrived_ || !armed_; });
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

[[nodiscard]] SchedulerOptions stopped(SchedulerOptions options) {
  options.auto_start = false;
  return options;
}

[[nodiscard]] std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] bool active(const AgentScheduler& scheduler, AssignmentId id) {
  const auto stored = scheduler.assignment(id);
  return stored.has_value() && assignment_state_is_active(stored->state);
}

/// Polls a condition for a bounded number of short steps; never terminates anything.
template <class Predicate>
[[nodiscard]] bool eventually(Predicate predicate) {
  for (int step = 0; step < 500; ++step) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return predicate();
}

[[nodiscard]] AgentClientOptions agent_options(std::uint16_t port, const AgentDescriptor& descriptor) {
  AgentClientOptions options;
  options.port = port;
  options.descriptor = descriptor;
  options.profile = make_profile(descriptor, {{"coding", 900}});
  options.health = healthy();
  options.availability = available();
  options.load = load();
  return options;
}

template <class Message>
[[nodiscard]] std::vector<std::uint8_t> frame_bytes(MessageType type, std::uint64_t correlation,
                                                    const Message& message) {
  ByteWriter writer;
  encode(writer, message, ResourceLimits{});
  Frame frame;
  frame.header.version = static_cast<std::uint16_t>(protocol_version);
  frame.header.type = static_cast<std::uint16_t>(type);
  frame.header.correlation = correlation;
  frame.payload = writer.take();
  return encode_frame(frame, ResourceLimits{}.max_frame_size);
}

}  // namespace

// ---------------------------------------------------------------------------
// bounded history
// ---------------------------------------------------------------------------

AS_TEST(regression, closed_assignment_history_and_leases_stay_within_their_bounds) {
  SchedulerOptions options = test_options();
  options.limits.max_active_assignments = 2;
  options.limits.max_historical_assignments = 2;
  options.limits.max_leases = 2;
  AgentScheduler scheduler(options);
  const AgentDescriptor agent = make_agent(1, 1, 2);
  AS_REQUIRE(register_full(scheduler, agent, {{"coding", 900}}).ok());
  for (std::uint64_t index = 1; index <= 4; ++index) {
    AS_REQUIRE(scheduler.submit_work(make_work(100 + index, {"coding"})).ok());
    const ScheduleDecision decision = scheduler.schedule(WorkId{100 + index}, WorkGeneration{1});
    AS_REQUIRE(decision.assignment.has_value());
    AS_REQUIRE(scheduler.complete_assignment(decision.assignment->id, decision.assignment->generation,
                                             agent.boot).ok());
  }
  AS_CHECK(scheduler.check_invariants().ok());
  ScratchDirectory scratch("regression_history");
  AS_REQUIRE(scheduler.save(scratch.file("state.asstate")).ok());
  AgentScheduler recovered(stopped(options));
  AS_CHECK(recovered.load(scratch.file("state.asstate")).ok());
}

AS_TEST(regression, active_assignments_never_exceed_the_configured_bound) {
  SchedulerOptions options = test_options();
  options.limits.max_active_assignments = 2;
  options.limits.max_historical_assignments = 2;
  AgentScheduler scheduler(options);
  std::size_t assigned = 0;
  for (std::uint64_t index = 1; index <= 3; ++index) {
    AS_REQUIRE(register_full(scheduler, make_agent(index, index, 1), {{"coding", 900}}).ok());
    AS_REQUIRE(scheduler.submit_work(make_work(100 + index, {"coding"})).ok());
  }
  for (std::uint64_t index = 1; index <= 3; ++index) {
    if (scheduler.schedule(WorkId{100 + index}, WorkGeneration{1}).assignment.has_value()) {
      ++assigned;
    }
  }
  AS_CHECK(assigned == 2);
  AS_CHECK(scheduler.check_invariants().ok());
}

AS_TEST(regression, fenced_incarnation_history_stays_within_its_bound) {
  SchedulerOptions options = test_options();
  options.limits.max_fenced_boots = 1;
  AgentScheduler scheduler(options);
  AS_REQUIRE(scheduler.declare_agent_lost(AgentId{1}, AgentBootId{derive(5, 1)}, "first report").ok());
  AS_REQUIRE(scheduler.declare_agent_lost(AgentId{2}, AgentBootId{derive(5, 2)}, "second report").ok());
  AS_CHECK(scheduler.check_invariants().ok());
  ScratchDirectory scratch("regression_fences");
  AS_REQUIRE(scheduler.save(scratch.file("state.asstate")).ok());
  AgentScheduler recovered(stopped(options));
  AS_CHECK(recovered.load(scratch.file("state.asstate")).ok());
}

// ---------------------------------------------------------------------------
// incarnation authority
// ---------------------------------------------------------------------------

AS_TEST(regression, fencing_names_one_incarnation_and_never_another_agent) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor first = make_agent(1, 1, 2);
  AgentDescriptor second = make_agent(2, 2, 2);
  second.boot = first.boot;  // one host process serving two agent identities
  AS_REQUIRE(register_full(scheduler, first, {{"coding", 900}}).ok());
  AS_REQUIRE(register_full(scheduler, second, {{"coding", 900}}).ok());
  WorkRequest work = make_work(100, {"coding"});
  work.requirements.affinity = {second.id};
  canonicalize(work);
  AS_REQUIRE(scheduler.submit_work(work).ok());
  const ScheduleDecision decision = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_REQUIRE(decision.assignment.has_value());
  AS_REQUIRE(decision.assignment->agent == second.id);

  AS_CHECK(scheduler.drain_agent(first.id, first.boot, true).ok());
  AS_CHECK(scheduler.declare_agent_lost(AgentId{999}, second.boot, "misdirected report").ok());
  AS_CHECK(active(scheduler, decision.assignment->id));
  AS_CHECK(scheduler.heartbeat(second.id, second.boot, second.registration_generation).ok());
  AS_CHECK(scheduler.check_invariants().ok());
}

AS_TEST(regression, same_incarnation_reregistration_cannot_regress_or_rewrite_eligibility) {
  AgentScheduler scheduler(test_options());
  AgentDescriptor agent = make_agent(1, 1, 2);
  agent.registration_generation = AgentRegistrationGeneration{2};
  AS_REQUIRE(register_full(scheduler, agent, {{"coding", 900}}).ok());

  AgentDescriptor stale = agent;
  stale.registration_generation = AgentRegistrationGeneration{1};
  AS_CHECK(!scheduler.register_agent(stale).ok());
  AS_CHECK(scheduler.heartbeat(agent.id, agent.boot, AgentRegistrationGeneration{2}).ok());

  AgentDescriptor moved = agent;
  moved.registration_generation = AgentRegistrationGeneration{3};
  moved.placement_domain = PlacementDomainId{2};
  moved.topology_epoch = TopologyEpoch{2};
  AS_CHECK(!scheduler.register_agent(moved).ok());
  AS_CHECK(scheduler.check_invariants().ok());
}

AS_TEST(regression, dispatch_revalidates_readiness_and_queue_generation) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor agent = make_agent(1, 1, 2);
  AS_REQUIRE(register_full(scheduler, agent, {{"coding", 900}}).ok());
  AS_REQUIRE(scheduler.submit_work(make_work(100, {"coding"})).ok());
  const ScheduleDecision decision = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_REQUIRE(decision.assignment.has_value());
  const AssignmentBinding binding = *decision.assignment;

  AvailabilityObservation not_ready = available(AgentAvailabilityGeneration{2});
  not_ready.readiness = AgentReadiness::NotReady;
  AS_REQUIRE(scheduler.publish_availability(agent.id, agent.boot, not_ready).ok());
  AS_CHECK(!scheduler.dispatch(binding.id, binding.generation).dispatched());

  AS_REQUIRE(scheduler.publish_availability(agent.id, agent.boot, available(AgentAvailabilityGeneration{3})).ok());
  AS_REQUIRE(scheduler.register_queue(QueueId{1}, QueueGeneration{2}, "default", 1).ok());
  AS_CHECK(!scheduler.dispatch(binding.id, binding.generation).dispatched());
}

AS_TEST(regression, a_new_generation_must_bring_a_new_boot) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor agent = make_agent(1, 1, 2);
  AS_REQUIRE(register_full(scheduler, agent, {{"coding", 900}}).ok());
  AgentDescriptor reused = agent;
  reused.generation = AgentGeneration{2};
  AS_CHECK(!scheduler.register_agent(reused).ok());
  AS_CHECK(scheduler.publish_health(agent.id, agent.boot, healthy(AgentHealthGeneration{2})).ok());

  const AgentBootId early{derive(9, 9)};
  AS_REQUIRE(scheduler.declare_agent_lost(AgentId{7}, early, "reported lost before it registered").ok());
  AgentDescriptor late = make_agent(7, 7, 2);
  late.boot = early;
  AS_CHECK(!scheduler.register_agent(late).ok());
}

AS_TEST(regression, an_observation_must_name_the_incarnation) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor agent = make_agent(1, 1, 2);
  AS_REQUIRE(register_full(scheduler, agent, {{"coding", 900}}).ok());
  AS_REQUIRE(scheduler.submit_work(make_work(100, {"coding"})).ok());
  const ScheduleDecision decision = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_REQUIRE(decision.assignment.has_value());
  AS_REQUIRE(scheduler.dispatch(decision.assignment->id, decision.assignment->generation).dispatched());
  AS_CHECK(!scheduler.complete_assignment(decision.assignment->id, decision.assignment->generation,
                                          AgentBootId{}).ok());
  AS_CHECK(active(scheduler, decision.assignment->id));
}

AS_TEST(regression, every_capability_in_a_profile_must_be_fresh) {
  constexpr std::uint64_t now = 10'000'000;
  SchedulerOptions options = test_options(std::make_shared<ManualClock>(now));
  options.policy.capability_staleness_ms = 60000;
  AgentScheduler scheduler(options);
  const AgentDescriptor agent = make_agent(1, 1, 2);
  AS_REQUIRE(scheduler.register_agent(agent).ok());
  CapabilityProfile profile = make_profile(agent, {{"a", 900}, {"gpu", 900}});
  for (CapabilityEvidence& evidence : profile.capabilities) {
    evidence.observed_at_ms = evidence.name == "gpu" ? now - 3'600'000 : now;
  }
  AS_REQUIRE(scheduler.publish_capabilities(agent.id, agent.boot, profile).ok());
  AS_REQUIRE(scheduler.publish_health(agent.id, agent.boot, healthy()).ok());
  AS_REQUIRE(scheduler.publish_availability(agent.id, agent.boot, available()).ok());
  AS_REQUIRE(scheduler.publish_load(agent.id, agent.boot, load()).ok());
  AS_REQUIRE(scheduler.submit_work(make_work(100, {"gpu"})).ok());
  AS_CHECK(!scheduler.schedule(WorkId{100}, WorkGeneration{1}).assignment.has_value());
}

// ---------------------------------------------------------------------------
// commit and admission atomicity
// ---------------------------------------------------------------------------

AS_TEST(regression, a_parked_commit_never_lands_on_finished_work) {
  SchedulerOptions options = test_options();
  auto latch = std::make_shared<LatchInterlock>();
  options.interlock = latch;
  AgentScheduler scheduler(options);
  const AgentDescriptor planned = make_agent(2, 2, 2);
  const AgentDescriptor late = make_agent(1, 1, 2);
  AS_REQUIRE(register_full(scheduler, planned, {{"coding", 800}}).ok());
  AS_REQUIRE(scheduler.register_agent(late).ok());  // registering: not yet eligible
  AS_REQUIRE(scheduler.submit_work(make_work(100, {"coding"})).ok());

  latch->arm(InterlockPoint::BeforeScheduleCommit);
  ScheduleDecision parked;
  std::thread blocked([&]() { parked = scheduler.schedule(WorkId{100}, WorkGeneration{1}); });
  latch->wait_until_arrived();
  AS_CHECK(scheduler.publish_capabilities(late.id, late.boot, make_profile(late, {{"coding", 950}})).ok());
  AS_CHECK(scheduler.publish_health(late.id, late.boot, healthy()).ok());
  AS_CHECK(scheduler.publish_availability(late.id, late.boot, available()).ok());
  AS_CHECK(scheduler.publish_load(late.id, late.boot, load()).ok());
  const ScheduleDecision won = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  bool completed = false;
  if (won.assignment.has_value()) {
    completed = scheduler.complete_assignment(won.assignment->id, won.assignment->generation,
                                              won.assignment->boot).ok();
  }
  latch->release();
  blocked.join();
  AS_CHECK(completed);
  AS_CHECK(parked.outcome != ScheduleOutcome::Assigned);
  AS_CHECK(scheduler.work_status(WorkId{100})->lifecycle == WorkLifecycle::Completed);
  AS_CHECK(scheduler.check_invariants().ok());
}

AS_TEST(regression, rejected_work_changes_leave_no_trace) {
  SchedulerOptions options = test_options();
  options.limits.max_tenants = 1;
  AgentScheduler scheduler(options);
  const AgentDescriptor agent = make_agent(1, 1, 2);
  AS_REQUIRE(register_full(scheduler, agent, {{"coding", 900}}).ok());
  AS_REQUIRE(scheduler.submit_work(make_work(100, {"coding"})).ok());
  const ScheduleDecision decision = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_REQUIRE(decision.assignment.has_value());

  // A re-admission for a tenant beyond max_tenants is refused and changes nothing.
  WorkRequest other_tenant = make_work(100, {"coding"}, 500, QueueId{1}, WorkGeneration{2});
  other_tenant.requirements.tenant = TenantId{2};
  canonicalize(other_tenant);
  AS_CHECK(!scheduler.submit_work(other_tenant).ok());
  AS_CHECK(scheduler.work_status(WorkId{100})->generation == WorkGeneration{1});
  AS_CHECK(active(scheduler, decision.assignment->id));

  // A supersession naming a stale queue generation is refused and changes nothing.
  AS_REQUIRE(scheduler.register_queue(QueueId{1}, QueueGeneration{2}, "default", 1).ok());
  const WorkRequest stale_queue = make_work(100, {"coding"}, 500, QueueId{1}, WorkGeneration{2});
  AS_CHECK(!scheduler.supersede_work(WorkId{100}, WorkGeneration{1}, stale_queue).ok());
  AS_CHECK(scheduler.work_status(WorkId{100})->generation == WorkGeneration{1});
  AS_CHECK(active(scheduler, decision.assignment->id));
  AS_CHECK(scheduler.check_invariants().ok());

  AS_REQUIRE(scheduler.shutdown().ok());
  WorkRequest after_close = make_work(100, {"coding"}, 500, QueueId{1}, WorkGeneration{3});
  after_close.queue_generation = QueueGeneration{2};
  AS_CHECK(!scheduler.supersede_work(WorkId{100}, WorkGeneration{1}, after_close).ok());
  AS_CHECK(scheduler.work_status(WorkId{100})->generation == WorkGeneration{1});
}

AS_TEST(regression, dispatch_handoff_rechecks_the_lease_after_the_pause) {
  auto clock = std::make_shared<ManualClock>(1000);
  SchedulerOptions options = test_options(clock);
  options.policy.assignment_lease_ttl_ms = 1000;
  auto latch = std::make_shared<LatchInterlock>();
  options.interlock = latch;
  AgentScheduler scheduler(options);
  const AgentDescriptor agent = make_agent(1, 1, 2);
  AS_REQUIRE(register_full(scheduler, agent, {{"coding", 900}}).ok());
  AS_REQUIRE(scheduler.submit_work(make_work(100, {"coding"})).ok());
  const ScheduleDecision decision = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  AS_REQUIRE(decision.assignment.has_value());

  latch->arm(InterlockPoint::BeforeDispatchHandoff);
  DispatchResult result;
  std::thread blocked([&]() { result = scheduler.dispatch(decision.assignment->id, decision.assignment->generation); });
  latch->wait_until_arrived();
  clock->advance(5000);
  latch->release();
  blocked.join();
  AS_CHECK(!result.dispatched());
}

// ---------------------------------------------------------------------------
// external feasibility runs outside the state lock
// ---------------------------------------------------------------------------

namespace {

/// A provider whose evaluation asks the scheduler about the work from another thread.
class ConsultingProvider final : public ExternalFeasibilityProvider {
 public:
  ExternalFeasibilityResult evaluate(const ExternalFeasibilityRequest& request) override {
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> finished = done->get_future();
    {
      const std::lock_guard<std::mutex> guard(mutex_);
      probes_.emplace_back([this, done, work = request.work]() {
        (void)scheduler->work_status(work);
        done->set_value();
      });
    }
    if (finished.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
      stalled.store(true);
    }
    ExternalFeasibilityResult result;
    result.resource = FeasibilityVerdict::Feasible;
    result.budget = FeasibilityVerdict::Feasible;
    result.slo = FeasibilityVerdict::Feasible;
    result.reservation = FeasibilityVerdict::Feasible;
    return result;
  }
  void join_probes() {
    std::vector<std::thread> probes;
    {
      const std::lock_guard<std::mutex> guard(mutex_);
      probes.swap(probes_);
    }
    for (std::thread& probe : probes) {
      probe.join();
    }
  }

  AgentScheduler* scheduler{nullptr};
  std::atomic<bool> stalled{false};

 private:
  std::mutex mutex_;
  std::vector<std::thread> probes_;
};

}  // namespace

AS_TEST(regression, a_feasibility_provider_may_consult_the_scheduler) {
  auto provider = std::make_shared<ConsultingProvider>();
  SchedulerOptions options = test_options();
  options.feasibility = provider;
  AgentScheduler scheduler(options);
  provider->scheduler = &scheduler;
  const AgentDescriptor agent = make_agent(1, 1, 2);
  AS_REQUIRE(register_full(scheduler, agent, {{"coding", 900}}).ok());
  WorkRequest work = make_work(100, {"coding"});
  work.requirements.require_resource_feasibility = true;
  canonicalize(work);
  AS_REQUIRE(scheduler.submit_work(work).ok());
  const ScheduleDecision decision = scheduler.schedule(WorkId{100}, WorkGeneration{1});
  bool dispatched = false;
  if (decision.assignment.has_value()) {
    dispatched = scheduler.dispatch(decision.assignment->id, decision.assignment->generation).dispatched();
  }
  provider->join_probes();
  AS_CHECK(!provider->stalled.load());
  AS_CHECK(decision.assignment.has_value());
  AS_CHECK(dispatched);
}

// ---------------------------------------------------------------------------
// persistence
// ---------------------------------------------------------------------------

AS_TEST(regression, saving_never_throws_and_long_reasons_still_persist) {
  AgentScheduler scheduler(test_options());
  AS_REQUIRE(scheduler.submit_work(make_work(100, {"coding"})).ok());
  AS_REQUIRE(scheduler.cancel_work(WorkId{100}, WorkGeneration{1}, std::string(600, 'x')).ok());
  ScratchDirectory scratch("regression_reason");
  MutationResult saved;
  bool threw = false;
  try {
    saved = scheduler.save(scratch.file("state.asstate"));
  } catch (const std::exception&) {
    threw = true;
  }
  AS_CHECK(!threw);
  AS_CHECK(saved.ok());
  AgentScheduler recovered(stopped(test_options()));
  AS_CHECK(recovered.load(scratch.file("state.asstate")).ok());
}

AS_TEST(regression, a_policy_that_cannot_be_persisted_is_refused_up_front) {
  SchedulerOptions options = test_options();
  options.limits.max_identifier_size = 16;
  ScratchDirectory scratch("regression_policy");
  bool constructed = false;
  bool saved = false;
  try {
    AgentScheduler scheduler(options);
    constructed = true;
    try {
      saved = scheduler.save(scratch.file("state.asstate")).ok();
    } catch (const std::exception&) {
      saved = false;
    }
  } catch (const std::invalid_argument&) {
    constructed = false;
  }
  AS_CHECK(!constructed || saved);
}

AS_TEST(regression, concurrent_saves_never_leave_an_older_state_on_disk) {
  ScratchDirectory scratch("regression_concurrent_save");
  for (int round = 0; round < 25; ++round) {
    AgentScheduler scheduler(test_options());
    const std::filesystem::path path = scratch.file("state-" + std::to_string(round) + ".asstate");
    std::atomic<int> ready{0};
    std::vector<std::thread> savers;
    for (std::uint64_t index = 0; index < 4; ++index) {
      savers.emplace_back([&scheduler, &path, &ready, index]() {
        ready.fetch_add(1);
        while (ready.load() < 4) {
          std::this_thread::yield();
        }
        (void)scheduler.submit_work(make_work(500 + index, {"coding"}));
        (void)scheduler.save(path);
      });
    }
    for (std::thread& saver : savers) {
      saver.join();
    }
    AgentScheduler recovered(stopped(test_options()));
    AS_REQUIRE(recovered.load(path).ok());
    AS_CHECK(recovered.all_work().size() == 4);
  }
}

AS_TEST(regression, a_drain_survives_recovery) {
  AgentScheduler scheduler(test_options());
  const AgentDescriptor agent = make_agent(1, 1, 2);
  AS_REQUIRE(register_full(scheduler, agent, {{"coding", 900}}).ok());
  AS_REQUIRE(scheduler.drain_agent(agent.id, agent.boot, false).ok());
  ScratchDirectory scratch("regression_drain");
  AS_REQUIRE(scheduler.save(scratch.file("state.asstate")).ok());

  AgentScheduler recovered(stopped(test_options()));
  AS_REQUIRE(recovered.load(scratch.file("state.asstate")).ok());
  AS_REQUIRE(recovered.start().ok());
  AS_REQUIRE(register_full(recovered, agent, {{"coding", 900}}).ok());
  AS_CHECK(recovered.agent_status(agent.id)->lifecycle == AgentLifecycle::Draining);
  AS_REQUIRE(recovered.submit_work(make_work(100, {"coding"})).ok());
  AS_CHECK(!recovered.schedule(WorkId{100}, WorkGeneration{1}).assignment.has_value());
}

AS_TEST(regression, a_saved_file_always_fits_the_load_bound) {
  const auto build = [](std::uint64_t max_bytes) {
    SchedulerOptions options = test_options();
    if (max_bytes != 0) {
      options.limits.max_persistence_bytes = max_bytes;
    }
    auto scheduler = std::make_unique<AgentScheduler>(options);
    for (std::uint64_t index = 1; index <= 8; ++index) {
      (void)register_full(*scheduler, make_agent(index, index, 2), {{"coding", 900}});
    }
    return scheduler;
  };
  ScratchDirectory scratch("regression_bound");
  const auto probe = build(0);
  AS_REQUIRE(probe->save(scratch.file("probe.asstate")).ok());
  const std::uint64_t file_size = std::filesystem::file_size(scratch.file("probe.asstate"));
  AS_REQUIRE(file_size > 1100);

  // The bound admits the payload but not the header and trailer around it.
  const auto tight = build(file_size - 1);
  const MutationResult saved = tight->save(scratch.file("tight.asstate"));
  if (saved.ok()) {
    SchedulerOptions reload = stopped(test_options());
    reload.limits.max_persistence_bytes = file_size - 1;
    AgentScheduler loader(reload);
    AS_CHECK(loader.load(scratch.file("tight.asstate")).ok());
  }
  AS_CHECK(!saved.ok());
}

AS_TEST(regression, a_file_with_a_repeated_section_is_refused) {
  AgentScheduler scheduler(test_options());  // no assignments, leases, or fences
  ScratchDirectory scratch("regression_sections");
  const std::filesystem::path path = scratch.file("state.asstate");
  AS_REQUIRE(scheduler.save(path).ok());
  std::vector<std::uint8_t> bytes = read_file(path);
  const std::size_t header = PersistenceFormat::header_bytes;
  const std::size_t trailer = PersistenceFormat::trailer_bytes;
  AS_REQUIRE(bytes.size() > header + trailer + 15);
  std::vector<std::uint8_t> payload(bytes.begin() + static_cast<std::ptrdiff_t>(header),
                                    bytes.end() - static_cast<std::ptrdiff_t>(trailer));
  // The payload ends with three empty sections: assignments (7), leases (8), fenced (9).
  AS_REQUIRE(payload[payload.size() - 10] == 8 && payload[payload.size() - 5] == 9);
  payload[payload.size() - 10] = 9;  // a second fenced section and no lease section

  // Re-seal every integrity field so only the section structure is wrong.
  std::vector<std::uint8_t> sealed(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(header));
  const std::uint32_t payload_crc = crc32(payload);
  std::memcpy(sealed.data() + 32, &payload_crc, sizeof(payload_crc));
  const Digest256 digest = sha256(payload);
  std::memcpy(sealed.data() + 40, digest.bytes.data(), digest.bytes.size());
  const std::uint32_t zero = 0;
  std::memcpy(sealed.data() + 36, &zero, sizeof(zero));
  const std::uint32_t header_crc = crc32(sealed);
  std::memcpy(sealed.data() + 36, &header_crc, sizeof(header_crc));
  sealed.insert(sealed.end(), payload.begin(), payload.end());
  std::vector<std::uint8_t> tail(bytes.end() - static_cast<std::ptrdiff_t>(trailer), bytes.end());
  const std::uint32_t trailer_crc = crc32(sealed) ^ 0x5A5A5A5Au;
  std::memcpy(tail.data() + 8, &trailer_crc, sizeof(trailer_crc));
  sealed.insert(sealed.end(), tail.begin(), tail.end());
  write_file(path, sealed);

  AgentScheduler recovered(stopped(test_options()));
  AS_CHECK(!recovered.load(path).ok());
}

// ---------------------------------------------------------------------------
// coordinator sessions
// ---------------------------------------------------------------------------

AS_TEST(regression, a_clean_coordinator_stop_does_not_declare_live_incarnations_lost) {
  ScratchDirectory scratch("regression_stop");
  const std::filesystem::path path = scratch.file("state.asstate");
  AgentScheduler scheduler(stopped(test_options()));
  CoordinatorOptions coordinator_options;
  coordinator_options.persistence_path = path;
  CoordinatorServer server(scheduler, coordinator_options);
  AS_REQUIRE(server.start().ok());
  const std::uint16_t port = server.port().value_or(0);

  const AgentDescriptor descriptor = make_agent(1, 1, 2);
  AgentClientOptions client_options = agent_options(port, descriptor);
  client_options.auto_complete = false;
  AgentClient client(client_options);
  AS_REQUIRE(client.connect_and_register().ok());
  AS_REQUIRE(client.publish_health().ok());
  AS_REQUIRE(client.publish_availability().ok());
  AS_REQUIRE(client.publish_load().ok());

  ControlClientOptions control_options;
  control_options.port = port;
  ControlClient control(control_options);
  AS_REQUIRE(control.connect().ok());
  AS_REQUIRE(control.submit_work(make_work(100, {"coding"})).ok());
  AS_REQUIRE(eventually([&]() { return client.received_assignments().size() == 1; }));
  control.close();

  AS_REQUIRE(server.stop().ok());
  client.stop();
  AgentScheduler recovered(stopped(test_options()));
  AS_REQUIRE(recovered.load(path).ok());
  AS_CHECK(recovered.fenced_boots().empty());
  for (const Assignment& assignment : recovered.all_assignments()) {
    AS_CHECK(assignment.state != AssignmentState::Lost);
  }
}

AS_TEST(regression, closing_a_superseded_session_does_not_fence_the_incarnation) {
  AgentScheduler scheduler(stopped(test_options()));
  CoordinatorServer server(scheduler, CoordinatorOptions{});
  AS_REQUIRE(server.start().ok());
  const std::uint16_t port = server.port().value_or(0);
  const AgentDescriptor descriptor = make_agent(1, 1, 2);

  AgentClient first(agent_options(port, descriptor));
  AS_REQUIRE(first.connect_and_register().ok());
  ControlClientOptions control_options;
  control_options.port = port;
  ControlClient control(control_options);
  AS_REQUIRE(control.connect().ok());
  AS_REQUIRE(control.register_agent(descriptor).ok());  // the incarnation now lives on this session

  first.stop();
  AS_REQUIRE(eventually([&]() { return server.connection_count() == 1; }));
  AS_CHECK(scheduler.fenced_boots().empty());
  AS_CHECK(scheduler.agent_status(descriptor.id)->lifecycle != AgentLifecycle::Lost);

  AS_REQUIRE(control.deregister_agent(descriptor.id, descriptor.boot, true).ok());
  control.close();
  AS_REQUIRE(eventually([&]() { return server.connection_count() == 0; }));
  AS_CHECK(scheduler.agent_status(descriptor.id)->lifecycle == AgentLifecycle::Retired);
  AS_REQUIRE(server.stop().ok());
}

AS_TEST(regression, frames_sent_just_before_a_close_are_still_processed) {
  AgentScheduler scheduler(stopped(test_options()));
  CoordinatorServer server(scheduler, CoordinatorOptions{});
  AS_REQUIRE(server.start().ok());
  const std::uint16_t port = server.port().value_or(0);
  constexpr std::uint64_t kClients = 40;
  for (std::uint64_t index = 0; index < kClients; ++index) {
    TcpStream stream;
    AS_REQUIRE(connect_loopback(port, stream).ok());
    SubmitWorkMessage message;
    message.request = make_work(1000 + index, {"coding"});
    const std::vector<std::uint8_t> bytes = frame_bytes(MessageType::SubmitWork, index + 1, message);
    stream.write_all(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
    stream.close();
  }
  AS_CHECK(eventually([&]() { return scheduler.all_work().size() == kClients; }));
  AS_REQUIRE(server.stop().ok());
}

AS_TEST(regression, an_incompatible_peer_is_told_why_before_the_session_closes) {
  AgentScheduler scheduler(stopped(test_options()));
  CoordinatorServer server(scheduler, CoordinatorOptions{});
  AS_REQUIRE(server.start().ok());
  const std::uint16_t port = server.port().value_or(0);
  for (std::uint64_t attempt = 0; attempt < 20; ++attempt) {
    TcpStream stream;
    AS_REQUIRE(connect_loopback(port, stream).ok());
    HelloMessage hello;
    hello.protocol_version = protocol_version + 1;
    hello.role = "future";
    const std::vector<std::uint8_t> bytes = frame_bytes(MessageType::Hello, attempt + 1, hello);
    stream.write_all(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
    FrameStreamDecoder decoder(ResourceLimits{}.max_frame_size, static_cast<std::uint16_t>(protocol_version));
    std::optional<Frame> reply;
    std::array<std::uint8_t, 4096> buffer{};
    while (!reply.has_value()) {
      const std::size_t received = stream.read_some(std::span<std::uint8_t>(buffer.data(), buffer.size()));
      if (received == 0) {
        break;
      }
      decoder.append(std::span<const std::uint8_t>(buffer.data(), received));
      FrameCodecStatus status = FrameCodecStatus::NeedMoreData;
      reply = decoder.next(status);
    }
    AS_CHECK(reply.has_value() && reply->header.type == static_cast<std::uint16_t>(MessageType::HelloResult));
    stream.close();
  }
  AS_REQUIRE(server.stop().ok());
}

AS_TEST(regression, an_agent_request_answered_with_an_error_completes) {
  AgentScheduler scheduler(stopped(test_options()));
  CoordinatorServer server(scheduler, CoordinatorOptions{});
  AS_REQUIRE(server.start().ok());
  const AgentDescriptor descriptor = make_agent(1, 1, 2);
  AgentClientOptions options = agent_options(server.port().value_or(0), descriptor);
  options.limits.max_capabilities_per_agent = 1024;  // more than the coordinator accepts
  std::vector<std::pair<std::string, std::uint32_t>> capabilities;
  for (int index = 0; index < 257; ++index) {
    capabilities.emplace_back("cap-" + std::to_string(1000 + index), 900);
  }
  options.profile = make_profile(descriptor, capabilities);
  AgentClient client(options);
  auto outcome = std::async(std::launch::async, [&client]() { return client.connect_and_register(); });
  const bool answered = outcome.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
  if (!answered) {
    client.stop();  // unblock the waiting request
  }
  const MutationResult result = outcome.get();
  AS_CHECK(answered);
  AS_CHECK(!result.ok());
  client.stop();
  AS_REQUIRE(server.stop().ok());
}

AS_TEST(regression, the_epoch_advance_is_durable_before_the_coordinator_serves) {
  ScratchDirectory scratch("regression_epoch");
  const std::filesystem::path path = scratch.file("state.asstate");
  {
    AgentScheduler first(test_options());
    AS_REQUIRE(register_full(first, make_agent(1, 1, 2), {{"coding", 900}}).ok());
    AS_REQUIRE(first.save(path).ok());
  }
  AgentScheduler scheduler(stopped(test_options()));
  CoordinatorOptions coordinator_options;
  coordinator_options.persistence_path = path;
  CoordinatorServer server(scheduler, coordinator_options);
  AS_REQUIRE(server.start().ok());
  const CoordinatorEpoch serving = scheduler.coordinator_epoch();
  const SchedulerEpoch serving_scheduler = scheduler.scheduler_epoch();

  AgentScheduler reader(stopped(test_options()));
  const RecoveryResult on_disk = reader.load(path);
  AS_REQUIRE(on_disk.ok());
  // A restart from this file must never reuse the epochs this coordinator serves with.
  AS_CHECK(on_disk.new_coordinator_epoch == serving);
  AS_CHECK(on_disk.new_scheduler_epoch.value() > serving_scheduler.value());
  AS_REQUIRE(server.stop().ok());
}
