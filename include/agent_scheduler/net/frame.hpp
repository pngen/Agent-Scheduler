// Agent Scheduler — bounded framed transport codec.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "agent_scheduler/codec.hpp"
#include "agent_scheduler/digest.hpp"

namespace agent_scheduler::net {

/// Frame magic: 'A','G','S','D' in little-endian byte order.
inline constexpr std::uint32_t frame_magic = 0x44534741u;
inline constexpr std::uint32_t frame_header_bytes = 32;
inline constexpr std::uint16_t frame_flag_none = 0;

enum class MessageType : std::uint16_t {
  Hello = 1,
  HelloResult = 2,
  RegisterAgent = 3,
  RegisterResult = 4,
  PublishCapabilities = 5,
  PublishCapabilitiesResult = 6,
  PublishHealth = 7,
  PublishHealthResult = 8,
  PublishAvailability = 9,
  PublishAvailabilityResult = 10,
  PublishLoad = 11,
  PublishLoadResult = 12,
  Heartbeat = 13,
  HeartbeatResult = 14,
  SubmitWork = 15,
  SubmitWorkResult = 16,
  CancelWork = 17,
  CancelWorkResult = 18,
  SupersedeWork = 19,
  SupersedeWorkResult = 20,
  AssignWork = 21,
  AssignmentAck = 22,
  AssignmentReject = 23,
  WorkStarted = 24,
  WorkReleased = 25,
  WorkCompleted = 26,
  WorkFailed = 27,
  DrainAgent = 28,
  DrainAgentResult = 29,
  DeregisterAgent = 30,
  DeregisterAgentResult = 31,
  Query = 32,
  QueryResult = 33,
  DispatchAssignment = 34,
  DispatchAssignmentResult = 35,
  Error = 36,
  ShutdownNotice = 37,
};

[[nodiscard]] const char* to_string(MessageType value) noexcept;
[[nodiscard]] bool is_known_message_type(std::uint16_t value) noexcept;

struct FrameHeader {
  std::uint32_t magic{frame_magic};
  std::uint16_t version{0};
  std::uint16_t type{0};
  std::uint16_t flags{frame_flag_none};
  std::uint16_t reserved{0};
  std::uint64_t correlation{0};
  std::uint32_t payload_length{0};
  std::uint32_t payload_crc32{0};
  std::uint32_t header_crc32{0};
};

struct Frame {
  FrameHeader header;
  std::vector<std::uint8_t> payload;
};

enum class FrameCodecStatus {
  Ok = 0,
  NeedMoreData = 1,
  BadMagic = 2,
  BadVersion = 3,
  BadHeaderCrc = 4,
  UnknownMessageType = 5,
  PayloadTooLarge = 6,
  BadPayloadCrc = 7,
  TruncatedPayload = 8,
};

[[nodiscard]] const char* to_string(FrameCodecStatus value) noexcept;

/// Encodes a complete frame including header and integrity fields.
[[nodiscard]] std::vector<std::uint8_t> encode_frame(const Frame& frame, std::uint32_t max_frame_size);

/// Incremental strict frame decoder over a byte stream. Rejects magic, version, header
/// checksum, payload checksum, unknown types, oversized payloads, and truncation.
class FrameStreamDecoder {
 public:
  explicit FrameStreamDecoder(std::uint32_t max_frame_size, std::uint16_t expected_version) noexcept;

  void append(std::span<const std::uint8_t> data);
  /// Returns the next complete frame, or std::nullopt. On a protocol violation the
  /// status is set and the decoder is reset to a clean state.
  [[nodiscard]] std::optional<Frame> next(FrameCodecStatus& status);
  void reset() noexcept;
  [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size() - consumed_; }

 private:
  std::vector<std::uint8_t> buffer_;
  std::size_t consumed_{0};
  std::uint32_t max_frame_size_;
  std::uint16_t expected_version_;
};

}  // namespace agent_scheduler::net
