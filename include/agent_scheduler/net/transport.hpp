// Agent Scheduler — reference loopback transport (Winsock).
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

#include "agent_scheduler/limits.hpp"
#include "agent_scheduler/result.hpp"

#if !defined(_WIN32)
#error "Agent Scheduler reference transport targets Winsock (Windows). The core scheduler is portable; the reference transport is not."
#endif

namespace agent_scheduler::net {

/// Native socket handle. Kept as an integer so that public headers never include winsock.
using socket_handle = std::uintptr_t;
inline constexpr socket_handle invalid_socket = ~static_cast<socket_handle>(0);

/// Transport failure. Never crosses the session boundary as an exception.
class IoError final : public std::runtime_error {
 public:
  explicit IoError(std::string what) : std::runtime_error(std::move(what)) {}
};

/// Initializes Winsock once per process. Idempotent and thread-safe.
[[nodiscard]] MutationResult initialize_network();
/// Releases Winsock. Safe to call repeatedly; never called implicitly at exit.
void shutdown_network();
[[nodiscard]] std::string last_socket_error();

/// Move-only RAII socket owner.
class Socket {
 public:
  Socket() noexcept = default;
  explicit Socket(socket_handle handle) noexcept : handle_(handle) {}
  ~Socket() { close(); }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept : handle_(other.release()) {}
  Socket& operator=(Socket&& other) noexcept;

  [[nodiscard]] bool valid() const noexcept { return handle_ != invalid_socket; }
  [[nodiscard]] socket_handle handle() const noexcept { return handle_; }
  [[nodiscard]] socket_handle release() noexcept;
  void close() noexcept;
  /// Unblocks a blocked read or write in another thread.
  void shutdown_both() noexcept;

 private:
  socket_handle handle_{invalid_socket};
};

class TcpListener {
 public:
  TcpListener() noexcept = default;
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;
  TcpListener(TcpListener&&) noexcept = default;
  TcpListener& operator=(TcpListener&&) noexcept = default;
  ~TcpListener() { close(); }

  /// Binds to 127.0.0.1 and starts listening. Port 0 selects an ephemeral port.
  [[nodiscard]] MutationResult listen_loopback(std::uint16_t port, std::uint32_t backlog);
  [[nodiscard]] std::optional<std::uint16_t> bound_port() const noexcept { return bound_port_; }
  /// Blocks until a connection arrives or the listener is closed. Returns std::nullopt
  /// when the listener was closed.
  [[nodiscard]] std::optional<Socket> accept_one();
  void close() noexcept;
  [[nodiscard]] bool valid() const noexcept { return socket_.valid(); }

 private:
  Socket socket_;
  std::optional<std::uint16_t> bound_port_;
};

/// Outcome of a non-blocking transfer attempt.
enum class IoResult : std::uint8_t {
  Data = 0,
  WouldBlock = 1,
  Closed = 2,
  Error = 3,
};

class TcpStream {
 public:
  TcpStream() noexcept = default;
  explicit TcpStream(Socket socket) noexcept : socket_(std::move(socket)) {}
  TcpStream(const TcpStream&) = delete;
  TcpStream& operator=(const TcpStream&) = delete;
  TcpStream(TcpStream&&) noexcept = default;
  TcpStream& operator=(TcpStream&&) noexcept = default;

  /// Returns the number of bytes read; zero means the peer closed the connection.
  std::size_t read_some(std::span<std::uint8_t> out);
  /// Writes the entire buffer, looping over partial writes.
  void write_all(std::span<const std::uint8_t> data);
  void shutdown_both() noexcept { socket_.shutdown_both(); }
  void close() noexcept { socket_.close(); }
  void set_no_delay(bool enable) noexcept;
  /// Switches the socket between blocking and non-blocking mode.
  [[nodiscard]] MutationResult set_nonblocking(bool enable) noexcept;
  /// Single non-blocking read attempt. `out` receives the byte count.
  [[nodiscard]] IoResult try_read(std::span<std::uint8_t> out, std::size_t& received) noexcept;
  /// Single non-blocking write attempt. `out` receives the byte count.
  [[nodiscard]] IoResult try_write(std::span<const std::uint8_t> data, std::size_t& written) noexcept;
  [[nodiscard]] bool valid() const noexcept { return socket_.valid(); }
  [[nodiscard]] socket_handle handle() const noexcept { return socket_.handle(); }
  [[nodiscard]] std::string peer() const;

 private:
  Socket socket_;
};

/// Connects to 127.0.0.1:port.
[[nodiscard]] MutationResult connect_loopback(std::uint16_t port, TcpStream& out);

}  // namespace agent_scheduler::net
