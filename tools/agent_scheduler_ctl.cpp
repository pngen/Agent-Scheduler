// Agent Scheduler — control tool for a running coordinator.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include <cstdio>
#include <string>
#include <vector>

#include "agent_scheduler/net/control_client.hpp"
#include "agent_scheduler/scheduler.hpp"
#include "agent_scheduler/version.hpp"
#include "cli_support.hpp"

namespace {

using namespace agent_scheduler;

void print_help() {
  std::printf(
      "agent_scheduler_ctl - drive a running Agent Scheduler coordinator\n"
      "\n"
      "usage: agent_scheduler_ctl --port <n> <command> [options]\n"
      "\n"
      "commands:\n"
      "  --query <kind>        version|summary|agents|work|assignments|leases|fenced|\n"
      "                        invariants|fairness|capabilities|queues|policy\n"
      "  --submit-work <id>    submit a work item\n"
      "  --cancel-work <id>    cancel a work item\n"
      "  --dispatch <hex>      attempt pre-dispatch revalidation of an assignment\n"
      "  --drain-agent <id>    drain an agent\n"
      "  --deregister-agent <id> deregister an agent\n"
      "\n"
      "options:\n"
      "  --port <n>            coordinator port (required)\n"
      "  --host <text>         coordinator host (default 127.0.0.1)\n"
      "  --generation <n>      work or assignment generation (default 1)\n"
      "  --priority <n>        work priority 0..1000 (default 500)\n"
      "  --queue <n>           queue id (default 1)\n"
      "  --require <name>      repeatable required capability\n"
      "  --prefer <name>       repeatable preferred capability\n"
      "  --concurrency <n>     unused placeholder for agent commands\n"
      "  --boot <hex>          agent incarnation for agent commands\n"
      "  --force               force flag for drain/deregister\n"
      "  --json                emit the raw JSON result\n"
      "  --version             print version and exit\n"
      "  --help                print this help and exit\n");
}

[[nodiscard]] bool parse_query_kind(const std::string& text, agent_scheduler::net::QueryKind& out) {
  using agent_scheduler::net::QueryKind;
  if (text == "version") { out = QueryKind::Version; return true; }
  if (text == "summary") { out = QueryKind::Summary; return true; }
  if (text == "agents") { out = QueryKind::Agents; return true; }
  if (text == "work") { out = QueryKind::Work; return true; }
  if (text == "assignments") { out = QueryKind::Assignments; return true; }
  if (text == "leases") { out = QueryKind::Leases; return true; }
  if (text == "fenced") { out = QueryKind::Fenced; return true; }
  if (text == "invariants") { out = QueryKind::Invariants; return true; }
  if (text == "fairness") { out = QueryKind::Fairness; return true; }
  if (text == "capabilities") { out = QueryKind::Capabilities; return true; }
  if (text == "queues") { out = QueryKind::Queues; return true; }
  if (text == "policy") { out = QueryKind::Policy; return true; }
  return false;
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
  agent_scheduler::net::ControlClientOptions options;
  options.port = static_cast<std::uint16_t>(args.number("port", 0));
  options.host = args.text("host", "127.0.0.1");
  agent_scheduler::net::ControlClient client(options);
  if (const auto connected = client.connect(); !connected.ok()) {
    std::fprintf(stderr, "connect failed: %s\n", connected.describe().c_str());
    return 1;
  }

  const std::string query = args.text("query");
  if (!query.empty()) {
    agent_scheduler::net::QueryKind kind = agent_scheduler::net::QueryKind::Summary;
    if (!parse_query_kind(query, kind)) {
      std::fprintf(stderr, "unknown query kind '%s'\n", query.c_str());
      return 2;
    }
    std::string json;
    const MutationResult result = client.query(kind, json);
    if (!result.ok()) {
      std::fprintf(stderr, "query failed: %s\n", result.describe().c_str());
      return 1;
    }
    std::printf("%s\n", json.c_str());
    return 0;
  }

  if (args.has("submit-work")) {
    WorkRequest request;
    request.id = WorkId{args.number("submit-work", 0)};
    request.generation = WorkGeneration{args.number("generation", 1)};
    request.queue = QueueId{args.number("queue", 1)};
    request.queue_generation = QueueGeneration{1};
    request.kind = "ctl";
    request.priority = static_cast<std::uint32_t>(args.number("priority", 500));
    request.fairness_class = "default";
    request.requirements.tenant = TenantId{args.number("tenant", 1)};
    request.requirements.name_space = NamespaceId{1};
    for (const std::string& name : args.values("require")) {
      CapabilityRequirement requirement;
      requirement.name = name;
      requirement.mode = CapabilityRequirementMode::Required;
      requirement.minimum_state = CapabilityState::Observed;
      request.requirements.capabilities.push_back(std::move(requirement));
    }
    for (const std::string& name : args.values("prefer")) {
      CapabilityRequirement requirement;
      requirement.name = name;
      requirement.mode = CapabilityRequirementMode::Preferred;
      requirement.minimum_state = CapabilityState::Observed;
      request.requirements.capabilities.push_back(std::move(requirement));
    }
    canonicalize(request);
    const MutationResult result = client.submit_work(request);
    std::printf("%s\n", result.ok() ? "OK SUBMIT_WORK" : result.describe().c_str());
    return result.ok() ? 0 : 1;
  }

  if (args.has("cancel-work")) {
    const MutationResult result = client.cancel_work(WorkId{args.number("cancel-work", 0)},
                                                     WorkGeneration{args.number("generation", 1)},
                                                     args.text("reason", "cancelled by ctl"));
    std::printf("%s\n", result.ok() ? "OK CANCEL_WORK" : result.describe().c_str());
    return result.ok() ? 0 : 1;
  }

  if (args.has("dispatch")) {
    const auto parsed = as_cli::Args::parse_hash128(args.text("dispatch"));
    if (!parsed.has_value()) {
      std::fprintf(stderr, "--dispatch must be 32 hexadecimal characters\n");
      return 2;
    }
    ScheduleOutcome outcome = ScheduleOutcome::NoChange;
    const MutationResult result = client.dispatch_assignment(AssignmentId{*parsed},
                                                             AssignmentGeneration{args.number("generation", 1)},
                                                             outcome);
    std::printf("outcome=%s %s\n", to_string(outcome), result.ok() ? "OK" : result.describe().c_str());
    return result.ok() ? 0 : 1;
  }

  if (args.has("drain-agent")) {
    const auto boot = as_cli::Args::parse_hash128(args.text("boot"));
    const AgentBootId boot_id = boot.has_value() ? AgentBootId{*boot} : AgentBootId{};
    const MutationResult result = client.drain_agent(AgentId{args.number("drain-agent", 0)}, boot_id,
                                                     args.has("force"));
    std::printf("%s\n", result.ok() ? "OK DRAIN_AGENT" : result.describe().c_str());
    return result.ok() ? 0 : 1;
  }

  if (args.has("deregister-agent")) {
    const auto boot = as_cli::Args::parse_hash128(args.text("boot"));
    const AgentBootId boot_id = boot.has_value() ? AgentBootId{*boot} : AgentBootId{};
    const MutationResult result = client.deregister_agent(AgentId{args.number("deregister-agent", 0)}, boot_id,
                                                          args.has("force"));
    std::printf("%s\n", result.ok() ? "OK DEREGISTER_AGENT" : result.describe().c_str());
    return result.ok() ? 0 : 1;
  }

  std::fprintf(stderr, "no command given; use --help\n");
  return 2;
}
