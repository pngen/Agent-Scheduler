// Agent Scheduler — synchronous control client for the reference transport.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/net/control_client.hpp"

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "agent_scheduler/version.hpp"

namespace agent_scheduler::net {

ControlClient::ControlClient(ControlClientOptions options)
    : options_(std::move(options)),
      decoder_(options_.limits.max_frame_size, options_.protocol_version) {}

ControlClient::~ControlClient() { close(); }

MutationResult ControlClient::connect() {
  if (stream_.valid()) {
    return MutationResult::success();
  }
  if (const auto init = initialize_network(); !init.ok()) {
    return init;
  }
  if (const auto connected = connect_loopback(options_.port, stream_); !connected.ok()) {
    return connected;
  }
  stream_.set_no_delay(true);
  decoder_.reset();
  return hello();
}

void ControlClient::close() { stream_.close(); }

bool ControlClient::read_frame(Frame& frame) {
  std::array<std::uint8_t, 16384> buffer{};
  for (;;) {
    FrameCodecStatus status = FrameCodecStatus::NeedMoreData;
    std::optional<Frame> decoded = decoder_.next(status);
    if (decoded.has_value()) {
      frame = std::move(*decoded);
      ++frames_received_;
      return true;
    }
    if (status != FrameCodecStatus::NeedMoreData) {
      return false;
    }
    const std::size_t received = stream_.read_some(std::span<std::uint8_t>(buffer.data(), buffer.size()));
    if (received == 0) {
      return false;
    }
    decoder_.append(std::span<const std::uint8_t>(buffer.data(), received));
  }
}

MutationResult ControlClient::send_raw(MessageType type,
                                       std::vector<std::uint8_t> payload,
                                       MessageType expected_reply) {
  if (!stream_.valid()) {
    return MutationResult::failure(make_error(ErrorCode::NotStarted, "control_client", to_string(type),
                                              "not connected", ScheduleOutcome::NoChange));
  }
  const std::uint64_t correlation = next_correlation_++;
  Frame frame;
  frame.header.version = options_.protocol_version;
  frame.header.type = static_cast<std::uint16_t>(type);
  frame.header.correlation = correlation;
  frame.payload = std::move(payload);
  try {
    stream_.write_all(std::span<const std::uint8_t>(encode_frame(frame, options_.limits.max_frame_size)));
  } catch (const std::exception& error) {
    return MutationResult::failure(make_error(ErrorCode::Internal, "control_client", to_string(type),
                                              error.what(), ScheduleOutcome::NoChange));
  }
  for (;;) {
    Frame reply;
    if (!read_frame(reply)) {
      return MutationResult::failure(make_error(ErrorCode::Internal, "control_client", to_string(type),
                                                "connection closed before a reply arrived",
                                                ScheduleOutcome::NoChange));
    }
    if (reply.header.correlation != correlation) {
      continue;
    }
    const auto reply_type = static_cast<MessageType>(reply.header.type);
    ByteReader reader(std::span<const std::uint8_t>(reply.payload.data(), reply.payload.size()));
    try {
      if (reply_type == MessageType::Error) {
        ErrorMessage message;
        decode(reader, message, options_.limits);
        return MutationResult::failure(make_error(message.code, message.operation, message.subject,
                                                  message.detail, ScheduleOutcome::RejectInvalidRequest));
      }
      if (reply_type != expected_reply) {
        return MutationResult::failure(make_error(ErrorCode::ProtocolError, "control_client", to_string(type),
                                                  std::string("unexpected reply type ") + to_string(reply_type),
                                                  ScheduleOutcome::NoChange));
      }
      ResultMessage message;
      decode(reader, message, options_.limits);
      reader.require_exhausted();
      if (message.ok) {
        return MutationResult::success();
      }
      return MutationResult::failure(message.error);
    } catch (const std::exception& error) {
      return MutationResult::failure(make_error(ErrorCode::DecodeError, "control_client", to_string(type),
                                                error.what(), ScheduleOutcome::NoChange));
    }
  }
}

template <class Message>
MutationResult ControlClient::send(MessageType type, const Message& message, MessageType expected_reply) {
  ByteWriter writer;
  encode(writer, message, options_.limits);
  return send_raw(type, writer.take(), expected_reply);
}

MutationResult ControlClient::hello() {
  HelloMessage message;
  message.protocol_version = agent_scheduler::protocol_version;
  message.role = "control";
  message.build = build_info();
  message.nonce = process_nonce();
  return send(MessageType::Hello, message, MessageType::HelloResult);
}

MutationResult ControlClient::register_agent(const AgentDescriptor& descriptor) {
  RegisterAgentMessage message;
  message.descriptor = descriptor;
  return send(MessageType::RegisterAgent, message, MessageType::RegisterResult);
}

MutationResult ControlClient::publish_capabilities(AgentId agent, AgentBootId boot, const CapabilityProfile& profile) {
  PublishCapabilitiesMessage message;
  message.agent = agent;
  message.boot = boot;
  message.profile = profile;
  return send(MessageType::PublishCapabilities, message, MessageType::PublishCapabilitiesResult);
}

MutationResult ControlClient::publish_health(AgentId agent, AgentBootId boot, const HealthObservation& observation) {
  PublishHealthMessage message;
  message.agent = agent;
  message.boot = boot;
  message.observation = observation;
  return send(MessageType::PublishHealth, message, MessageType::PublishHealthResult);
}

MutationResult ControlClient::publish_availability(AgentId agent,
                                                  AgentBootId boot,
                                                  const AvailabilityObservation& observation) {
  PublishAvailabilityMessage message;
  message.agent = agent;
  message.boot = boot;
  message.observation = observation;
  return send(MessageType::PublishAvailability, message, MessageType::PublishAvailabilityResult);
}

MutationResult ControlClient::publish_load(AgentId agent, AgentBootId boot, const LoadObservation& observation) {
  PublishLoadMessage message;
  message.agent = agent;
  message.boot = boot;
  message.observation = observation;
  return send(MessageType::PublishLoad, message, MessageType::PublishLoadResult);
}

MutationResult ControlClient::submit_work(const WorkRequest& request) {
  SubmitWorkMessage message;
  message.request = request;
  return send(MessageType::SubmitWork, message, MessageType::SubmitWorkResult);
}

MutationResult ControlClient::cancel_work(WorkId work, WorkGeneration generation, std::string reason) {
  CancelWorkMessage message;
  message.work = work;
  message.generation = generation;
  message.reason = std::move(reason);
  return send(MessageType::CancelWork, message, MessageType::CancelWorkResult);
}

MutationResult ControlClient::supersede_work(WorkId work,
                                             WorkGeneration expected_generation,
                                             const WorkRequest& replacement) {
  SupersedeWorkMessage message;
  message.work = work;
  message.expected_generation = expected_generation;
  message.replacement = replacement;
  return send(MessageType::SupersedeWork, message, MessageType::SupersedeWorkResult);
}

MutationResult ControlClient::drain_agent(AgentId agent, AgentBootId boot, bool force) {
  DrainAgentMessage message;
  message.agent = agent;
  message.boot = boot;
  message.force = force;
  return send(MessageType::DrainAgent, message, MessageType::DrainAgentResult);
}

MutationResult ControlClient::deregister_agent(AgentId agent, AgentBootId boot, bool force) {
  DeregisterAgentMessage message;
  message.agent = agent;
  message.boot = boot;
  message.force = force;
  return send(MessageType::DeregisterAgent, message, MessageType::DeregisterAgentResult);
}

MutationResult ControlClient::assignment_ack(const AssignmentBinding& binding, bool accepted, std::string reason) {
  AssignmentAckMessage message;
  message.binding = binding;
  message.accepted = accepted;
  message.reason = std::move(reason);
  return send(MessageType::AssignmentAck, message, MessageType::AssignmentAck);
}

MutationResult ControlClient::observation(MessageType type,
                                          const AssignmentBinding& binding,
                                          std::string reason) {
  AssignmentObservationMessage message;
  message.binding = binding;
  message.reason = std::move(reason);
  return send(type, message, type);
}

MutationResult ControlClient::dispatch_assignment(AssignmentId assignment,
                                                  AssignmentGeneration generation,
                                                  ScheduleOutcome& outcome) {
  DispatchAssignmentMessage message;
  message.assignment = assignment;
  message.generation = generation;
  ByteWriter writer;
  encode(writer, message, options_.limits);
  const MutationResult result =
      send_raw(MessageType::DispatchAssignment, writer.take(), MessageType::DispatchAssignmentResult);
  if (result.ok()) {
    outcome = ScheduleOutcome::Assigned;
  } else if (result.error.has_value()) {
    outcome = result.error->outcome;
  }
  return result;
}

MutationResult ControlClient::query(QueryKind kind, std::string& json) {
  return query(kind, AgentId{}, AgentBootId{}, WorkId{}, AssignmentId{}, json);
}

MutationResult ControlClient::query(QueryKind kind,
                                    AgentId agent,
                                    AgentBootId boot,
                                    WorkId work,
                                    AssignmentId assignment,
                                    std::string& json) {
  QueryMessage message;
  message.kind = kind;
  message.agent = agent;
  message.boot = boot;
  message.work = work;
  message.assignment = assignment;
  if (!stream_.valid()) {
    return MutationResult::failure(make_error(ErrorCode::NotStarted, "control_client", "query", "not connected",
                                              ScheduleOutcome::NoChange));
  }
  const std::uint64_t correlation = next_correlation_++;
  Frame frame;
  frame.header.version = options_.protocol_version;
  frame.header.type = static_cast<std::uint16_t>(MessageType::Query);
  frame.header.correlation = correlation;
  ByteWriter writer;
  encode(writer, message, options_.limits);
  frame.payload = writer.take();
  try {
    stream_.write_all(std::span<const std::uint8_t>(encode_frame(frame, options_.limits.max_frame_size)));
  } catch (const std::exception& error) {
    return MutationResult::failure(make_error(ErrorCode::Internal, "control_client", "query", error.what(),
                                              ScheduleOutcome::NoChange));
  }
  for (;;) {
    Frame reply;
    if (!read_frame(reply)) {
      return MutationResult::failure(make_error(ErrorCode::Internal, "control_client", "query",
                                                "connection closed before a reply arrived",
                                                ScheduleOutcome::NoChange));
    }
    if (reply.header.correlation != correlation) {
      continue;
    }
    if (static_cast<MessageType>(reply.header.type) == MessageType::Error) {
      ByteReader reader(std::span<const std::uint8_t>(reply.payload.data(), reply.payload.size()));
      ErrorMessage error_message;
      try {
        decode(reader, error_message, options_.limits);
      } catch (const std::exception& error) {
        return MutationResult::failure(make_error(ErrorCode::DecodeError, "control_client", "query", error.what(),
                                                  ScheduleOutcome::NoChange));
      }
      return MutationResult::failure(make_error(error_message.code, error_message.operation,
                                                error_message.subject, error_message.detail,
                                                ScheduleOutcome::RejectInvalidRequest));
    }
    ByteReader reader(std::span<const std::uint8_t>(reply.payload.data(), reply.payload.size()));
    try {
      QueryResultMessage result;
      decode(reader, result, options_.limits);
      reader.require_exhausted();
      json = std::move(result.json);
      return MutationResult::success();
    } catch (const std::exception& error) {
      return MutationResult::failure(make_error(ErrorCode::DecodeError, "control_client", "query", error.what(),
                                                ScheduleOutcome::NoChange));
    }
  }
}

}  // namespace agent_scheduler::net
