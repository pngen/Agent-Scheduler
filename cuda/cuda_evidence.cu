// Agent Scheduler - optional real CUDA hardware evidence probe (device + host code).
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
//
// This translation unit is compiled by nvcc, and only when
// AGENT_SCHEDULER_ENABLE_CUDA=ON. Nothing in the core agent_scheduler library includes,
// links, or references it.
//
// The probe is real or it reports failure: it never fabricates a device, a name, a
// capability code, or a checksum. If the toolkit, the driver, or the hardware cannot do
// the work, run_cuda_evidence() returns false with a concrete diagnostic.

#include "cuda_evidence.hpp"

#include <cuda_runtime.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace agent_scheduler_cuda {
namespace {

constexpr std::uint32_t kElements = 1u << 20;
constexpr std::uint32_t kModulus = 1000003u;
constexpr unsigned kThreadsPerBlock = 256u;
constexpr int kTimedLaunches = 64;

/// Deterministic element-wise transform. Values stay well inside uint32 range:
/// input < 1000003, so input * 3 + 7 < 3000016.
__global__ void transform_kernel(const std::uint32_t* input,
                                 std::uint32_t* output,
                                 std::uint32_t count) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index < count) {
    output[index] = (input[index] * 3u + 7u) % kModulus;
  }
}

/// Device-side additive reduction. Integer addition is associative and commutative, so
/// the atomic accumulation order cannot change the result: the reduced value is exactly
/// the CPU reference checksum whenever every element matches.
__global__ void sum_kernel(const std::uint32_t* values,
                           std::uint32_t count,
                           unsigned long long* total) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  const std::uint32_t stride = blockDim.x * gridDim.x;
  unsigned long long local = 0ull;
  for (std::uint32_t i = index; i < count; i += stride) {
    local += static_cast<unsigned long long>(values[i]);
  }
  if (local != 0ull) {
    atomicAdd(total, local);
  }
}

std::string cuda_failure_text(const char* operation, cudaError_t status) {
  std::string text(operation);
  text += " failed: ";
  text += cudaGetErrorName(status);
  text += " (";
  text += cudaGetErrorString(status);
  text += ")";
  return text;
}

/// Owns every device allocation made by the probe. memory_freed becomes true only when
/// cudaFree succeeded for all allocations that were actually made.
class DeviceBuffers {
 public:
  void* input{nullptr};
  void* output{nullptr};
  void* total{nullptr};
  bool all_freed{false};

  /// Idempotent: a second call cannot change the recorded result.
  void release() noexcept {
    if (released_) {
      return;
    }
    released_ = true;
    int allocated = 0;
    int freed = 0;
    allocated += release_one(input);
    allocated += release_one(output);
    allocated += release_one(total);
    freed = freed_count_;
    all_freed = allocated > 0 && freed == allocated;
  }

 private:
  /// Returns 1 when the pointer referred to a live allocation, 0 otherwise.
  int release_one(void*& pointer) noexcept {
    if (pointer == nullptr) {
      return 0;
    }
    void* const target = pointer;
    pointer = nullptr;
    if (cudaFree(target) == cudaSuccess) {
      ++freed_count_;
    }
    return 1;
  }

  bool released_{false};
  int freed_count_{0};
};

}  // namespace

bool run_cuda_evidence(CudaDeviceEvidence& out) {
  out = CudaDeviceEvidence{};
  out.elements = kElements;

  DeviceBuffers buffers;
  cudaEvent_t start_event = nullptr;
  cudaEvent_t stop_event = nullptr;

  const auto cleanup = [&]() {
    buffers.release();
    out.memory_freed = buffers.all_freed;
    if (start_event != nullptr) {
      (void)cudaEventDestroy(start_event);
      start_event = nullptr;
    }
    if (stop_event != nullptr) {
      (void)cudaEventDestroy(stop_event);
      stop_event = nullptr;
    }
  };

  const auto fail = [&](std::string detail) -> bool {
    cleanup();
    out.available = false;
    out.diagnostic = std::move(detail);
    return false;
  };

  // ---- enumerate -----------------------------------------------------------
  int device_count = 0;
  cudaError_t status = cudaGetDeviceCount(&device_count);
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("cudaGetDeviceCount", status));
  }
  if (device_count <= 0) {
    return fail("cudaGetDeviceCount reported no CUDA device on this host");
  }

  // ---- select --------------------------------------------------------------
  const int device_index = 0;
  status = cudaSetDevice(device_index);
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("cudaSetDevice(0)", status));
  }

  cudaDeviceProp properties{};
  status = cudaGetDeviceProperties(&properties, device_index);
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("cudaGetDeviceProperties", status));
  }

  out.device_index = device_index;
  out.device_name = properties.name;
  out.compute_major = properties.major;
  out.compute_minor = properties.minor;
  out.total_memory_bytes = static_cast<std::uint64_t>(properties.totalGlobalMem);
  out.multiprocessor_count = properties.multiProcessorCount;

  status = cudaDriverGetVersion(&out.driver_version);
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("cudaDriverGetVersion", status));
  }
  status = cudaRuntimeGetVersion(&out.runtime_version);
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("cudaRuntimeGetVersion", status));
  }

  // ---- host reference data -------------------------------------------------
  const std::size_t bytes = static_cast<std::size_t>(kElements) * sizeof(std::uint32_t);
  std::vector<std::uint32_t> host_input(kElements);
  for (std::uint32_t i = 0; i < kElements; ++i) {
    const std::uint64_t mixed =
        (static_cast<std::uint64_t>(i) * 2654435761ull + 12345ull) % kModulus;
    host_input[i] = static_cast<std::uint32_t>(mixed);
  }

  std::vector<std::uint32_t> cpu_expected(kElements);
  std::uint64_t cpu_checksum = 0;
  for (std::uint32_t i = 0; i < kElements; ++i) {
    const std::uint32_t value = (host_input[i] * 3u + 7u) % kModulus;
    cpu_expected[i] = value;
    cpu_checksum += static_cast<std::uint64_t>(value);
  }
  out.cpu_checksum = cpu_checksum;

  // ---- allocate ------------------------------------------------------------
  status = cudaMalloc(&buffers.input, bytes);
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("cudaMalloc(input)", status));
  }
  status = cudaMalloc(&buffers.output, bytes);
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("cudaMalloc(output)", status));
  }
  status = cudaMalloc(&buffers.total, sizeof(unsigned long long));
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("cudaMalloc(total)", status));
  }

  // ---- H2D -----------------------------------------------------------------
  status = cudaMemcpy(buffers.input, host_input.data(), bytes, cudaMemcpyHostToDevice);
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("cudaMemcpy(host->device)", status));
  }
  out.bytes_uploaded += static_cast<std::uint64_t>(bytes);

  const unsigned blocks = (kElements + kThreadsPerBlock - 1u) / kThreadsPerBlock;

  // ---- kernel (warm-up, then timed batch) ----------------------------------
  transform_kernel<<<blocks, kThreadsPerBlock>>>(
      static_cast<const std::uint32_t*>(buffers.input),
      static_cast<std::uint32_t*>(buffers.output),
      kElements);
  status = cudaGetLastError();
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("transform_kernel launch", status));
  }
  status = cudaDeviceSynchronize();
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("cudaDeviceSynchronize (warm-up)", status));
  }

  status = cudaEventCreate(&start_event);
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("cudaEventCreate(start)", status));
  }
  status = cudaEventCreate(&stop_event);
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("cudaEventCreate(stop)", status));
  }

  status = cudaEventRecord(start_event, nullptr);
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("cudaEventRecord(start)", status));
  }
  for (int launch = 0; launch < kTimedLaunches; ++launch) {
    transform_kernel<<<blocks, kThreadsPerBlock>>>(
        static_cast<const std::uint32_t*>(buffers.input),
        static_cast<std::uint32_t*>(buffers.output),
        kElements);
  }
  status = cudaEventRecord(stop_event, nullptr);
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("cudaEventRecord(stop)", status));
  }
  status = cudaDeviceSynchronize();
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("cudaDeviceSynchronize (timed batch)", status));
  }

  float elapsed_ms = 0.0f;
  status = cudaEventElapsedTime(&elapsed_ms, start_event, stop_event);
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("cudaEventElapsedTime", status));
  }
  out.kernel_millis = elapsed_ms > 0.0f
                          ? static_cast<std::uint64_t>(std::ceil(static_cast<double>(elapsed_ms)))
                          : 1ull;

  // ---- device-side reduction ----------------------------------------------
  status = cudaMemset(buffers.total, 0, sizeof(unsigned long long));
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("cudaMemset(total)", status));
  }
  const unsigned sum_blocks = blocks < 1024u ? blocks : 1024u;
  sum_kernel<<<sum_blocks, kThreadsPerBlock>>>(
      static_cast<const std::uint32_t*>(buffers.output),
      kElements,
      static_cast<unsigned long long*>(buffers.total));
  status = cudaGetLastError();
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("sum_kernel launch", status));
  }
  status = cudaDeviceSynchronize();
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("cudaDeviceSynchronize (reduction)", status));
  }

  // ---- D2H -----------------------------------------------------------------
  std::vector<std::uint32_t> host_output(kElements);
  status = cudaMemcpy(host_output.data(), buffers.output, bytes, cudaMemcpyDeviceToHost);
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("cudaMemcpy(device->host output)", status));
  }
  out.bytes_downloaded += static_cast<std::uint64_t>(bytes);

  unsigned long long device_total = 0ull;
  status = cudaMemcpy(&device_total, buffers.total, sizeof(device_total), cudaMemcpyDeviceToHost);
  if (status != cudaSuccess) {
    return fail(cuda_failure_text("cudaMemcpy(device->host total)", status));
  }
  out.bytes_downloaded += static_cast<std::uint64_t>(sizeof(device_total));
  out.gpu_checksum = static_cast<std::uint64_t>(device_total);

  // ---- verify against the CPU reference ------------------------------------
  std::size_t mismatches = 0;
  std::uint32_t first_bad_index = 0;
  std::uint32_t first_bad_gpu = 0;
  std::uint32_t first_bad_cpu = 0;
  for (std::uint32_t i = 0; i < kElements; ++i) {
    if (host_output[i] != cpu_expected[i]) {
      if (mismatches == 0) {
        first_bad_index = i;
        first_bad_gpu = host_output[i];
        first_bad_cpu = cpu_expected[i];
      }
      ++mismatches;
    }
  }
  if (mismatches != 0) {
    std::string detail = "device result differs from the CPU reference at ";
    detail += std::to_string(static_cast<unsigned long long>(mismatches));
    detail += " element(s); first at index ";
    detail += std::to_string(static_cast<unsigned long long>(first_bad_index));
    detail += " (gpu=";
    detail += std::to_string(static_cast<unsigned long long>(first_bad_gpu));
    detail += ", cpu=";
    detail += std::to_string(static_cast<unsigned long long>(first_bad_cpu));
    detail += ")";
    return fail(std::move(detail));
  }
  if (out.gpu_checksum != out.cpu_checksum) {
    std::string detail = "checksum mismatch: gpu=";
    detail += std::to_string(out.gpu_checksum);
    detail += " cpu=";
    detail += std::to_string(out.cpu_checksum);
    return fail(std::move(detail));
  }

  // ---- free ----------------------------------------------------------------
  cleanup();
  if (!out.memory_freed) {
    out.available = false;
    out.diagnostic = "cudaFree failed for at least one probe allocation";
    return false;
  }

  // ---- reset ---------------------------------------------------------------
  status = cudaDeviceReset();
  if (status != cudaSuccess) {
    out.available = false;
    out.diagnostic = cuda_failure_text("cudaDeviceReset", status);
    return false;
  }
  out.device_reset = true;

  out.available = true;
  out.diagnostic.clear();
  return true;
}

agent_scheduler::CapabilityEvidence make_verified_cuda_capability(
    const CudaDeviceEvidence& evidence,
    agent_scheduler::AgentCapabilityGeneration generation,
    agent_scheduler::AgentBootId boot,
    std::uint64_t observed_at_ms) {
  agent_scheduler::CapabilityEvidence out;
  out.name = std::string(kCapabilityName);
  out.state = agent_scheduler::CapabilityState::Verified;
  out.quality = 1000;
  out.generation = generation;
  out.boot = boot;
  out.observed_at_ms = observed_at_ms;

  std::string provenance = "cuda device ";
  provenance += std::to_string(evidence.device_index);
  provenance += " \"";
  provenance += evidence.device_name;
  provenance += "\" cc";
  provenance += std::to_string(evidence.compute_major);
  provenance += ".";
  provenance += std::to_string(evidence.compute_minor);
  provenance += " sm=";
  provenance += std::to_string(evidence.multiprocessor_count);
  provenance += " elements=";
  provenance += std::to_string(evidence.elements);
  provenance += " checksum=";
  provenance += std::to_string(evidence.gpu_checksum);
  provenance += " verified=cpu-match";
  out.provenance = std::move(provenance);
  return out;
}

}  // namespace agent_scheduler_cuda
