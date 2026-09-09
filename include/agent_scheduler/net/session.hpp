// Agent Scheduler — bounded, correctly synchronized network session.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "agent_scheduler/ids.hpp"
#include "agent_scheduler/limits.hpp"
#include "agent_scheduler/net/frame.hpp"
#include "agent_scheduler/net/transport.hpp"

namespace agent_scheduler::net {

class Session;

/// Session event sink. Called on the session's reader thread. Implementations must not
/// hold scheduler state locks across socket I/O and must not re-enter the session.
class SessionHandler {
 public:
  virtual ~SessionHandler() = default;
  virtual void on_frame(Session& session, const Frame& frame) = 0;
  virtual void on_closed(Session& session, std::string reason) = 0;
};

struct SessionOptions {
  std::uint32_t max_frame_size{1u << 20};
  std::uint32_t max_send_queue{4096};
  std::uint32_t max_payload_size{1u << 20};
  std::uint16_t protocol_version{1};
};

/// One framed TCP session with a bounded outbound queue and a dedicated writer thread.
/// Writes are serialized by the writer thread, so frames are never interleaved.
class Session final : public std::enable_shared_from_this<Session> {
 public:
  Session(std::uint64_t id, TcpStream stream, SessionOptions options, SessionHandler& handler);
  ~Session();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  void start();
  /// Signals both loops to stop and unblocks the socket. Never blocks.
  void request_close();
  /// Joins the reader and writer threads. Safe to call once, after request_close.
  void join();

  [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
  [[nodiscard]] bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }
  [[nodiscard]] std::string peer() const;
  [[nodiscard]] std::size_t send_queue_depth() const;
  [[nodiscard]] std::uint64_t frames_sent() const noexcept { return frames_sent_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t frames_received() const noexcept {
    return frames_received_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t dropped_frames() const noexcept {
    return dropped_frames_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] const std::string& protocol_violation() const noexcept { return protocol_violation_; }
  void note_protocol_violation(std::string reason);

  /// Queues a frame for the writer thread. Returns false when the bounded queue is
  /// full or the session is closing; the caller must treat that as a delivery failure.
  [[nodiscard]] bool enqueue(Frame frame);

  void bind_agent(AgentId agent, AgentBootId boot);
  void unbind_agent();
  [[nodiscard]] std::optional<std::pair<AgentId, AgentBootId>> bound_agent() const;

 private:
  enum class IoWait : std::uint8_t { Read = 0, Write = 1, Closed = 2 };

  void reader_loop();
  void writer_loop();
  void close_locked(std::string reason);
  /// Blocks until the socket is readable/writable or the session is closing. This is
  /// event-driven, so a close request wakes the reader and writer without relying on
  /// shutdown() to interrupt a pending blocking call.
  [[nodiscard]] IoWait wait_for_io(bool for_write);
  void destroy_events() noexcept;

  std::uint64_t id_;
  TcpStream stream_;
  SessionOptions options_;
  SessionHandler& handler_;

  mutable std::mutex mutex_;
  std::condition_variable writable_;
  std::deque<std::vector<std::uint8_t>> outbound_;
  std::string close_reason_;
  std::string protocol_violation_;
  std::optional<std::pair<AgentId, AgentBootId>> bound_agent_;
  bool closing_{false};
  bool reader_started_{false};
  bool writer_started_{false};
  bool joined_{false};

  std::atomic<bool> closed_{false};
  std::atomic<std::uint64_t> frames_sent_{0};
  std::atomic<std::uint64_t> frames_received_{0};
  std::atomic<std::uint64_t> dropped_frames_{0};

  std::thread reader_;
  std::thread writer_;
  std::uintptr_t socket_event_{0};
  std::uintptr_t close_event_{0};
};

}  // namespace agent_scheduler::net
