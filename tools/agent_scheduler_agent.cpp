// Agent Scheduler — persistent agent worker process tool.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agent_scheduler/clock.hpp"
#include "agent_scheduler/net/agent_client.hpp"
#include "agent_scheduler/scheduler.hpp"
#include "agent_scheduler/version.hpp"
#include "cli_support.hpp"

namespace {

void print_help() {
  std::printf(
      "agent_scheduler_agent - run one persistent agent worker process\n"
      "\n"
      "usage: agent_scheduler_agent --port <n> [options]\n"
      "\n"
      "  --port <n>            coordinator port (required)\n"
      "  --agent-id <n>        agent identity (default 1)\n"
      "  --generation <n>      agent generation (default 1)\n"
      "  --boot <hex>          incarnation id; omitted means a fresh one\n"
      "  --concurrency <n>     declared maximum concurrent assignments (default 2)\n"
      "  --capability <name>   repeatable capability name (default: coding)\n"
      "  --tenant <n>          tenant id (default 1)\n"
      "  --domain <n>          placement domain id (default 1)\n"
      "  --quality <n>         capability quality 0..1000 (default 800)\n"
      "  --work-ms <n>         simulated handling duration per assignment\n"
      "  --run-ms <n>          stop after n milliseconds (0 runs until stopped)\n"
      "  --stop-file <path>    stop when this file exists\n"
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
  if (!args.has("port")) {
    std::fprintf(stderr, "missing required --port\n");
    return 2;
  }

  agent_scheduler::net::AgentClientOptions options;
  options.port = static_cast<std::uint16_t>(args.number("port", 0));
  options.descriptor.id = agent_scheduler::AgentId{args.number("agent-id", 1)};
  options.descriptor.generation = agent_scheduler::AgentGeneration{args.number("generation", 1)};
  const std::string boot_text = args.text("boot");
  if (!boot_text.empty()) {
    const auto boot = as_cli::Args::parse_hash128(boot_text);
    if (!boot.has_value()) {
      std::fprintf(stderr, "--boot must be 32 hexadecimal characters\n");
      return 2;
    }
    options.descriptor.boot = agent_scheduler::AgentBootId{*boot};
  } else {
    agent_scheduler::Hasher256 hasher;
    hasher.update_field_tag(0xB0);
    hasher.update(agent_scheduler::process_nonce());
    hasher.update(agent_scheduler::wall_clock_ms());
    options.descriptor.boot = agent_scheduler::AgentBootId{agent_scheduler::hash128_of(hasher.final())};
  }
  options.descriptor.registration_generation = agent_scheduler::AgentRegistrationGeneration{1};
  options.descriptor.tenant = agent_scheduler::TenantId{args.number("tenant", 1)};
  options.descriptor.name_space = agent_scheduler::NamespaceId{1};
  options.descriptor.display_name = "agent-" + std::to_string(options.descriptor.id.value());
  options.descriptor.policy_labels = {"worker"};
  options.descriptor.placement_domain = agent_scheduler::PlacementDomainId{args.number("domain", 1)};
  options.descriptor.topology_epoch = agent_scheduler::TopologyEpoch{1};
  options.descriptor.max_concurrency = static_cast<std::uint32_t>(args.number("concurrency", 2));
  options.descriptor.lease_ttl_ms = 600000;
  options.descriptor.provenance = "agent_scheduler_agent";
  options.simulated_work_ms = args.number("work-ms", 0);
  options.run_duration_ms = args.number("run-ms", 0);

  std::vector<std::string> capability_names = args.values("capability");
  if (capability_names.empty()) {
    capability_names.push_back("coding");
  }
  const std::uint32_t quality = static_cast<std::uint32_t>(args.number("quality", 800));
  options.profile.profile_id = agent_scheduler::CapabilityProfileId{options.descriptor.id.value()};
  options.profile.generation = agent_scheduler::AgentCapabilityGeneration{1};
  options.profile.boot = options.descriptor.boot;
  for (const std::string& name : capability_names) {
    agent_scheduler::CapabilityEvidence evidence;
    evidence.name = name;
    evidence.state = agent_scheduler::CapabilityState::Observed;
    evidence.quality = quality;
    evidence.generation = options.profile.generation;
    evidence.boot = options.descriptor.boot;
    evidence.provenance = "agent_scheduler_agent";
    options.profile.capabilities.push_back(std::move(evidence));
  }
  options.health.generation = agent_scheduler::AgentHealthGeneration{1};
  options.health.health = agent_scheduler::AgentHealth::Healthy;
  options.health.health_quality = 900;
  options.health.detail = "agent tool";
  options.availability.generation = agent_scheduler::AgentAvailabilityGeneration{1};
  options.availability.availability = agent_scheduler::AgentAvailability::Available;
  options.availability.reachability = agent_scheduler::AgentReachability::Reachable;
  options.availability.readiness = agent_scheduler::AgentReadiness::Ready;
  options.load.generation = agent_scheduler::AgentLoadGeneration{1};
  options.load.max_concurrency = options.descriptor.max_concurrency;
  options.load.estimated_dispatch_latency_ms = 2;
  options.load.cost_index = 0.5;
  options.load.slo_headroom_fraction = 0.9;

  agent_scheduler::net::AgentClient client(std::move(options));
  if (const auto registered = client.connect_and_register(); !registered.ok()) {
    std::fprintf(stderr, "registration failed: %s\n", registered.describe().c_str());
    return 1;
  }
  if (const auto published = client.publish_health(); !published.ok()) {
    std::fprintf(stderr, "health publication failed: %s\n", published.describe().c_str());
    return 1;
  }
  if (const auto published = client.publish_availability(); !published.ok()) {
    std::fprintf(stderr, "availability publication failed: %s\n", published.describe().c_str());
    return 1;
  }
  if (const auto published = client.publish_load(); !published.ok()) {
    std::fprintf(stderr, "load publication failed: %s\n", published.describe().c_str());
    return 1;
  }
  std::printf("AGENT_READY %llu %s\n", static_cast<unsigned long long>(client.descriptor().id.value()),
              client.descriptor().boot.to_string().c_str());
  std::fflush(stdout);

  std::atomic<bool> stop{false};
  client.on_shutdown = [&stop]() { stop.store(true); };
  const std::string stop_file = args.text("stop-file");
  std::thread watcher([&]() {
    while (!stop.load()) {
      if (!stop_file.empty() && std::filesystem::exists(stop_file)) {
        stop.store(true);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });
  (void)client.run(stop);
  stop.store(true);
  watcher.join();
  client.stop();
  std::printf("AGENT_STOPPED %llu assignments=%zu\n",
              static_cast<unsigned long long>(client.descriptor().id.value()),
              client.received_assignments().size());
  std::fflush(stdout);
  return 0;
}
