// Agent Scheduler — framed transport codec and wire protocol.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include <limits>
#include <vector>

#include "agent_scheduler/net/frame.hpp"
#include "agent_scheduler/version.hpp"
#include "agent_scheduler/net/protocol.hpp"
#include "scheduler_fixture.hpp"
#include "test_support.hpp"

using namespace agent_scheduler;
using namespace agent_scheduler::net;
using namespace as_fixture;

namespace {

[[nodiscard]] ResourceLimits default_limits() { return ResourceLimits{}; }

[[nodiscard]] Frame make_frame(MessageType type, std::uint64_t correlation, std::vector<std::uint8_t> payload) {
  Frame frame;
  frame.header.version = static_cast<std::uint16_t>(protocol_version);
  frame.header.type = static_cast<std::uint16_t>(type);
  frame.header.correlation = correlation;
  frame.payload = std::move(payload);
  return frame;
}

template <class Message>
[[nodiscard]] std::vector<std::uint8_t> encode_of(const Message& message) {
  ByteWriter writer;
  encode(writer, message, default_limits());
  return writer.take();
}

[[nodiscard]] FrameCodecStatus decode_status(const std::vector<std::uint8_t>& bytes,
                                             std::uint32_t max_frame_size = 1u << 20) {
  FrameStreamDecoder decoder(max_frame_size, static_cast<std::uint16_t>(protocol_version));
  decoder.append(bytes);
  FrameCodecStatus status = FrameCodecStatus::NeedMoreData;
  (void)decoder.next(status);
  return status;
}

}  // namespace

AS_TEST(protocol, frame_round_trip_preserves_identity_and_payload) {
  const Frame frame = make_frame(MessageType::Query, 0x123456789ABCDEFull, {1, 2, 3, 4, 5});
  const std::vector<std::uint8_t> bytes = encode_frame(frame, 1u << 20);
  AS_CHECK(bytes.size() == frame_header_bytes + 5);
  FrameStreamDecoder decoder(1u << 20, static_cast<std::uint16_t>(protocol_version));
  decoder.append(bytes);
  FrameCodecStatus status = FrameCodecStatus::NeedMoreData;
  const auto decoded = decoder.next(status);
  AS_REQUIRE(decoded.has_value());
  AS_CHECK(status == FrameCodecStatus::Ok);
  AS_CHECK(decoded->header.type == frame.header.type);
  AS_CHECK(decoded->header.correlation == frame.header.correlation);
  AS_CHECK(decoded->payload == frame.payload);
  AS_CHECK(decoder.buffered() == 0);
}

AS_TEST(protocol, malformed_frames_are_rejected) {
  const Frame frame = make_frame(MessageType::Heartbeat, 7, {9, 9, 9});
  const std::vector<std::uint8_t> good = encode_frame(frame, 1u << 20);

  { auto bytes = good; bytes[0] = 0x00; AS_CHECK(decode_status(bytes) == FrameCodecStatus::BadMagic); }
  { auto bytes = good; bytes[4] = 0x09; AS_CHECK(decode_status(bytes) == FrameCodecStatus::BadVersion); }
  { auto bytes = good; bytes[6] = 0x7F; AS_CHECK(decode_status(bytes) == FrameCodecStatus::UnknownMessageType); }
  { auto bytes = good; bytes[10] = static_cast<std::uint8_t>(bytes[10] ^ 0x40); AS_CHECK(decode_status(bytes) == FrameCodecStatus::BadHeaderCrc); }
  { auto bytes = good; bytes.back() = static_cast<std::uint8_t>(bytes.back() ^ 0xFF); AS_CHECK(decode_status(bytes) == FrameCodecStatus::BadPayloadCrc); }
  {
    auto bytes = good;
    FrameStreamDecoder decoder(2, static_cast<std::uint16_t>(protocol_version));
    decoder.append(bytes);
    FrameCodecStatus status = FrameCodecStatus::NeedMoreData;
    (void)decoder.next(status);
    AS_CHECK(status == FrameCodecStatus::PayloadTooLarge);
  }
  {
    FrameStreamDecoder decoder(1u << 20, static_cast<std::uint16_t>(protocol_version));
    decoder.append(std::span<const std::uint8_t>(good.data(), good.size() - 2));
    FrameCodecStatus status = FrameCodecStatus::NeedMoreData;
    AS_CHECK(!decoder.next(status).has_value());
    AS_CHECK(status == FrameCodecStatus::NeedMoreData);
  }
}

AS_TEST(protocol, decoder_handles_split_and_pipelined_frames) {
  const std::vector<std::uint8_t> first = encode_frame(make_frame(MessageType::Heartbeat, 1, {1}), 1u << 20);
  const std::vector<std::uint8_t> second = encode_frame(make_frame(MessageType::Heartbeat, 2, {2, 2}), 1u << 20);
  std::vector<std::uint8_t> stream = first;
  stream.insert(stream.end(), second.begin(), second.end());

  FrameStreamDecoder decoder(1u << 20, static_cast<std::uint16_t>(protocol_version));
  for (std::size_t index = 0; index < stream.size(); ++index) {
    decoder.append(std::span<const std::uint8_t>(&stream[index], 1));
  }
  FrameCodecStatus status = FrameCodecStatus::NeedMoreData;
  const auto one = decoder.next(status);
  AS_REQUIRE(one.has_value());
  AS_CHECK(one->header.correlation == 1);
  const auto two = decoder.next(status);
  AS_REQUIRE(two.has_value());
  AS_CHECK(two->header.correlation == 2);
  AS_CHECK(decoder.buffered() == 0);
}

AS_TEST(protocol, message_round_trips) {
  const ResourceLimits limits = default_limits();
  const AgentDescriptor descriptor = make_agent(5, 55, 3);
  {
    RegisterAgentMessage message;
    message.descriptor = descriptor;
    const std::vector<std::uint8_t> payload_bytes = encode_of(message);
    ByteReader reader(payload_bytes);
    RegisterAgentMessage decoded;
    decode(reader, decoded, limits);
    reader.require_exhausted();
    if (decoded.descriptor.id != descriptor.id) {
      AS_FAIL("register round trip id mismatch: encoded=" + descriptor.id.to_string() +
              " decoded=" + decoded.descriptor.id.to_string() +
              " gen=" + std::to_string(decoded.descriptor.generation.value()) +
              " max=" + std::to_string(decoded.descriptor.max_concurrency) +
              " name=" + decoded.descriptor.display_name);
    }
    AS_CHECK(decoded.descriptor.boot == descriptor.boot);
    AS_CHECK(decoded.descriptor.max_concurrency == descriptor.max_concurrency);
  }
  {
    PublishCapabilitiesMessage message;
    message.agent = descriptor.id;
    message.boot = descriptor.boot;
    message.profile = make_profile(descriptor, {{"coding", 900}, {"tool:shell", 700}});
    const std::vector<std::uint8_t> payload_bytes = encode_of(message);
    ByteReader reader(payload_bytes);
    PublishCapabilitiesMessage decoded;
    decode(reader, decoded, limits);
    reader.require_exhausted();
    AS_CHECK(decoded.profile.capabilities.size() == 2);
    AS_CHECK(decoded.profile.boot == descriptor.boot);
  }
  {
    PublishLoadMessage message;
    message.agent = descriptor.id;
    message.boot = descriptor.boot;
    message.observation = load(AgentLoadGeneration{4}, 250, 1.5, 0.25);
    message.observation.warm_state_keys = {"repo", "cache"};
    const std::vector<std::uint8_t> payload_bytes = encode_of(message);
    ByteReader reader(payload_bytes);
    PublishLoadMessage decoded;
    decode(reader, decoded, limits);
    reader.require_exhausted();
    AS_CHECK(decoded.observation.estimated_dispatch_latency_ms == 250);
    AS_CHECK(decoded.observation.cost_index == 1.5);
    AS_CHECK(decoded.observation.warm_state_keys.size() == 2);
  }
  {
    SubmitWorkMessage message;
    message.request = make_work(100, {"coding", "tool:shell"}, 750);
    message.request.requirements.require_resource_feasibility = true;
    message.request.requirements.resource_generation = ResourceGeneration{3};
    message.request.requirements.resource_feasibility = FeasibilityVerdict::Feasible;
    canonicalize(message.request);
    const std::vector<std::uint8_t> payload_bytes = encode_of(message);
    ByteReader reader(payload_bytes);
    SubmitWorkMessage decoded;
    decode(reader, decoded, limits);
    reader.require_exhausted();
    AS_CHECK(decoded.request == message.request);
  }
  {
    AssignWorkMessage message;
    message.binding.work = WorkId{9};
    message.binding.agent = descriptor.id;
    message.binding.boot = descriptor.boot;
    message.binding.generation = AssignmentGeneration{2};
    message.kind = "unit";
    const std::vector<std::uint8_t> payload_bytes = encode_of(message);
    ByteReader reader(payload_bytes);
    AssignWorkMessage decoded;
    decode(reader, decoded, limits);
    reader.require_exhausted();
    AS_CHECK(decoded.binding.work == WorkId{9});
    AS_CHECK(decoded.binding.generation == AssignmentGeneration{2});
  }
  {
    ResultMessage message;
    message.request_type = MessageType::SubmitWork;
    message.ok = false;
    message.outcome = ScheduleOutcome::RejectStalePolicy;
    message.error = make_error(ErrorCode::StaleGeneration, "submit_work", "0x1", "stale",
                               ScheduleOutcome::RejectStalePolicy);
    const std::vector<std::uint8_t> payload_bytes = encode_of(message);
    ByteReader reader(payload_bytes);
    ResultMessage decoded;
    decode(reader, decoded, limits);
    reader.require_exhausted();
    AS_CHECK(!decoded.ok);
    AS_CHECK(decoded.error.code == ErrorCode::StaleGeneration);
    AS_CHECK(decoded.outcome == ScheduleOutcome::RejectStalePolicy);
  }
  {
    QueryResultMessage message;
    message.kind = QueryKind::Summary;
    message.json = "{\"ok\":true}";
    const std::vector<std::uint8_t> payload_bytes = encode_of(message);
    ByteReader reader(payload_bytes);
    QueryResultMessage decoded;
    decode(reader, decoded, limits);
    reader.require_exhausted();
    AS_CHECK(decoded.json == message.json);
  }
}

AS_TEST(protocol, strict_decoding_rejects_hostile_payloads) {
  const ResourceLimits limits = default_limits();
  {
    ByteWriter writer;
    writer.put_u32(static_cast<std::uint32_t>(protocol_version));
    writer.put_string("agent", limits.max_identifier_size);
    writer.put_string("build", limits.max_string_size);
    writer.put_u64(1);
    writer.put_u8(0xFF);  // trailing garbage
    ByteReader reader(writer.bytes());
    HelloMessage message;
    decode(reader, message, limits);
    bool threw = false;
    try {
      reader.require_exhausted();
    } catch (const DecodeError&) {
      threw = true;
    }
    AS_CHECK(threw);
  }
  {
    ByteWriter writer;
    writer.put_u64(0x7FF0000000000000ull);  // +infinity
    ByteReader reader(writer.bytes());
    bool threw = false;
    try {
      (void)reader.get_f64();
    } catch (const DecodeError&) {
      threw = true;
    }
    AS_CHECK(threw);
  }
  {
    ByteWriter writer;
    writer.put_u64(0x7FF8000000000000ull);  // NaN
    ByteReader reader(writer.bytes());
    bool threw = false;
    try {
      (void)reader.get_f64();
    } catch (const DecodeError&) {
      threw = true;
    }
    AS_CHECK(threw);
  }
  {
    ByteWriter writer;
    writer.put_u32(100000);  // declared string length far beyond the limit
    ByteReader reader(writer.bytes());
    bool threw = false;
    try {
      (void)reader.get_string(limits.max_string_size);
    } catch (const DecodeError&) {
      threw = true;
    }
    AS_CHECK(threw);
  }
  {
    ByteWriter writer;
    writer.put_u32(100000);  // declared identifier length far beyond the limit
    ByteReader reader(writer.bytes());
    bool threw = false;
    try {
      (void)reader.get_string(limits.max_identifier_size);
    } catch (const DecodeError&) {
      threw = true;
    }
    AS_CHECK(threw);
  }
  {
    ByteWriter writer;
    writer.put_u8(0x2);  // not a boolean
    ByteReader reader(writer.bytes());
    bool threw = false;
    try {
      (void)reader.get_bool();
    } catch (const DecodeError&) {
      threw = true;
    }
    AS_CHECK(threw);
  }
  {
    PublishHealthMessage message;
    message.agent = AgentId{1};
    ByteWriter writer;
    encode(writer, message, limits);
    std::vector<std::uint8_t> bytes = writer.take();
    // layout: agent u64, boot high u64, boot low u64, generation u64, health u8
    bytes[32] = 0x7F;  // corrupt the health enum
    ByteReader reader(bytes);
    PublishHealthMessage decoded;
    bool threw = false;
    try {
      decode(reader, decoded, limits);
    } catch (const DecodeError&) {
      threw = true;
    }
    AS_CHECK(threw);
  }
  {
    ByteWriter writer;
    writer.put_u16(9999);  // unknown query kind
    ByteReader reader(writer.bytes());
    QueryMessage message;
    bool threw = false;
    try {
      decode(reader, message, limits);
    } catch (const DecodeError&) {
      threw = true;
    }
    AS_CHECK(threw);
  }
}

AS_TEST(protocol, encode_rejects_oversized_and_nonfinite_values) {
  bool threw = false;
  try {
    ByteWriter writer;
    writer.put_string(std::string(1000, 'x'), 16);
  } catch (const DecodeError&) {
    threw = true;
  }
  AS_CHECK(threw);

  threw = false;
  try {
    ByteWriter writer;
    writer.put_f64(std::numeric_limits<double>::quiet_NaN());
  } catch (const DecodeError&) {
    threw = true;
  }
  AS_CHECK(threw);

  threw = false;
  try {
    Frame oversized = make_frame(MessageType::Heartbeat, 1, std::vector<std::uint8_t>(64, 0));
    (void)encode_frame(oversized, 8);
  } catch (const DecodeError&) {
    threw = true;
  }
  AS_CHECK(threw);
}
