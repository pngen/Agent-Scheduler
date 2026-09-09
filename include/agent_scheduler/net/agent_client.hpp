// Agent Scheduler — reference agent process client.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agent_scheduler/agent.hpp"
#include "agent_scheduler/limits.hpp"
#include "agent_scheduler/net/protocol.hpp"
#include "agent_scheduler/net/session.hpp"

namespace agent_scheduler::net {

struct AgentClientOptions {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  AgentDescriptor descriptor;
  CapabilityProfile profile;
  HealthObservation health;
  AvailabilityObservation availability;
  LoadObservation load;
  ResourceLimits limits{};
  /// Acknowledge accepted assignments automatically.
  bool auto_ack{true};
  /// Report WORK_STARTED, then WORK_COMPLETED after simulated_work_ms.
  bool auto_complete{true};
  /// Simulated handling duration in milliseconds. Zero reports completion immediately.
  std::uint64_t simulated_work_ms{0};
  /// Stop the run loop after this many milliseconds. Zero runs until stopped.
  std::uint64_t run_duration_ms{0};
  std::uint64_t heartbeat_interval_ms{1000};
};

/// Connects to a coordinator, registers an incarnation, publishes evidence, and handles
/// assignment traffic. Used by the agent tool and by the multiprocess proof harness.
class AgentClient final : public SessionHandler {
 public:
  explicit AgentClient(AgentClientOptions options);
  ~AgentClient() override;

  AgentClient(const AgentClient&) = delete;
  AgentClient& operator=(const AgentClient&) = delete;

  [[nodiscard]] MutationResult connect_and_register();
  [[nodiscard]] MutationResult publish_capabilities();
  [[nodiscard]] MutationResult publish_health();
  [[nodiscard]] MutationResult publish_availability();
  [[nodiscard]] MutationResult publish_load();
  [[nodiscard]] MutationResult heartbeat();
  /// Processes pending inbound frames. Returns the number of frames handled.
  std::size_t pump();
  /// Runs the heartbeat/assignment loop until stopped, deregistered, or the configured
  /// run duration elapses.
  [[nodiscard]] MutationResult run(std::atomic<bool>& stop_flag);
  void stop();

  [[nodiscard]] bool connected() const noexcept { return session_ != nullptr && !session_->closed(); }
  [[nodiscard]] const AgentDescriptor& descriptor() const noexcept { return options_.descriptor; }
  [[nodiscard]] std::vector<AssignWorkMessage> received_assignments() const;
  [[nodiscard]] std::uint64_t frames_handled() const noexcept {
    return frames_handled_.load(std::memory_order_relaxed);
  }

  std::function<void(const AssignWorkMessage&)> on_assignment;
  std::function<void()> on_drain;
  std::function<void()> on_shutdown;

  // SessionHandler
  void on_frame(Session& session, const Frame& frame) override;
  void on_closed(Session& session, std::string reason) override;

 private:
  [[nodiscard]] MutationResult send_and_wait(MessageType type,
                                             std::vector<std::uint8_t> payload,
                                             MessageType expected_reply);
  void handle_frame(const Frame& frame);
  void report_observation(MessageType type, const AssignmentBinding& binding, std::string reason);

  AgentClientOptions options_;
  std::unique_ptr<Session> session_;
  std::atomic<bool> stopped_{false};

  mutable std::mutex mutex_;
  std::vector<AssignWorkMessage> assignments_;
  std::atomic<std::uint64_t> frames_handled_{0};

  std::mutex reply_mutex_;
  std::condition_variable reply_cv_;
  std::optional<ResultMessage> last_result_;
  std::uint64_t last_correlation_{0};
  std::atomic<std::uint64_t> next_correlation_{1};
};

}  // namespace agent_scheduler::net
