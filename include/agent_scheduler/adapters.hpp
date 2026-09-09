// Agent Scheduler — narrow external evidence adapters.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "agent_scheduler/ids.hpp"
#include "agent_scheduler/work.hpp"

namespace agent_scheduler {

/// Request presented to an external feasibility authority. The scheduler asks a single
/// narrow question and consumes the verdict; it never reproduces resource, cost, SLO,
/// capacity, or reservation modelling.
struct ExternalFeasibilityRequest {
  WorkId work{};
  WorkGeneration work_generation{};
  TenantId tenant{};
  NamespaceId name_space{};
  ResourceGeneration resource_generation{};
  BudgetGeneration budget_generation{};
  SloGeneration slo_generation{};
  ReservationGeneration reservation_generation{};
  std::uint64_t deadline_ms{0};
};

/// Verdicts returned by an external authority. UNKNOWN means the authority could not
/// answer; it never satisfies a hard requirement.
struct ExternalFeasibilityResult {
  FeasibilityVerdict resource{FeasibilityVerdict::Unknown};
  FeasibilityVerdict budget{FeasibilityVerdict::Unknown};
  FeasibilityVerdict slo{FeasibilityVerdict::Unknown};
  FeasibilityVerdict reservation{FeasibilityVerdict::Unknown};
  std::string detail;
};

/// Replaceable adapter for Resource Broker / Cost Governor / SLO Fabric / Reservation
/// Fabric / Capacity Fabric evidence. Implementations must be thread-safe.
class ExternalFeasibilityProvider {
 public:
  virtual ~ExternalFeasibilityProvider() = default;
  [[nodiscard]] virtual ExternalFeasibilityResult evaluate(const ExternalFeasibilityRequest& request) = 0;
};

/// A provider that answers UNKNOWN for everything. Used when no adjacent runtime is
/// present: work items that do not require external feasibility still schedule normally.
class NullFeasibilityProvider final : public ExternalFeasibilityProvider {
 public:
  [[nodiscard]] ExternalFeasibilityResult evaluate(const ExternalFeasibilityRequest& request) override;
};

/// A deterministic provider driven by explicit generation-keyed verdicts. Intended for
/// tests, the synthetic laboratory, and integration harnesses.
class ScriptedFeasibilityProvider final : public ExternalFeasibilityProvider {
 public:
  void set_resource(ResourceGeneration generation, FeasibilityVerdict verdict);
  void set_budget(BudgetGeneration generation, FeasibilityVerdict verdict);
  void set_slo(SloGeneration generation, FeasibilityVerdict verdict);
  void set_reservation(ReservationGeneration generation, FeasibilityVerdict verdict);
  void set_detail(std::string detail);

  [[nodiscard]] ExternalFeasibilityResult evaluate(const ExternalFeasibilityRequest& request) override;

 private:
  std::mutex mutex_;
  std::map<std::uint64_t, FeasibilityVerdict> resource_;
  std::map<std::uint64_t, FeasibilityVerdict> budget_;
  std::map<std::uint64_t, FeasibilityVerdict> slo_;
  std::map<std::uint64_t, FeasibilityVerdict> reservation_;
  std::string detail_;
};

}  // namespace agent_scheduler
