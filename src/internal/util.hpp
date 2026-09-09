// Agent Scheduler — internal validation and canonicalization helpers.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "agent_scheduler/result.hpp"

namespace agent_scheduler::internal {

/// True when the text contains a byte that could inject log lines or break framing.
[[nodiscard]] bool contains_control_chars(std::string_view text) noexcept;

/// True when the text is a bounded identifier: non-empty, restricted charset.
[[nodiscard]] bool is_identifier(std::string_view text, std::uint32_t max_length) noexcept;

/// Validates bounded free text: length limit, no control characters, valid UTF-8 shape.
[[nodiscard]] std::optional<std::string> text_problem(std::string_view text,
                                                      std::uint32_t max_length,
                                                      std::string_view field);

/// Validates an identifier field.
[[nodiscard]] std::optional<std::string> identifier_problem(std::string_view text,
                                                            std::uint32_t max_length,
                                                            std::string_view field);

/// Sorts and removes duplicate entries.
void canonicalize(std::vector<std::string>& values);
/// Sorts and removes duplicate entries, returning false when a duplicate was present.
[[nodiscard]] bool canonicalize_unique(std::vector<std::string>& values);

/// Finite, non-negative double check used for externally supplied evidence.
[[nodiscard]] bool is_finite_non_negative(double value) noexcept;
[[nodiscard]] bool is_finite_unit_fraction(double value) noexcept;

/// Clamps an unsigned counter into a fixed-point 0..1000 range.
[[nodiscard]] std::int64_t clamp_unit_scale(std::uint64_t value, std::uint64_t ceiling) noexcept;

}  // namespace agent_scheduler::internal
