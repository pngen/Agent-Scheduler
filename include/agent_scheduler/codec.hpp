// Agent Scheduler — portable bounds-checked canonical binary codec.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "agent_scheduler/digest.hpp"

namespace agent_scheduler {

/// Strict decode failure. Never crosses a session or persistence boundary as an exception.
class DecodeError final : public std::runtime_error {
 public:
  explicit DecodeError(std::string what) : std::runtime_error(std::move(what)) {}
};

/// Bounds-checked canonical little-endian writer.
class ByteWriter {
 public:
  void put_u8(std::uint8_t value);
  void put_u16(std::uint16_t value);
  void put_u32(std::uint32_t value);
  void put_u64(std::uint64_t value);
  void put_i64(std::int64_t value);
  void put_bool(bool value);
  /// Encodes a finite double as its IEEE-754 bit pattern. Non-finite values are rejected.
  void put_f64(double value);
  /// Length-prefixed UTF-8 string. Enforces a byte ceiling.
  void put_string(std::string_view value, std::uint32_t max_bytes);
  /// Appends raw bytes with no length prefix. Used for fixed-size fields such as magic.
  void put_raw(std::span<const std::uint8_t> value);
  void put_digest(const Digest256& value);

  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return buffer_; }
  [[nodiscard]] std::vector<std::uint8_t> take() noexcept { return std::move(buffer_); }

 private:
  void require(std::size_t extra) const;
  std::vector<std::uint8_t> buffer_;
};

/// Bounds-checked canonical little-endian reader. Any violation throws DecodeError.
class ByteReader {
 public:
  explicit ByteReader(std::span<const std::uint8_t> data) noexcept : data_(data) {}

  [[nodiscard]] std::uint8_t get_u8();
  [[nodiscard]] std::uint16_t get_u16();
  [[nodiscard]] std::uint32_t get_u32();
  [[nodiscard]] std::uint64_t get_u64();
  [[nodiscard]] std::int64_t get_i64();
  [[nodiscard]] bool get_bool();
  /// Rejects NaN and infinities: non-finite evidence never enters scheduler state.
  [[nodiscard]] double get_f64();
  [[nodiscard]] std::string get_string(std::uint32_t max_bytes);
  [[nodiscard]] Digest256 get_digest();

  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] bool exhausted() const noexcept { return offset_ == data_.size(); }
  /// Rejects a payload that carries bytes the decoder did not consume.
  void require_exhausted() const;

 private:
  void require(std::size_t count) const;
  std::span<const std::uint8_t> data_;
  std::size_t offset_{0};
};

}  // namespace agent_scheduler
