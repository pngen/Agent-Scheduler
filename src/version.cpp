// Agent Scheduler — version and build identity.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/version.hpp"

#include <string>

namespace agent_scheduler {

std::string build_info() {
  std::string out(version_string);
  out += " ";
#if defined(NDEBUG)
  out += "release";
#else
  out += "debug";
#endif
  out += " ";
#if defined(__clang__)
  out += "clang-";
  out += __clang_version__;
#elif defined(_MSC_VER)
  out += "msvc-";
  out += std::to_string(_MSC_VER);
#elif defined(__GNUC__)
  out += "gcc-";
  out += __VERSION__;
#else
  out += "unknown-compiler";
#endif
  out += " cxx20 protocol=";
  out += std::to_string(protocol_version);
  out += " persistence=";
  out += std::to_string(persistence_format_version);
  return out;
}

}  // namespace agent_scheduler
