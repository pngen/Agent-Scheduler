// Agent Scheduler — structured outcomes and errors.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace agent_scheduler {

/// Stable machine-readable error code. This is part of the public contract and of the
/// wire protocol; codes are never renumbered.
enum class ErrorCode : std::uint32_t {
  Ok = 0,
  InvalidArgument = 1,
  LimitExceeded = 2,
  NotFound = 3,
  StaleGeneration = 4,
  Conflict = 5,
  Duplicate = 6,
  NotEligible = 7,
  NoCapacity = 8,
  PolicyRejected = 9,
  CapabilityRejected = 10,
  NotReady = 11,
  Unhealthy = 12,
  Unreachable = 13,
  LeaseExpired = 14,
  Fenced = 15,
  Cancelled = 16,
  Superseded = 17,
  ShuttingDown = 18,
  AdmissionClosed = 19,
  PersistenceFormat = 20,
  PersistenceIntegrity = 21,
  PersistenceIo = 22,
  PersistenceUnsupportedVersion = 23,
  ProtocolError = 24,
  DecodeError = 25,
  RecoveryRejected = 26,
  InvariantViolation = 27,
  NotStarted = 28,
  AlreadyStarted = 29,
  Internal = 30,
  DeadlineExpired = 31,
  FeasibilityRejected = 32,
  DrainBarrier = 33,
  ExclusiveConflict = 34,
};

[[nodiscard]] const char* to_string(ErrorCode code) noexcept;

/// Outcome of a scheduling operation. Names are the public contract.
enum class ScheduleOutcome : std::uint32_t {
  Assigned = 0,
  NoChange = 1,
  Deferred = 2,
  NoEligibleAgent = 3,
  RejectStaleSchedulerEpoch = 4,
  RejectStaleCoordinatorEpoch = 5,
  RejectStaleAgentBoot = 6,
  RejectStaleAgentGeneration = 7,
  RejectStaleWorkGeneration = 8,
  RejectStaleAssignment = 9,
  RejectStalePolicy = 10,
  RejectStaleCapability = 11,
  RejectResourceFeasibility = 12,
  RejectBudget = 13,
  RejectSlo = 14,
  RejectPolicy = 15,
  RejectCancelled = 16,
  RejectSuperseded = 17,
  RejectConflict = 18,
  RevalidationRequired = 19,
  ShuttingDown = 20,
  RejectInvalidRequest = 21,
  RejectLimitExceeded = 22,
  RejectHealth = 23,
  RejectReachability = 24,
  RejectReadiness = 25,
  RejectCapacity = 26,
  RejectAffinity = 27,
  RejectLocality = 28,
  RejectTenant = 29,
  RejectLeaseExpired = 30,
  RejectDrain = 31,
  RejectDuplicate = 32,
  RejectExpired = 33,
  RejectFenced = 34,
  RejectReservation = 35,
  RejectLifecycle = 36,
  RejectDeadline = 37,
  RejectCapability = 38,
  RejectStaleQueueGeneration = 39,
};

[[nodiscard]] const char* to_string(ScheduleOutcome outcome) noexcept;
[[nodiscard]] bool is_rejection(ScheduleOutcome outcome) noexcept;

/// Structured, safe-to-publish error. Never carries secrets or exception text.
struct SchedulerError {
  ErrorCode code{ErrorCode::Ok};
  std::string operation;
  std::string subject;
  std::uint64_t current_generation{0};
  std::uint64_t expected_generation{0};
  std::vector<std::string> factors;
  std::string detail;
  ScheduleOutcome outcome{ScheduleOutcome::NoChange};

  /// Deterministic single-line rendering used by tools and diagnostics.
  [[nodiscard]] std::string describe() const;
};

/// Result of a state-changing operation. A failure is never a bare false: it always
/// carries a structured SchedulerError.
struct MutationResult {
  std::optional<SchedulerError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  static MutationResult success() noexcept { return MutationResult{}; }
  static MutationResult failure(SchedulerError error_value) noexcept {
    MutationResult result;
    result.error = std::move(error_value);
    return result;
  }
  [[nodiscard]] std::string describe() const;
};

/// Convenience constructor for a structured error.
[[nodiscard]] SchedulerError make_error(ErrorCode code,
                                        std::string operation,
                                        std::string subject,
                                        std::string detail,
                                        ScheduleOutcome outcome = ScheduleOutcome::NoChange);

}  // namespace agent_scheduler
