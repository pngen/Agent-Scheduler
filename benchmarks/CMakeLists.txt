# Agent Scheduler — benchmark suite.
# Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#
# One benchmark executable, no CTest registration, no timeouts: the suite is
# noninteractive, prompts for nothing, and runs to completion naturally.
#
# The benchmark populations are SYNTHETIC (logical profiles from SyntheticLaboratory
# with explicit seeds) and every measurement is single-process and in-memory. No
# multi-node or multi-GPU claim is implied or measured.

add_executable(agent_scheduler_bench agent_scheduler_bench.cpp)

target_include_directories(agent_scheduler_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Repo build-list workaround: src/internal/util.cpp defines
# agent_scheduler::internal::{identifier_problem, canonicalize_unique,
# is_finite_non_negative, is_finite_unit_fraction} but is not part of
# AGENT_SCHEDULER_SOURCES, so the static library archive leaves those symbols
# undefined for every consumer. Compile that translation unit into this executable
# while the library source list omits it; this block becomes a no-op automatically
# once the library carries it.
if(NOT AGENT_SCHEDULER_SOURCES MATCHES "src/internal/util.cpp")
  target_sources(agent_scheduler_bench PRIVATE ${CMAKE_SOURCE_DIR}/src/internal/util.cpp)
  target_include_directories(agent_scheduler_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)
  message(STATUS "agent_scheduler_bench: compiling src/internal/util.cpp into the benchmark executable because the library source list omits it")
endif()

# Keep the executable at <build>/<config>/agent_scheduler_bench.exe for every
# configuration so the documented run commands work verbatim.
set_target_properties(agent_scheduler_bench PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
  RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/RelWithDebInfo"
  RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/MinSizeRel")

target_link_libraries(agent_scheduler_bench PRIVATE agent_scheduler)
agent_scheduler_apply_warnings(agent_scheduler_bench)
target_compile_features(agent_scheduler_bench PRIVATE cxx_std_20)
