// Agent Scheduler — capability evidence, provenance, and generations.
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

/// Capability evidence classes. UNKNOWN is first-class and never satisfies a hard
/// requirement. Declared evidence is an unverified claim; Observed and Verified are
/// backed by a current incarnation. Revoked and Stale never authorize work.
enum class CapabilityState : std::uint8_t {
  Unknown = 0,
  Declared = 1,
  Observed = 2,
  Verified = 3,
  Revoked = 4,
  Stale = 5,
};

[[nodiscard]] const char* to_string(CapabilityState state) noexcept;

/// Comparable strength of a capability state. Revoked and Stale rank below Declared
/// so that a minimum-state predicate can never be satisfied by a dead claim.
[[nodiscard]] int capability_state_rank(CapabilityState state) noexcept;

/// True when the state may participate in a hard requirement.
[[nodiscard]] bool capability_state_is_usable(CapabilityState state) noexcept;

/// A single capability assertion. Names are free-form and namespaced by convention
/// ("coding", "tool:shell", "model:claude", "accelerator:cuda", "platform:windows"),
/// so heterogeneous worker families are supported without hard-coding a taxonomy.
struct CapabilityEvidence {
  std::string name;
  CapabilityState state{CapabilityState::Unknown};
  /// 0..1000 completeness/quality of the evidence. Ranking input only.
  std::uint32_t quality{0};
  AgentCapabilityGeneration generation{};
  /// Provenance: the incarnation that produced this evidence.
  AgentBootId boot{};
  std::uint64_t observed_at_ms{0};
  std::string provenance;

  friend bool operator==(const CapabilityEvidence&, const CapabilityEvidence&) = default;
};

/// A generation-bound capability profile published by one incarnation.
struct CapabilityProfile {
  CapabilityProfileId profile_id{};
  AgentCapabilityGeneration generation{};
  AgentBootId boot{};
  std::vector<CapabilityEvidence> capabilities;
};

/// How a work item treats a capability name.
enum class CapabilityRequirementMode : std::uint8_t { Required = 0, Preferred = 1 };

struct CapabilityRequirement {
  std::string name;
  CapabilityRequirementMode mode{CapabilityRequirementMode::Required};
  /// Minimum acceptable evidence state. Default Observed: a bare claim is not enough.
  CapabilityState minimum_state{CapabilityState::Observed};
  std::uint32_t minimum_quality{0};

  friend bool operator==(const CapabilityRequirement&, const CapabilityRequirement&) = default;
};

/// Sorts capabilities by name and rejects duplicate names, so profiles are canonical.
[[nodiscard]] MutationResult canonicalize(CapabilityProfile& profile, const ResourceLimits& limits);

/// Validates a capability name: bounded length, restricted charset, no control bytes.
[[nodiscard]] bool is_valid_identifier(std::string_view value, std::uint32_t max_length) noexcept;

/// Looks up a capability by name in a canonicalized profile.
[[nodiscard]] const CapabilityEvidence* find_capability(const CapabilityProfile& profile,
                                                        std::string_view name) noexcept;

/// Marks every evidence entry stale and clears provenance freshness. Used by recovery:
/// evidence from a previous process must never silently become current.
void invalidate_evidence(CapabilityProfile& profile) noexcept;

}  // namespace agent_scheduler
