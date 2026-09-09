// Agent Scheduler — deterministic synthetic scheduler laboratory (SYNTHETIC).
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "agent_scheduler/scheduler.hpp"

namespace agent_scheduler {

/// Deterministic 64-bit generator (SplitMix64). Reproducible from an explicit seed.
class SyntheticRandom {
 public:
  explicit SyntheticRandom(std::uint64_t seed) noexcept : state_(seed) {}
  [[nodiscard]] std::uint64_t next() noexcept;
  [[nodiscard]] std::uint32_t next_bounded(std::uint32_t bound) noexcept;
  [[nodiscard]] bool next_percent(std::uint32_t percent) noexcept;
  [[nodiscard]] double next_unit() noexcept;
  void reseed(std::uint64_t seed) noexcept { state_ = seed; }

 private:
  std::uint64_t state_;
};

/// Configuration of a synthetic population. Everything here is SYNTHETIC: it describes
/// logical profiles, not physically present hardware or remote nodes.
struct SyntheticConfig {
  std::uint64_t seed{1};
  std::uint32_t agent_count{8};
  std::uint32_t queue_count{4};
  std::uint32_t tenant_count{2};
  std::uint32_t work_count{64};
  std::uint32_t capability_density{4};
  std::uint32_t min_concurrency{1};
  std::uint32_t max_concurrency{4};
  std::uint32_t priority_levels{4};
  std::uint32_t placement_domains{2};
  std::uint32_t policy_generations{1};
  std::uint32_t agent_death_percent{0};
  std::uint32_t reincarnation_percent{0};
  std::uint32_t capability_churn_percent{0};
  std::uint32_t stale_evidence_percent{0};
  std::uint32_t health_degradation_percent{0};
  std::uint32_t availability_change_percent{0};
  std::uint32_t affinity_percent{0};
  std::uint32_t anti_affinity_percent{0};
  std::uint32_t infeasible_resource_percent{0};
  std::uint32_t infeasible_budget_percent{0};
  std::uint32_t infeasible_slo_percent{0};
  std::uint32_t arrival_burst{1};
  bool require_external_feasibility{false};
  /// Number of capability names in the synthetic namespace.
  std::uint32_t capability_pool{12};
};

struct SyntheticPopulation {
  std::vector<QueueId> queues;
  std::vector<TenantId> tenants;
  std::vector<AgentDescriptor> agents;
  std::vector<CapabilityProfile> profiles;
  std::vector<WorkRequest> work;
  PolicySnapshot policy;
  SyntheticConfig config;
  /// Capability names assigned to each agent, index-aligned with agents.
  std::vector<std::vector<std::string>> agent_capabilities;
};

/// Deterministic generator plus driver. All results are reproducible from the seed.
class SyntheticLaboratory {
 public:
  explicit SyntheticLaboratory(SyntheticConfig config) noexcept;

  [[nodiscard]] const SyntheticConfig& config() const noexcept { return config_; }
  [[nodiscard]] const PolicySnapshot& policy() const noexcept { return policy_; }
  [[nodiscard]] SyntheticPopulation generate() const;

  struct RunReport {
    std::size_t agents_registered{0};
    std::size_t work_admitted{0};
    std::size_t assignments{0};
    std::size_t deferred{0};
    std::size_t rejected{0};
    std::size_t starved{0};
    std::size_t agent_deaths{0};
    std::size_t reincarnations{0};
    std::uint64_t rounds{0};
    Digest256 final_state_digest{};
    InvariantReport invariants;
  };

  /// Registers the synthetic population, submits its work, and drives scheduling for
  /// the requested number of rounds. Deterministic for a given seed and round count.
  RunReport run(AgentScheduler& scheduler, std::uint32_t rounds) const;

  /// Capability name for a pool index, stable for a given configuration.
  [[nodiscard]] static std::string capability_name(std::uint32_t index);

 private:
  SyntheticConfig config_;
  PolicySnapshot policy_;
};

}  // namespace agent_scheduler
