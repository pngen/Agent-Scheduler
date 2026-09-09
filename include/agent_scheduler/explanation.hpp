// Agent Scheduler — deterministic scheduling explanations.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "agent_scheduler/ids.hpp"
#include "agent_scheduler/result.hpp"

namespace agent_scheduler {

/// UNKNOWN factors stay explicit. An unknown factor contributes zero, so it can never
/// improve a candidate's rank.
enum class FactorStatus : std::uint8_t { Known = 0, Unknown = 1 };
[[nodiscard]] const char* to_string(FactorStatus value) noexcept;

struct FactorScore {
  std::string name;
  FactorStatus status{FactorStatus::Unknown};
  /// Raw factor value in its natural unit (milliseconds, counts, or 0..1000 quality).
  std::int64_t raw_value{0};
  /// Normalized contribution in [0,1000]. Zero when the factor is unknown.
  std::int64_t normalized_value{0};
  std::uint32_t weight{0};
  std::string note;
};

/// Evaluation of one candidate agent for one work item. Ineligible candidates carry a
/// specific rejection outcome and never carry a score.
struct CandidateEvaluation {
  AgentId agent{};
  AgentGeneration agent_generation{};
  AgentBootId boot{};
  bool eligible{false};
  ScheduleOutcome rejection{ScheduleOutcome::NoChange};
  std::string rejection_detail;
  std::vector<FactorScore> factors;
  /// Weighted score over all configured weights. Unknown factors contribute zero.
  std::int64_t score{0};
  std::uint32_t known_weight{0};
  std::uint32_t total_weight{0};
  std::uint32_t active_assignments{0};
  std::uint32_t available_capacity{0};
  /// One-based rank among eligible candidates; zero when ineligible.
  std::uint32_t rank{0};
  bool selected{false};
  bool tie_break_decided{false};
  std::string tie_break_reason;
};

/// Fairness accounting for one admitted work item.
struct FairnessEntry {
  WorkId work{};
  QueueId queue{};
  TenantId tenant{};
  std::string fairness_class;
  std::uint32_t priority{0};
  std::int64_t effective_priority{0};
  std::uint32_t bypass_count{0};
  std::uint32_t bypass_ceiling{0};
  std::uint32_t starvation_rounds{0};
  std::uint64_t admitted_sequence{0};
  bool starved{false};
  bool must_run{false};
};

struct FairnessSnapshot {
  std::vector<FairnessEntry> entries;
  std::uint32_t starved_count{0};
  std::uint32_t must_run_count{0};
  std::uint32_t max_bypass_observed{0};
  std::uint64_t scheduling_rounds{0};
  std::uint64_t aging_rounds_per_step{0};
  std::uint32_t max_priority_bypass{0};
  bool bounded_starvation{true};
};

}  // namespace agent_scheduler
