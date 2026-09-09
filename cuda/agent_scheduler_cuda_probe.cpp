// Agent Scheduler - optional real CUDA hardware evidence probe (CLI).
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
//
// Strictly non-interactive: this tool never prompts, never reads stdin, never waits for
// input, and never opens a message box. Exit codes: 0 = verified real evidence,
// 1 = no device or no usable evidence, 2 = usage error.
//
// This is host code compiled by MSVC (not nvcc), so it carries the repository's strict
// /W4 /WX /utf-8 warning policy. It includes only the CUDA-free public surface, so the
// CUDA runtime headers never leak into host compilation.

#include "cuda_evidence.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

#include "agent_scheduler/capability.hpp"
#include "agent_scheduler/json.hpp"
#include "agent_scheduler/version.hpp"

namespace {

constexpr int kExitVerified = 0;
constexpr int kExitNoEvidence = 1;
constexpr int kExitUsage = 2;

constexpr const char* kToolName = "agent_scheduler_cuda_probe";

void print_usage(std::ostream& out, std::string_view program) {
  out << "usage: " << program << " [--json] [--help] [--version]\n"
      << "\n"
      << "Runs a real CUDA probe on this host: enumerate devices, select device 0,\n"
      << "allocate, upload, run a deterministic kernel, synchronize, download, verify\n"
      << "against an independent CPU reference, free every allocation, and reset.\n"
      << "No device data is ever synthesised.\n"
      << "\n"
      << "options:\n"
      << "  --json     print one deterministic JSON object with the evidence fields\n"
      << "  --help     print this help and exit 0\n"
      << "  --version  print version and build identity, then exit 0\n"
      << "\n"
      << "exit codes: 0 verified evidence, 1 no device or no verified evidence,\n"
      << "            2 usage error\n"
      << "\n"
      << "The last stdout line is always:\n"
      << "  CUDA_EVIDENCE: REAL <device_name> cc<major>.<minor> elements=<n> verified=<yes|no>\n";
}

void print_version(std::ostream& out) {
  out << kToolName << " " << agent_scheduler::version_string << "\n"
      << agent_scheduler::product_name << " " << agent_scheduler::version_string << " - "
      << agent_scheduler::build_info() << "\n"
      << "capability published: " << agent_scheduler_cuda::kCapabilityName << "\n";
}

/// One deterministic JSON object. Members are emitted in a fixed canonical order and
/// every numeric member is cast explicitly, so the same run shape always serialises
/// byte for byte identically apart from the measured kernel time.
std::string to_json(const agent_scheduler_cuda::CudaDeviceEvidence& evidence) {
  agent_scheduler::JsonWriter writer;
  writer.begin_object();
  writer.key("available").value(evidence.available);
  writer.key("device_index").value(evidence.device_index);
  writer.key("device_name").value(evidence.device_name);
  writer.key("compute_major").value(evidence.compute_major);
  writer.key("compute_minor").value(evidence.compute_minor);
  writer.key("total_memory_bytes").value(evidence.total_memory_bytes);
  writer.key("multiprocessor_count").value(evidence.multiprocessor_count);
  writer.key("driver_version").value(evidence.driver_version);
  writer.key("runtime_version").value(evidence.runtime_version);
  writer.key("bytes_uploaded").value(evidence.bytes_uploaded);
  writer.key("bytes_downloaded").value(evidence.bytes_downloaded);
  writer.key("elements").value(static_cast<std::uint64_t>(evidence.elements));
  writer.key("cpu_checksum").value(evidence.cpu_checksum);
  writer.key("gpu_checksum").value(evidence.gpu_checksum);
  writer.key("kernel_millis").value(evidence.kernel_millis);
  writer.key("memory_freed").value(evidence.memory_freed);
  writer.key("device_reset").value(evidence.device_reset);
  writer.key("diagnostic").value(evidence.diagnostic);
  writer.end_object();
  return writer.str();
}

void print_text(std::ostream& out, const agent_scheduler_cuda::CudaDeviceEvidence& evidence) {
  const auto boolean = [](bool value) { return value ? "true" : "false"; };
  out << "available:            " << boolean(evidence.available) << "\n"
      << "device_index:         " << evidence.device_index << "\n"
      << "device_name:          " << evidence.device_name << "\n"
      << "compute_capability:   " << evidence.compute_major << "." << evidence.compute_minor << "\n"
      << "total_memory_bytes:   " << evidence.total_memory_bytes << "\n"
      << "multiprocessor_count: " << evidence.multiprocessor_count << "\n"
      << "driver_version:       " << evidence.driver_version << "\n"
      << "runtime_version:      " << evidence.runtime_version << "\n"
      << "bytes_uploaded:       " << evidence.bytes_uploaded << "\n"
      << "bytes_downloaded:     " << evidence.bytes_downloaded << "\n"
      << "elements:             " << evidence.elements << "\n"
      << "cpu_checksum:         " << evidence.cpu_checksum << "\n"
      << "gpu_checksum:         " << evidence.gpu_checksum << "\n"
      << "kernel_millis:        " << evidence.kernel_millis << "\n"
      << "memory_freed:         " << boolean(evidence.memory_freed) << "\n"
      << "device_reset:         " << boolean(evidence.device_reset) << "\n"
      << "diagnostic:           "
      << (evidence.diagnostic.empty() ? "none" : evidence.diagnostic) << "\n";
}

/// Exercises the public-API publishing path so the tool proves the capability entry it
/// would hand to the scheduler, without pretending the entry itself is hardware
/// evidence: the values come from the probe, the generation/boot are the caller's.
void print_capability(std::ostream& out, const agent_scheduler_cuda::CudaDeviceEvidence& evidence) {
  if (!evidence.available) {
    out << "capability:           not published (no verified evidence)\n";
    return;
  }
  const agent_scheduler::CapabilityEvidence capability =
      agent_scheduler_cuda::make_verified_cuda_capability(
          evidence,
          agent_scheduler::AgentCapabilityGeneration{1},
          agent_scheduler::AgentBootId{},
          0);
  out << "capability:           " << capability.name << " state="
      << agent_scheduler::to_string(capability.state) << " quality=" << capability.quality
      << " generation=" << capability.generation.to_string() << "\n"
      << "provenance:           " << capability.provenance << "\n";
}

void print_evidence_line(std::ostream& out, const agent_scheduler_cuda::CudaDeviceEvidence& evidence) {
  const bool verified = evidence.available && evidence.gpu_checksum == evidence.cpu_checksum &&
                        evidence.memory_freed && evidence.device_reset;
  out << "CUDA_EVIDENCE: REAL "
      << (evidence.device_name.empty() ? "none" : evidence.device_name) << " cc"
      << evidence.compute_major << "." << evidence.compute_minor
      << " elements=" << evidence.elements << " verified=" << (verified ? "yes" : "no") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  const std::string_view program = (argc > 0 && argv[0] != nullptr)
                                       ? std::string_view{argv[0]}
                                       : std::string_view{kToolName};
  bool json_output = false;

  for (int index = 1; index < argc; ++index) {
    if (argv[index] == nullptr) {
      continue;
    }
    const std::string_view argument{argv[index]};
    if (argument == "--json") {
      json_output = true;
    } else if (argument == "--help" || argument == "-h") {
      print_usage(std::cout, program);
      return kExitVerified;
    } else if (argument == "--version" || argument == "-V") {
      print_version(std::cout);
      return kExitVerified;
    } else {
      std::cerr << kToolName << ": unrecognized argument '" << argument << "'\n\n";
      print_usage(std::cerr, program);
      return kExitUsage;
    }
  }

  agent_scheduler_cuda::CudaDeviceEvidence evidence;
  const bool verified = agent_scheduler_cuda::run_cuda_evidence(evidence);

  if (json_output) {
    std::cout << to_json(evidence) << "\n";
  } else {
    print_text(std::cout, evidence);
    print_capability(std::cout, evidence);
  }
  print_evidence_line(std::cout, evidence);

  if (!verified || !evidence.available) {
    if (!evidence.diagnostic.empty()) {
      std::cerr << kToolName << ": " << evidence.diagnostic << "\n";
    } else {
      std::cerr << kToolName << ": no verified CUDA evidence on this host\n";
    }
    return kExitNoEvidence;
  }
  return kExitVerified;
}
