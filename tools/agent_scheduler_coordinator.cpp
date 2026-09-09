// Agent Scheduler — coordinator process tool.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#include "agent_scheduler/net/coordinator.hpp"
#include "agent_scheduler/scheduler.hpp"
#include "agent_scheduler/version.hpp"
#include "cli_support.hpp"

namespace {

void print_help() {
  std::printf(
      "agent_scheduler_coordinator - run the Agent Scheduler coordinator\n"
      "\n"
      "usage: agent_scheduler_coordinator [options]\n"
      "\n"
      "  --port <n>            listen port (0 selects an ephemeral port, default 0)\n"
      "  --state <path>        durable state file (default: none)\n"
      "  --no-load             do not load an existing state file\n"
      "  --no-persist          do not save on stop\n"
      "  --run-ms <n>          stop after n milliseconds (0 runs until stopped)\n"
      "  --stop-file <path>    stop when this file exists\n"
      "  --no-auto-dispatch    do not dispatch assignments automatically\n"
      "  --version             print version and exit\n"
      "  --help                print this help and exit\n");
}

}  // namespace

int main(int argc, char** argv) {
  const as_cli::Args args(argc, argv);
  if (args.has("help")) {
    print_help();
    return 0;
  }
  if (args.has("version")) {
    std::printf("%s %s\n", std::string(agent_scheduler::product_name).c_str(),
                agent_scheduler::build_info().c_str());
    return 0;
  }
  if (const auto init = agent_scheduler::net::initialize_network(); !init.ok()) {
    std::fprintf(stderr, "network initialization failed: %s\n", init.describe().c_str());
    return 1;
  }

  agent_scheduler::SchedulerOptions scheduler_options;
  scheduler_options.scheduler_id = agent_scheduler::SchedulerId{1};
  scheduler_options.auto_start = false;
  agent_scheduler::AgentScheduler scheduler(scheduler_options);

  agent_scheduler::net::CoordinatorOptions options;
  options.port = static_cast<std::uint16_t>(args.number("port", 0));
  options.persistence_path = args.text("state");
  options.load_on_start = !args.has("no-load");
  options.persist_on_stop = !args.has("no-persist");
  options.auto_dispatch = !args.has("no-auto-dispatch");

  agent_scheduler::net::CoordinatorServer server(scheduler, options);
  if (const auto started = server.start(); !started.ok()) {
    std::fprintf(stderr, "coordinator start failed: %s\n", started.describe().c_str());
    return 1;
  }
  std::printf("LISTEN %u\n", static_cast<unsigned>(server.port().value_or(0)));
  std::fflush(stdout);

  const std::uint64_t run_ms = args.number("run-ms", 0);
  const std::string stop_file = args.text("stop-file");
  const auto start_time = std::chrono::steady_clock::now();
  for (;;) {
    if (!stop_file.empty() && std::filesystem::exists(stop_file)) {
      break;
    }
    if (run_ms != 0) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start_time)
                               .count();
      if (static_cast<std::uint64_t>(elapsed) >= run_ms) {
        break;
      }
    }
    if (!server.port().has_value()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (const auto stopped = server.stop(); !stopped.ok()) {
    std::fprintf(stderr, "coordinator stop failed: %s\n", stopped.describe().c_str());
    return 1;
  }
  const agent_scheduler::SchedulerSummary summary = scheduler.summary();
  std::printf("STOPPED agents=%zu work=%zu active_assignments=%zu fenced=%zu digest=%s\n",
              summary.agent_count,
              summary.work_admitted + summary.work_assigned + summary.work_executing,
              summary.active_assignments, summary.fenced_boots,
              summary.state_digest.to_string().c_str());
  std::fflush(stdout);
  agent_scheduler::net::shutdown_network();
  return 0;
}
