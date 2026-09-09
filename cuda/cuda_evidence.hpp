// Agent Scheduler - optional real CUDA hardware evidence helper (public surface).
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
//
// This header is deliberately CUDA-free: it includes no CUDA toolkit header and uses no
// nvcc-only construct, so the optional probe CLI (and any host caller) can include it
// with a plain C++ compiler. The core agent_scheduler library never includes this header
// and never links the CUDA target, so the core library has no CUDA dependency at all.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "agent_scheduler/capability.hpp"
#include "agent_scheduler/ids.hpp"

namespace agent_scheduler_cuda {

/// Capability name published for verified CUDA accelerator evidence.
inline constexpr std::string_view kCapabilityName = "accelerator:cuda";

/// Real evidence captured from one CUDA device during a single probe run.
///
/// Every field is produced by the CUDA runtime and by the probe itself. No field is
/// synthesised, inferred, or defaulted into a "looks present" value: available stays
/// false until the complete sequence (enumerate -> select -> allocate -> H2D ->
/// kernel -> synchronize -> D2H -> verify against a CPU reference -> free -> reset)
/// has finished successfully.
struct CudaDeviceEvidence {
  /// True only when a real device was used successfully end to end.
  bool available{false};
  /// Index passed to cudaSetDevice / cudaGetDeviceProperties.
  int device_index{0};
  /// cudaDeviceProp::name of the selected device.
  std::string device_name;
  /// cudaDeviceProp::major.
  int compute_major{0};
  /// cudaDeviceProp::minor.
  int compute_minor{0};
  /// cudaDeviceProp::totalGlobalMem in bytes.
  std::uint64_t total_memory_bytes{0};
  /// cudaDeviceProp::multiProcessorCount.
  int multiprocessor_count{0};
  /// cudaDriverGetVersion result (e.g. 12090 for CUDA 12.9).
  int driver_version{0};
  /// cudaRuntimeGetVersion result.
  int runtime_version{0};
  /// Host-to-device bytes transferred by this probe.
  std::uint64_t bytes_uploaded{0};
  /// Device-to-host bytes transferred by this probe.
  std::uint64_t bytes_downloaded{0};
  /// Element count of the transformed vector (1 << 20).
  std::uint32_t elements{0};
  /// Additive checksum of the CPU reference result. Order-independent, so the device
  /// reduction is bit-exact against it.
  std::uint64_t cpu_checksum{0};
  /// Additive checksum of the device result, reduced on the device.
  std::uint64_t gpu_checksum{0};
  /// Whole milliseconds of GPU time measured with CUDA events for the timed kernel
  /// batch (kTimedLaunches back-to-back launches), rounded up, and at least 1 whenever
  /// the kernel actually ran. A single launch of this trivial kernel is far below one
  /// millisecond, so the batch is what makes the figure a real non-zero measurement.
  std::uint64_t kernel_millis{0};
  /// True only after cudaFree succeeded for every allocation made by this probe.
  bool memory_freed{false};
  /// True only after a successful cudaDeviceReset.
  bool device_reset{false};
  /// Human-readable failure detail. Empty on success; set whenever available is false.
  std::string diagnostic;
};

/// Runs the complete real sequence: enumerate -> select -> allocate -> H2D -> kernel ->
/// cudaDeviceSynchronize -> D2H -> verify against a CPU reference -> free -> reset.
///
/// Returns true only if every step succeeded and the GPU result matched the CPU result
/// element for element and checksum for checksum. On failure it returns false, fills
/// out.diagnostic with the concrete CUDA error or mismatch detail, leaves
/// out.available false, and still frees every allocation it made (out.memory_freed
/// reports whether that cleanup succeeded).
[[nodiscard]] bool run_cuda_evidence(CudaDeviceEvidence& out);

/// Publishes verified evidence as capability evidence on an agent incarnation.
///
/// Only called when evidence.available is true: the produced entry carries
/// CapabilityState::Verified, quality 1000, and the generation/boot supplied by the
/// caller. Provenance is a deterministic one-line summary of the real device.
[[nodiscard]] agent_scheduler::CapabilityEvidence make_verified_cuda_capability(
    const CudaDeviceEvidence& evidence,
    agent_scheduler::AgentCapabilityGeneration generation,
    agent_scheduler::AgentBootId boot,
    std::uint64_t observed_at_ms);

}  // namespace agent_scheduler_cuda
