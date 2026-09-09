// Agent Scheduler — shared command-line helpers for the tools.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agent_scheduler/ids.hpp"

namespace as_cli {

/// Minimal non-interactive argument parser: "--name value" or "--flag".
class Args {
 public:
  Args(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
      const std::string argument = argv[index];
      if (argument.rfind("--", 0) != 0) {
        positional_.push_back(argument);
        continue;
      }
      const std::size_t equals = argument.find('=');
      if (equals != std::string::npos) {
        entries_.push_back({argument.substr(2, equals - 2), argument.substr(equals + 1)});
        continue;
      }
      const std::string name = argument.substr(2);
      if (index + 1 < argc) {
        const std::string next = argv[index + 1];
        if (next.rfind("--", 0) != 0) {
          entries_.push_back({name, next});
          ++index;
          continue;
        }
      }
      entries_.push_back({name, std::string()});
    }
  }

  [[nodiscard]] bool has(std::string_view name) const {
    for (const auto& entry : entries_) {
      if (entry.first == name) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] std::optional<std::string> value(std::string_view name) const {
    for (const auto& entry : entries_) {
      if (entry.first == name) {
        return entry.second;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::vector<std::string> values(std::string_view name) const {
    std::vector<std::string> out;
    for (const auto& entry : entries_) {
      if (entry.first == name) {
        out.push_back(entry.second);
      }
    }
    return out;
  }

  [[nodiscard]] std::string text(std::string_view name, std::string fallback = std::string()) const {
    const auto found = value(name);
    return found.has_value() ? *found : std::move(fallback);
  }

  [[nodiscard]] std::uint64_t number(std::string_view name, std::uint64_t fallback) const {
    const auto found = value(name);
    if (!found.has_value()) {
      return fallback;
    }
    const auto parsed = parse_u64(*found);
    return parsed.has_value() ? *parsed : fallback;
  }

  [[nodiscard]] const std::vector<std::string>& positional() const noexcept { return positional_; }

  static std::optional<std::uint64_t> parse_u64(std::string_view text) {
    if (text.empty()) {
      return std::nullopt;
    }
    std::uint64_t value = 0;
    std::size_t index = 0;
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
      base = 16;
      index = 2;
    }
    for (; index < text.size(); ++index) {
      const char character = text[index];
      int digit = -1;
      if (character >= '0' && character <= '9') {
        digit = character - '0';
      } else if (base == 16 && character >= 'a' && character <= 'f') {
        digit = character - 'a' + 10;
      } else if (base == 16 && character >= 'A' && character <= 'F') {
        digit = character - 'A' + 10;
      }
      if (digit < 0 || digit >= base) {
        return std::nullopt;
      }
      if (value > (UINT64_MAX - static_cast<std::uint64_t>(digit)) / static_cast<std::uint64_t>(base)) {
        return std::nullopt;
      }
      value = value * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(digit);
    }
    return value;
  }

  static std::optional<agent_scheduler::Hash128> parse_hash128(std::string_view text) {
    if (text.size() != 32) {
      return std::nullopt;
    }
    const auto high = parse_u64("0x" + std::string(text.substr(0, 16)));
    const auto low = parse_u64("0x" + std::string(text.substr(16, 16)));
    if (!high.has_value() || !low.has_value()) {
      return std::nullopt;
    }
    return agent_scheduler::Hash128{*high, *low};
  }

 private:
  std::vector<std::pair<std::string, std::string>> entries_;
  std::vector<std::string> positional_;
};

[[nodiscard]] inline std::string hex(std::uint64_t value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int index = 15; index >= 0; --index) {
    out[static_cast<std::size_t>(index)] = kDigits[value & 0xFu];
    value >>= 4;
  }
  return out;
}

}  // namespace as_cli
