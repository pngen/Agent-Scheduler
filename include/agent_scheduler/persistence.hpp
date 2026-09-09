// Agent Scheduler — versioned, integrity-checked durable persistence.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "agent_scheduler/digest.hpp"
#include "agent_scheduler/ids.hpp"
#include "agent_scheduler/result.hpp"

namespace agent_scheduler {

/// On-disk layout constants. The magic is fixed; the version is explicit and any other
/// value is rejected before the payload is interpreted.
struct PersistenceFormat {
  static constexpr std::string_view magic = "AGSDCHST";
  static constexpr std::uint32_t version = 1;
  static constexpr std::uint32_t header_bytes = 72;
  static constexpr std::uint32_t trailer_bytes = 16;
};

struct PersistOptions {
  /// Flush file data to stable storage before the atomic replacement.
  bool fsync{true};
  /// Re-read and verify the written file before the replacement is committed.
  bool verify_after_write{true};
  /// Byte ceiling for the encoded payload. Zero uses the scheduler's configured limit.
  std::uint64_t max_bytes{0};
};

struct LoadOptions {
  /// Require the stored semantic digest to match the decoded state.
  bool require_semantic_digest{true};
  /// Skip the conservative dynamic-evidence invalidation. Used only to inspect a file
  /// without changing its interpretation; never enables dispatch of recovered evidence.
  bool inspect_only{false};
};

struct PersistenceHeaderInfo {
  std::uint32_t format_version{0};
  std::uint32_t flags{0};
  std::uint64_t payload_bytes{0};
  std::uint64_t record_count{0};
  std::uint32_t payload_crc32{0};
  Digest256 semantic_digest{};
  std::uint64_t total_bytes{0};
};

/// Reads and validates only the header and trailer of a persistence file. Detects
/// truncation, corruption, trailing garbage, and unsupported versions without decoding
/// the payload.
[[nodiscard]] MutationResult read_persistence_header(const std::filesystem::path& path,
                                                     PersistenceHeaderInfo& out);

struct RecoveryResult {
  std::optional<SchedulerError> error;
  std::uint32_t format_version{0};
  SchedulerEpoch previous_scheduler_epoch{};
  SchedulerEpoch new_scheduler_epoch{};
  CoordinatorEpoch previous_coordinator_epoch{};
  CoordinatorEpoch new_coordinator_epoch{};
  std::size_t agents_loaded{0};
  std::size_t work_loaded{0};
  std::size_t assignments_loaded{0};
  std::size_t assignments_requiring_revalidation{0};
  std::size_t leases_invalidated{0};
  std::size_t fenced_boots_loaded{0};
  std::size_t dynamic_evidence_invalidated{0};
  std::size_t queues_loaded{0};
  Digest256 stored_digest{};
  Digest256 computed_digest{};
  std::uint64_t bytes_read{0};
  std::uint64_t elapsed_ms{0};
  std::string detail;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
  [[nodiscard]] std::string describe() const;
};

}  // namespace agent_scheduler
