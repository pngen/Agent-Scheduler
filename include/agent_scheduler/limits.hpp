// Agent Scheduler — externally influenced growth bounds and checked arithmetic.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace agent_scheduler {

/// Every externally influenced growth dimension is bounded here. Limits are validated
/// before allocation; nothing in the runtime grows without a configured ceiling.
struct ResourceLimits {
  // Population bounds.
  std::uint32_t max_agents = 200000;
  std::uint32_t max_queues = 4096;
  std::uint32_t max_work_items = 500000;
  std::uint32_t max_active_assignments = 200000;
  std::uint32_t max_historical_assignments = 500000;
  std::uint32_t max_leases = 200000;
  std::uint32_t max_fenced_boots = 500000;
  std::uint32_t max_tenants = 8192;

  // Per-record bounds.
  std::uint32_t max_capabilities_per_agent = 256;
  std::uint32_t max_policy_labels = 64;
  std::uint32_t max_capability_requirements = 128;
  std::uint32_t max_affinity_entries = 256;
  std::uint32_t max_warm_state_keys = 64;
  std::uint32_t max_explanation_factors = 64;
  std::uint32_t max_candidates_examined = 200000;
  std::uint32_t max_retained_history = 100000;
  std::uint32_t max_batch_assignments = 100000;

  // Text and collection bounds.
  std::uint32_t max_string_size = 512;
  std::uint32_t max_identifier_size = 64;
  std::uint32_t max_frame_size = 1u << 20;      // 1 MiB
  std::uint32_t max_payload_size = 1u << 20;    // 1 MiB
  std::uint32_t max_query_result_bytes = 1u << 22;

  // Transport bounds.
  std::uint32_t max_connections = 64;
  std::uint32_t max_session_threads = 160;
  std::uint32_t max_send_queue = 4096;
  std::uint32_t max_accept_backlog = 64;

  // Durability bounds.
  std::uint64_t max_persistence_bytes = 1ull << 30;  // 1 GiB

  /// Returns a non-empty reason when the limit set is internally invalid.
  [[nodiscard]] std::optional<std::string_view> validate() const noexcept;
};

/// Saturating-free checked arithmetic. Returns std::nullopt on overflow.
[[nodiscard]] std::optional<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b) noexcept;
[[nodiscard]] std::optional<std::uint64_t> checked_mul(std::uint64_t a, std::uint64_t b) noexcept;
[[nodiscard]] std::optional<std::int64_t> checked_add(std::int64_t a, std::int64_t b) noexcept;
[[nodiscard]] std::optional<std::int64_t> checked_mul(std::int64_t a, std::int64_t b) noexcept;

}  // namespace agent_scheduler
