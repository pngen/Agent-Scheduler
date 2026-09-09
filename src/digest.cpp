// Agent Scheduler — digests, checksums, and identifier derivation.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/digest.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace agent_scheduler {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, unsigned shift) noexcept {
  return (value >> shift) | (value << (32u - shift));
}

[[nodiscard]] constexpr std::array<std::uint32_t, 256> make_crc_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256; ++index) {
    std::uint32_t value = index;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1u) != 0u ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
    }
    table[index] = value;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrcTable = make_crc_table();

[[nodiscard]] std::string to_hex(const std::uint8_t* data, std::size_t size) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(size * 2, '0');
  for (std::size_t index = 0; index < size; ++index) {
    out[index * 2] = kHex[(data[index] >> 4) & 0xFu];
    out[index * 2 + 1] = kHex[data[index] & 0xFu];
  }
  return out;
}

}  // namespace

bool Digest256::is_zero() const noexcept {
  for (const std::uint8_t byte : bytes) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

std::string Digest256::to_string() const { return to_hex(bytes.data(), bytes.size()); }

std::string Hash128::to_string() const {
  std::array<std::uint8_t, 16> raw{};
  for (int index = 0; index < 8; ++index) {
    raw[static_cast<std::size_t>(index)] =
        static_cast<std::uint8_t>((high >> (56 - 8 * index)) & 0xFFu);
    raw[static_cast<std::size_t>(index) + 8] =
        static_cast<std::uint8_t>((low >> (56 - 8 * index)) & 0xFFu);
  }
  return to_hex(raw.data(), raw.size());
}

Hasher256::Hasher256() noexcept {
  state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
}

void Hasher256::compress(const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t index = 0; index < 16; ++index) {
    schedule[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) |
                      (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
                      (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
                      static_cast<std::uint32_t>(block[index * 4 + 3]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const std::uint32_t s0 = rotr(schedule[index - 15], 7) ^ rotr(schedule[index - 15], 18) ^
                             (schedule[index - 15] >> 3);
    const std::uint32_t s1 = rotr(schedule[index - 2], 17) ^ rotr(schedule[index - 2], 19) ^
                             (schedule[index - 2] >> 10);
    schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t choice = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + choice + kRoundConstants[index] + schedule[index];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Hasher256::update(std::span<const std::uint8_t> data) noexcept {
  total_bytes_ += static_cast<std::uint64_t>(data.size());
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t take = std::min(buffer_.size() - buffered_, data.size() - offset);
    std::memcpy(buffer_.data() + buffered_, data.data() + offset, take);
    buffered_ += take;
    offset += take;
    if (buffered_ == buffer_.size()) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
}

void Hasher256::update(std::string_view data) noexcept {
  update(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
}

void Hasher256::update(std::uint8_t value) noexcept { update(std::span<const std::uint8_t>(&value, 1)); }

void Hasher256::update(std::uint32_t value) noexcept {
  const std::array<std::uint8_t, 4> raw = {static_cast<std::uint8_t>(value & 0xFFu),
                                           static_cast<std::uint8_t>((value >> 8) & 0xFFu),
                                           static_cast<std::uint8_t>((value >> 16) & 0xFFu),
                                           static_cast<std::uint8_t>((value >> 24) & 0xFFu)};
  update(std::span<const std::uint8_t>(raw.data(), raw.size()));
}

void Hasher256::update(std::uint64_t value) noexcept {
  std::array<std::uint8_t, 8> raw{};
  for (int index = 0; index < 8; ++index) {
    raw[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFu);
  }
  update(std::span<const std::uint8_t>(raw.data(), raw.size()));
}

void Hasher256::update_length_prefixed(std::string_view data) noexcept {
  update(static_cast<std::uint32_t>(data.size()));
  update(data);
}

void Hasher256::update_field_tag(std::uint8_t tag) noexcept {
  update(tag);
  update(std::uint32_t{0xA5A5A5A5u});
}

Digest256 Hasher256::final() {
  const std::uint64_t bit_length = total_bytes_ * 8u;
  const std::uint8_t pad = 0x80u;
  update(std::span<const std::uint8_t>(&pad, 1));
  const std::uint8_t zero = 0x00u;
  while (buffered_ != 56) {
    update(std::span<const std::uint8_t>(&zero, 1));
  }
  std::array<std::uint8_t, 8> length{};
  for (int index = 0; index < 8; ++index) {
    length[static_cast<std::size_t>(index)] =
        static_cast<std::uint8_t>((bit_length >> (56 - 8 * index)) & 0xFFu);
  }
  update(std::span<const std::uint8_t>(length.data(), length.size()));

  Digest256 out;
  for (std::size_t index = 0; index < 8; ++index) {
    out.bytes[index * 4] = static_cast<std::uint8_t>((state_[index] >> 24) & 0xFFu);
    out.bytes[index * 4 + 1] = static_cast<std::uint8_t>((state_[index] >> 16) & 0xFFu);
    out.bytes[index * 4 + 2] = static_cast<std::uint8_t>((state_[index] >> 8) & 0xFFu);
    out.bytes[index * 4 + 3] = static_cast<std::uint8_t>(state_[index] & 0xFFu);
  }
  return out;
}

Digest256 sha256(std::span<const std::uint8_t> data) noexcept {
  Hasher256 hasher;
  hasher.update(data);
  return hasher.final();
}

Digest256 sha256(std::string_view data) noexcept {
  Hasher256 hasher;
  hasher.update(data);
  return hasher.final();
}

Hash128 hash128_of(const Digest256& digest) noexcept {
  Hash128 out;
  for (int index = 0; index < 8; ++index) {
    out.high = (out.high << 8) | digest.bytes[static_cast<std::size_t>(index)];
    out.low = (out.low << 8) | digest.bytes[static_cast<std::size_t>(index) + 8];
  }
  return out;
}

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t seed) noexcept {
  std::uint32_t value = seed ^ 0xFFFFFFFFu;
  for (const std::uint8_t byte : data) {
    value = kCrcTable[(value ^ byte) & 0xFFu] ^ (value >> 8);
  }
  return value ^ 0xFFFFFFFFu;
}

std::uint32_t crc32(std::string_view data, std::uint32_t seed) noexcept {
  return crc32(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()),
               seed);
}

}  // namespace agent_scheduler
