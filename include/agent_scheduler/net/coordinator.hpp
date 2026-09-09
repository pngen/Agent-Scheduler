// Agent Scheduler — coordinator server: framed TCP front end for the scheduler.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "agent_scheduler/limits.hpp"
#include "agent_scheduler/net/protocol.hpp"
#include "agent_scheduler/net/session.hpp"
#include "agent_scheduler/scheduler.hpp"

namespace agent_scheduler::net {

struct CoordinatorOptions {
  std::uint16_t port{0};
  std::string bind_address{"127.0.0.1"};
  ResourceLimits limits{};
  /// Optional durable state file. Loaded at start when present, saved on stop.
  std::filesystem::path persistence_path;
  bool load_on_start{true};
  bool persist_on_stop{true};
  /// Dispatch newly created assignments immediately.
  bool auto_dispatch{true};
  /// Publish a ShutdownNotice to connected sessions during stop.
  bool notify_sessions_on_stop{true};
};

/// Accepts framed TCP connections, decodes requests, and drives the scheduler. The
/// authoritative scheduler lock is never held across socket I/O: request handling locks
/// the scheduler, releases, then enqueues the response frame.
class CoordinatorServer final : public SessionHandler {
 public:
  explicit CoordinatorServer(AgentScheduler& scheduler, CoordinatorOptions options = {});
  ~CoordinatorServer();

  CoordinatorServer(const CoordinatorServer&) = delete;
  CoordinatorServer& operator=(const CoordinatorServer&) = delete;

  [[nodiscard]] MutationResult start();
  [[nodiscard]] MutationResult stop();

  [[nodiscard]] std::optional<std::uint16_t> port() const noexcept { return port_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::size_t connection_count() const;
  [[nodiscard]] std::uint64_t frames_processed() const noexcept {
    return frames_processed_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t protocol_violations() const noexcept {
    return protocol_violations_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t rejected_frames() const noexcept {
    return rejected_frames_.load(std::memory_order_relaxed);
  }

  /// Delivers an assignment to the incarnation's session. Returns false when the
  /// incarnation has no live session or its send queue is saturated.
  [[nodiscard]] bool push_assignment(AgentId agent, AgentBootId boot, const AssignWorkMessage& message);

  // SessionHandler
  void on_frame(Session& session, const Frame& frame) override;
  void on_closed(Session& session, std::string reason) override;

 private:
  void accept_loop();
  void reap_loop();
  void handle_frame(std::shared_ptr<Session> session, const Frame& frame);
  void send_result(std::shared_ptr<Session> session,
                   MessageType type,
                   std::uint64_t correlation,
                   const MutationResult& result,
                   ScheduleOutcome outcome);
  void send_error(std::shared_ptr<Session> session,
                  std::uint64_t correlation,
                  ErrorCode code,
                  std::string operation,
                  std::string subject,
                  std::string detail);
  [[nodiscard]] std::string query_json(const QueryMessage& query) const;
  /// Runs one scheduling pass and pushes dispatched assignments to bound sessions.
  void run_scheduling_pass();
  void forget_agent_session(AgentId agent, AgentBootId boot);

  AgentScheduler& scheduler_;
  CoordinatorOptions options_;
  TcpListener listener_;
  std::atomic<std::uint16_t> port_{0};
  std::atomic<bool> running_{false};

  mutable std::mutex sessions_mutex_;
  std::map<std::uint64_t, std::shared_ptr<Session>> sessions_;
  std::map<AgentId, std::pair<AgentBootId, std::uint64_t>> agent_sessions_;

  // Sessions are destroyed on a dedicated reaper thread. A session's own reader thread
  // is the thread that observes its close, and destroying it there would make the
  // session join itself.
  std::mutex reap_mutex_;
  std::condition_variable reap_cv_;
  std::deque<std::shared_ptr<Session>> reap_queue_;
  bool reap_stopping_{false};
  std::thread reap_thread_;

  std::thread accept_thread_;
  std::atomic<std::uint64_t> next_session_id_{1};
  std::atomic<std::uint64_t> frames_processed_{0};
  std::atomic<std::uint64_t> protocol_violations_{0};
  std::atomic<std::uint64_t> rejected_frames_{0};
  bool joined_{false};
};

}  // namespace agent_scheduler::net
