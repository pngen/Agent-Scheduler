// Agent Scheduler — deterministic scheduling explanations.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/explanation.hpp"

namespace agent_scheduler {

const char* to_string(FactorStatus value) noexcept {
  switch (value) {
    case FactorStatus::Known: return "KNOWN";
    case FactorStatus::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

}  // namespace agent_scheduler
