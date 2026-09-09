// Agent Scheduler — strongly typed identity and generation values.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <compare>
#include <concepts>
#include <cstdint>
#include <string>
#include <type_traits>

#include "agent_scheduler/digest.hpp"

namespace agent_scheduler {

/// A strongly typed identifier or generation. Tag prevents accidental interchange
/// between identity kinds; Rep is either a 64-bit counter or a derived 128-bit value.
template <class Tag, class Rep = std::uint64_t>
class StrongId {
 public:
  using rep_type = Rep;
  using tag_type = Tag;

  constexpr StrongId() noexcept = default;
  constexpr explicit StrongId(Rep value) noexcept : value_(value) {}

  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }

  [[nodiscard]] constexpr bool is_zero() const noexcept {
    if constexpr (std::is_integral_v<Rep>) {
      return value_ == 0;
    } else {
      return value_.is_zero();
    }
  }

  /// Prefixed identifier text: 16 hex digits for 64-bit values, 32 for derived 128-bit values.
  [[nodiscard]] std::string to_string() const {
    if constexpr (std::is_integral_v<Rep>) {
      static constexpr char kHex[] = "0123456789abcdef";
      std::string out(16, '0');
      std::uint64_t v = static_cast<std::uint64_t>(value_);
      for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kHex[v & 0xFu];
        v >>= 4;
      }
      return out;
    } else {
      return value_.to_string();
    }
  }

  friend constexpr bool operator==(const StrongId&, const StrongId&) noexcept = default;
  friend constexpr auto operator<=>(const StrongId&, const StrongId&) noexcept = default;

 private:
  Rep value_{};
};

/// Declares a distinct identity kind.
#define AGENT_SCHEDULER_DECLARE_ID(name) \
  struct name##Tag {};                   \
  using name = StrongId<name##Tag>

#define AGENT_SCHEDULER_DECLARE_DERIVED_ID(name) \
  struct name##Tag {};                           \
  using name = StrongId<name##Tag, Hash128>

// Scheduler and process authority.
AGENT_SCHEDULER_DECLARE_ID(SchedulerId);
AGENT_SCHEDULER_DECLARE_ID(SchedulerEpoch);
AGENT_SCHEDULER_DECLARE_ID(CoordinatorEpoch);

// Persistent agent identity, incarnation, and evidence generations.
AGENT_SCHEDULER_DECLARE_ID(AgentId);
AGENT_SCHEDULER_DECLARE_ID(AgentGeneration);
AGENT_SCHEDULER_DECLARE_DERIVED_ID(AgentBootId);
AGENT_SCHEDULER_DECLARE_ID(AgentRegistrationGeneration);
AGENT_SCHEDULER_DECLARE_ID(AgentCapabilityGeneration);
AGENT_SCHEDULER_DECLARE_ID(AgentHealthGeneration);
AGENT_SCHEDULER_DECLARE_ID(AgentAvailabilityGeneration);
AGENT_SCHEDULER_DECLARE_ID(AgentLoadGeneration);

// Work, queues, and dispatch correlation.
AGENT_SCHEDULER_DECLARE_ID(WorkId);
AGENT_SCHEDULER_DECLARE_ID(WorkGeneration);
AGENT_SCHEDULER_DECLARE_DERIVED_ID(WorkAttemptId);
AGENT_SCHEDULER_DECLARE_ID(QueueId);
AGENT_SCHEDULER_DECLARE_ID(QueueGeneration);

// Assignment authority.
AGENT_SCHEDULER_DECLARE_DERIVED_ID(AssignmentId);
AGENT_SCHEDULER_DECLARE_ID(AssignmentGeneration);
AGENT_SCHEDULER_DECLARE_DERIVED_ID(DispatchId);
AGENT_SCHEDULER_DECLARE_ID(DispatchGeneration);
AGENT_SCHEDULER_DECLARE_DERIVED_ID(LeaseId);
AGENT_SCHEDULER_DECLARE_ID(LeaseGeneration);

// Policy and capability authority.
AGENT_SCHEDULER_DECLARE_ID(PolicyId);
AGENT_SCHEDULER_DECLARE_ID(PolicyGeneration);
AGENT_SCHEDULER_DECLARE_ID(CapabilityProfileId);

// Externally supplied authority generations and scope.
AGENT_SCHEDULER_DECLARE_ID(PlacementDomainId);
AGENT_SCHEDULER_DECLARE_ID(TopologyEpoch);
AGENT_SCHEDULER_DECLARE_ID(BudgetGeneration);
AGENT_SCHEDULER_DECLARE_ID(SloGeneration);
AGENT_SCHEDULER_DECLARE_ID(ResourceGeneration);
AGENT_SCHEDULER_DECLARE_ID(ReservationGeneration);
AGENT_SCHEDULER_DECLARE_ID(TenantId);
AGENT_SCHEDULER_DECLARE_ID(NamespaceId);

// Operation correlation.
AGENT_SCHEDULER_DECLARE_ID(OperationId);
AGENT_SCHEDULER_DECLARE_ID(RequestId);

#undef AGENT_SCHEDULER_DECLARE_ID
#undef AGENT_SCHEDULER_DECLARE_DERIVED_ID

/// Concept matching any StrongId instantiation.
template <class T>
concept AnyStrongId = requires(const T& v) {
  typename T::rep_type;
  typename T::tag_type;
  { v.is_zero() } -> std::convertible_to<bool>;
};

}  // namespace agent_scheduler
