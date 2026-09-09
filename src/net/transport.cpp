// Agent Scheduler — reference loopback transport (Winsock).
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/net/transport.hpp"

#include <mutex>
#include <string>
#include <utility>

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace agent_scheduler::net {
namespace {

std::once_flag g_wsa_once;
bool g_wsa_ready = false;

[[nodiscard]] bool would_block_error(int code) noexcept {
  return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS;
}

[[nodiscard]] bool closed_error(int code) noexcept {
  switch (code) {
    case WSAECONNRESET:
    case WSAECONNABORTED:
    case WSAENOTSOCK:
    case WSAEINTR:
    case WSAESHUTDOWN:
    case WSAEBADF:
      return true;
    default:
      return false;
  }
}

}  // namespace

MutationResult initialize_network() {
  std::call_once(g_wsa_once, []() {
    WSADATA data{};
    g_wsa_ready = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  });
  if (!g_wsa_ready) {
    return MutationResult::failure(make_error(ErrorCode::Internal, "initialize_network", "",
                                              "WSAStartup failed", ScheduleOutcome::NoChange));
  }
  return MutationResult::success();
}

void shutdown_network() { ::WSACleanup(); }

std::string last_socket_error() { return std::to_string(::WSAGetLastError()); }

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.release();
  }
  return *this;
}

socket_handle Socket::release() noexcept {
  const socket_handle value = handle_;
  handle_ = invalid_socket;
  return value;
}

void Socket::close() noexcept {
  if (handle_ != invalid_socket) {
    ::closesocket(static_cast<SOCKET>(handle_));
    handle_ = invalid_socket;
  }
}

void Socket::shutdown_both() noexcept {
  if (handle_ != invalid_socket) {
    ::shutdown(static_cast<SOCKET>(handle_), SD_BOTH);
  }
}

MutationResult TcpListener::listen_loopback(std::uint16_t port, std::uint32_t backlog) {
  if (const auto init = initialize_network(); !init.ok()) {
    return init;
  }
  SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    return MutationResult::failure(make_error(ErrorCode::Internal, "listen_loopback", "",
                                              "socket() failed: " + last_socket_error(),
                                              ScheduleOutcome::NoChange));
  }
  BOOL exclusive = TRUE;
  (void)::setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = ::htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
    const std::string detail = "bind() failed: " + last_socket_error();
    ::closesocket(listener);
    return MutationResult::failure(
        make_error(ErrorCode::Internal, "listen_loopback", std::to_string(port), detail,
                   ScheduleOutcome::NoChange));
  }
  if (::listen(listener, static_cast<int>(backlog)) == SOCKET_ERROR) {
    const std::string detail = "listen() failed: " + last_socket_error();
    ::closesocket(listener);
    return MutationResult::failure(
        make_error(ErrorCode::Internal, "listen_loopback", std::to_string(port), detail,
                   ScheduleOutcome::NoChange));
  }
  sockaddr_in bound{};
  int length = sizeof(bound);
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
    bound_port_ = ::ntohs(bound.sin_port);
  } else {
    bound_port_ = port;
  }
  socket_ = Socket{static_cast<socket_handle>(listener)};
  return MutationResult::success();
}

std::optional<Socket> TcpListener::accept_one() {
  if (!socket_.valid()) {
    return std::nullopt;
  }
  SOCKET client = ::accept(static_cast<SOCKET>(socket_.handle()), nullptr, nullptr);
  if (client == INVALID_SOCKET) {
    return std::nullopt;
  }
  return Socket{static_cast<socket_handle>(client)};
}

void TcpListener::close() noexcept { socket_.close(); }

std::size_t TcpStream::read_some(std::span<std::uint8_t> out) {
  if (!socket_.valid() || out.empty()) {
    return 0;
  }
  const int received = ::recv(static_cast<SOCKET>(socket_.handle()),
                              reinterpret_cast<char*>(out.data()), static_cast<int>(out.size()), 0);
  if (received == 0) {
    return 0;
  }
  if (received == SOCKET_ERROR) {
    const int code = ::WSAGetLastError();
    if (would_block_error(code) || closed_error(code)) {
      return 0;
    }
    throw IoError("recv() failed: " + std::to_string(code));
  }
  return static_cast<std::size_t>(received);
}

void TcpStream::write_all(std::span<const std::uint8_t> data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const int sent = ::send(static_cast<SOCKET>(socket_.handle()),
                            reinterpret_cast<const char*>(data.data() + offset),
                            static_cast<int>(data.size() - offset), 0);
    if (sent == SOCKET_ERROR) {
      const int code = ::WSAGetLastError();
      if (closed_error(code)) {
        throw IoError("send() failed on a closed socket: " + std::to_string(code));
      }
      throw IoError("send() failed: " + std::to_string(code));
    }
    if (sent == 0) {
      throw IoError("send() returned zero bytes");
    }
    offset += static_cast<std::size_t>(sent);
  }
}

MutationResult TcpStream::set_nonblocking(bool enable) noexcept {
  if (!socket_.valid()) {
    return MutationResult::failure(make_error(ErrorCode::NotStarted, "set_nonblocking", "",
                                              "socket is not valid", ScheduleOutcome::NoChange));
  }
  u_long mode = enable ? 1ul : 0ul;
  if (::ioctlsocket(static_cast<SOCKET>(socket_.handle()), FIONBIO, &mode) == SOCKET_ERROR) {
    return MutationResult::failure(make_error(ErrorCode::Internal, "set_nonblocking", "",
                                              "ioctlsocket(FIONBIO) failed: " + last_socket_error(),
                                              ScheduleOutcome::NoChange));
  }
  return MutationResult::success();
}

IoResult TcpStream::try_read(std::span<std::uint8_t> out, std::size_t& received) noexcept {
  received = 0;
  if (!socket_.valid() || out.empty()) {
    return IoResult::Closed;
  }
  const int result = ::recv(static_cast<SOCKET>(socket_.handle()), reinterpret_cast<char*>(out.data()),
                            static_cast<int>(out.size()), 0);
  if (result > 0) {
    received = static_cast<std::size_t>(result);
    return IoResult::Data;
  }
  if (result == 0) {
    return IoResult::Closed;
  }
  const int code = ::WSAGetLastError();
  if (code == WSAEWOULDBLOCK) {
    return IoResult::WouldBlock;
  }
  if (closed_error(code)) {
    return IoResult::Closed;
  }
  return IoResult::Error;
}

IoResult TcpStream::try_write(std::span<const std::uint8_t> data, std::size_t& written) noexcept {
  written = 0;
  if (!socket_.valid() || data.empty()) {
    return IoResult::Closed;
  }
  const int result = ::send(static_cast<SOCKET>(socket_.handle()),
                            reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0);
  if (result > 0) {
    written = static_cast<std::size_t>(result);
    return IoResult::Data;
  }
  if (result == SOCKET_ERROR) {
    const int code = ::WSAGetLastError();
    if (code == WSAEWOULDBLOCK) {
      return IoResult::WouldBlock;
    }
    if (closed_error(code)) {
      return IoResult::Closed;
    }
    return IoResult::Error;
  }
  return IoResult::Error;
}

void TcpStream::set_no_delay(bool enable) noexcept {
  if (!socket_.valid()) {
    return;
  }
  BOOL value = enable ? TRUE : FALSE;
  (void)::setsockopt(static_cast<SOCKET>(socket_.handle()), IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&value), sizeof(value));
}

std::string TcpStream::peer() const {
  if (!socket_.valid()) {
    return "<closed>";
  }
  sockaddr_in address{};
  int length = sizeof(address);
  if (::getpeername(static_cast<SOCKET>(socket_.handle()), reinterpret_cast<sockaddr*>(&address), &length) !=
      0) {
    return "<unknown>";
  }
  char text[INET_ADDRSTRLEN] = {0};
  ::inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text));
  return std::string(text) + ":" + std::to_string(::ntohs(address.sin_port));
}

MutationResult connect_loopback(std::uint16_t port, TcpStream& out) {
  if (const auto init = initialize_network(); !init.ok()) {
    return init;
  }
  SOCKET client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (client == INVALID_SOCKET) {
    return MutationResult::failure(make_error(ErrorCode::Internal, "connect_loopback", "",
                                              "socket() failed: " + last_socket_error(),
                                              ScheduleOutcome::NoChange));
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = ::htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
    const std::string detail = "connect() failed: " + last_socket_error();
    ::closesocket(client);
    return MutationResult::failure(
        make_error(ErrorCode::Internal, "connect_loopback", std::to_string(port), detail,
                   ScheduleOutcome::NoChange));
  }
  out = TcpStream{Socket{static_cast<socket_handle>(client)}};
  out.set_no_delay(true);
  return MutationResult::success();
}

}  // namespace agent_scheduler::net
