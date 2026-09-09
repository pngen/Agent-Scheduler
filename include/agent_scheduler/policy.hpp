// Agent Scheduler — scheduling policy, fairness contract, and ranking weights.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "agent_scheduler/ids.hpp"
#include "agent_scheduler/limits.hpp"
#include "agent_scheduler/result.hpp"

namespace agent_scheduler {

/// Named ranking factors. Every factor is evaluated explicitly; none is an opaque score.
namespace ranking_factor {
inline constexpr std::string_view load_headroom = "load_headroom";
inline constexpr std::string_view remaining_capacity = "remaining_capacity";
inline constexpr std::string_view capability_quality = "capability_quality";
inline constexpr std::string_view capability_completeness = "capability_completeness";
inline constexpr std::string_view warm_state_affinity = "warm_state_affinity";
inline constexpr std::string_view locality_match = "locality_match";
inline constexpr std::string_view health_quality = "health_quality";
inline constexpr std::string_view readiness = "readiness";
inline constexpr std::string_view queue_age = "queue_age";
inline constexpr std::string_view fairness_deficit = "fairness_deficit";
inline constexpr std::string_view dispatch_latency = "dispatch_latency";
inline constexpr std::string_view cost_evidence = "cost_evidence";
inline constexpr std::string_view slo_headroom = "slo_headroom";
inline constexpr std::string_view resource_headroom = "resource_headroom";
inline constexpr std::string_view reliability_history = "reliability_history";
inline constexpr std::string_view anti_concentration = "anti_concentration";
inline constexpr std::string_view failure_domain_diversity = "failure_domain_diversity";
inline constexpr std::string_view handoff_cost = "handoff_cost";
}  // namespace ranking_factor

struct RankingWeight {
  std::string factor;
  std::uint32_t weight{0};

  friend bool operator==(const RankingWeight&, const RankingWeight&) = default;
};

struct QueueClassWeight {
  std::string fairness_class;
  std::uint32_t weight{1};

  friend bool operator==(const QueueClassWeight&, const QueueClassWeight&) = default;
};

/// Scheduling policy. A policy generation change invalidates any assignment bound to
/// the previous generation; it never silently re-authorizes an old decision.
struct PolicySnapshot {
  PolicyId id{};
  PolicyGeneration generation{};
  /// Rounds of being passed over per one priority step of aging credit.
  std::uint32_t aging_rounds_per_step{1};
  /// Hard bound on how many times an admissible item may be passed over in favour of a
  /// strictly higher-priority item before it must run.
  std::uint32_t max_priority_bypass{32};
  /// Rounds of starvation after which the item is reported as starved.
  std::uint32_t starvation_alert_rounds{256};
  /// Explicitly opt out of the bounded-starvation contract. Default is bounded.
  bool allow_unbounded_starvation{false};
  std::vector<QueueClassWeight> fairness_class_weights;
  /// Window of recent assignments used by the anti-concentration factor.
  std::uint32_t anti_concentration_window{8};
  std::uint32_t anti_concentration_penalty{200};
  /// Freshness bounds for dynamic evidence. Older evidence is not current.
  std::uint64_t capability_staleness_ms{60000};
  std::uint64_t health_staleness_ms{60000};
  std::uint64_t load_staleness_ms{60000};
  std::uint64_t availability_staleness_ms{60000};
  /// Lease durations.
  std::uint64_t assignment_lease_ttl_ms{120000};
  std::uint64_t registration_lease_ttl_ms{300000};
  /// On shutdown, invalidate assignments that have not yet been dispatched.
  bool close_dispatch_authority_on_shutdown{true};
  std::vector<RankingWeight> ranking_weights;

  friend bool operator==(const PolicySnapshot&, const PolicySnapshot&) = default;
};

/// Default ranking weights used when a policy does not override them.
[[nodiscard]] std::vector<RankingWeight> default_ranking_weights();

/// Default fairness class weights used when a policy does not override them.
[[nodiscard]] std::vector<QueueClassWeight> default_fairness_class_weights();

/// Validates a policy: known factor names, bounded text, no duplicate classes.
[[nodiscard]] MutationResult validate(const PolicySnapshot& policy, const ResourceLimits& limits);

/// Weight of a fairness class; unknown classes use the default class weight.
[[nodiscard]] std::uint32_t fairness_class_weight(const PolicySnapshot& policy,
                                                  std::string_view fairness_class) noexcept;

/// Effective bypass ceiling for a fairness class under this policy. Guarantees a hard
/// bound on starvation when allow_unbounded_starvation is false.
[[nodiscard]] std::uint32_t bypass_ceiling(const PolicySnapshot& policy,
                                           std::string_view fairness_class) noexcept;

}  // namespace agent_scheduler
