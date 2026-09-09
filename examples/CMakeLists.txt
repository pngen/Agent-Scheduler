# Agent Scheduler — runnable examples.
# Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.

set(AGENT_SCHEDULER_EXAMPLES
  embedded_scheduler
  heterogeneous_capabilities
  fairness
  reincarnation
  distributed_coordinator)

foreach(example_name IN LISTS AGENT_SCHEDULER_EXAMPLES)
  add_executable(${example_name} ${example_name}.cpp)
  target_link_libraries(${example_name} PRIVATE agent_scheduler::agent_scheduler)
  agent_scheduler_apply_warnings(${example_name})
endforeach()
