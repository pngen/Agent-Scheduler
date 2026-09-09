// Agent Scheduler — state inspection tool.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "agent_scheduler/json.hpp"
#include "agent_scheduler/persistence.hpp"
#include "agent_scheduler/scheduler.hpp"
#include "agent_scheduler/synthetic.hpp"
#include "agent_scheduler/version.hpp"
#include "cli_support.hpp"

namespace {

using namespace agent_scheduler;

void print_help() {
  std::printf(
      "agent_scheduler_inspect - inspect Agent Scheduler state\n"
      "\n"
      "usage: agent_scheduler_inspect [options]\n"
      "\n"
      "  --state <path>        durable state file to inspect\n"
      "  --validate            validate the file header and check invariants\n"
      "  --summary             scheduler summary\n"
      "  --agents              agent records\n"
      "  --queues              queue and work accounting\n"
      "  --assignments         assignment records\n"
      "  --leases              lease records\n"
      "  --fenced              fenced incarnations\n"
      "  --capabilities        capability evidence\n"
      "  --fairness            fairness accounting\n"
      "  --invariants          invariant report\n"
      "  --policy              active policy\n"
      "  --synthetic           build a deterministic synthetic population\n"
      "  --seed <n>            synthetic seed (default 1)\n"
      "  --agents-count <n>    synthetic agent count (default 32)\n"
      "  --work-count <n>      synthetic work count (default 128)\n"
      "  --rounds <n>          synthetic scheduling rounds (default 4)\n"
      "  --json                emit deterministic JSON\n"
      "  --version             print version and exit\n"
      "  --help                print this help and exit\n");
}

void json_error(JsonWriter& writer, const SchedulerError& error) {
  writer.begin_object();
  writer.key("code").value(to_string(error.code));
  writer.key("operation").value(error.operation);
  writer.key("subject").value(error.subject);
  writer.key("outcome").value(to_string(error.outcome));
  writer.key("detail").value(error.detail);
  writer.end_object();
}

void render_summary(JsonWriter& writer, const SchedulerSummary& summary) {
  writer.begin_object();
  writer.key("scheduler_id").value(summary.scheduler_id.to_string());
  writer.key("scheduler_epoch").value(summary.scheduler_epoch.value());
  writer.key("coordinator_epoch").value(summary.coordinator_epoch.value());
  writer.key("policy_id").value(summary.policy.to_string());
  writer.key("policy_generation").value(summary.policy_generation.value());
  writer.key("running").value(summary.running);
  writer.key("admission_open").value(summary.admission_open);
  writer.key("shutting_down").value(summary.shutting_down);
  writer.key("agents").value(static_cast<std::uint64_t>(summary.agent_count));
  writer.key("queues").value(static_cast<std::uint64_t>(summary.queue_count));
  writer.key("work_admitted").value(static_cast<std::uint64_t>(summary.work_admitted));
  writer.key("work_assigned").value(static_cast<std::uint64_t>(summary.work_assigned));
  writer.key("work_executing").value(static_cast<std::uint64_t>(summary.work_executing));
  writer.key("work_completed").value(static_cast<std::uint64_t>(summary.work_completed));
  writer.key("work_cancelled").value(static_cast<std::uint64_t>(summary.work_cancelled));
  writer.key("active_assignments").value(static_cast<std::uint64_t>(summary.active_assignments));
  writer.key("dispatchable_assignments").value(static_cast<std::uint64_t>(summary.dispatchable_assignments));
  writer.key("historical_assignments").value(static_cast<std::uint64_t>(summary.historical_assignments));
  writer.key("current_leases").value(static_cast<std::uint64_t>(summary.current_leases));
  writer.key("fenced_boots").value(static_cast<std::uint64_t>(summary.fenced_boots));
  writer.key("total_capacity").value(summary.total_capacity);
  writer.key("used_capacity").value(summary.used_capacity);
  writer.key("scheduling_rounds").value(summary.scheduling_rounds);
  writer.key("state_revision").value(summary.state_revision);
  writer.key("state_digest").value(summary.state_digest.to_string());
  writer.end_object();
}

void render_agents(JsonWriter& writer, const std::vector<AgentStatus>& agents) {
  writer.begin_array();
  for (const AgentStatus& agent : agents) {
    writer.begin_object();
    writer.key("id").value(agent.id.to_string());
    writer.key("generation").value(agent.generation.value());
    writer.key("boot").value(agent.boot.to_string());
    writer.key("lifecycle").value(to_string(agent.lifecycle));
    writer.key("health").value(to_string(agent.health));
    writer.key("readiness").value(to_string(agent.readiness));
    writer.key("reachability").value(to_string(agent.reachability));
    writer.key("availability").value(to_string(agent.availability));
    writer.key("max_concurrency").value(static_cast<std::uint64_t>(agent.max_concurrency));
    writer.key("active_assignments").value(static_cast<std::uint64_t>(agent.active_assignments));
    writer.key("available_capacity").value(static_cast<std::uint64_t>(agent.available_capacity));
    writer.key("lease_current").value(agent.lease_current);
    writer.key("evidence_current").value(agent.evidence_current);
    writer.key("capability_generation").value(agent.capability_generation.value());
    writer.key("placement_domain").value(agent.placement_domain.to_string());
    writer.key("reason").value(agent.reason);
    writer.end_object();
  }
  writer.end_array();
}

void render_assignments(JsonWriter& writer, const std::vector<Assignment>& assignments) {
  writer.begin_array();
  for (const Assignment& assignment : assignments) {
    writer.begin_object();
    writer.key("id").value(assignment.binding.id.to_string());
    writer.key("generation").value(assignment.binding.generation.value());
    writer.key("work").value(assignment.binding.work.to_string());
    writer.key("work_generation").value(assignment.binding.work_generation.value());
    writer.key("agent").value(assignment.binding.agent.to_string());
    writer.key("agent_generation").value(assignment.binding.agent_generation.value());
    writer.key("boot").value(assignment.binding.boot.to_string());
    writer.key("scheduler_epoch").value(assignment.binding.scheduler_epoch.value());
    writer.key("coordinator_epoch").value(assignment.binding.coordinator_epoch.value());
    writer.key("policy_generation").value(assignment.binding.policy_generation.value());
    writer.key("capability_generation").value(assignment.binding.capability_generation.value());
    writer.key("lease").value(assignment.binding.lease.to_string());
    writer.key("dispatch").value(assignment.binding.dispatch.to_string());
    writer.key("state").value(to_string(assignment.state));
    writer.key("invalidation").value(to_string(assignment.invalidation));
    writer.key("lease_current").value(assignment.lease_current);
    writer.key("replica_index").value(static_cast<std::uint64_t>(assignment.binding.replica_index));
    writer.key("exclusive").value(assignment.binding.exclusive);
    writer.key("reason").value(assignment.reason);
    writer.end_object();
  }
  writer.end_array();
}

void render_leases(JsonWriter& writer, const std::vector<Lease>& leases) {
  writer.begin_array();
  for (const Lease& lease : leases) {
    writer.begin_object();
    writer.key("id").value(lease.id.to_string());
    writer.key("generation").value(lease.generation.value());
    writer.key("assignment").value(lease.assignment.to_string());
    writer.key("agent").value(lease.agent.to_string());
    writer.key("boot").value(lease.boot.to_string());
    writer.key("current").value(lease.current);
    writer.key("expires_at_ms").value(lease.expires_at_ms);
    writer.end_object();
  }
  writer.end_array();
}

void render_fenced(JsonWriter& writer, const std::vector<FencedBoot>& fenced) {
  writer.begin_array();
  for (const FencedBoot& entry : fenced) {
    writer.begin_object();
    writer.key("agent").value(entry.agent.to_string());
    writer.key("boot").value(entry.boot.to_string());
    writer.key("generation").value(entry.generation.value());
    writer.key("reason").value(entry.reason);
    writer.end_object();
  }
  writer.end_array();
}

void render_fairness(JsonWriter& writer, const FairnessSnapshot& fairness) {
  writer.begin_object();
  writer.key("bounded_starvation").value(fairness.bounded_starvation);
  writer.key("max_priority_bypass").value(static_cast<std::uint64_t>(fairness.max_priority_bypass));
  writer.key("aging_rounds_per_step").value(fairness.aging_rounds_per_step);
  writer.key("scheduling_rounds").value(fairness.scheduling_rounds);
  writer.key("starved").value(static_cast<std::uint64_t>(fairness.starved_count));
  writer.key("must_run").value(static_cast<std::uint64_t>(fairness.must_run_count));
  writer.key("entries");
  writer.begin_array();
  for (const FairnessEntry& entry : fairness.entries) {
    writer.begin_object();
    writer.key("work").value(entry.work.to_string());
    writer.key("queue").value(entry.queue.to_string());
    writer.key("fairness_class").value(entry.fairness_class);
    writer.key("priority").value(static_cast<std::uint64_t>(entry.priority));
    writer.key("effective_priority").value(entry.effective_priority);
    writer.key("bypass_count").value(static_cast<std::uint64_t>(entry.bypass_count));
    writer.key("bypass_ceiling").value(static_cast<std::uint64_t>(entry.bypass_ceiling));
    writer.key("starvation_rounds").value(static_cast<std::uint64_t>(entry.starvation_rounds));
    writer.key("must_run").value(entry.must_run);
    writer.key("starved").value(entry.starved);
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
}

void render_invariants(JsonWriter& writer, const InvariantReport& report) {
  writer.begin_object();
  writer.key("ok").value(report.ok());
  writer.key("checks").value(static_cast<std::uint64_t>(report.checks_run));
  writer.key("state_revision").value(report.state_revision);
  writer.key("violations");
  writer.begin_array();
  for (const InvariantViolation& violation : report.violations) {
    writer.begin_object();
    writer.key("code").value(violation.code);
    writer.key("subject").value(violation.subject);
    writer.key("detail").value(violation.detail);
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
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

  agent_scheduler::SchedulerOptions options;
  options.scheduler_id = agent_scheduler::SchedulerId{1};
  options.auto_start = false;
  agent_scheduler::AgentScheduler scheduler(options);
  bool loaded = false;

  const std::string state_path = args.text("state");
  if (!state_path.empty()) {
    PersistenceHeaderInfo header;
    if (const auto header_result = read_persistence_header(state_path, header); !header_result.ok()) {
      std::fprintf(stderr, "state file rejected: %s\n", header_result.describe().c_str());
      return 1;
    }
    if (const auto loaded_result = scheduler.load(state_path); !loaded_result.ok()) {
      std::fprintf(stderr, "state file rejected: %s\n", loaded_result.describe().c_str());
      return 1;
    }
    loaded = true;
  }

  if (args.has("synthetic")) {
    SyntheticConfig config;
    config.seed = args.number("seed", 1);
    config.agent_count = static_cast<std::uint32_t>(args.number("agents-count", 32));
    config.work_count = static_cast<std::uint32_t>(args.number("work-count", 128));
    config.queue_count = 4;
    config.tenant_count = 2;
    config.capability_density = 4;
    config.placement_domains = 2;
    config.agent_death_percent = 10;
    config.reincarnation_percent = 10;
    agent_scheduler::SchedulerOptions run_options;
    run_options.scheduler_id = agent_scheduler::SchedulerId{1};
    agent_scheduler::AgentScheduler synthetic_scheduler(run_options);
    const SyntheticLaboratory laboratory(config);
    const SyntheticLaboratory::RunReport report =
        laboratory.run(synthetic_scheduler, static_cast<std::uint32_t>(args.number("rounds", 4)));
    JsonWriter writer;
    writer.begin_object();
    writer.key("synthetic").value(true);
    writer.key("seed").value(config.seed);
    writer.key("agents_registered").value(static_cast<std::uint64_t>(report.agents_registered));
    writer.key("work_admitted").value(static_cast<std::uint64_t>(report.work_admitted));
    writer.key("assignments").value(static_cast<std::uint64_t>(report.assignments));
    writer.key("deferred").value(static_cast<std::uint64_t>(report.deferred));
    writer.key("rejected").value(static_cast<std::uint64_t>(report.rejected));
    writer.key("agent_deaths").value(static_cast<std::uint64_t>(report.agent_deaths));
    writer.key("reincarnations").value(static_cast<std::uint64_t>(report.reincarnations));
    writer.key("rounds").value(report.rounds);
    writer.key("state_digest").value(report.final_state_digest.to_string());
    writer.key("invariants");
    render_invariants(writer, report.invariants);
    writer.end_object();
    std::printf("%s\n", writer.str().c_str());
    return report.invariants.ok() ? 0 : 1;
  }

  if (!loaded) {
    std::fprintf(stderr, "no --state file and no --synthetic request\n");
    return 2;
  }

  const bool json = args.has("json");
  const bool any_view = args.has("summary") || args.has("agents") || args.has("queues") ||
                        args.has("assignments") || args.has("leases") || args.has("fenced") ||
                        args.has("capabilities") || args.has("fairness") || args.has("invariants") ||
                        args.has("policy") || args.has("validate");
  JsonWriter writer;
  writer.begin_object();
  writer.key("state_file").value(state_path);
  writer.key("version").value(std::string(agent_scheduler::version_string));
  if (!any_view || args.has("summary") || args.has("validate")) {
    writer.key("summary");
    render_summary(writer, scheduler.summary());
  }
  if (args.has("agents") || args.has("capabilities")) {
    writer.key("agents");
    render_agents(writer, scheduler.all_agents());
  }
  if (args.has("queues")) {
    writer.key("work");
    writer.begin_array();
    for (const WorkStatus& work : scheduler.all_work()) {
      writer.begin_object();
      writer.key("id").value(work.id.to_string());
      writer.key("queue").value(work.queue.to_string());
      writer.key("generation").value(work.generation.value());
      writer.key("lifecycle").value(to_string(work.lifecycle));
      writer.key("priority").value(static_cast<std::uint64_t>(work.priority));
      writer.key("fairness_class").value(work.fairness_class);
      writer.key("current_assignments").value(static_cast<std::uint64_t>(work.current_assignments));
      writer.key("bypass_count").value(static_cast<std::uint64_t>(work.bypass_count));
      writer.key("starvation_rounds").value(static_cast<std::uint64_t>(work.starvation_rounds));
      writer.end_object();
    }
    writer.end_array();
  }
  if (args.has("assignments")) {
    writer.key("assignments");
    render_assignments(writer, scheduler.all_assignments());
  }
  if (args.has("leases")) {
    writer.key("leases");
    render_leases(writer, scheduler.leases());
  }
  if (args.has("fenced")) {
    writer.key("fenced");
    render_fenced(writer, scheduler.fenced_boots());
  }
  if (args.has("fairness")) {
    writer.key("fairness");
    render_fairness(writer, scheduler.fairness());
  }
  if (args.has("policy")) {
    const PolicySnapshot policy = scheduler.policy();
    writer.key("policy");
    writer.begin_object();
    writer.key("id").value(policy.id.to_string());
    writer.key("generation").value(policy.generation.value());
    writer.key("max_priority_bypass").value(static_cast<std::uint64_t>(policy.max_priority_bypass));
    writer.key("aging_rounds_per_step").value(static_cast<std::uint64_t>(policy.aging_rounds_per_step));
    writer.key("starvation_alert_rounds").value(static_cast<std::uint64_t>(policy.starvation_alert_rounds));
    writer.key("allow_unbounded_starvation").value(policy.allow_unbounded_starvation);
    writer.key("assignment_lease_ttl_ms").value(policy.assignment_lease_ttl_ms);
    writer.key("registration_lease_ttl_ms").value(policy.registration_lease_ttl_ms);
    writer.end_object();
  }
  const InvariantReport report = scheduler.check_invariants();
  if (!any_view || args.has("invariants") || args.has("validate")) {
    writer.key("invariants");
    render_invariants(writer, report);
  }
  writer.end_object();
  std::printf("%s\n", writer.str().c_str());
  (void)json;
  return report.ok() ? 0 : 1;
}
