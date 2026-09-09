// Agent Scheduler — bounded framed transport codec.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/net/frame.hpp"

#include <cstring>
#include <utility>

namespace agent_scheduler::net {
namespace {

constexpr std::size_t kHeaderCrcOffset = 28;

[[nodiscard]] std::uint16_t read_u16(const std::uint8_t* data) noexcept {
  return static_cast<std::uint16_t>(data[0]) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8);
}

[[nodiscard]] std::uint32_t read_u32(const std::uint8_t* data) noexcept {
  return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) | (static_cast<std::uint32_t>(data[3]) << 24);
}

[[nodiscard]] std::uint64_t read_u64(const std::uint8_t* data) noexcept {
  std::uint64_t value = 0;
  for (int index = 7; index >= 0; --index) {
    value = (value << 8) | data[index];
  }
  return value;
}

}  // namespace

const char* to_string(MessageType value) noexcept {
  switch (value) {
    case MessageType::Hello: return "HELLO";
    case MessageType::HelloResult: return "HELLO_RESULT";
    case MessageType::RegisterAgent: return "REGISTER_AGENT";
    case MessageType::RegisterResult: return "REGISTER_RESULT";
    case MessageType::PublishCapabilities: return "PUBLISH_CAPABILITIES";
    case MessageType::PublishCapabilitiesResult: return "PUBLISH_CAPABILITIES_RESULT";
    case MessageType::PublishHealth: return "PUBLISH_HEALTH";
    case MessageType::PublishHealthResult: return "PUBLISH_HEALTH_RESULT";
    case MessageType::PublishAvailability: return "PUBLISH_AVAILABILITY";
    case MessageType::PublishAvailabilityResult: return "PUBLISH_AVAILABILITY_RESULT";
    case MessageType::PublishLoad: return "PUBLISH_LOAD";
    case MessageType::PublishLoadResult: return "PUBLISH_LOAD_RESULT";
    case MessageType::Heartbeat: return "HEARTBEAT";
    case MessageType::HeartbeatResult: return "HEARTBEAT_RESULT";
    case MessageType::SubmitWork: return "SUBMIT_WORK";
    case MessageType::SubmitWorkResult: return "SUBMIT_WORK_RESULT";
    case MessageType::CancelWork: return "CANCEL_WORK";
    case MessageType::CancelWorkResult: return "CANCEL_WORK_RESULT";
    case MessageType::SupersedeWork: return "SUPERSEDE_WORK";
    case MessageType::SupersedeWorkResult: return "SUPERSEDE_WORK_RESULT";
    case MessageType::AssignWork: return "ASSIGN_WORK";
    case MessageType::AssignmentAck: return "ASSIGNMENT_ACK";
    case MessageType::AssignmentReject: return "ASSIGNMENT_REJECT";
    case MessageType::WorkStarted: return "WORK_STARTED";
    case MessageType::WorkReleased: return "WORK_RELEASED";
    case MessageType::WorkCompleted: return "WORK_COMPLETED";
    case MessageType::WorkFailed: return "WORK_FAILED";
    case MessageType::DrainAgent: return "DRAIN_AGENT";
    case MessageType::DrainAgentResult: return "DRAIN_AGENT_RESULT";
    case MessageType::DeregisterAgent: return "DEREGISTER_AGENT";
    case MessageType::DeregisterAgentResult: return "DEREGISTER_AGENT_RESULT";
    case MessageType::Query: return "QUERY";
    case MessageType::QueryResult: return "QUERY_RESULT";
    case MessageType::DispatchAssignment: return "DISPATCH_ASSIGNMENT";
    case MessageType::DispatchAssignmentResult: return "DISPATCH_ASSIGNMENT_RESULT";
    case MessageType::Error: return "ERROR";
    case MessageType::ShutdownNotice: return "SHUTDOWN_NOTICE";
  }
  return "UNKNOWN";
}

bool is_known_message_type(std::uint16_t value) noexcept {
  return value >= static_cast<std::uint16_t>(MessageType::Hello) &&
         value <= static_cast<std::uint16_t>(MessageType::ShutdownNotice);
}

const char* to_string(FrameCodecStatus value) noexcept {
  switch (value) {
    case FrameCodecStatus::Ok: return "OK";
    case FrameCodecStatus::NeedMoreData: return "NEED_MORE_DATA";
    case FrameCodecStatus::BadMagic: return "BAD_MAGIC";
    case FrameCodecStatus::BadVersion: return "BAD_VERSION";
    case FrameCodecStatus::BadHeaderCrc: return "BAD_HEADER_CRC";
    case FrameCodecStatus::UnknownMessageType: return "UNKNOWN_MESSAGE_TYPE";
    case FrameCodecStatus::PayloadTooLarge: return "PAYLOAD_TOO_LARGE";
    case FrameCodecStatus::BadPayloadCrc: return "BAD_PAYLOAD_CRC";
    case FrameCodecStatus::TruncatedPayload: return "TRUNCATED_PAYLOAD";
  }
  return "UNKNOWN";
}

std::vector<std::uint8_t> encode_frame(const Frame& frame, std::uint32_t max_frame_size) {
  if (frame.payload.size() > max_frame_size) {
    throw DecodeError("frame payload exceeds the configured maximum frame size");
  }
  ByteWriter writer;
  writer.put_u32(frame_magic);
  writer.put_u16(frame.header.version);
  writer.put_u16(frame.header.type);
  writer.put_u16(frame.header.flags);
  writer.put_u16(frame.header.reserved);
  writer.put_u64(frame.header.correlation);
  writer.put_u32(static_cast<std::uint32_t>(frame.payload.size()));
  writer.put_u32(crc32(frame.payload));
  writer.put_u32(0);
  std::vector<std::uint8_t> header = writer.take();
  const std::uint32_t header_crc =
      crc32(std::span<const std::uint8_t>(header.data(), kHeaderCrcOffset));
  std::memcpy(header.data() + kHeaderCrcOffset, &header_crc, sizeof(header_crc));
  header.insert(header.end(), frame.payload.begin(), frame.payload.end());
  return header;
}

FrameStreamDecoder::FrameStreamDecoder(std::uint32_t max_frame_size, std::uint16_t expected_version) noexcept
    : max_frame_size_(max_frame_size), expected_version_(expected_version) {}

void FrameStreamDecoder::append(std::span<const std::uint8_t> data) {
  if (consumed_ > 0 && consumed_ == buffer_.size()) {
    buffer_.clear();
    consumed_ = 0;
  } else if (consumed_ > 4096) {
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(consumed_));
    consumed_ = 0;
  }
  buffer_.insert(buffer_.end(), data.begin(), data.end());
}

void FrameStreamDecoder::reset() noexcept {
  buffer_.clear();
  consumed_ = 0;
}

std::optional<Frame> FrameStreamDecoder::next(FrameCodecStatus& status) {
  status = FrameCodecStatus::NeedMoreData;
  if (buffer_.size() - consumed_ < frame_header_bytes) {
    return std::nullopt;
  }
  const std::uint8_t* header = buffer_.data() + consumed_;
  if (read_u32(header) != frame_magic) {
    status = FrameCodecStatus::BadMagic;
    reset();
    return std::nullopt;
  }
  const std::uint16_t version = read_u16(header + 4);
  if (version != expected_version_) {
    status = FrameCodecStatus::BadVersion;
    reset();
    return std::nullopt;
  }
  const std::uint16_t type = read_u16(header + 6);
  if (!is_known_message_type(type)) {
    status = FrameCodecStatus::UnknownMessageType;
    reset();
    return std::nullopt;
  }
  const std::uint32_t stored_header_crc = read_u32(header + kHeaderCrcOffset);
  const std::uint32_t computed_header_crc =
      crc32(std::span<const std::uint8_t>(header, kHeaderCrcOffset));
  if (stored_header_crc != computed_header_crc) {
    status = FrameCodecStatus::BadHeaderCrc;
    reset();
    return std::nullopt;
  }
  const std::uint32_t payload_length = read_u32(header + 20);
  if (payload_length > max_frame_size_) {
    status = FrameCodecStatus::PayloadTooLarge;
    reset();
    return std::nullopt;
  }
  const std::size_t total = frame_header_bytes + static_cast<std::size_t>(payload_length);
  if (buffer_.size() - consumed_ < total) {
    return std::nullopt;
  }
  const std::uint8_t* payload = header + frame_header_bytes;
  if (crc32(std::span<const std::uint8_t>(payload, payload_length)) != read_u32(header + 24)) {
    status = FrameCodecStatus::BadPayloadCrc;
    reset();
    return std::nullopt;
  }
  Frame frame;
  frame.header.magic = frame_magic;
  frame.header.version = version;
  frame.header.type = type;
  frame.header.flags = read_u16(header + 8);
  frame.header.reserved = read_u16(header + 10);
  frame.header.correlation = read_u64(header + 12);
  frame.header.payload_length = payload_length;
  frame.header.payload_crc32 = read_u32(header + 24);
  frame.header.header_crc32 = stored_header_crc;
  frame.payload.assign(payload, payload + payload_length);
  consumed_ += total;
  status = FrameCodecStatus::Ok;
  return frame;
}

}  // namespace agent_scheduler::net
