// Agent Scheduler — deterministic JSON writer.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/json.hpp"

#include <array>

namespace agent_scheduler {
namespace {

void append_hex_escape(std::string& out, unsigned char byte) {
  static constexpr char kHex[] = "0123456789abcdef";
  out += "\\u00";
  out += kHex[(byte >> 4) & 0xFu];
  out += kHex[byte & 0xFu];
}

}  // namespace

std::string escape_json(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    switch (byte) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (byte < 0x20u || byte == 0x7Fu || (byte >= 0x80u && byte <= 0x9Fu)) {
          append_hex_escape(out, byte);
        } else {
          out += character;
        }
        break;
    }
  }
  return out;
}

void JsonWriter::separate() {
  if (first_.empty()) {
    return;
  }
  if (expecting_value_) {
    expecting_value_ = false;
    out_ += ": ";
    return;
  }
  if (!first_.back()) {
    out_ += ',';
    out_ += '\n';
    indent();
  }
  first_.back() = false;
}

void JsonWriter::indent() {
  for (std::size_t index = 1; index < first_.size(); ++index) {
    out_ += "  ";
  }
}

JsonWriter& JsonWriter::begin_object() {
  separate();
  out_ += '{';
  first_.push_back(true);
  return *this;
}

JsonWriter& JsonWriter::end_object() {
  if (!first_.empty()) {
    first_.pop_back();
  }
  if (!first_.empty()) {
    out_ += '\n';
    indent();
  }
  out_ += '}';
  return *this;
}

JsonWriter& JsonWriter::begin_array() {
  separate();
  out_ += '[';
  first_.push_back(true);
  return *this;
}

JsonWriter& JsonWriter::end_array() {
  if (!first_.empty()) {
    first_.pop_back();
  }
  if (!first_.empty()) {
    out_ += '\n';
    indent();
  }
  out_ += ']';
  return *this;
}

JsonWriter& JsonWriter::key(std::string_view name) {
  separate();
  out_ += '"';
  out_ += escape_json(name);
  out_ += '"';
  expecting_value_ = true;
  return *this;
}

JsonWriter& JsonWriter::value(std::string_view text) {
  separate();
  out_ += '"';
  out_ += escape_json(text);
  out_ += '"';
  return *this;
}

JsonWriter& JsonWriter::value(const char* text) { return value(std::string_view(text)); }

JsonWriter& JsonWriter::value(std::int64_t number) {
  separate();
  out_ += std::to_string(number);
  return *this;
}

JsonWriter& JsonWriter::value(std::uint64_t number) {
  separate();
  out_ += std::to_string(number);
  return *this;
}

JsonWriter& JsonWriter::value(int number) { return value(static_cast<std::int64_t>(number)); }

JsonWriter& JsonWriter::value(bool boolean) {
  separate();
  out_ += boolean ? "true" : "false";
  return *this;
}

JsonWriter& JsonWriter::null_value() {
  separate();
  out_ += "null";
  return *this;
}

JsonWriter& JsonWriter::raw(std::string_view literal) {
  separate();
  out_ += literal;
  return *this;
}

}  // namespace agent_scheduler
