// Agent Scheduler — example: run a coordinator that independent agent processes attach to.
//
// Start this example, then in another terminal run the shipped agent tool against the
// printed port. The coordinator persists state on a clean stop and reloads it on the
// next start.
//
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#include "agent_scheduler/net/coordinator.hpp"
#include "agent_scheduler/scheduler.hpp"

using namespace agent_scheduler;

int main(int argc, char** argv) {
  std::uint64_t run_seconds = 20;
  std::string state_path;
  if (argc > 1) {
    run_seconds = static_cast<std::uint64_t>(std::strtoull(argv[1], nullptr, 10));
  }
  if (argc > 2) {
    state_path = argv[2];
  }

  SchedulerOptions scheduler_options;
  scheduler_options.scheduler_id = SchedulerId{21};
  scheduler_options.auto_start = false;
  AgentScheduler scheduler(scheduler_options);

  net::CoordinatorOptions coordinator_options;
  coordinator_options.port = 0;
  coordinator_options.persistence_path = state_path;
  coordinator_options.load_on_start = !state_path.empty();

  net::CoordinatorServer server(scheduler, coordinator_options);
  if (const auto started = server.start(); !started.ok()) {
    std::printf("coordinator failed to start: %s\n", started.describe().c_str());
    return 1;
  }
  const std::uint16_t port = server.port().value_or(0);
  std::printf("coordinator listening on 127.0.0.1:%u\n", static_cast<unsigned>(port));
  std::printf("attach agents with:\n");
  std::printf("  agent_scheduler_agent --port %u --agent-id 1 --concurrency 2 --capability coding\n",
              static_cast<unsigned>(port));
  std::printf("submit work with:\n");
  std::printf("  agent_scheduler_ctl --port %u --submit-work 1 --require coding\n",
              static_cast<unsigned>(port));
  std::printf("inspect with:\n");
  std::printf("  agent_scheduler_ctl --port %u --query summary\n", static_cast<unsigned>(port));
  std::fflush(stdout);

  for (std::uint64_t second = 0; second < run_seconds; ++second) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  if (const auto stopped = server.stop(); !stopped.ok()) {
    std::printf("coordinator failed to stop: %s\n", stopped.describe().c_str());
    return 1;
  }
  const SchedulerSummary summary = scheduler.summary();
  std::printf("stopped: agents=%zu active_assignments=%zu fenced=%zu digest=%s\n", summary.agent_count,
              summary.active_assignments, summary.fenced_boots, summary.state_digest.to_string().c_str());
  if (!state_path.empty()) {
    std::printf("state written to %s\n", state_path.c_str());
  }
  return 0;
}
