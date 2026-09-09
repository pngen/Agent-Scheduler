// Agent Scheduler — public scheduling API.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agent_scheduler/adapters.hpp"
#include "agent_scheduler/agent.hpp"
#include "agent_scheduler/assignment.hpp"
#include "agent_scheduler/clock.hpp"
#include "agent_scheduler/explanation.hpp"
#include "agent_scheduler/ids.hpp"
#include "agent_scheduler/limits.hpp"
#include "agent_scheduler/persistence.hpp"
#include "agent_scheduler/policy.hpp"
#include "agent_scheduler/result.hpp"
#include "agent_scheduler/snapshot.hpp"
#include "agent_scheduler/work.hpp"

namespace agent_scheduler {

/// Deterministic interleaving points. The race suite installs an interlock to hold a
/// mutating operation between its decision phase and its commit phase, so that races
/// are exercised by barriers rather than by timing.
enum class InterlockPoint : std::uint8_t {
  BeforeWorkAdmissionCommit = 0,
  BeforeAgentRegistrationCommit = 1,
  BeforeScheduleCommit = 2,
  BeforeDispatchHandoff = 3,
  BeforeReleaseCommit = 4,
  BeforeCancellationCommit = 5,
  BeforeRecoveryCommit = 6,
  BeforePolicyChangeCommit = 7,
  BeforeLossCommit = 8,
};

[[nodiscard]] const char* to_string(InterlockPoint value) noexcept;

/// Test seam for deterministic race coverage. The default implementation does nothing.
/// Implementations must not call back into the scheduler that invoked them.
class ScheduleInterlock {
 public:
  virtual ~ScheduleInterlock() = default;
  virtual void at(InterlockPoint point) { (void)point; }
};

struct SchedulerOptions {
  SchedulerId scheduler_id{};
  ResourceLimits limits{};
  PolicySnapshot policy{};
  std::shared_ptr<Clock> clock;
  std::shared_ptr<ExternalFeasibilityProvider> feasibility;
  std::shared_ptr<ScheduleInterlock> interlock;
  /// Starts the scheduler immediately (admission open, coordinator epoch advanced).
  bool auto_start{true};
  /// Default path used by save()/load() when the caller passes an empty path.
  std::filesystem::path persistence_path;
};

/// One scheduling pass request.
struct ScheduleRequest {
  /// Empty means every admissible work item, in canonical fairness order.
  std::vector<WorkId> work;
  /// Empty means every queue.
  std::vector<QueueId> queues;
  std::uint32_t max_assignments{1};
  bool dry_run{false};
};

/// Complete, inspectable result of one scheduling decision.
struct ScheduleDecision {
  ScheduleOutcome outcome{ScheduleOutcome::NoChange};
  WorkId work{};
  WorkGeneration work_generation{};
  std::size_t candidate_count{0};
  std::size_t eligible_count{0};
  /// Every examined candidate, in canonical AgentId order. Rejections are explicit.
  std::vector<CandidateEvaluation> candidates;
  /// Indices into candidates, ordered best-first among eligible candidates.
  std::vector<std::size_t> ranking_order;
  AgentId selected_agent{};
  AgentGeneration selected_agent_generation{};
  AgentBootId selected_boot{};
  std::optional<AssignmentBinding> assignment;
  std::string tie_break_reason;
  /// Ordered "outcome=count" rejection histogram.
  std::vector<std::string> rejection_summary;
  SchedulerEpoch scheduler_epoch{};
  CoordinatorEpoch coordinator_epoch{};
  PolicyGeneration policy_generation{};
  std::string detail;

  [[nodiscard]] bool assigned() const noexcept { return outcome == ScheduleOutcome::Assigned; }
};

struct BatchDecision {
  std::vector<ScheduleDecision> decisions;
  std::size_t examined{0};
  std::size_t assigned{0};
  std::size_t deferred{0};
  std::size_t rejected{0};
  ScheduleOutcome outcome{ScheduleOutcome::NoChange};
};

/// Result of pre-dispatch revalidation plus handoff preparation. The handoff binding is
/// the only authority the transport may transmit.
struct DispatchResult {
  ScheduleOutcome outcome{ScheduleOutcome::NoChange};
  std::optional<AssignmentBinding> handoff;
  std::string detail;

  [[nodiscard]] bool dispatched() const noexcept { return outcome == ScheduleOutcome::Assigned; }
};

struct RevalidationReport {
  std::size_t examined{0};
  std::size_t promoted{0};
  std::size_t invalidated{0};
  std::size_t work_returned{0};
  std::vector<std::string> details;
};

/// Invariant identifiers. Stable strings used by tests, tools, and inspection output.
namespace invariant {
inline constexpr std::string_view single_scheduler_epoch = "single_scheduler_epoch";
inline constexpr std::string_view exclusive_assignment_uniqueness = "exclusive_assignment_uniqueness";
inline constexpr std::string_view assignment_references_known_work = "assignment_references_known_work";
inline constexpr std::string_view assignment_references_known_agent = "assignment_references_known_agent";
inline constexpr std::string_view dispatchable_boot_current = "dispatchable_boot_current";
inline constexpr std::string_view fenced_boot_has_no_authority = "fenced_boot_has_no_authority";
inline constexpr std::string_view assignment_generation_monotonic = "assignment_generation_monotonic";
inline constexpr std::string_view work_generation_monotonic = "work_generation_monotonic";
inline constexpr std::string_view agent_generation_monotonic = "agent_generation_monotonic";
inline constexpr std::string_view queue_accounting_agrees = "queue_accounting_agrees";
inline constexpr std::string_view agent_accounting_agrees = "agent_accounting_agrees";
inline constexpr std::string_view capacity_not_exceeded = "capacity_not_exceeded";
inline constexpr std::string_view no_dispatchable_cancelled_work = "no_dispatchable_cancelled_work";
inline constexpr std::string_view no_work_for_retired_agent = "no_work_for_retired_agent";
inline constexpr std::string_view no_stale_authority_dispatchable = "no_stale_authority_dispatchable";
inline constexpr std::string_view recovered_evidence_not_current = "recovered_evidence_not_current";
inline constexpr std::string_view parallel_fanout_bounded = "parallel_fanout_bounded";
inline constexpr std::string_view exclusive_work_single_assignment = "exclusive_work_single_assignment";
inline constexpr std::string_view lease_does_not_resurrect = "lease_does_not_resurrect";
inline constexpr std::string_view index_matches_canonical = "index_matches_canonical";
inline constexpr std::string_view digest_matches_canonical = "digest_matches_canonical";
inline constexpr std::string_view counters_never_negative = "counters_never_negative";
inline constexpr std::string_view shutdown_blocks_new_assignment = "shutdown_blocks_new_assignment";
inline constexpr std::string_view active_assignment_has_current_lease = "active_assignment_has_current_lease";
inline constexpr std::string_view work_lifecycle_agrees = "work_lifecycle_agrees";
}  // namespace invariant

struct InvariantViolation {
  std::string code;
  std::string subject;
  std::string detail;
};

struct InvariantReport {
  std::vector<InvariantViolation> violations;
  std::size_t checks_run{0};
  std::uint64_t state_revision{0};

  [[nodiscard]] bool ok() const noexcept { return violations.empty(); }
  [[nodiscard]] std::string describe() const;
};

/// Schedules persistent autonomous workers. Owns scheduling authority only.
///
/// Concurrency: every public method is safe to call concurrently from any thread. The
/// authoritative state lock is never held across socket I/O, filesystem I/O, thread
/// joins, external callbacks, or the interlock.
class AgentScheduler {
 public:
  /// Opaque implementation handle. Defined only in an internal translation unit;
  /// no representation detail is exposed through this forward declaration.
  struct Impl;

  explicit AgentScheduler(SchedulerOptions options = {});
  ~AgentScheduler();

  AgentScheduler(const AgentScheduler&) = delete;
  AgentScheduler& operator=(const AgentScheduler&) = delete;
  AgentScheduler(AgentScheduler&&) = delete;
  AgentScheduler& operator=(AgentScheduler&&) = delete;

  // ---- identity and lifecycle ------------------------------------------------

  [[nodiscard]] SchedulerId id() const noexcept;
  [[nodiscard]] SchedulerEpoch scheduler_epoch() const noexcept;
  [[nodiscard]] CoordinatorEpoch coordinator_epoch() const noexcept;
  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] const ResourceLimits& limits() const noexcept;

  /// Opens admission and advances process authority (CoordinatorEpoch). Idempotent.
  MutationResult start();
  /// Closes admission, invalidates undispatched dispatch authority, and marks shutdown.
  /// Idempotent. Committed state-changing operations complete safely.
  MutationResult shutdown();
  /// Notified once when shutdown closes sessions. Invoked without any scheduler lock.
  void set_shutdown_notifier(std::function<void()> notifier);

  // ---- policy ----------------------------------------------------------------

  MutationResult set_policy(const PolicySnapshot& policy);
  [[nodiscard]] PolicySnapshot policy() const;

  // ---- queues ----------------------------------------------------------------

  MutationResult register_queue(QueueId queue,
                                QueueGeneration generation,
                                std::string fairness_class,
                                std::uint32_t weight);

  // ---- agent lifecycle -------------------------------------------------------

  MutationResult register_agent(const AgentDescriptor& descriptor);
  MutationResult publish_capabilities(AgentId agent, AgentBootId boot, const CapabilityProfile& profile);
  MutationResult publish_health(AgentId agent, AgentBootId boot, const HealthObservation& observation);
  MutationResult publish_availability(AgentId agent,
                                      AgentBootId boot,
                                      const AvailabilityObservation& observation);
  MutationResult publish_load(AgentId agent, AgentBootId boot, const LoadObservation& observation);
  MutationResult heartbeat(AgentId agent, AgentBootId boot, AgentRegistrationGeneration registration_generation);
  MutationResult drain_agent(AgentId agent, AgentBootId boot, bool force = false);
  MutationResult deregister_agent(AgentId agent, AgentBootId boot, bool force = false);
  /// Declares an incarnation lost. Fences the boot, invalidates its assignments, and
  /// returns affected work to admission. Never affects other agents.
  MutationResult declare_agent_lost(AgentId agent, AgentBootId boot, std::string reason);

  // ---- work lifecycle --------------------------------------------------------

  MutationResult submit_work(const WorkRequest& request);
  MutationResult cancel_work(WorkId work, WorkGeneration generation, std::string reason);
  MutationResult supersede_work(WorkId work,
                                WorkGeneration expected_generation,
                                const WorkRequest& replacement);
  MutationResult expire_deadlines();

  // ---- scheduling ------------------------------------------------------------

  ScheduleDecision schedule(WorkId work, WorkGeneration generation);
  BatchDecision schedule_batch(const ScheduleRequest& request);
  DispatchResult dispatch(AssignmentId assignment, AssignmentGeneration generation);

  MutationResult acknowledge(AssignmentId assignment,
                             AssignmentGeneration generation,
                             DispatchId dispatch_id,
                             DispatchGeneration dispatch_generation,
                             AgentBootId boot);
  MutationResult mark_executing(AssignmentId assignment, AssignmentGeneration generation, AgentBootId boot);
  MutationResult release(AssignmentId assignment,
                         AssignmentGeneration generation,
                         AgentBootId boot,
                         std::string reason);
  MutationResult complete_assignment(AssignmentId assignment,
                                     AssignmentGeneration generation,
                                     AgentBootId boot);
  MutationResult fail_assignment(AssignmentId assignment,
                                 AssignmentGeneration generation,
                                 AgentBootId boot,
                                 std::string reason);
  MutationResult reject_assignment(AssignmentId assignment,
                                   AssignmentGeneration generation,
                                   AgentBootId boot,
                                   std::string reason);

  /// Re-checks every assignment that requires revalidation against current authority.
  RevalidationReport revalidate_assignments();
  RevalidationReport revalidate_agent_assignments(AgentId agent, AgentBootId boot);
  MutationResult expire_leases();

  // ---- inspection ------------------------------------------------------------

  [[nodiscard]] SchedulerSummary summary() const;
  [[nodiscard]] SchedulerSnapshot snapshot() const;
  [[nodiscard]] std::optional<AgentStatus> agent_status(AgentId agent) const;
  [[nodiscard]] std::optional<WorkStatus> work_status(WorkId work) const;
  [[nodiscard]] std::optional<Assignment> assignment(AssignmentId assignment) const;
  [[nodiscard]] std::vector<Assignment> assignments_for_work(WorkId work) const;
  [[nodiscard]] std::vector<Assignment> assignments_for_agent(AgentId agent) const;
  [[nodiscard]] std::vector<Assignment> all_assignments() const;
  [[nodiscard]] std::optional<Lease> lease(LeaseId lease_id) const;
  [[nodiscard]] std::vector<Lease> leases() const;
  [[nodiscard]] std::vector<FencedBoot> fenced_boots() const;
  [[nodiscard]] FairnessSnapshot fairness() const;
  [[nodiscard]] InvariantReport check_invariants() const;
  [[nodiscard]] Digest256 state_digest() const;
  /// Deterministic digest of every agent incarnation generation in canonical order.
  [[nodiscard]] Digest256 agent_generation_digest() const;
  [[nodiscard]] std::vector<AgentStatus> all_agents() const;
  [[nodiscard]] std::vector<WorkStatus> all_work() const;

  // ---- persistence -----------------------------------------------------------

  MutationResult save(const std::filesystem::path& path, const PersistOptions& options = {}) const;
  RecoveryResult load(const std::filesystem::path& path, const LoadOptions& options = {});

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace agent_scheduler
