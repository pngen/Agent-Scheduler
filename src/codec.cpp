// Agent Scheduler — portable bounds-checked canonical binary codec.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/codec.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace agent_scheduler {
namespace {

constexpr std::size_t kMaxEncodeBytes = 1u << 30;

}  // namespace

void ByteWriter::require(std::size_t extra) const {
  if (buffer_.size() > kMaxEncodeBytes - extra) {
    throw DecodeError("encoded message exceeds the maximum encodable size");
  }
}

void ByteWriter::put_u8(std::uint8_t value) {
  require(1);
  buffer_.push_back(value);
}

void ByteWriter::put_u16(std::uint16_t value) {
  require(2);
  buffer_.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  buffer_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void ByteWriter::put_u32(std::uint32_t value) {
  require(4);
  for (int index = 0; index < 4; ++index) {
    buffer_.push_back(static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFu));
  }
}

void ByteWriter::put_u64(std::uint64_t value) {
  require(8);
  for (int index = 0; index < 8; ++index) {
    buffer_.push_back(static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFu));
  }
}

void ByteWriter::put_i64(std::int64_t value) { put_u64(static_cast<std::uint64_t>(value)); }

void ByteWriter::put_bool(bool value) { put_u8(value ? 1u : 0u); }

void ByteWriter::put_f64(double value) {
  if (!std::isfinite(value)) {
    throw DecodeError("non-finite double is not encodable");
  }
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  put_u64(bits);
}

void ByteWriter::put_string(std::string_view value, std::uint32_t max_bytes) {
  if (value.size() > max_bytes) {
    throw DecodeError("string exceeds the configured maximum size");
  }
  put_u32(static_cast<std::uint32_t>(value.size()));
  require(value.size());
  buffer_.insert(buffer_.end(), value.begin(), value.end());
}

void ByteWriter::put_raw(std::span<const std::uint8_t> value) {
  require(value.size());
  buffer_.insert(buffer_.end(), value.begin(), value.end());
}

void ByteWriter::put_digest(const Digest256& value) {
  require(value.bytes.size());
  buffer_.insert(buffer_.end(), value.bytes.begin(), value.bytes.end());
}

void ByteReader::require(std::size_t count) const {
  if (count > data_.size() - offset_) {
    throw DecodeError("truncated message: not enough bytes remain");
  }
}

std::uint8_t ByteReader::get_u8() {
  require(1);
  return data_[offset_++];
}

std::uint16_t ByteReader::get_u16() {
  require(2);
  std::uint16_t value = static_cast<std::uint16_t>(data_[offset_]) |
                        static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[offset_ + 1]) << 8);
  offset_ += 2;
  return value;
}

std::uint32_t ByteReader::get_u32() {
  require(4);
  std::uint32_t value = 0;
  for (int index = 3; index >= 0; --index) {
    value = (value << 8) | data_[offset_ + static_cast<std::size_t>(index)];
  }
  offset_ += 4;
  return value;
}

std::uint64_t ByteReader::get_u64() {
  require(8);
  std::uint64_t value = 0;
  for (int index = 7; index >= 0; --index) {
    value = (value << 8) | data_[offset_ + static_cast<std::size_t>(index)];
  }
  offset_ += 8;
  return value;
}

std::int64_t ByteReader::get_i64() { return static_cast<std::int64_t>(get_u64()); }

bool ByteReader::get_bool() {
  const std::uint8_t value = get_u8();
  if (value > 1) {
    throw DecodeError("boolean field must be 0 or 1");
  }
  return value != 0;
}

double ByteReader::get_f64() {
  const std::uint64_t bits = get_u64();
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof(value));
  if (!std::isfinite(value)) {
    throw DecodeError("non-finite double is not accepted");
  }
  return value;
}

std::string ByteReader::get_string(std::uint32_t max_bytes) {
  const std::uint32_t size = get_u32();
  if (size > max_bytes) {
    throw DecodeError("string length exceeds the configured maximum size");
  }
  require(size);
  std::string out(reinterpret_cast<const char*>(data_.data() + offset_), size);
  offset_ += size;
  return out;
}

Digest256 ByteReader::get_digest() {
  require(32);
  Digest256 out;
  std::memcpy(out.bytes.data(), data_.data() + offset_, 32);
  offset_ += 32;
  return out;
}

void ByteReader::require_exhausted() const {
  if (!exhausted()) {
    throw DecodeError("message payload carries trailing bytes");
  }
}

}  // namespace agent_scheduler
