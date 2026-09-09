# Agent Scheduler — test targets. No CTest timeouts are set anywhere.
# Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.

set(AGENT_SCHEDULER_TEST_SUPPORT
  test_support.cpp)

set(AGENT_SCHEDULER_TESTS
  test_core
  test_capability
  test_fairness
  test_assignment
  test_persistence
  test_recovery
  test_invariants
  test_property
  test_races
  test_protocol
  test_limits
  test_synthetic
  test_shutdown
  test_net)

foreach(test_name IN LISTS AGENT_SCHEDULER_TESTS)
  add_executable(${test_name} ${test_name}.cpp ${AGENT_SCHEDULER_TEST_SUPPORT})
  target_link_libraries(${test_name} PRIVATE agent_scheduler::agent_scheduler)
  agent_scheduler_apply_warnings(${test_name})
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Real multiprocess proof: coordinator and agent processes are spawned as independent
# operating-system processes by this harness. It requires the tool targets.
if(TARGET agent_scheduler_coordinator AND TARGET agent_scheduler_agent)
  add_executable(agent_scheduler_multiprocess_test multiprocess_test.cpp)
  target_link_libraries(agent_scheduler_multiprocess_test PRIVATE agent_scheduler::agent_scheduler)
  agent_scheduler_apply_warnings(agent_scheduler_multiprocess_test)
  add_test(NAME multiprocess
    COMMAND agent_scheduler_multiprocess_test
            --coordinator $<TARGET_FILE:agent_scheduler_coordinator>
            --agent $<TARGET_FILE:agent_scheduler_agent>)
else()
  message(STATUS "agent_scheduler: multiprocess proof skipped (build tools to enable it)")
endif()
