// Agent Scheduler — resource bounds and checked arithmetic.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/limits.hpp"

#include <limits>

namespace agent_scheduler {

std::optional<std::string_view> ResourceLimits::validate() const noexcept {
  if (max_agents == 0 || max_work_items == 0 || max_queues == 0) {
    return "population limits must be non-zero";
  }
  if (max_string_size == 0 || max_identifier_size == 0) {
    return "text limits must be non-zero";
  }
  if (max_identifier_size > max_string_size) {
    return "max_identifier_size must not exceed max_string_size";
  }
  if (max_frame_size < 64 || max_payload_size < 64) {
    return "frame and payload limits must be at least 64 bytes";
  }
  if (max_payload_size > max_frame_size) {
    return "max_payload_size must not exceed max_frame_size";
  }
  if (max_send_queue == 0 || max_connections == 0 || max_session_threads == 0) {
    return "transport limits must be non-zero";
  }
  if (max_session_threads < 2 * max_connections) {
    return "max_session_threads must allow a reader and writer per connection";
  }
  if (max_capabilities_per_agent == 0 || max_capability_requirements == 0) {
    return "capability limits must be non-zero";
  }
  if (max_historical_assignments < max_active_assignments) {
    return "max_historical_assignments must be at least max_active_assignments";
  }
  if (max_persistence_bytes < 1024) {
    return "max_persistence_bytes must be at least 1024";
  }
  return std::nullopt;
}

std::optional<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b) noexcept {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) {
    return std::nullopt;
  }
  return a + b;
}

std::optional<std::uint64_t> checked_mul(std::uint64_t a, std::uint64_t b) noexcept {
  if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
    return std::nullopt;
  }
  return a * b;
}

std::optional<std::int64_t> checked_add(std::int64_t a, std::int64_t b) noexcept {
  if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
      (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b)) {
    return std::nullopt;
  }
  return a + b;
}

std::optional<std::int64_t> checked_mul(std::int64_t a, std::int64_t b) noexcept {
  if (a == 0 || b == 0) {
    return 0;
  }
  if (a == -1 && b == std::numeric_limits<std::int64_t>::min()) {
    return std::nullopt;
  }
  if (b == -1 && a == std::numeric_limits<std::int64_t>::min()) {
    return std::nullopt;
  }
  const std::int64_t result = a * b;
  if (result / b != a) {
    return std::nullopt;
  }
  return result;
}

}  // namespace agent_scheduler
