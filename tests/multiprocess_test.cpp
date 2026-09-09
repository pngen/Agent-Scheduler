// Agent Scheduler — real multiprocess distributed proof harness.
//
// This harness spawns the coordinator and each agent as independent operating-system
// processes and drives them over real loopback TCP. Process death is a real OS process
// termination, not a flag and not a thread exit.
//
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#error "The Agent Scheduler multiprocess proof targets Windows (Winsock) in this release."
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>

#include "agent_scheduler/net/control_client.hpp"
#include "agent_scheduler/scheduler.hpp"
#include "agent_scheduler/version.hpp"

using namespace agent_scheduler;
using namespace agent_scheduler::net;

namespace {

int g_failures = 0;
bool g_verbose = false;

void report(const std::string& line) {
  if (g_verbose) {
    std::printf("  %s\n", line.c_str());
    std::fflush(stdout);
  }
}

void require(bool condition, const std::string& what) {
  if (!condition) {
    ++g_failures;
    std::printf("FAIL %s\n", what.c_str());
  } else {
    report("ok " + what);
  }
  std::fflush(stdout);
}

[[nodiscard]] std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
  return out;
}

[[nodiscard]] std::string quote(const std::filesystem::path& path) { return "\"" + path.string() + "\""; }

[[nodiscard]] std::optional<Hash128> parse_hex128(const std::string& text) {
  if (text.size() != 32) {
    return std::nullopt;
  }
  std::uint64_t high = 0;
  std::uint64_t low = 0;
  for (int index = 0; index < 32; ++index) {
    const char character = text[static_cast<std::size_t>(index)];
    int digit = -1;
    if (character >= '0' && character <= '9') {
      digit = character - '0';
    } else if (character >= 'a' && character <= 'f') {
      digit = character - 'a' + 10;
    } else if (character >= 'A' && character <= 'F') {
      digit = character - 'A' + 10;
    }
    if (digit < 0) {
      return std::nullopt;
    }
    if (index < 16) {
      high = (high << 4) | static_cast<std::uint64_t>(digit);
    } else {
      low = (low << 4) | static_cast<std::uint64_t>(digit);
    }
  }
  return Hash128{high, low};
}

[[nodiscard]] std::string hex16(std::uint64_t value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int index = 15; index >= 0; --index) {
    out[static_cast<std::size_t>(index)] = kDigits[value & 0xFu];
    value >>= 4;
  }
  return out;
}

[[nodiscard]] bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

/// A real child process with a redirected stdout pipe and no console window.
class ChildProcess {
 public:
  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ~ChildProcess() { dispose(); }

  [[nodiscard]] bool spawn(const std::filesystem::path& executable, const std::string& arguments) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    if (::CreatePipe(&read_handle, &write_handle, &attributes, 0) == 0) {
      return false;
    }
    ::SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0);

    // A dedicated NUL stdin keeps the child non-interactive and avoids inheriting a
    // parent stdin handle that may not be inheritable (CreateProcess then fails).
    HANDLE nul_handle = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                      OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_handle;
    startup.hStdError = write_handle;
    startup.hStdInput = nul_handle;

    PROCESS_INFORMATION information{};
    std::error_code path_error;
    const std::filesystem::path absolute = std::filesystem::absolute(executable, path_error);
    const std::filesystem::path resolved = path_error ? executable : absolute;
    std::wstring command = widen(quote(resolved) + " " + arguments);
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    const BOOL created = ::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, flags,
                                          nullptr, nullptr, &startup, &information);
    ::CloseHandle(write_handle);
    if (nul_handle != INVALID_HANDLE_VALUE) {
      ::CloseHandle(nul_handle);
    }
    if (created == 0) {
      std::printf("FAIL CreateProcessW failed with Windows error %lu for '%s'\n",
                  static_cast<unsigned long>(::GetLastError()), resolved.string().c_str());
      ::CloseHandle(read_handle);
      return false;
    }
    information_ = information;
    stdout_read_ = read_handle;
    valid_ = true;
    return true;
  }

  [[nodiscard]] std::string read_line() {
    std::string line;
    char buffer = '\0';
    DWORD read = 0;
    while (::ReadFile(stdout_read_, &buffer, 1, &read, nullptr) != 0 && read == 1) {
      if (buffer == '\n') {
        break;
      }
      if (buffer != '\r') {
        line.push_back(buffer);
      }
    }
    return line;
  }

  [[nodiscard]] bool alive() const {
    if (!valid_) {
      return false;
    }
    DWORD code = 0;
    if (::GetExitCodeProcess(information_.hProcess, &code) == 0) {
      return false;
    }
    return code == STILL_ACTIVE;
  }

  /// Real OS process termination. No cleanup runs in the child.
  void terminate() {
    if (valid_ && alive()) {
      ::TerminateProcess(information_.hProcess, 137);
    }
  }

  [[nodiscard]] int wait() {
    if (!valid_) {
      return -1;
    }
    ::WaitForSingleObject(information_.hProcess, INFINITE);
    DWORD code = 0;
    (void)::GetExitCodeProcess(information_.hProcess, &code);
    return static_cast<int>(code);
  }

  void dispose() {
    if (stdout_read_ != nullptr) {
      ::CloseHandle(stdout_read_);
      stdout_read_ = nullptr;
    }
    if (valid_) {
      if (alive()) {
        ::TerminateProcess(information_.hProcess, 137);
      }
      ::CloseHandle(information_.hThread);
      ::CloseHandle(information_.hProcess);
      valid_ = false;
    }
  }

 private:
  PROCESS_INFORMATION information_{};
  HANDLE stdout_read_{nullptr};
  bool valid_{false};
};

/// Bounded observation loop. This waits for a real distributed state change to become
/// observable through the control path and reports a defect if it never arrives.
template <class Predicate>
[[nodiscard]] bool observe(Predicate predicate, const std::string& what, std::uint32_t attempts = 6000) {
  for (std::uint32_t attempt = 0; attempt < attempts; ++attempt) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::printf("FAIL observation never satisfied: %s\n", what.c_str());
  ++g_failures;
  return false;
}

[[nodiscard]] std::string json_string_field(const std::string& json,
                                            const std::string& key,
                                            std::size_t from = 0) {
  const std::string needle = "\"" + key + "\": \"";
  const std::size_t start = json.find(needle, from);
  if (start == std::string::npos) {
    return std::string();
  }
  const std::size_t begin = start + needle.size();
  const std::size_t end = json.find('"', begin);
  if (end == std::string::npos) {
    return std::string();
  }
  return json.substr(begin, end - begin);
}

[[nodiscard]] std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
  std::size_t count = 0;
  std::size_t position = haystack.find(needle);
  while (position != std::string::npos) {
    ++count;
    position = haystack.find(needle, position + needle.size());
  }
  return count;
}

[[nodiscard]] AgentDescriptor descriptor_for(std::uint64_t id,
                                             std::uint64_t generation,
                                             AgentBootId boot,
                                             std::uint32_t concurrency) {
  AgentDescriptor descriptor;
  descriptor.id = AgentId{id};
  descriptor.generation = AgentGeneration{generation};
  descriptor.boot = boot;
  descriptor.registration_generation = AgentRegistrationGeneration{1};
  descriptor.tenant = TenantId{1};
  descriptor.name_space = NamespaceId{1};
  descriptor.display_name = "agent-" + std::to_string(id);
  descriptor.policy_labels = {"worker"};
  descriptor.placement_domain = PlacementDomainId{1};
  descriptor.topology_epoch = TopologyEpoch{1};
  descriptor.max_concurrency = concurrency;
  descriptor.lease_ttl_ms = 600000;
  descriptor.provenance = "multiprocess-harness";
  return descriptor;
}

[[nodiscard]] WorkRequest work_for_many(std::uint64_t id, const std::vector<std::string>& capabilities) {
  WorkRequest request;
  request.id = WorkId{id};
  request.generation = WorkGeneration{1};
  request.queue = QueueId{1};
  request.queue_generation = QueueGeneration{1};
  request.kind = "multiprocess";
  request.priority = 500;
  request.fairness_class = "default";
  request.requirements.tenant = TenantId{1};
  request.requirements.name_space = NamespaceId{1};
  request.requirements.required_policy_labels = {"worker"};
  for (const std::string& capability : capabilities) {
    CapabilityRequirement requirement;
    requirement.name = capability;
    requirement.mode = CapabilityRequirementMode::Required;
    requirement.minimum_state = CapabilityState::Observed;
    request.requirements.capabilities.push_back(std::move(requirement));
  }
  canonicalize(request);
  return request;
}

[[nodiscard]] WorkRequest work_for(std::uint64_t id, const std::string& capability) {
  return work_for_many(id, {capability});
}

struct Harness {
  std::filesystem::path coordinator_exe;
  std::filesystem::path agent_exe;
  std::filesystem::path scratch;
  std::filesystem::path state_file;
};

struct RunningCoordinator {
  ChildProcess process;
  std::uint16_t port{0};
  std::filesystem::path stop_file;
  bool started{false};
};

[[nodiscard]] bool start_coordinator(Harness& harness,
                                     RunningCoordinator& running,
                                     bool load_state,
                                     const std::string& label) {
  running.stop_file = harness.scratch / (label + ".stop");
  std::error_code error;
  std::filesystem::remove(running.stop_file, error);
  const std::string arguments = "--port 0 --state " + quote(harness.state_file) +
                                (load_state ? std::string() : std::string(" --no-load")) +
                                " --stop-file " + quote(running.stop_file);
  if (!running.process.spawn(harness.coordinator_exe, arguments)) {
    std::printf("FAIL coordinator process could not be started\n");
    return false;
  }
  const std::string line = running.process.read_line();
  if (!contains(line, "LISTEN")) {
    std::printf("FAIL coordinator did not announce its port (got '%s')\n", line.c_str());
    return false;
  }
  running.port = static_cast<std::uint16_t>(std::strtoul(line.substr(7).c_str(), nullptr, 10));
  running.started = running.port != 0;
  report("coordinator listening on port " + std::to_string(running.port));
  return running.started;
}

[[nodiscard]] bool stop_coordinator(RunningCoordinator& running) {
  std::ofstream(running.stop_file.string()).put('1');
  const int code = running.process.wait();
  report("coordinator exited with code " + std::to_string(code));
  return code == 0;
}

struct RunningAgent {
  ChildProcess process;
  AgentId id{};
  AgentBootId boot{};
  bool started{false};
};

[[nodiscard]] bool start_agent(Harness& harness,
                               std::uint16_t port,
                               std::uint64_t id,
                               std::uint64_t generation,
                               std::uint32_t concurrency,
                               const std::string& capability,
                               RunningAgent& running,
                               const std::optional<AgentBootId>& boot_override = std::nullopt,
                               const std::string& extra = std::string()) {
  const std::string boot_argument =
      boot_override.has_value() ? (" --boot " + boot_override->to_string()) : std::string();
  const std::string arguments = "--port " + std::to_string(port) + " --agent-id " + std::to_string(id) +
                                " --generation " + std::to_string(generation) + " --concurrency " +
                                std::to_string(concurrency) + " --capability " + capability +
                                boot_argument + extra;
  if (!running.process.spawn(harness.agent_exe, arguments)) {
    std::printf("FAIL agent process could not be started\n");
    return false;
  }
  const std::string line = running.process.read_line();
  if (!contains(line, "AGENT_READY")) {
    std::printf("FAIL agent did not announce readiness (got '%s')\n", line.c_str());
    return false;
  }
  const std::size_t first_space = line.find(' ');
  const std::size_t second_space = line.find(' ', first_space + 1);
  running.id = AgentId{std::strtoull(line.substr(first_space + 1).c_str(), nullptr, 10)};
  const std::string boot_text = line.substr(second_space + 1);
  const auto boot = parse_hex128(boot_text);
  if (!boot.has_value()) {
    std::printf("FAIL agent announced an unparsable boot id '%s'\n", boot_text.c_str());
    return false;
  }
  running.boot = AgentBootId{*boot};
  running.started = true;
  report("agent " + std::to_string(running.id.value()) + " ready with boot " + boot_text);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path coordinator_exe;
  std::filesystem::path agent_exe;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--coordinator" && index + 1 < argc) {
      coordinator_exe = argv[++index];
    } else if (argument == "--agent" && index + 1 < argc) {
      agent_exe = argv[++index];
    } else if (argument == "--verbose") {
      g_verbose = true;
    }
  }
  if (coordinator_exe.empty() || agent_exe.empty()) {
    std::printf("usage: agent_scheduler_multiprocess_test --coordinator <exe> --agent <exe> [--verbose]\n");
    return 2;
  }
  if (const auto init = initialize_network(); !init.ok()) {
    std::printf("FAIL network initialization: %s\n", init.describe().c_str());
    return 1;
  }

  Harness harness;
  harness.coordinator_exe = coordinator_exe;
  harness.agent_exe = agent_exe;
  harness.scratch = std::filesystem::temp_directory_path() /
                    ("agent_scheduler_multiprocess_" + std::to_string(::GetCurrentProcessId()));
  std::filesystem::create_directories(harness.scratch);
  harness.state_file = harness.scratch / "coordinator.asstate";

  std::printf("== REAL multiprocess proof: independent OS processes over loopback TCP ==\n");
  std::printf("scratch: %s\n", harness.scratch.string().c_str());

  RunningCoordinator first;
  require(start_coordinator(harness, first, false, "first"), "coordinator process started");

  RunningAgent agent_a;
  RunningAgent agent_b;
  // Agent A holds its assignment long enough to be killed while the assignment is still
  // active, which is what the death proof requires.
  require(start_agent(harness, first.port, 1, 1, 3, "coding", agent_a, std::nullopt, " --work-ms 120000"),
          "agent A process started");
  require(start_agent(harness, first.port, 2, 1, 1, "coding", agent_b), "agent B process started");
  require(agent_a.boot != agent_b.boot, "agent incarnations are independent");

  ControlClientOptions client_options;
  client_options.port = first.port;
  ControlClient client(client_options);
  {
    const MutationResult connected = client.connect();
    if (!connected.ok()) {
      std::printf("connect diagnostic: %s\n", connected.describe().c_str());
      std::printf("coordinator alive after failed connect: %s\n",
                  first.process.alive() ? "yes" : "no");
    }
    require(connected.ok(), "control client connected to the coordinator process");
  }
  {
    std::string json;
    require(client.query(QueryKind::Agents, json).ok(), "agent query succeeded");
    require(contains(json, agent_a.boot.to_string()), "coordinator reports agent A incarnation");
    require(contains(json, agent_b.boot.to_string()), "coordinator reports agent B incarnation");
  }

  require(client.submit_work(work_for(1, "coding")).ok(), "work 1 submitted");
  require(observe(
              [&]() {
                std::string json;
                if (!client.query(QueryKind::Assignments, AgentId{}, AgentBootId{}, WorkId{1}, AssignmentId{},
                                  json)
                         .ok()) {
                  return false;
                }
                return contains(json, agent_a.boot.to_string()) && contains(json, "\"state\": \"DISPATCHED\"");
              },
              "work 1 was assigned to agent A and dispatched"),
          "work 1 assigned to agent A and dispatched");

  std::string assignments_json;
  require(client
              .query(QueryKind::Assignments, AgentId{}, AgentBootId{}, WorkId{1}, AssignmentId{},
                     assignments_json)
              .ok(),
          "assignment query succeeded");
  const std::string assignment_id_text = json_string_field(assignments_json, "id");
  const auto assignment_id = parse_hex128(assignment_id_text);
  require(assignment_id.has_value(), "assignment identity parses");
  const AgentBootId stale_boot = agent_a.boot;

  std::printf("-- killing agent A as a real OS process --\n");
  agent_a.process.terminate();
  require(!agent_a.process.alive(), "agent A process is dead");
  require(observe(
              [&]() {
                std::string json;
                if (!client.query(QueryKind::Agents, json).ok()) {
                  return false;
                }
                const std::size_t start = json.find(stale_boot.to_string());
                if (start == std::string::npos) {
                  return false;
                }
                return json.find("\"lifecycle\": \"LOST\"", start) != std::string::npos;
              },
              "coordinator observed agent A loss through the real control path"),
          "coordinator observed agent A loss");
  {
    std::string json;
    require(client.query(QueryKind::Fenced, json).ok(), "fenced query succeeded");
    require(contains(json, stale_boot.to_string()), "agent A incarnation is permanently fenced");
  }
  require(observe(
              [&]() {
                std::string json;
                if (!client.query(QueryKind::Assignments, AgentId{}, AgentBootId{}, WorkId{1}, AssignmentId{},
                                  json)
                         .ok()) {
                  return false;
                }
                return contains(json, "\"state\": \"LOST\"") ||
                       contains(json, "\"state\": \"INVALIDATED\"");
              },
              "the assignment bound to the dead incarnation was invalidated"),
          "the assignment bound to the dead incarnation was invalidated");

  if (assignment_id.has_value()) {
    ScheduleOutcome outcome = ScheduleOutcome::NoChange;
    const MutationResult dispatch =
        client.dispatch_assignment(AssignmentId{*assignment_id}, AssignmentGeneration{1}, outcome);
    require(!dispatch.ok(), "old assignment can no longer authorize fresh dispatch");
    require(outcome == ScheduleOutcome::RejectStaleAgentBoot || outcome == ScheduleOutcome::RejectFenced ||
                outcome == ScheduleOutcome::RejectStaleAssignment ||
                outcome == ScheduleOutcome::RejectLifecycle,
            "old assignment rejection is specific");
  }

  require(client.submit_work(work_for(2, "coding")).ok(), "work 2 submitted after agent A death");
  require(observe(
              [&]() {
                std::string json;
                if (!client.query(QueryKind::Assignments, AgentId{}, AgentBootId{}, WorkId{2}, AssignmentId{},
                                  json)
                         .ok()) {
                  return false;
                }
                return contains(json, agent_b.boot.to_string()) &&
                       json.find("\"agent\": ") != std::string::npos &&
                       !contains(json, agent_a.boot.to_string());
              },
              "work 2 was assigned to agent B"),
          "new work is never assigned to the dead incarnation");

  {
    ControlClientOptions replay_options;
    replay_options.port = first.port;
    ControlClient replay(replay_options);
    require(replay.connect().ok(), "replay client connected");
    const MutationResult stale_register = replay.register_agent(descriptor_for(1, 1, stale_boot, 3));
    require(!stale_register.ok(), "replayed registration from the dead incarnation is rejected");
    require(stale_register.error->outcome == ScheduleOutcome::RejectFenced ||
                stale_register.error->outcome == ScheduleOutcome::RejectStaleAgentGeneration,
            "stale registration rejection is specific");

    CapabilityProfile profile;
    profile.profile_id = CapabilityProfileId{1};
    profile.generation = AgentCapabilityGeneration{2};
    profile.boot = stale_boot;
    CapabilityEvidence evidence;
    evidence.name = "coding";
    evidence.state = CapabilityState::Observed;
    evidence.quality = 900;
    evidence.generation = profile.generation;
    evidence.boot = stale_boot;
    profile.capabilities.push_back(evidence);
    const MutationResult stale_publish = replay.publish_capabilities(AgentId{1}, stale_boot, profile);
    require(!stale_publish.ok(), "replayed capability publication is rejected");
    require(stale_publish.error->outcome == ScheduleOutcome::RejectStaleAgentBoot ||
                stale_publish.error->outcome == ScheduleOutcome::RejectFenced,
            "stale capability rejection is specific");

    AssignmentBinding binding;
    if (assignment_id.has_value()) {
      binding.id = AssignmentId{*assignment_id};
    }
    binding.generation = AssignmentGeneration{1};
    binding.agent = AgentId{1};
    binding.boot = stale_boot;
    require(!replay.assignment_ack(binding, true, "late ack").ok(),
            "replayed acknowledgement from the dead incarnation is rejected");
    require(!replay.observation(MessageType::WorkReleased, binding, "late release").ok(),
            "replayed release from the dead incarnation is rejected");
    require(!replay.observation(MessageType::WorkCompleted, binding, "late completion").ok(),
            "replayed completion from the dead incarnation is rejected");
    replay.close();
  }

  {
    std::string json;
    require(client
                .query(QueryKind::Agents, agent_b.id, AgentBootId{}, WorkId{}, AssignmentId{}, json)
                .ok(),
            "agent query after loss succeeded");
    require(contains(json, agent_b.boot.to_string()), "agent B is still known");
    require(!contains(json, "\"lifecycle\": \"LOST\""),
            "agent B authority was not affected by agent A death");
    require(!contains(json, agent_a.boot.to_string()), "the dead incarnation is not reported as current");
  }

  RunningAgent agent_a_prime;
  require(start_agent(harness, first.port, 1, 2, 5, "coding", agent_a_prime, std::nullopt,
                      " --capability accelerator:cuda"),
          "agent A-prime process started with the same identity and a fresh incarnation");
  require(agent_a_prime.boot != stale_boot, "A-prime has a fresh AgentBootId");
  require(observe(
              [&]() {
                std::string json;
                if (!client
                         .query(QueryKind::Agents, agent_a_prime.id, AgentBootId{}, WorkId{}, AssignmentId{},
                                json)
                         .ok()) {
                  return false;
                }
                return contains(json, agent_a_prime.boot.to_string()) &&
                       (contains(json, "\"lifecycle\": \"READY\"") ||
                        contains(json, "\"lifecycle\": \"BUSY\""));
              },
              "A-prime became eligible"),
          "A-prime became eligible");

  // Work 3 requires a capability only A-prime holds, so the fresh higher-generation
  // incarnation is the deterministic legal owner and no peer can satisfy it.
  require(client.submit_work(work_for_many(3, {"coding", "accelerator:cuda"})).ok(), "work 3 submitted");
  require(observe(
              [&]() {
                std::string json;
                if (!client.query(QueryKind::Assignments, AgentId{}, AgentBootId{}, WorkId{3}, AssignmentId{},
                                  json)
                         .ok()) {
                  return false;
                }
                return contains(json, agent_a_prime.boot.to_string()) &&
                       !contains(json, agent_a.boot.to_string());
              },
              "a fresh assignment targets A-prime"),
          "a fresh assignment may target A-prime when it legitimately wins");
  {
    std::string json;
    require(client.query(QueryKind::Fenced, json).ok(), "fenced query after reincarnation succeeded");
    require(contains(json, stale_boot.to_string()), "the old incarnation remains permanently fenced");
  }

  agent_b.process.terminate();
  require(observe(
              [&]() {
                std::string json;
                if (!client.query(QueryKind::Agents, agent_b.id, AgentBootId{}, WorkId{}, AssignmentId{}, json)
                         .ok()) {
                  return false;
                }
                return contains(json, "\"lifecycle\": \"LOST\"");
              },
              "coordinator observed agent B loss"),
          "coordinator observed agent B loss");
  {
    std::string json;
    require(client
                .query(QueryKind::Agents, agent_a_prime.id, AgentBootId{}, WorkId{}, AssignmentId{}, json)
                .ok(),
            "agent query after B death succeeded");
    require(contains(json, agent_a_prime.boot.to_string()), "A-prime is still known");
    require(!contains(json, "\"lifecycle\": \"LOST\""),
            "A-prime authority was not affected by agent B death");
  }

  client.close();

  std::printf("-- stopping the coordinator process cleanly and persisting state --\n");
  require(stop_coordinator(first), "first coordinator stopped cleanly");
  require(std::filesystem::exists(harness.state_file), "durable state file was written");

  RunningCoordinator second;
  require(start_coordinator(harness, second, true, "second"),
          "second coordinator process started and loaded state");
  ControlClientOptions restarted_options;
  restarted_options.port = second.port;
  ControlClient restarted(restarted_options);
  require(restarted.connect().ok(), "control client connected to the second coordinator");

  {
    std::string json;
    require(restarted.query(QueryKind::Agents, json).ok(), "agent query after restart succeeded");
    require(json.find("\"lifecycle\": \"READY\"") == std::string::npos,
            "no recovered agent is silently READY");
    require(json.find("\"lifecycle\": \"BUSY\"") == std::string::npos,
            "no recovered agent is silently BUSY");
    require(contains(json, "REVALIDATION_REQUIRED") || contains(json, "\"lifecycle\": \"LOST\""),
            "recovered agents require revalidation or remain lost");
  }
  {
    std::string json;
    require(restarted.query(QueryKind::Assignments, json).ok(), "assignment query after restart succeeded");
    require(json.find("\"state\": \"DISPATCHED\"") == std::string::npos,
            "no recovered assignment is still dispatchable");
    require(json.find("\"lease_current\": true") == std::string::npos,
            "no recovered assignment holds a current lease");
  }
  {
    std::string json;
    require(restarted.query(QueryKind::Invariants, json).ok(), "invariant query after restart succeeded");
    require(contains(json, "\"ok\": true"), "recovered state satisfies every invariant");
  }

  RunningAgent agent_c;
  require(start_agent(harness, second.port, 1, 3, 5, "coding", agent_c),
          "a fresh agent process re-registered after recovery");
  require(observe(
              [&]() {
                std::string json;
                if (!restarted
                         .query(QueryKind::Agents, agent_c.id, AgentBootId{}, WorkId{}, AssignmentId{}, json)
                         .ok()) {
                  return false;
                }
                return contains(json, agent_c.boot.to_string()) &&
                       (contains(json, "\"lifecycle\": \"READY\"") ||
                        contains(json, "\"lifecycle\": \"BUSY\""));
              },
              "the fresh incarnation became eligible after recovery"),
          "scheduling resumed under the new coordinator authority");

  require(restarted.submit_work(work_for(5, "coding")).ok(), "work 5 submitted after recovery");
  require(observe(
              [&]() {
                std::string json;
                if (!restarted
                         .query(QueryKind::Assignments, AgentId{}, AgentBootId{}, WorkId{5}, AssignmentId{},
                                json)
                         .ok()) {
                  return false;
                }
                return contains(json, agent_c.boot.to_string());
              },
              "work 5 was assigned after recovery"),
          "work was assigned after recovery");
  {
    // Work 3 was bound to a pre-restart assignment: recovery must have rejected that
    // authority rather than silently reviving it, and no second exclusive owner exists.
    std::string json;
    require(restarted
                .query(QueryKind::Assignments, AgentId{}, AgentBootId{}, WorkId{3}, AssignmentId{}, json)
                .ok(),
            "pre-restart assignment query succeeded");
    require(count_occurrences(json, "\"state\": \"ASSIGNED\"") +
                    count_occurrences(json, "\"state\": \"DISPATCHED\"") <=
                1,
            "no duplicate exclusive ownership was created during recovery");
    require(contains(json, "COORDINATOR_EPOCH_CHANGED") || contains(json, "\"state\": \"REVALIDATION_REQUIRED\"") ||
                contains(json, "\"state\": \"COMPLETED\"") || contains(json, "\"state\": \"RELEASED\"") ||
                contains(json, "\"state\": \"INVALIDATED\""),
            "pre-restart assignment authority was rejected rather than silently revived");
  }
  {
    std::string json;
    require(restarted.query(QueryKind::Invariants, json).ok(), "final invariant query succeeded");
    require(contains(json, "\"ok\": true"), "final state satisfies every invariant");
  }

  restarted.close();
  require(stop_coordinator(second), "second coordinator stopped cleanly");

  std::printf("harness complete: failures=%d\n", g_failures);
  std::error_code cleanup_error;
  std::filesystem::remove_all(harness.scratch, cleanup_error);
  return g_failures == 0 ? 0 : 1;
}
