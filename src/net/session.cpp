// Agent Scheduler — bounded, correctly synchronized network session.
//
// Session I/O is event-driven (WSAEventSelect plus a per-session close event) rather
// than blocking recv/send. On Windows a pending blocking recv is not reliably
// interrupted by shutdown(), so a close request must wake the reader and writer through
// an event instead of relying on socket state changes.
//
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/net/session.hpp"

#include <array>
#include <string>
#include <utility>
#include <vector>

#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <winsock2.h>

namespace agent_scheduler::net {
namespace {

[[nodiscard]] HANDLE as_handle(std::uintptr_t value) noexcept { return reinterpret_cast<HANDLE>(value); }

}  // namespace

Session::Session(std::uint64_t id, TcpStream stream, SessionOptions options, SessionHandler& handler)
    : id_(id), stream_(std::move(stream)), options_(options), handler_(handler) {
  if (stream_.valid()) {
    const WSAEVENT socket_event = ::WSACreateEvent();
    const WSAEVENT close_event = ::WSACreateEvent();
    if (socket_event != WSA_INVALID_EVENT && close_event != WSA_INVALID_EVENT) {
      socket_event_ = reinterpret_cast<std::uintptr_t>(socket_event);
      close_event_ = reinterpret_cast<std::uintptr_t>(close_event);
      if (::WSAEventSelect(static_cast<SOCKET>(stream_.handle()), socket_event,
                           FD_READ | FD_WRITE | FD_CLOSE) == SOCKET_ERROR) {
        destroy_events();
      }
    } else {
      if (socket_event != WSA_INVALID_EVENT) {
        ::WSACloseEvent(socket_event);
      }
      if (close_event != WSA_INVALID_EVENT) {
        ::WSACloseEvent(close_event);
      }
    }
  }
}

Session::~Session() {
  request_close();
  join();
}

void Session::destroy_events() noexcept {
  if (socket_event_ != 0) {
    ::WSACloseEvent(as_handle(socket_event_));
    socket_event_ = 0;
  }
  if (close_event_ != 0) {
    ::WSACloseEvent(as_handle(close_event_));
    close_event_ = 0;
  }
}

void Session::start() {
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (reader_started_ || writer_started_) {
      return;
    }
    reader_started_ = true;
    writer_started_ = true;
  }
  reader_ = std::thread([this]() { reader_loop(); });
  writer_ = std::thread([this]() { writer_loop(); });
}

void Session::request_close() {
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (!closing_) {
      closing_ = true;
      close_reason_ = "close requested";
    }
    outbound_.clear();
  }
  closed_.store(true, std::memory_order_release);
  if (close_event_ != 0) {
    ::WSASetEvent(as_handle(close_event_));
  }
  stream_.shutdown_both();
  writable_.notify_all();
}

void Session::join() {
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (joined_) {
      return;
    }
    joined_ = true;
  }
  if (reader_.joinable()) {
    reader_.join();
  }
  if (writer_.joinable()) {
    writer_.join();
  }
  stream_.close();
  destroy_events();
}

std::string Session::peer() const { return stream_.peer(); }

std::size_t Session::send_queue_depth() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return outbound_.size();
}

void Session::note_protocol_violation(std::string reason) {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (protocol_violation_.empty()) {
    protocol_violation_ = std::move(reason);
  }
}

bool Session::enqueue(Frame frame) {
  std::vector<std::uint8_t> bytes;
  try {
    bytes = encode_frame(frame, options_.max_frame_size);
  } catch (const std::exception&) {
    dropped_frames_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (closing_ || closed_.load(std::memory_order_acquire)) {
      dropped_frames_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    if (outbound_.size() >= options_.max_send_queue) {
      dropped_frames_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    outbound_.push_back(std::move(bytes));
  }
  writable_.notify_one();
  return true;
}

void Session::bind_agent(AgentId agent, AgentBootId boot) {
  const std::lock_guard<std::mutex> guard(mutex_);
  bound_agent_ = std::make_pair(agent, boot);
}

void Session::unbind_agent() {
  const std::lock_guard<std::mutex> guard(mutex_);
  bound_agent_.reset();
}

std::optional<std::pair<AgentId, AgentBootId>> Session::bound_agent() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return bound_agent_;
}

void Session::close_locked(std::string reason) {
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (closing_) {
      return;
    }
    closing_ = true;
    close_reason_ = std::move(reason);
    outbound_.clear();
  }
  closed_.store(true, std::memory_order_release);
  if (close_event_ != 0) {
    ::WSASetEvent(as_handle(close_event_));
  }
  writable_.notify_all();
}

Session::IoWait Session::wait_for_io(bool for_write) {
  if (socket_event_ == 0 || close_event_ == 0) {
    return IoWait::Closed;
  }
  HANDLE events[2] = {as_handle(socket_event_), as_handle(close_event_)};
  const DWORD index = ::WSAWaitForMultipleEvents(2, events, FALSE, WSA_INFINITE, FALSE);
  if (index == WSA_WAIT_EVENT_0 + 1) {
    return IoWait::Closed;
  }
  if (index != WSA_WAIT_EVENT_0) {
    return IoWait::Closed;
  }
  WSANETWORKEVENTS network_events{};
  if (::WSAEnumNetworkEvents(static_cast<SOCKET>(stream_.handle()), as_handle(socket_event_),
                             &network_events) == SOCKET_ERROR) {
    return IoWait::Closed;
  }
  if ((network_events.lNetworkEvents & FD_CLOSE) != 0) {
    return IoWait::Closed;
  }
  if (closed_.load(std::memory_order_acquire)) {
    return IoWait::Closed;
  }
  return for_write ? IoWait::Write : IoWait::Read;
}

void Session::reader_loop() {
  FrameStreamDecoder decoder(options_.max_frame_size, options_.protocol_version);
  std::array<std::uint8_t, 16384> buffer{};
  bool finished = false;
  while (!finished && !closed_.load(std::memory_order_acquire)) {
    if (wait_for_io(false) == IoWait::Closed) {
      break;
    }
    for (;;) {
      std::size_t received = 0;
      const IoResult result = stream_.try_read(std::span<std::uint8_t>(buffer.data(), buffer.size()), received);
      if (result == IoResult::WouldBlock) {
        break;
      }
      if (result == IoResult::Closed) {
        finished = true;
        break;
      }
      if (result == IoResult::Error) {
        note_protocol_violation("read failed");
        finished = true;
        break;
      }
      decoder.append(std::span<const std::uint8_t>(buffer.data(), received));
      for (;;) {
        FrameCodecStatus status = FrameCodecStatus::NeedMoreData;
        std::optional<Frame> frame = decoder.next(status);
        if (frame.has_value()) {
          frames_received_.fetch_add(1, std::memory_order_relaxed);
          handler_.on_frame(*this, *frame);
          if (closed_.load(std::memory_order_acquire)) {
            finished = true;
            break;
          }
          continue;
        }
        if (status != FrameCodecStatus::NeedMoreData) {
          note_protocol_violation(std::string("frame rejected: ") + to_string(status));
          close_locked(std::string("protocol violation: ") + to_string(status));
          finished = true;
        }
        break;
      }
    }
  }
  close_locked("peer closed or read ended");
  handler_.on_closed(*this, "reader loop finished");
}

void Session::writer_loop() {
  std::vector<std::uint8_t> frame;
  std::size_t offset = 0;
  for (;;) {
    if (offset == frame.size()) {
      frame.clear();
      offset = 0;
      std::unique_lock<std::mutex> lock(mutex_);
      writable_.wait(lock, [this]() { return closing_ || !outbound_.empty(); });
      if (closing_) {
        return;
      }
      frame = std::move(outbound_.front());
      outbound_.pop_front();
    }
    std::size_t written = 0;
    const IoResult result = stream_.try_write(
        std::span<const std::uint8_t>(frame.data() + offset, frame.size() - offset), written);
    offset += written;
    if (result == IoResult::WouldBlock) {
      if (wait_for_io(true) == IoWait::Closed) {
        return;
      }
      continue;
    }
    if (result == IoResult::Closed || result == IoResult::Error) {
      note_protocol_violation("write failed");
      close_locked("write failed");
      return;
    }
    if (offset == frame.size()) {
      frames_sent_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

}  // namespace agent_scheduler::net
