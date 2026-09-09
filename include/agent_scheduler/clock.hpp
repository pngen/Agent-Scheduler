// Agent Scheduler — injectable monotonic clock.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace agent_scheduler {

/// Monotonic millisecond clock. All scheduler timing derives from this interface so
/// that lease, freshness, aging, and deadline behaviour is fully testable.
class Clock {
 public:
  virtual ~Clock() = default;
  [[nodiscard]] virtual std::uint64_t now_ms() const noexcept = 0;
};

/// Process-local steady clock. Values are monotonic and not comparable across processes,
/// which is exactly the semantics lease and freshness logic requires.
class SystemClock final : public Clock {
 public:
  [[nodiscard]] std::uint64_t now_ms() const noexcept override;
};

/// Deterministic clock for tests, examples, and the synthetic laboratory.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(std::uint64_t start_ms = 0) noexcept : now_(start_ms) {}

  [[nodiscard]] std::uint64_t now_ms() const noexcept override { return now_.load(std::memory_order_relaxed); }
  void set(std::uint64_t value_ms) noexcept { now_.store(value_ms, std::memory_order_relaxed); }
  void advance(std::uint64_t delta_ms) noexcept { now_.fetch_add(delta_ms, std::memory_order_relaxed); }

 private:
  std::atomic<std::uint64_t> now_{0};
};

/// Wall-clock milliseconds since the Unix epoch. Diagnostics only; never used for
/// authority decisions.
[[nodiscard]] std::uint64_t wall_clock_ms() noexcept;

/// Process-unique nonce used to seed incarnation identity.
[[nodiscard]] std::uint64_t process_nonce() noexcept;

}  // namespace agent_scheduler
