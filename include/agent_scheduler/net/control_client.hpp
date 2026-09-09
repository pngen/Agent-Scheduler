// Agent Scheduler — synchronous control client for the reference transport.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "agent_scheduler/clock.hpp"
#include "agent_scheduler/limits.hpp"
#include "agent_scheduler/net/frame.hpp"
#include "agent_scheduler/net/protocol.hpp"
#include "agent_scheduler/net/transport.hpp"

namespace agent_scheduler::net {

struct ControlClientOptions {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  ResourceLimits limits{};
  std::uint16_t protocol_version{1};
};

/// Blocking request/response client. Every call sends exactly one frame and waits for
/// the reply carrying the same correlation id. Used by the control tool and by the
/// multiprocess proof harness, which drives the coordinator as an independent process.
class ControlClient final {
 public:
  explicit ControlClient(ControlClientOptions options = {});
  ~ControlClient();

  ControlClient(const ControlClient&) = delete;
  ControlClient& operator=(const ControlClient&) = delete;

  [[nodiscard]] MutationResult connect();
  void close();
  [[nodiscard]] bool connected() const noexcept { return stream_.valid(); }
  [[nodiscard]] const ControlClientOptions& options() const noexcept { return options_; }

  [[nodiscard]] MutationResult hello();
  [[nodiscard]] MutationResult register_agent(const AgentDescriptor& descriptor);
  [[nodiscard]] MutationResult publish_capabilities(AgentId agent,
                                                    AgentBootId boot,
                                                    const CapabilityProfile& profile);
  [[nodiscard]] MutationResult publish_health(AgentId agent, AgentBootId boot, const HealthObservation& observation);
  [[nodiscard]] MutationResult publish_availability(AgentId agent,
                                                    AgentBootId boot,
                                                    const AvailabilityObservation& observation);
  [[nodiscard]] MutationResult publish_load(AgentId agent, AgentBootId boot, const LoadObservation& observation);
  [[nodiscard]] MutationResult submit_work(const WorkRequest& request);
  [[nodiscard]] MutationResult cancel_work(WorkId work, WorkGeneration generation, std::string reason);
  [[nodiscard]] MutationResult supersede_work(WorkId work,
                                              WorkGeneration expected_generation,
                                              const WorkRequest& replacement);
  [[nodiscard]] MutationResult drain_agent(AgentId agent, AgentBootId boot, bool force);
  [[nodiscard]] MutationResult deregister_agent(AgentId agent, AgentBootId boot, bool force);
  [[nodiscard]] MutationResult assignment_ack(const AssignmentBinding& binding, bool accepted, std::string reason);
  [[nodiscard]] MutationResult observation(MessageType type,
                                           const AssignmentBinding& binding,
                                           std::string reason);
  [[nodiscard]] MutationResult dispatch_assignment(AssignmentId assignment,
                                                   AssignmentGeneration generation,
                                                   ScheduleOutcome& outcome);
  [[nodiscard]] MutationResult query(QueryKind kind, std::string& json);
  [[nodiscard]] MutationResult query(QueryKind kind,
                                     AgentId agent,
                                     AgentBootId boot,
                                     WorkId work,
                                     AssignmentId assignment,
                                     std::string& json);

  [[nodiscard]] std::uint64_t frames_received() const noexcept { return frames_received_; }

 private:
  template <class Message>
  [[nodiscard]] MutationResult send(MessageType type, const Message& message, MessageType expected_reply);
  [[nodiscard]] MutationResult send_raw(MessageType type, std::vector<std::uint8_t> payload, MessageType expected_reply);
  [[nodiscard]] bool read_frame(Frame& frame);

  ControlClientOptions options_;
  TcpStream stream_;
  FrameStreamDecoder decoder_;
  std::uint64_t next_correlation_{1};
  std::uint64_t frames_received_{0};
};

}  // namespace agent_scheduler::net
