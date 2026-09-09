// Agent Scheduler — reference transport session behaviour.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "agent_scheduler/net/frame.hpp"
#include "agent_scheduler/net/session.hpp"
#include "agent_scheduler/net/transport.hpp"
#include "agent_scheduler/version.hpp"
#include "test_support.hpp"

using namespace agent_scheduler;
using namespace agent_scheduler::net;

namespace {

class CountingHandler final : public SessionHandler {
 public:
  void on_frame(Session& session, const Frame& frame) override {
    (void)session;
    (void)frame;
    frames.fetch_add(1);
  }
  void on_closed(Session& session, std::string reason) override {
    (void)session;
    close_reason = std::move(reason);
    closed.store(true);
  }

  std::atomic<int> frames{0};
  std::atomic<bool> closed{false};
  std::string close_reason;
};

struct Pair {
  TcpListener listener;
  TcpStream client;
  TcpStream server;
};

[[nodiscard]] bool make_pair(Pair& pair) {
  if (!pair.listener.listen_loopback(0, 4).ok()) {
    return false;
  }
  const std::uint16_t port = pair.listener.bound_port().value_or(0);
  if (port == 0) {
    return false;
  }
  std::thread connector([&pair, port]() {
    (void)connect_loopback(port, pair.client);
  });
  std::optional<Socket> accepted = pair.listener.accept_one();
  connector.join();
  if (!accepted.has_value() || !pair.client.valid()) {
    return false;
  }
  pair.server = TcpStream{std::move(*accepted)};
  return true;
}

[[nodiscard]] Frame heartbeat(std::uint64_t correlation, std::size_t payload_size) {
  Frame frame;
  frame.header.version = static_cast<std::uint16_t>(protocol_version);
  frame.header.type = static_cast<std::uint16_t>(MessageType::Heartbeat);
  frame.header.correlation = correlation;
  frame.payload.assign(payload_size, 0x5A);
  return frame;
}

/// Reads exactly the expected number of bytes, or stops early on an orderly close.
[[nodiscard]] std::size_t drain_exact(TcpStream& stream, std::size_t expected) {
  std::array<std::uint8_t, 16384> buffer{};
  std::size_t total = 0;
  while (total < expected) {
    const std::size_t received = stream.read_some(std::span<std::uint8_t>(buffer.data(), buffer.size()));
    if (received == 0) {
      break;
    }
    total += received;
  }
  return total;
}

}  // namespace

AS_TEST(net, send_queue_is_bounded_and_writes_are_serialized) {
  Pair pair;
  AS_REQUIRE(make_pair(pair));
  auto handler = std::make_shared<CountingHandler>();
  SessionOptions options;
  options.max_send_queue = 2;
  options.max_frame_size = 1u << 20;
  options.protocol_version = static_cast<std::uint16_t>(protocol_version);
  auto session = std::make_shared<Session>(1, std::move(pair.server), options, *handler);
  session->start();

  std::size_t accepted = 0;
  std::size_t dropped = 0;
  for (std::uint64_t index = 0; index < 4000; ++index) {
    if (session->enqueue(heartbeat(index, 4096))) {
      ++accepted;
    } else {
      ++dropped;
    }
  }
  AS_CHECK(dropped > 0);
  AS_CHECK(accepted + dropped == 4000);
  AS_CHECK(session->dropped_frames() == dropped);

  // Draining the peer lets the writer thread flush everything that was accepted. The
  // expected byte count is exact, so the read cannot block on an open session.
  const std::size_t expected = accepted * (frame_header_bytes + 4096);
  const std::size_t bytes = drain_exact(pair.client, expected);
  AS_CHECK(bytes == expected);
  AS_CHECK(session->frames_sent() > 0);
  session->request_close();
  session->join();
  AS_CHECK(session->closed());
}

AS_TEST(net, protocol_violation_closes_the_session_cleanly) {
  Pair pair;
  AS_REQUIRE(make_pair(pair));
  auto handler = std::make_shared<CountingHandler>();
  SessionOptions options;
  options.max_frame_size = 1u << 16;
  options.protocol_version = static_cast<std::uint16_t>(protocol_version);
  auto session = std::make_shared<Session>(2, std::move(pair.server), options, *handler);
  session->start();

  // A full header's worth of garbage is required before the decoder can judge it.
  std::array<std::uint8_t, 64> garbage{};
  garbage[0] = 0xDE;
  garbage[1] = 0xAD;
  garbage[2] = 0xBE;
  garbage[3] = 0xEF;
  pair.client.write_all(std::span<const std::uint8_t>(garbage.data(), garbage.size()));
  for (int attempt = 0; attempt < 2000 && !handler->closed.load(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  AS_CHECK(handler->closed.load());
  AS_CHECK(!session->protocol_violation().empty());
  session->request_close();
  session->join();
}

AS_TEST(net, unknown_message_type_is_rejected) {
  Pair pair;
  AS_REQUIRE(make_pair(pair));
  auto handler = std::make_shared<CountingHandler>();
  SessionOptions options;
  options.max_frame_size = 1u << 16;
  options.protocol_version = static_cast<std::uint16_t>(protocol_version);
  auto session = std::make_shared<Session>(3, std::move(pair.server), options, *handler);
  session->start();

  Frame unknown = heartbeat(7, 8);
  unknown.header.type = 9999;
  const std::vector<std::uint8_t> bytes = encode_frame(unknown, 1u << 16);
  pair.client.write_all(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
  for (int attempt = 0; attempt < 2000 && !handler->closed.load(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  AS_CHECK(handler->closed.load());
  AS_CHECK(handler->frames.load() == 0);
  AS_CHECK(session->protocol_violation().find("UNKNOWN_MESSAGE_TYPE") != std::string::npos);
  session->request_close();
  session->join();
}

AS_TEST(net, oversized_frame_is_rejected_before_allocation) {
  Pair pair;
  AS_REQUIRE(make_pair(pair));
  auto handler = std::make_shared<CountingHandler>();
  SessionOptions options;
  options.max_frame_size = 64;
  options.protocol_version = static_cast<std::uint16_t>(protocol_version);
  auto session = std::make_shared<Session>(4, std::move(pair.server), options, *handler);
  session->start();

  const std::vector<std::uint8_t> bytes = encode_frame(heartbeat(9, 512), 1u << 16);
  pair.client.write_all(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
  for (int attempt = 0; attempt < 2000 && !handler->closed.load(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  AS_CHECK(handler->closed.load());
  AS_CHECK(handler->frames.load() == 0);
  AS_CHECK(session->protocol_violation().find("PAYLOAD_TOO_LARGE") != std::string::npos);
  session->request_close();
  session->join();
}

AS_TEST(net, close_unblocks_a_blocked_reader_and_releases_the_socket) {
  Pair pair;
  AS_REQUIRE(make_pair(pair));
  auto handler = std::make_shared<CountingHandler>();
  SessionOptions options;
  options.max_frame_size = 1u << 16;
  options.protocol_version = static_cast<std::uint16_t>(protocol_version);
  auto session = std::make_shared<Session>(5, std::move(pair.server), options, *handler);
  session->start();

  // The reader is blocked on recv; request_close must unblock it without a timeout.
  std::thread closer([&session]() { session->request_close(); });
  session->join();
  closer.join();
  AS_CHECK(session->closed());
  AS_CHECK(handler->closed.load());

  // The peer observes an orderly close rather than a hang.
  std::array<std::uint8_t, 8> buffer{};
  AS_CHECK(pair.client.read_some(std::span<std::uint8_t>(buffer.data(), buffer.size())) == 0);
  pair.client.close();
  pair.listener.close();
}
