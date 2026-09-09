// Agent Scheduler — persistent autonomous worker model.
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

/// Explicit agent lifecycle. READY must be earned by publishing current evidence for
/// the current incarnation; being connected is not proof of readiness.
enum class AgentLifecycle : std::uint8_t {
  Unregistered = 0,
  Registering = 1,
  Ready = 2,
  Busy = 3,
  Draining = 4,
  Unavailable = 5,
  RevalidationRequired = 6,
  Lost = 7,
  Retired = 8,
};

enum class AgentHealth : std::uint8_t { Unknown = 0, Healthy = 1, Degraded = 2, Unhealthy = 3 };
enum class AgentReadiness : std::uint8_t { Unknown = 0, Ready = 1, NotReady = 2 };
enum class AgentReachability : std::uint8_t { Unknown = 0, Reachable = 1, Unreachable = 2 };
enum class AgentAvailability : std::uint8_t { Unknown = 0, Available = 1, Unavailable = 2 };

[[nodiscard]] const char* to_string(AgentLifecycle value) noexcept;
[[nodiscard]] const char* to_string(AgentHealth value) noexcept;
[[nodiscard]] const char* to_string(AgentReadiness value) noexcept;
[[nodiscard]] const char* to_string(AgentReachability value) noexcept;
[[nodiscard]] const char* to_string(AgentAvailability value) noexcept;

/// True when the lifecycle admits new work under ordinary policy.
[[nodiscard]] bool lifecycle_accepts_new_work(AgentLifecycle value) noexcept;
/// True when the lifecycle is terminal for scheduling authority.
[[nodiscard]] bool lifecycle_is_terminal(AgentLifecycle value) noexcept;

/// Registration input. Identity, incarnation, and scope only; evidence is published
/// separately and is always generation-bound to the incarnation.
struct AgentDescriptor {
  AgentId id{};
  AgentGeneration generation{};
  AgentBootId boot{};
  AgentRegistrationGeneration registration_generation{};
  TenantId tenant{};
  NamespaceId name_space{};
  std::string display_name;
  std::vector<std::string> policy_labels;
  PlacementDomainId placement_domain{};
  TopologyEpoch topology_epoch{};
  /// Declared maximum concurrent assignments this worker accepts.
  std::uint32_t max_concurrency{1};
  /// Registration/session lease duration. Zero selects the policy default.
  std::uint64_t lease_ttl_ms{0};
  std::string provenance;
};

/// Dynamic load evidence reported by an incarnation. Advisory only: canonical capacity
/// accounting always comes from assignment state, never from a report.
struct LoadObservation {
  AgentLoadGeneration generation{};
  std::uint32_t reported_active_assignments{0};
  std::uint32_t max_concurrency{0};
  std::uint32_t queue_depth{0};
  std::uint32_t cpu_pressure_percent{0};
  std::uint64_t estimated_dispatch_latency_ms{0};
  /// Externally supplied cost index. Must be finite and non-negative.
  double cost_index{0.0};
  /// Externally supplied remaining SLO headroom fraction in [0,1]. Must be finite.
  double slo_headroom_fraction{0.0};
  std::vector<std::string> warm_state_keys;
  std::uint64_t observed_at_ms{0};
};

struct HealthObservation {
  AgentHealthGeneration generation{};
  AgentHealth health{AgentHealth::Unknown};
  /// 0..1000 quality of the reported health.
  std::uint32_t health_quality{0};
  std::string detail;
  std::uint64_t observed_at_ms{0};
};

struct AvailabilityObservation {
  AgentAvailabilityGeneration generation{};
  AgentAvailability availability{AgentAvailability::Unknown};
  AgentReachability reachability{AgentReachability::Unknown};
  AgentReadiness readiness{AgentReadiness::Unknown};
  std::uint64_t observed_at_ms{0};
};

/// Point-in-time agent inspection result. Distinguishes every dimension explicitly.
struct AgentStatus {
  AgentId id{};
  AgentGeneration generation{};
  AgentBootId boot{};
  AgentRegistrationGeneration registration_generation{};
  AgentLifecycle lifecycle{AgentLifecycle::Unregistered};
  AgentHealth health{AgentHealth::Unknown};
  AgentReadiness readiness{AgentReadiness::Unknown};
  AgentReachability reachability{AgentReachability::Unknown};
  AgentAvailability availability{AgentAvailability::Unknown};
  AgentHealthGeneration health_generation{};
  AgentAvailabilityGeneration availability_generation{};
  AgentLoadGeneration load_generation{};
  AgentCapabilityGeneration capability_generation{};
  TenantId tenant{};
  NamespaceId name_space{};
  std::string display_name;
  std::vector<std::string> policy_labels;
  PlacementDomainId placement_domain{};
  TopologyEpoch topology_epoch{};
  std::uint32_t max_concurrency{0};
  std::uint32_t active_assignments{0};
  std::uint32_t available_capacity{0};
  std::uint32_t reported_active_assignments{0};
  std::uint64_t lease_expires_at_ms{0};
  bool lease_current{false};
  bool evidence_current{false};
  std::uint64_t last_heartbeat_ms{0};
  std::uint64_t registered_at_ms{0};
  std::vector<CapabilityEvidence> capabilities;
  std::string reason;
};

/// Validates a descriptor against limits: identity non-zero, bounded text, valid labels.
[[nodiscard]] MutationResult validate(const AgentDescriptor& descriptor, const ResourceLimits& limits);
[[nodiscard]] MutationResult validate(const LoadObservation& observation, const ResourceLimits& limits);
[[nodiscard]] MutationResult validate(const HealthObservation& observation, const ResourceLimits& limits);
[[nodiscard]] MutationResult validate(const AvailabilityObservation& observation,
                                     const ResourceLimits& limits);

}  // namespace agent_scheduler
