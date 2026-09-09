// Agent Scheduler — injectable monotonic clock.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/clock.hpp"

#include <chrono>
#include <functional>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace agent_scheduler {

std::uint64_t SystemClock::now_ms() const noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::uint64_t wall_clock_ms() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::uint64_t process_nonce() noexcept {
#if defined(_WIN32)
  const std::uint64_t pid = static_cast<std::uint64_t>(::_getpid());
#else
  const std::uint64_t pid = static_cast<std::uint64_t>(::getpid());
#endif
  const std::uint64_t ticks = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
  const std::uint64_t thread_id = static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
  std::uint64_t mixed = pid * 0x9E3779B97F4A7C15ull;
  mixed ^= ticks + 0x165667B19E3779F9ull + (mixed << 6) + (mixed >> 2);
  mixed ^= thread_id * 0xC2B2AE3D27D4EB4Full;
  return mixed;
}

}  // namespace agent_scheduler
