// Agent Scheduler — internal validation and canonicalization helpers.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "internal/util.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>

namespace agent_scheduler::internal {
namespace {

/// Minimal UTF-8 shape validation: rejects overlong forms, surrogate halves, and
/// truncated sequences so that hostile bytes cannot smuggle invalid text into output.
[[nodiscard]] bool valid_utf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto byte = static_cast<unsigned char>(text[index]);
    if (byte < 0x80u) {
      ++index;
      continue;
    }
    std::size_t extra = 0;
    std::uint32_t code_point = 0;
    if ((byte & 0xE0u) == 0xC0u) {
      extra = 1;
      code_point = byte & 0x1Fu;
      if (code_point == 0) {
        return false;
      }
    } else if ((byte & 0xF0u) == 0xE0u) {
      extra = 2;
      code_point = byte & 0x0Fu;
    } else if ((byte & 0xF8u) == 0xF0u) {
      extra = 3;
      code_point = byte & 0x07u;
    } else {
      return false;
    }
    if (index + extra >= text.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset <= extra; ++offset) {
      const auto continuation = static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xC0u) != 0x80u) {
        return false;
      }
      code_point = (code_point << 6) | (continuation & 0x3Fu);
    }
    if (extra == 1 && code_point < 0x80u) {
      return false;
    }
    if (extra == 2 && code_point < 0x800u) {
      return false;
    }
    if (extra == 3 && code_point < 0x10000u) {
      return false;
    }
    if (code_point > 0x10FFFFu) {
      return false;
    }
    if (code_point >= 0xD800u && code_point <= 0xDFFFu) {
      return false;
    }
    index += extra + 1;
  }
  return true;
}

}  // namespace

bool contains_control_chars(std::string_view text) noexcept {
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20u || byte == 0x7Fu) {
      return true;
    }
    if (byte >= 0x80u && byte <= 0x9Fu) {
      return true;
    }
  }
  return false;
}

bool is_identifier(std::string_view text, std::uint32_t max_length) noexcept {
  if (text.empty() || text.size() > max_length) {
    return false;
  }
  for (const char character : text) {
    const bool ok = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                    (character >= '0' && character <= '9') || character == '_' || character == '-' ||
                    character == '.' || character == ':' || character == '/' || character == '+';
    if (!ok) {
      return false;
    }
  }
  return true;
}

std::optional<std::string> text_problem(std::string_view text,
                                        std::uint32_t max_length,
                                        std::string_view field) {
  if (text.size() > max_length) {
    std::string problem(field);
    problem += " exceeds maximum length";
    return problem;
  }
  if (contains_control_chars(text)) {
    std::string problem(field);
    problem += " contains control characters";
    return problem;
  }
  if (!valid_utf8(text)) {
    std::string problem(field);
    problem += " is not valid UTF-8";
    return problem;
  }
  return std::nullopt;
}

std::optional<std::string> identifier_problem(std::string_view text,
                                              std::uint32_t max_length,
                                              std::string_view field) {
  if (text.empty()) {
    std::string problem(field);
    problem += " must not be empty";
    return problem;
  }
  if (!is_identifier(text, max_length)) {
    std::string problem(field);
    problem += " contains characters outside the identifier charset";
    return problem;
  }
  return std::nullopt;
}

void canonicalize(std::vector<std::string>& values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

bool canonicalize_unique(std::vector<std::string>& values) {
  std::sort(values.begin(), values.end());
  const auto duplicate = std::adjacent_find(values.begin(), values.end());
  const bool unique = duplicate == values.end();
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return unique;
}

bool is_finite_non_negative(double value) noexcept { return std::isfinite(value) && value >= 0.0; }

bool is_finite_unit_fraction(double value) noexcept {
  return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

std::int64_t clamp_unit_scale(std::uint64_t value, std::uint64_t ceiling) noexcept {
  if (ceiling == 0) {
    return 0;
  }
  if (value >= ceiling) {
    return 1000;
  }
  constexpr std::uint64_t kScale = 1000;
  if (value > std::numeric_limits<std::uint64_t>::max() / kScale) {
    return 1000;
  }
  return static_cast<std::int64_t>((value * kScale) / ceiling);
}

}  // namespace agent_scheduler::internal
