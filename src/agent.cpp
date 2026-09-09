// Agent Scheduler — persistent autonomous worker model.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/agent.hpp"

#include <algorithm>
#include <utility>

#include "internal/util.hpp"

namespace agent_scheduler {

const char* to_string(AgentLifecycle value) noexcept {
  switch (value) {
    case AgentLifecycle::Unregistered: return "UNREGISTERED";
    case AgentLifecycle::Registering: return "REGISTERING";
    case AgentLifecycle::Ready: return "READY";
    case AgentLifecycle::Busy: return "BUSY";
    case AgentLifecycle::Draining: return "DRAINING";
    case AgentLifecycle::Unavailable: return "UNAVAILABLE";
    case AgentLifecycle::RevalidationRequired: return "REVALIDATION_REQUIRED";
    case AgentLifecycle::Lost: return "LOST";
    case AgentLifecycle::Retired: return "RETIRED";
  }
  return "UNREGISTERED";
}

const char* to_string(AgentHealth value) noexcept {
  switch (value) {
    case AgentHealth::Unknown: return "UNKNOWN";
    case AgentHealth::Healthy: return "HEALTHY";
    case AgentHealth::Degraded: return "DEGRADED";
    case AgentHealth::Unhealthy: return "UNHEALTHY";
  }
  return "UNKNOWN";
}

const char* to_string(AgentReadiness value) noexcept {
  switch (value) {
    case AgentReadiness::Unknown: return "UNKNOWN";
    case AgentReadiness::Ready: return "READY";
    case AgentReadiness::NotReady: return "NOT_READY";
  }
  return "UNKNOWN";
}

const char* to_string(AgentReachability value) noexcept {
  switch (value) {
    case AgentReachability::Unknown: return "UNKNOWN";
    case AgentReachability::Reachable: return "REACHABLE";
    case AgentReachability::Unreachable: return "UNREACHABLE";
  }
  return "UNKNOWN";
}

const char* to_string(AgentAvailability value) noexcept {
  switch (value) {
    case AgentAvailability::Unknown: return "UNKNOWN";
    case AgentAvailability::Available: return "AVAILABLE";
    case AgentAvailability::Unavailable: return "UNAVAILABLE";
  }
  return "UNKNOWN";
}

bool lifecycle_accepts_new_work(AgentLifecycle value) noexcept {
  return value == AgentLifecycle::Ready || value == AgentLifecycle::Busy;
}

bool lifecycle_is_terminal(AgentLifecycle value) noexcept {
  return value == AgentLifecycle::Retired;
}

MutationResult validate(const AgentDescriptor& descriptor, const ResourceLimits& limits) {
  if (descriptor.id.is_zero()) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "register_agent", "",
                                              "agent id must be non-zero",
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  if (descriptor.generation.is_zero()) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "register_agent",
                                              descriptor.id.to_string(),
                                              "agent generation must be non-zero",
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  if (descriptor.boot.is_zero()) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "register_agent",
                                              descriptor.id.to_string(),
                                              "agent boot id must be non-zero",
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  if (descriptor.registration_generation.is_zero()) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "register_agent",
                                              descriptor.id.to_string(),
                                              "registration generation must be non-zero",
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  if (descriptor.max_concurrency == 0) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "register_agent",
                                              descriptor.id.to_string(),
                                              "max_concurrency must be at least 1",
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  if (descriptor.max_concurrency > limits.max_active_assignments) {
    return MutationResult::failure(make_error(ErrorCode::LimitExceeded, "register_agent",
                                              descriptor.id.to_string(),
                                              "max_concurrency exceeds max_active_assignments",
                                              ScheduleOutcome::RejectLimitExceeded));
  }
  if (const auto problem = internal::text_problem(descriptor.display_name, limits.max_string_size,
                                                  "display_name")) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "register_agent",
                                              descriptor.id.to_string(), *problem,
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  if (const auto problem = internal::text_problem(descriptor.provenance, limits.max_string_size,
                                                  "provenance")) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "register_agent",
                                              descriptor.id.to_string(), *problem,
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  if (descriptor.policy_labels.size() > limits.max_policy_labels) {
    return MutationResult::failure(make_error(ErrorCode::LimitExceeded, "register_agent",
                                              descriptor.id.to_string(),
                                              "policy label count exceeds max_policy_labels",
                                              ScheduleOutcome::RejectLimitExceeded));
  }
  for (const std::string& label : descriptor.policy_labels) {
    if (const auto problem = internal::identifier_problem(label, limits.max_identifier_size, "policy label")) {
      return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "register_agent",
                                                descriptor.id.to_string(), *problem,
                                                ScheduleOutcome::RejectInvalidRequest));
    }
  }
  return MutationResult::success();
}

MutationResult validate(const LoadObservation& observation, const ResourceLimits& limits) {
  if (observation.generation.is_zero()) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "publish_load", "",
                                              "load generation must be non-zero",
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  if (observation.cpu_pressure_percent > 100) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "publish_load", "",
                                              "cpu_pressure_percent must be 0..100",
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  if (!internal::is_finite_non_negative(observation.cost_index)) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "publish_load", "",
                                              "cost_index must be finite and non-negative",
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  if (!internal::is_finite_unit_fraction(observation.slo_headroom_fraction)) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "publish_load", "",
                                              "slo_headroom_fraction must be finite and within [0,1]",
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  if (observation.warm_state_keys.size() > limits.max_warm_state_keys) {
    return MutationResult::failure(make_error(ErrorCode::LimitExceeded, "publish_load", "",
                                              "warm_state_keys exceeds max_warm_state_keys",
                                              ScheduleOutcome::RejectLimitExceeded));
  }
  for (const std::string& key : observation.warm_state_keys) {
    if (const auto problem = internal::identifier_problem(key, limits.max_identifier_size, "warm state key")) {
      return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "publish_load", "", *problem,
                                                ScheduleOutcome::RejectInvalidRequest));
    }
  }
  return MutationResult::success();
}

MutationResult validate(const HealthObservation& observation, const ResourceLimits& limits) {
  if (observation.generation.is_zero()) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "publish_health", "",
                                              "health generation must be non-zero",
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  if (observation.health_quality > 1000) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "publish_health", "",
                                              "health_quality must be 0..1000",
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  if (const auto problem = internal::text_problem(observation.detail, limits.max_string_size,
                                                  "health detail")) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "publish_health", "", *problem,
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  return MutationResult::success();
}

MutationResult validate(const AvailabilityObservation& observation, const ResourceLimits& limits) {
  (void)limits;
  if (observation.generation.is_zero()) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "publish_availability", "",
                                              "availability generation must be non-zero",
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  return MutationResult::success();
}

}  // namespace agent_scheduler
