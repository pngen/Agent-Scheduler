# Agent Scheduler — inspection and control tools.
# Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.

set(AGENT_SCHEDULER_TOOLS
  agent_scheduler_inspect
  agent_scheduler_ctl
  agent_scheduler_coordinator
  agent_scheduler_agent)

foreach(tool_name IN LISTS AGENT_SCHEDULER_TOOLS)
  add_executable(${tool_name} ${tool_name}.cpp)
  target_link_libraries(${tool_name} PRIVATE agent_scheduler::agent_scheduler)
  target_include_directories(${tool_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  agent_scheduler_apply_warnings(${tool_name})
  install(TARGETS ${tool_name} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endforeach()
