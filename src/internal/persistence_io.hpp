// Agent Scheduler — persistence file I/O and integrity verification.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "agent_scheduler/persistence.hpp"
#include "agent_scheduler/result.hpp"

namespace agent_scheduler::internal {

/// Reads a whole file, rejecting sizes above the configured ceiling.
[[nodiscard]] MutationResult read_all(const std::filesystem::path& path,
                                      std::uint64_t max_bytes,
                                      std::vector<std::uint8_t>& out);

/// Validates magic, version, header checksum, trailer, length, and payload checksum.
/// Never interprets the payload.
[[nodiscard]] MutationResult verify_file(const std::vector<std::uint8_t>& file,
                                         PersistenceHeaderInfo& info,
                                         std::vector<std::uint8_t>& payload);

/// Writes bytes to a unique temporary sibling, optionally flushes to stable storage,
/// optionally re-reads and verifies, then replaces the destination atomically.
[[nodiscard]] MutationResult write_atomically(const std::filesystem::path& path,
                                              const std::vector<std::uint8_t>& bytes,
                                              const PersistOptions& options);

/// Removes a temporary file if present. Failures are reported.
[[nodiscard]] MutationResult remove_if_present(const std::filesystem::path& path);

}  // namespace agent_scheduler::internal
