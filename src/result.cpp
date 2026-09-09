// Agent Scheduler — structured outcomes and errors.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/result.hpp"

#include <utility>

namespace agent_scheduler {

const char* to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "OK";
    case ErrorCode::InvalidArgument: return "INVALID_ARGUMENT";
    case ErrorCode::LimitExceeded: return "LIMIT_EXCEEDED";
    case ErrorCode::NotFound: return "NOT_FOUND";
    case ErrorCode::StaleGeneration: return "STALE_GENERATION";
    case ErrorCode::Conflict: return "CONFLICT";
    case ErrorCode::Duplicate: return "DUPLICATE";
    case ErrorCode::NotEligible: return "NOT_ELIGIBLE";
    case ErrorCode::NoCapacity: return "NO_CAPACITY";
    case ErrorCode::PolicyRejected: return "POLICY_REJECTED";
    case ErrorCode::CapabilityRejected: return "CAPABILITY_REJECTED";
    case ErrorCode::NotReady: return "NOT_READY";
    case ErrorCode::Unhealthy: return "UNHEALTHY";
    case ErrorCode::Unreachable: return "UNREACHABLE";
    case ErrorCode::LeaseExpired: return "LEASE_EXPIRED";
    case ErrorCode::Fenced: return "FENCED";
    case ErrorCode::Cancelled: return "CANCELLED";
    case ErrorCode::Superseded: return "SUPERSEDED";
    case ErrorCode::ShuttingDown: return "SHUTTING_DOWN";
    case ErrorCode::AdmissionClosed: return "ADMISSION_CLOSED";
    case ErrorCode::PersistenceFormat: return "PERSISTENCE_FORMAT";
    case ErrorCode::PersistenceIntegrity: return "PERSISTENCE_INTEGRITY";
    case ErrorCode::PersistenceIo: return "PERSISTENCE_IO";
    case ErrorCode::PersistenceUnsupportedVersion: return "PERSISTENCE_UNSUPPORTED_VERSION";
    case ErrorCode::ProtocolError: return "PROTOCOL_ERROR";
    case ErrorCode::DecodeError: return "DECODE_ERROR";
    case ErrorCode::RecoveryRejected: return "RECOVERY_REJECTED";
    case ErrorCode::InvariantViolation: return "INVARIANT_VIOLATION";
    case ErrorCode::NotStarted: return "NOT_STARTED";
    case ErrorCode::AlreadyStarted: return "ALREADY_STARTED";
    case ErrorCode::Internal: return "INTERNAL";
    case ErrorCode::DeadlineExpired: return "DEADLINE_EXPIRED";
    case ErrorCode::FeasibilityRejected: return "FEASIBILITY_REJECTED";
    case ErrorCode::DrainBarrier: return "DRAIN_BARRIER";
    case ErrorCode::ExclusiveConflict: return "EXCLUSIVE_CONFLICT";
  }
  return "UNKNOWN_ERROR_CODE";
}

const char* to_string(ScheduleOutcome outcome) noexcept {
  switch (outcome) {
    case ScheduleOutcome::Assigned: return "ASSIGNED";
    case ScheduleOutcome::NoChange: return "NO_CHANGE";
    case ScheduleOutcome::Deferred: return "DEFERRED";
    case ScheduleOutcome::NoEligibleAgent: return "NO_ELIGIBLE_AGENT";
    case ScheduleOutcome::RejectStaleSchedulerEpoch: return "REJECT_STALE_SCHEDULER_EPOCH";
    case ScheduleOutcome::RejectStaleCoordinatorEpoch: return "REJECT_STALE_COORDINATOR_EPOCH";
    case ScheduleOutcome::RejectStaleAgentBoot: return "REJECT_STALE_AGENT_BOOT";
    case ScheduleOutcome::RejectStaleAgentGeneration: return "REJECT_STALE_AGENT_GENERATION";
    case ScheduleOutcome::RejectStaleWorkGeneration: return "REJECT_STALE_WORK_GENERATION";
    case ScheduleOutcome::RejectStaleAssignment: return "REJECT_STALE_ASSIGNMENT";
    case ScheduleOutcome::RejectStalePolicy: return "REJECT_STALE_POLICY";
    case ScheduleOutcome::RejectStaleCapability: return "REJECT_STALE_CAPABILITY";
    case ScheduleOutcome::RejectResourceFeasibility: return "REJECT_RESOURCE_FEASIBILITY";
    case ScheduleOutcome::RejectBudget: return "REJECT_BUDGET";
    case ScheduleOutcome::RejectSlo: return "REJECT_SLO";
    case ScheduleOutcome::RejectPolicy: return "REJECT_POLICY";
    case ScheduleOutcome::RejectCancelled: return "REJECT_CANCELLED";
    case ScheduleOutcome::RejectSuperseded: return "REJECT_SUPERSEDED";
    case ScheduleOutcome::RejectConflict: return "REJECT_CONFLICT";
    case ScheduleOutcome::RevalidationRequired: return "REVALIDATION_REQUIRED";
    case ScheduleOutcome::ShuttingDown: return "SHUTTING_DOWN";
    case ScheduleOutcome::RejectInvalidRequest: return "REJECT_INVALID_REQUEST";
    case ScheduleOutcome::RejectLimitExceeded: return "REJECT_LIMIT_EXCEEDED";
    case ScheduleOutcome::RejectHealth: return "REJECT_HEALTH";
    case ScheduleOutcome::RejectReachability: return "REJECT_REACHABILITY";
    case ScheduleOutcome::RejectReadiness: return "REJECT_READINESS";
    case ScheduleOutcome::RejectCapacity: return "REJECT_CAPACITY";
    case ScheduleOutcome::RejectAffinity: return "REJECT_AFFINITY";
    case ScheduleOutcome::RejectLocality: return "REJECT_LOCALITY";
    case ScheduleOutcome::RejectTenant: return "REJECT_TENANT";
    case ScheduleOutcome::RejectLeaseExpired: return "REJECT_LEASE_EXPIRED";
    case ScheduleOutcome::RejectDrain: return "REJECT_DRAIN";
    case ScheduleOutcome::RejectDuplicate: return "REJECT_DUPLICATE";
    case ScheduleOutcome::RejectExpired: return "REJECT_EXPIRED";
    case ScheduleOutcome::RejectFenced: return "REJECT_FENCED";
    case ScheduleOutcome::RejectReservation: return "REJECT_RESERVATION";
    case ScheduleOutcome::RejectLifecycle: return "REJECT_LIFECYCLE";
    case ScheduleOutcome::RejectDeadline: return "REJECT_DEADLINE";
    case ScheduleOutcome::RejectCapability: return "REJECT_CAPABILITY";
    case ScheduleOutcome::RejectStaleQueueGeneration: return "REJECT_STALE_QUEUE_GENERATION";
  }
  return "UNKNOWN_OUTCOME";
}

bool is_rejection(ScheduleOutcome outcome) noexcept {
  switch (outcome) {
    case ScheduleOutcome::Assigned:
    case ScheduleOutcome::NoChange:
    case ScheduleOutcome::Deferred:
    case ScheduleOutcome::NoEligibleAgent:
      return false;
    default:
      return true;
  }
}

std::string SchedulerError::describe() const {
  std::string out;
  out += to_string(code);
  out += " op=";
  out += operation;
  out += " subject=";
  out += subject;
  out += " outcome=";
  out += to_string(outcome);
  if (current_generation != 0 || expected_generation != 0) {
    out += " current=";
    out += std::to_string(current_generation);
    out += " expected=";
    out += std::to_string(expected_generation);
  }
  for (const std::string& factor : factors) {
    out += " factor=";
    out += factor;
  }
  if (!detail.empty()) {
    out += " detail=";
    out += detail;
  }
  return out;
}

std::string MutationResult::describe() const {
  if (!error.has_value()) {
    return "OK";
  }
  return error->describe();
}

SchedulerError make_error(ErrorCode code,
                          std::string operation,
                          std::string subject,
                          std::string detail,
                          ScheduleOutcome outcome) {
  SchedulerError error;
  error.code = code;
  error.operation = std::move(operation);
  error.subject = std::move(subject);
  error.detail = std::move(detail);
  error.outcome = outcome;
  return error;
}

}  // namespace agent_scheduler
