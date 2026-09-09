// Agent Scheduler — SHA-256 digests used for semantic integrity and deterministic identifiers.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace agent_scheduler {

/// A 256-bit semantic digest. Used for state digests, persistence integrity, and
/// deterministic derivation of identity values from canonical content.
struct Digest256 {
  std::array<std::uint8_t, 32> bytes{};

  [[nodiscard]] bool is_zero() const noexcept;
  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const Digest256& a, const Digest256& b) noexcept { return a.bytes == b.bytes; }
  friend bool operator!=(const Digest256& a, const Digest256& b) noexcept { return !(a == b); }
  friend bool operator<(const Digest256& a, const Digest256& b) noexcept { return a.bytes < b.bytes; }
  friend bool operator>(const Digest256& a, const Digest256& b) noexcept { return b < a; }
  friend bool operator<=(const Digest256& a, const Digest256& b) noexcept { return !(b < a); }
  friend bool operator>=(const Digest256& a, const Digest256& b) noexcept { return !(a < b); }
};

/// A 128-bit value derived from a digest. Used as a collision-resistant identity.
struct Hash128 {
  std::uint64_t high{0};
  std::uint64_t low{0};

  [[nodiscard]] constexpr bool is_zero() const noexcept { return high == 0 && low == 0; }
  [[nodiscard]] std::string to_string() const;

  friend constexpr bool operator==(const Hash128&, const Hash128&) noexcept = default;
  friend constexpr auto operator<=>(const Hash128&, const Hash128&) noexcept = default;
};

/// Streaming SHA-256. The hash is computed over an explicit little-endian canonical
/// byte stream, so identical logical content always yields an identical digest.
class Hasher256 {
 public:
  Hasher256() noexcept;

  void update(std::span<const std::uint8_t> data) noexcept;
  void update(std::string_view data) noexcept;
  void update(std::uint8_t value) noexcept;
  void update(std::uint32_t value) noexcept;
  void update(std::uint64_t value) noexcept;
  /// Length-prefixed string: 4-byte length followed by the raw bytes.
  void update_length_prefixed(std::string_view data) noexcept;
  /// Marks a structural boundary so that distinct structures cannot collide by concatenation.
  void update_field_tag(std::uint8_t tag) noexcept;

  [[nodiscard]] Digest256 final();

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::uint64_t total_bytes_{0};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_{0};
};

/// One-shot digest of a byte range.
[[nodiscard]] Digest256 sha256(std::span<const std::uint8_t> data) noexcept;
[[nodiscard]] Digest256 sha256(std::string_view data) noexcept;

/// Derives a 128-bit identity from a digest (high = first 8 bytes, low = next 8).
[[nodiscard]] Hash128 hash128_of(const Digest256& digest) noexcept;

/// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320). Transport and storage check.
[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t seed = 0) noexcept;
[[nodiscard]] std::uint32_t crc32(std::string_view data, std::uint32_t seed = 0) noexcept;

}  // namespace agent_scheduler
