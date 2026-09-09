// Agent Scheduler — capability evidence, provenance, and generations.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/capability.hpp"

#include <algorithm>
#include <utility>

#include "internal/util.hpp"

namespace agent_scheduler {

const char* to_string(CapabilityState state) noexcept {
  switch (state) {
    case CapabilityState::Unknown: return "UNKNOWN";
    case CapabilityState::Declared: return "DECLARED";
    case CapabilityState::Observed: return "OBSERVED";
    case CapabilityState::Verified: return "VERIFIED";
    case CapabilityState::Revoked: return "REVOKED";
    case CapabilityState::Stale: return "STALE";
  }
  return "UNKNOWN";
}

int capability_state_rank(CapabilityState state) noexcept {
  switch (state) {
    case CapabilityState::Unknown: return 0;
    case CapabilityState::Declared: return 1;
    case CapabilityState::Observed: return 2;
    case CapabilityState::Verified: return 3;
    case CapabilityState::Revoked: return -2;
    case CapabilityState::Stale: return -1;
  }
  return -3;
}

bool capability_state_is_usable(CapabilityState state) noexcept {
  return capability_state_rank(state) >= capability_state_rank(CapabilityState::Declared);
}

bool is_valid_identifier(std::string_view value, std::uint32_t max_length) noexcept {
  return internal::is_identifier(value, max_length);
}

MutationResult canonicalize(CapabilityProfile& profile, const ResourceLimits& limits) {
  if (profile.capabilities.size() > limits.max_capabilities_per_agent) {
    return MutationResult::failure(make_error(ErrorCode::LimitExceeded, "publish_capabilities",
                                              profile.profile_id.to_string(),
                                              "capability count exceeds max_capabilities_per_agent",
                                              ScheduleOutcome::RejectLimitExceeded));
  }
  if (profile.generation.is_zero()) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "publish_capabilities",
                                              profile.profile_id.to_string(),
                                              "capability generation must be non-zero",
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  if (profile.boot.is_zero()) {
    return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "publish_capabilities",
                                              profile.profile_id.to_string(),
                                              "capability profile must name its originating boot",
                                              ScheduleOutcome::RejectInvalidRequest));
  }
  for (CapabilityEvidence& evidence : profile.capabilities) {
    if (const auto problem = internal::identifier_problem(evidence.name, limits.max_identifier_size,
                                                          "capability name")) {
      return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "publish_capabilities",
                                                profile.profile_id.to_string(), *problem,
                                                ScheduleOutcome::RejectInvalidRequest));
    }
    if (evidence.quality > 1000) {
      return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "publish_capabilities",
                                                evidence.name, "capability quality must be 0..1000",
                                                ScheduleOutcome::RejectInvalidRequest));
    }
    if (const auto problem = internal::text_problem(evidence.provenance, limits.max_string_size,
                                                    "capability provenance")) {
      return MutationResult::failure(make_error(ErrorCode::InvalidArgument, "publish_capabilities",
                                                evidence.name, *problem,
                                                ScheduleOutcome::RejectInvalidRequest));
    }
  }
  std::sort(profile.capabilities.begin(), profile.capabilities.end(),
            [](const CapabilityEvidence& left, const CapabilityEvidence& right) {
              return left.name < right.name;
            });
  const auto duplicate = std::adjacent_find(
      profile.capabilities.begin(), profile.capabilities.end(),
      [](const CapabilityEvidence& left, const CapabilityEvidence& right) { return left.name == right.name; });
  if (duplicate != profile.capabilities.end()) {
    return MutationResult::failure(make_error(ErrorCode::Duplicate, "publish_capabilities",
                                              duplicate->name, "duplicate capability name in profile",
                                              ScheduleOutcome::RejectDuplicate));
  }
  return MutationResult::success();
}

const CapabilityEvidence* find_capability(const CapabilityProfile& profile, std::string_view name) noexcept {
  const auto found = std::lower_bound(
      profile.capabilities.begin(), profile.capabilities.end(), name,
      [](const CapabilityEvidence& evidence, std::string_view value) { return evidence.name < value; });
  if (found == profile.capabilities.end() || found->name != name) {
    return nullptr;
  }
  return &*found;
}

void invalidate_evidence(CapabilityProfile& profile) noexcept {
  for (CapabilityEvidence& evidence : profile.capabilities) {
    evidence.state = CapabilityState::Stale;
    evidence.observed_at_ms = 0;
  }
}

}  // namespace agent_scheduler
