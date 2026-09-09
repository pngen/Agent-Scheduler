// Agent Scheduler — schedulable work model.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "agent_scheduler/capability.hpp"
#include "agent_scheduler/ids.hpp"
#include "agent_scheduler/limits.hpp"
#include "agent_scheduler/result.hpp"

namespace agent_scheduler {

enum class WorkLifecycle : std::uint8_t {
  Unknown = 0,
  Admitted = 1,
  Assigned = 2,
  Executing = 3,
  Completed = 4,
  Failed = 5,
  Cancelled = 6,
  Superseded = 7,
  Expired = 8,
};

[[nodiscard]] const char* to_string(WorkLifecycle value) noexcept;
[[nodiscard]] bool work_accepts_assignment(WorkLifecycle value) noexcept;
[[nodiscard]] bool work_is_terminal(WorkLifecycle value) noexcept;

/// Exclusive: exactly one authoritative scheduler assignment per work generation.
/// Parallel: explicit bounded fan-out declared by policy, never an accidental duplicate.
enum class SchedulingMode : std::uint8_t { Exclusive = 0, Parallel = 1 };

/// External feasibility verdicts consumed through a narrow adapter. UNKNOWN never
/// satisfies a hard requirement.
enum class FeasibilityVerdict : std::uint8_t { Unknown = 0, Feasible = 1, Infeasible = 2 };
[[nodiscard]] const char* to_string(FeasibilityVerdict value) noexcept;

struct WorkRequirements {
  /// Required and preferred capabilities. Mode selects the semantics.
  std::vector<CapabilityRequirement> capabilities;
  TenantId tenant{};
  NamespaceId name_space{};
  std::vector<std::string> required_policy_labels;
  std::vector<AgentId> affinity;
  std::vector<AgentId> anti_affinity;
  bool restrict_placement_domain{false};
  PlacementDomainId placement_domain{};
  TopologyEpoch topology_epoch{};
  bool require_healthy{true};
  bool require_reachable{true};
  bool require_ready{true};
  bool require_current_lease{true};
  /// When true, externally supplied feasibility evidence must be present and Feasible.
  bool require_resource_feasibility{false};
  ResourceGeneration resource_generation{};
  FeasibilityVerdict resource_feasibility{FeasibilityVerdict::Unknown};
  bool require_budget_feasibility{false};
  BudgetGeneration budget_generation{};
  FeasibilityVerdict budget_feasibility{FeasibilityVerdict::Unknown};
  bool require_slo_feasibility{false};
  SloGeneration slo_generation{};
  FeasibilityVerdict slo_feasibility{FeasibilityVerdict::Unknown};
  bool require_reservation{false};
  ReservationGeneration reservation_generation{};
  FeasibilityVerdict reservation_feasibility{FeasibilityVerdict::Unknown};
  /// Requires an agent that reports at least one matching warm-state key.
  bool require_warm_state{false};
  std::vector<std::string> warm_state_keys;
  /// Absolute monotonic deadline in milliseconds. Zero means no deadline.
  std::uint64_t deadline_ms{0};
  /// Maximum concurrent authoritative assignments for this work generation.
  std::uint32_t max_parallel{1};
  /// Maximum concurrent assignments of this work on any single agent.
  std::uint32_t max_assignments_per_agent{1};

  friend bool operator==(const WorkRequirements&, const WorkRequirements&) = default;
};

/// Scheduling metadata for one logical work item. The payload is opaque: the scheduler
/// owns admission, eligibility, ranking, and assignment authority, not execution.
struct WorkRequest {
  WorkId id{};
  WorkGeneration generation{};
  QueueId queue{};
  QueueGeneration queue_generation{};
  /// Opaque scheduling-metadata label, bounded. Never interpreted as execution logic.
  std::string kind;
  std::uint32_t priority{0};
  std::string fairness_class{"default"};
  SchedulingMode mode{SchedulingMode::Exclusive};
  WorkRequirements requirements;
  /// Opaque external identity (for example a Workload Fabric handle). Not interpreted.
  std::string payload_ref;
  /// Absolute monotonic earliest start in milliseconds. Zero means immediately.
  std::uint64_t earliest_start_ms{0};
  std::uint64_t expected_duration_ms{0};

  friend bool operator==(const WorkRequest&, const WorkRequest&) = default;
};

struct WorkStatus {
  WorkId id{};
  WorkGeneration generation{};
  QueueId queue{};
  QueueGeneration queue_generation{};
  WorkLifecycle lifecycle{WorkLifecycle::Unknown};
  std::uint32_t priority{0};
  std::string fairness_class;
  SchedulingMode mode{SchedulingMode::Exclusive};
  std::uint32_t current_assignments{0};
  std::uint32_t max_parallel{1};
  AssignmentGeneration last_assignment_generation{};
  std::uint64_t admitted_at_ms{0};
  std::uint64_t admitted_sequence{0};
  std::uint32_t bypass_count{0};
  std::uint32_t starvation_rounds{0};
  std::uint32_t required_capabilities{0};
  std::uint32_t preferred_capabilities{0};
  std::uint64_t deadline_ms{0};
  std::string kind;
  std::string reason;
};

/// Validates a work request against limits: identity, text bounds, capability sets,
/// fan-out bounds, priority range, and duplicate requirement names.
[[nodiscard]] MutationResult validate(const WorkRequest& request, const ResourceLimits& limits);

/// Canonicalizes a request in place (sorted capabilities, labels, affinity sets).
void canonicalize(WorkRequest& request);

}  // namespace agent_scheduler
