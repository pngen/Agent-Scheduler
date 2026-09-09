// Agent Scheduler — version and build identity.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace agent_scheduler {

inline constexpr int version_major = 1;
inline constexpr int version_minor = 0;
inline constexpr int version_patch = 0;
inline constexpr std::string_view version_string = "1.0.0";
inline constexpr std::string_view product_name = "Agent Scheduler";
inline constexpr std::string_view license_notice =
    "Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.";

/// Wire protocol version. Frames carrying any other value are rejected before decode.
inline constexpr std::uint32_t protocol_version = 1;
/// Persistence format version. Files carrying any other value are rejected before decode.
inline constexpr std::uint32_t persistence_format_version = 1;

/// Deterministic one-line build identity: version, configuration, and compiler.
[[nodiscard]] std::string build_info();

}  // namespace agent_scheduler
