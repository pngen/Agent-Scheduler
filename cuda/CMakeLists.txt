# Agent Scheduler - optional real CUDA capability evidence helper.
# Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#
# This directory is added by the root CMakeLists.txt only when
# AGENT_SCHEDULER_ENABLE_CUDA=ON, and the root runs enable_language(CUDA) first, so CUDA
# is already enabled here. Nothing in this directory is part of the core agent_scheduler
# target, its install set, or its exported package: with AGENT_SCHEDULER_ENABLE_CUDA=OFF
# the core library builds and installs with no CUDA toolkit present at all.

if(NOT CMAKE_CUDA_COMPILER)
  message(FATAL_ERROR
    "agent_scheduler: AGENT_SCHEDULER_ENABLE_CUDA=ON but no CUDA compiler is configured. "
    "The parent scope must call enable_language(CUDA) before add_subdirectory(cuda), and a "
    "CUDA toolkit must be installed. Configure with -DAGENT_SCHEDULER_ENABLE_CUDA=OFF to "
    "build the core library without CUDA.")
endif()

find_package(CUDAToolkit REQUIRED)

# The probe TU includes the repository's public headers (cuda_evidence.hpp ->
# agent_scheduler/capability.hpp -> ids.hpp -> digest.hpp), which use C++20 language
# features: std::span, concepts, operator<=>, and defaulted friend comparisons. nvcc in
# -std=c++17 mode rejects every one of them ("= default cannot be specified on a friend
# declaration", "identifier concept is undefined", ...), so the CUDA standard is 20 to
# match the C++20 core library. C++17 is not a viable option for this TU.
set(CMAKE_CUDA_STANDARD 20)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_EXTENSIONS OFF)

# ---- architecture -----------------------------------------------------------
# No architecture is hard-coded here. CMake policy CMP0104 (NEW at the repository's
# cmake_minimum_required(3.20)) requires CUDA_ARCHITECTURES to be non-empty, and the
# toolkit's own default is not usable on every host: CUDA 13.x defaults to sm_75, which
# cannot execute on a Blackwell device such as the installed RTX 5090 (compute
# capability 12.0). The CUDA targets therefore default to CMake's "native" resolution,
# which detects the GPU actually installed in this machine instead of naming an
# architecture. Override for cross-compilation or a GPU-less CI host with
# -DAGENT_SCHEDULER_CUDA_ARCHITECTURES=86 (any CMake architecture list is accepted).
set(AGENT_SCHEDULER_CUDA_ARCHITECTURES "" CACHE STRING
  "CUDA architectures for the optional probe targets (empty = CMake native detection)")
if(AGENT_SCHEDULER_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES "${AGENT_SCHEDULER_CUDA_ARCHITECTURES}")
else()
  set(CMAKE_CUDA_ARCHITECTURES native)
endif()

# ---- probe library ----------------------------------------------------------
# All CUDA code lives in cuda_evidence.cu; the public header is CUDA-free so host code
# can include it without the toolkit. No host-only .cpp is needed.
add_library(agent_scheduler_cuda_probe_lib STATIC cuda_evidence.cu)
target_include_directories(agent_scheduler_cuda_probe_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# agent_scheduler is linked PRIVATE: the probe publishes evidence through the public API
# types, and the core library never links anything from this directory.
target_link_libraries(agent_scheduler_cuda_probe_lib PRIVATE agent_scheduler CUDA::cudart)
set_target_properties(agent_scheduler_cuda_probe_lib PROPERTIES
  CUDA_STANDARD 20
  CUDA_STANDARD_REQUIRED ON
  CUDA_RUNTIME_LIBRARY Static
  POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  # nvcc compiles the .cu host side with the MSVC host compiler. The CUDA toolset's own
  # default host warning level (/W1) is kept for device/host code inside the .cu (nvcc
  # has no /WX and its host diagnostics are not /W4-clean); /utf-8 is forwarded so UTF-8
  # comments never trip the host compiler's code page. The repository-wide /W4 /WX policy
  # is unchanged for every other target, and is applied in full to the host .cpp of the
  # CLI below.
  target_compile_options(agent_scheduler_cuda_probe_lib PRIVATE "-Xcompiler=/utf-8")
endif()

# ---- CLI --------------------------------------------------------------------
add_executable(agent_scheduler_cuda_probe agent_scheduler_cuda_probe.cpp)
target_link_libraries(agent_scheduler_cuda_probe PRIVATE
  agent_scheduler_cuda_probe_lib agent_scheduler CUDA::cudart)
set_target_properties(agent_scheduler_cuda_probe PROPERTIES CUDA_RUNTIME_LIBRARY Static)

# The probe is run as build/.../Release/agent_scheduler_cuda_probe.exe, so pin outputs to
# the build root instead of letting the multi-config generator bury them in a mirrored
# source subdirectory (build/cuda/cuda/Release). Multi-config generators still append the
# configuration name, which is exactly the documented verification path.
set_target_properties(agent_scheduler_cuda_probe_lib PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set_target_properties(agent_scheduler_cuda_probe PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Pre-existing repository gap, not caused by this directory: the root CMakeLists
# AGENT_SCHEDULER_SOURCES list omits src/internal/util.cpp, so linking ANY executable
# against agent_scheduler fails with
#   LNK2019 unresolved external symbol agent_scheduler::internal::is_identifier ...
#   LNK2019 unresolved external symbol agent_scheduler::internal::text_problem ...
#   LNK2019 unresolved external symbol agent_scheduler::internal::identifier_problem ...
# (a static archive never reports this, which is why the library alone "builds clean").
# This optional tool therefore compiles that one missing core TU itself - and only while
# the core target really lacks it, so the workaround disappears by itself once the core
# library lists the file. No existing file is edited either way.
get_target_property(_agent_scheduler_core_sources agent_scheduler SOURCES)
if(NOT _agent_scheduler_core_sources MATCHES "src/internal/util[.]cpp")
  target_sources(agent_scheduler_cuda_probe PRIVATE ${PROJECT_SOURCE_DIR}/src/internal/util.cpp)
  target_include_directories(agent_scheduler_cuda_probe PRIVATE ${PROJECT_SOURCE_DIR}/src)
endif()

# Host code gets the repository's exact strict warning policy. The helper function is
# defined by the root CMakeLists.txt, with a local fallback for out-of-tree reuse.
if(COMMAND agent_scheduler_apply_warnings)
  agent_scheduler_apply_warnings(agent_scheduler_cuda_probe)
elseif(MSVC)
  target_compile_options(agent_scheduler_cuda_probe PRIVATE
    /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
else()
  target_compile_options(agent_scheduler_cuda_probe PRIVATE
    -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion
    -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference)
endif()

message(STATUS "  cuda probe: nvcc ${CMAKE_CUDA_COMPILER_VERSION}, CUDAToolkit ${CUDAToolkit_VERSION}, arch=${CMAKE_CUDA_ARCHITECTURES}")
