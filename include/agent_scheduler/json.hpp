// Agent Scheduler — deterministic JSON writer used by tools and query results.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace agent_scheduler {

/// Minimal deterministic JSON writer. Object members are emitted in the order the
/// caller specifies; the runtime always specifies canonical order.
class JsonWriter {
 public:
  JsonWriter& begin_object();
  JsonWriter& end_object();
  JsonWriter& begin_array();
  JsonWriter& end_array();
  JsonWriter& key(std::string_view name);
  JsonWriter& value(std::string_view text);
  JsonWriter& value(const char* text);
  JsonWriter& value(std::int64_t number);
  JsonWriter& value(std::uint64_t number);
  JsonWriter& value(int number);
  JsonWriter& value(bool boolean);
  JsonWriter& null_value();
  JsonWriter& raw(std::string_view literal);

  [[nodiscard]] const std::string& str() const noexcept { return out_; }

 private:
  void separate();
  void indent();

  std::string out_;
  std::vector<bool> first_;
  bool expecting_value_{false};
};

/// Escapes a string for JSON: quotes, backslashes, and control bytes (including all
/// C1 bytes) are escaped so that untrusted text cannot inject structure or log content.
[[nodiscard]] std::string escape_json(std::string_view text);

}  // namespace agent_scheduler
