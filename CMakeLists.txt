# Agent Scheduler — CMake build.
# Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
cmake_minimum_required(VERSION 3.20)

project(agent_scheduler
  VERSION 1.0.0
  DESCRIPTION "Scheduling authority for persistent autonomous workers"
  HOMEPAGE_URL "https://github.com/pngen/Agent-Scheduler"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(AGENT_SCHEDULER_TOP_LEVEL ON)
else()
  set(AGENT_SCHEDULER_TOP_LEVEL OFF)
endif()

option(AGENT_SCHEDULER_BUILD_TESTS "Build the Agent Scheduler test suite" ${AGENT_SCHEDULER_TOP_LEVEL})
option(AGENT_SCHEDULER_BUILD_TOOLS "Build inspection and control tools" ${AGENT_SCHEDULER_TOP_LEVEL})
option(AGENT_SCHEDULER_BUILD_EXAMPLES "Build runnable examples" ${AGENT_SCHEDULER_TOP_LEVEL})
option(AGENT_SCHEDULER_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(AGENT_SCHEDULER_ENABLE_ASAN "Instrument first-party targets with AddressSanitizer" OFF)
option(AGENT_SCHEDULER_ENABLE_CUDA "Build the optional real CUDA capability evidence helper" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- strict first-party warnings -------------------------------------------
# Applied per target so that nothing leaks into the installed package export.
function(agent_scheduler_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor
      /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14311 /w14545 /w14546 /w14547
      /w14549 /w14555 /w14619 /w14640 /w14826 /w14905 /w14906 /w14928)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion
      -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference)
  endif()
  if(WIN32)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
  endif()
  if(AGENT_SCHEDULER_ENABLE_ASAN)
    if(MSVC)
      # /Zi gives AddressSanitizer line information and silences C5072, which /WX
      # would otherwise promote to an error.
      target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
      target_link_options(${target} PRIVATE /fsanitize=address /DEBUG)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
  endif()
endfunction()

# ---- library ----------------------------------------------------------------
set(AGENT_SCHEDULER_SOURCES
  src/adapters.cpp
  src/agent.cpp
  src/assignment.cpp
  src/capability.cpp
  src/clock.cpp
  src/codec.cpp
  src/digest.cpp
  src/explanation.cpp
  src/internal/util.cpp
  src/invariants.cpp
  src/json.cpp
  src/limits.cpp
  src/persistence.cpp
  src/persistence_io.cpp
  src/policy.cpp
  src/result.cpp
  src/scheduler.cpp
  src/scheduler_state.cpp
  src/scheduling.cpp
  src/snapshot.cpp
  src/synthetic.cpp
  src/version.cpp
  src/work.cpp
  src/net/agent_client.cpp
  src/net/control_client.cpp
  src/net/coordinator.cpp
  src/net/frame.cpp
  src/net/protocol.cpp
  src/net/session.cpp
  src/net/transport.cpp)

set(AGENT_SCHEDULER_PUBLIC_HEADERS
  include/agent_scheduler/adapters.hpp
  include/agent_scheduler/agent.hpp
  include/agent_scheduler/assignment.hpp
  include/agent_scheduler/capability.hpp
  include/agent_scheduler/clock.hpp
  include/agent_scheduler/codec.hpp
  include/agent_scheduler/digest.hpp
  include/agent_scheduler/explanation.hpp
  include/agent_scheduler/ids.hpp
  include/agent_scheduler/json.hpp
  include/agent_scheduler/limits.hpp
  include/agent_scheduler/persistence.hpp
  include/agent_scheduler/policy.hpp
  include/agent_scheduler/result.hpp
  include/agent_scheduler/scheduler.hpp
  include/agent_scheduler/snapshot.hpp
  include/agent_scheduler/synthetic.hpp
  include/agent_scheduler/version.hpp
  include/agent_scheduler/work.hpp
  include/agent_scheduler/net/agent_client.hpp
  include/agent_scheduler/net/control_client.hpp
  include/agent_scheduler/net/coordinator.hpp
  include/agent_scheduler/net/frame.hpp
  include/agent_scheduler/net/protocol.hpp
  include/agent_scheduler/net/session.hpp
  include/agent_scheduler/net/transport.hpp)

add_library(agent_scheduler ${AGENT_SCHEDULER_SOURCES})
add_library(agent_scheduler::agent_scheduler ALIAS agent_scheduler)

target_include_directories(agent_scheduler
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

agent_scheduler_apply_warnings(agent_scheduler)
target_compile_features(agent_scheduler PUBLIC cxx_std_20)

if(WIN32)
  target_link_libraries(agent_scheduler PRIVATE ws2_32)
endif()

set_target_properties(agent_scheduler PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

# ---- install ----------------------------------------------------------------
install(TARGETS agent_scheduler
  EXPORT agent_schedulerTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/agent_scheduler
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.hpp")

install(EXPORT agent_schedulerTargets
  FILE agent_schedulerTargets.cmake
  NAMESPACE agent_scheduler::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/agent_scheduler)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/agent_schedulerConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/agent_schedulerConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/agent_scheduler)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/agent_schedulerConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/agent_schedulerConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/agent_schedulerConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/agent_scheduler)

install(FILES LICENSE NOTICE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/agent_scheduler)

# ---- optional extras --------------------------------------------------------
if(AGENT_SCHEDULER_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
if(AGENT_SCHEDULER_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(AGENT_SCHEDULER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(AGENT_SCHEDULER_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
if(AGENT_SCHEDULER_ENABLE_CUDA)
  enable_language(CUDA)
  add_subdirectory(cuda)
endif()

# ---- summary ----------------------------------------------------------------
message(STATUS "Agent Scheduler ${PROJECT_VERSION} (${CMAKE_BUILD_TYPE})")
message(STATUS "  tests:     ${AGENT_SCHEDULER_BUILD_TESTS}")
message(STATUS "  tools:     ${AGENT_SCHEDULER_BUILD_TOOLS}")
message(STATUS "  examples:  ${AGENT_SCHEDULER_BUILD_EXAMPLES}")
message(STATUS "  benchmarks:${AGENT_SCHEDULER_BUILD_BENCHMARKS}")
message(STATUS "  asan:      ${AGENT_SCHEDULER_ENABLE_ASAN}")
message(STATUS "  cuda:      ${AGENT_SCHEDULER_ENABLE_CUDA}")
