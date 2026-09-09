// Agent Scheduler — reference wire protocol (version 1).
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "agent_scheduler/agent.hpp"
#include "agent_scheduler/assignment.hpp"
#include "agent_scheduler/capability.hpp"
#include "agent_scheduler/limits.hpp"
#include "agent_scheduler/net/frame.hpp"
#include "agent_scheduler/result.hpp"
#include "agent_scheduler/work.hpp"

namespace agent_scheduler::net {

/// Query kinds supported by the coordinator. Results are canonical JSON text.
enum class QueryKind : std::uint16_t {
  Version = 1,
  Summary = 2,
  Agents = 3,
  Work = 4,
  Assignments = 5,
  Leases = 6,
  Fenced = 7,
  Invariants = 8,
  Fairness = 9,
  Capabilities = 10,
  Queues = 11,
  Policy = 12,
};

[[nodiscard]] const char* to_string(QueryKind value) noexcept;
[[nodiscard]] bool is_known_query_kind(std::uint16_t value) noexcept;

struct HelloMessage {
  std::uint32_t protocol_version{0};
  std::string role;
  std::string build;
  std::uint64_t nonce{0};
};

struct RegisterAgentMessage {
  AgentDescriptor descriptor;
};

struct PublishCapabilitiesMessage {
  AgentId agent{};
  AgentBootId boot{};
  CapabilityProfile profile;
};

struct PublishHealthMessage {
  AgentId agent{};
  AgentBootId boot{};
  HealthObservation observation;
};

struct PublishAvailabilityMessage {
  AgentId agent{};
  AgentBootId boot{};
  AvailabilityObservation observation;
};

struct PublishLoadMessage {
  AgentId agent{};
  AgentBootId boot{};
  LoadObservation observation;
};

struct HeartbeatMessage {
  AgentId agent{};
  AgentBootId boot{};
  AgentRegistrationGeneration registration_generation{};
};

struct SubmitWorkMessage {
  WorkRequest request;
};

struct CancelWorkMessage {
  WorkId work{};
  WorkGeneration generation{};
  std::string reason;
};

struct SupersedeWorkMessage {
  WorkId work{};
  WorkGeneration expected_generation{};
  WorkRequest replacement;
};

struct AssignWorkMessage {
  AssignmentBinding binding;
  std::string kind;
  std::string payload_ref;
};

struct AssignmentAckMessage {
  AssignmentBinding binding;
  bool accepted{true};
  std::string reason;
};

struct AssignmentObservationMessage {
  AssignmentBinding binding;
  std::string reason;
};

struct DrainAgentMessage {
  AgentId agent{};
  AgentBootId boot{};
  bool force{false};
};

struct DeregisterAgentMessage {
  AgentId agent{};
  AgentBootId boot{};
  bool force{false};
};

struct DispatchAssignmentMessage {
  AssignmentId assignment{};
  AssignmentGeneration generation{};
};

struct QueryMessage {
  QueryKind kind{QueryKind::Summary};
  AgentId agent{};
  AgentBootId boot{};
  WorkId work{};
  AssignmentId assignment{};
};

struct QueryResultMessage {
  QueryKind kind{QueryKind::Summary};
  std::string json;
};

struct ErrorMessage {
  ErrorCode code{ErrorCode::Ok};
  std::string operation;
  std::string subject;
  std::string detail;
};

/// Uniform result payload for every mutating request.
struct ResultMessage {
  MessageType request_type{MessageType::Hello};
  bool ok{true};
  ScheduleOutcome outcome{ScheduleOutcome::NoChange};
  SchedulerError error;
};

// ---- canonical encoders -------------------------------------------------------

void encode(ByteWriter& writer, const HelloMessage& message, const ResourceLimits& limits);
void decode(ByteReader& reader, HelloMessage& message, const ResourceLimits& limits);
void encode(ByteWriter& writer, const RegisterAgentMessage& message, const ResourceLimits& limits);
void decode(ByteReader& reader, RegisterAgentMessage& message, const ResourceLimits& limits);
void encode(ByteWriter& writer, const PublishCapabilitiesMessage& message, const ResourceLimits& limits);
void decode(ByteReader& reader, PublishCapabilitiesMessage& message, const ResourceLimits& limits);
void encode(ByteWriter& writer, const PublishHealthMessage& message, const ResourceLimits& limits);
void decode(ByteReader& reader, PublishHealthMessage& message, const ResourceLimits& limits);
void encode(ByteWriter& writer, const PublishAvailabilityMessage& message, const ResourceLimits& limits);
void decode(ByteReader& reader, PublishAvailabilityMessage& message, const ResourceLimits& limits);
void encode(ByteWriter& writer, const PublishLoadMessage& message, const ResourceLimits& limits);
void decode(ByteReader& reader, PublishLoadMessage& message, const ResourceLimits& limits);
void encode(ByteWriter& writer, const HeartbeatMessage& message, const ResourceLimits& limits);
void decode(ByteReader& reader, HeartbeatMessage& message, const ResourceLimits& limits);
void encode(ByteWriter& writer, const SubmitWorkMessage& message, const ResourceLimits& limits);
void decode(ByteReader& reader, SubmitWorkMessage& message, const ResourceLimits& limits);
void encode(ByteWriter& writer, const CancelWorkMessage& message, const ResourceLimits& limits);
void decode(ByteReader& reader, CancelWorkMessage& message, const ResourceLimits& limits);
void encode(ByteWriter& writer, const SupersedeWorkMessage& message, const ResourceLimits& limits);
void decode(ByteReader& reader, SupersedeWorkMessage& message, const ResourceLimits& limits);
void encode(ByteWriter& writer, const AssignWorkMessage& message, const ResourceLimits& limits);
void decode(ByteReader& reader, AssignWorkMessage& message, const ResourceLimits& limits);
void encode(ByteWriter& writer, const AssignmentAckMessage& message, const ResourceLimits& limits);
void decode(ByteReader& reader, AssignmentAckMessage& message, const ResourceLimits& limits);
void encode(ByteWriter& writer, const AssignmentObservationMessage& message, const ResourceLimits& limits);
void decode(ByteReader& reader, AssignmentObservationMessage& message, const ResourceLimits& limits);
void encode(ByteWriter& writer, const DrainAgentMessage& message, const ResourceLimits& limits);
void decode(ByteReader& reader, DrainAgentMessage& message, const ResourceLimits& limits);
void encode(ByteWriter& writer, const DeregisterAgentMessage& message, const ResourceLimits& limits);
void decode(ByteReader& reader, DeregisterAgentMessage& message, const ResourceLimits& limits);
void encode(ByteWriter& writer, const DispatchAssignmentMessage& message, const ResourceLimits& limits);
void decode(ByteReader& reader, DispatchAssignmentMessage& message, const ResourceLimits& limits);
void encode(ByteWriter& writer, const QueryMessage& message, const ResourceLimits& limits);
void decode(ByteReader& reader, QueryMessage& message, const ResourceLimits& limits);
void encode(ByteWriter& writer, const QueryResultMessage& message, const ResourceLimits& limits);
void decode(ByteReader& reader, QueryResultMessage& message, const ResourceLimits& limits);
void encode(ByteWriter& writer, const ErrorMessage& message, const ResourceLimits& limits);
void decode(ByteReader& reader, ErrorMessage& message, const ResourceLimits& limits);
void encode(ByteWriter& writer, const ResultMessage& message, const ResourceLimits& limits);
void decode(ByteReader& reader, ResultMessage& message, const ResourceLimits& limits);

// ---- shared field encoders ----------------------------------------------------

void encode(ByteWriter& writer, const SchedulerError& error, const ResourceLimits& limits);
void decode(ByteReader& reader, SchedulerError& error, const ResourceLimits& limits);
void encode(ByteWriter& writer, const AgentDescriptor& descriptor, const ResourceLimits& limits);
void decode(ByteReader& reader, AgentDescriptor& descriptor, const ResourceLimits& limits);
void encode(ByteWriter& writer, const CapabilityProfile& profile, const ResourceLimits& limits);
void decode(ByteReader& reader, CapabilityProfile& profile, const ResourceLimits& limits);
void encode(ByteWriter& writer, const HealthObservation& observation, const ResourceLimits& limits);
void decode(ByteReader& reader, HealthObservation& observation, const ResourceLimits& limits);
void encode(ByteWriter& writer, const AvailabilityObservation& observation, const ResourceLimits& limits);
void decode(ByteReader& reader, AvailabilityObservation& observation, const ResourceLimits& limits);
void encode(ByteWriter& writer, const LoadObservation& observation, const ResourceLimits& limits);
void decode(ByteReader& reader, LoadObservation& observation, const ResourceLimits& limits);
void encode(ByteWriter& writer, const WorkRequest& request, const ResourceLimits& limits);
void decode(ByteReader& reader, WorkRequest& request, const ResourceLimits& limits);
void encode(ByteWriter& writer, const AssignmentBinding& binding, const ResourceLimits& limits);
void decode(ByteReader& reader, AssignmentBinding& binding, const ResourceLimits& limits);

}  // namespace agent_scheduler::net
