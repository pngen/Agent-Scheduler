// Agent Scheduler — invariant checking over canonical state and derived indexes.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "agent_scheduler/scheduler.hpp"
#include "internal/scheduler_impl.hpp"

namespace agent_scheduler {
namespace {

using internal::Impl;

void violate(InvariantReport& report, std::string_view code, std::string subject, std::string detail) {
  InvariantViolation violation;
  violation.code = std::string(code);
  violation.subject = std::move(subject);
  violation.detail = std::move(detail);
  report.violations.push_back(std::move(violation));
}

[[nodiscard]] std::uint32_t canonical_active_on_agent(const Impl& impl, AgentId agent) {
  std::uint32_t count = 0;
  for (const auto& entry : impl.assignments) {
    if (entry.second.binding.agent == agent && assignment_state_is_active(entry.second.state)) {
      ++count;
    }
  }
  return count;
}

[[nodiscard]] std::uint32_t canonical_active_on_work(const Impl& impl, WorkId work) {
  std::uint32_t count = 0;
  for (const auto& entry : impl.assignments) {
    if (entry.second.binding.work == work && assignment_state_is_active(entry.second.state)) {
      ++count;
    }
  }
  return count;
}

}  // namespace

std::string InvariantReport::describe() const {
  if (violations.empty()) {
    return "OK checks=" + std::to_string(checks_run);
  }
  std::string out = "VIOLATIONS=" + std::to_string(violations.size()) + " checks=" +
                    std::to_string(checks_run);
  for (const InvariantViolation& violation : violations) {
    out += "\n  ";
    out += violation.code;
    out += " subject=";
    out += violation.subject;
    out += " ";
    out += violation.detail;
  }
  return out;
}

InvariantReport AgentScheduler::check_invariants() const {
  const std::lock_guard<std::mutex> guard(impl_->mutex);
  const Impl& impl = *impl_;
  InvariantReport report;
  report.state_revision = impl.state_revision;
  auto check = [&report](bool condition) {
    ++report.checks_run;
    return condition;
  };

  if (!check(!impl.scheduler_id.is_zero())) {
    violate(report, invariant::single_scheduler_epoch, "", "scheduler id is zero");
  }
  if (!check(impl.scheduler_epoch.value() != 0)) {
    violate(report, invariant::single_scheduler_epoch, "", "scheduler epoch is zero");
  }
  if (!check(impl.running ? impl.coordinator_epoch.value() != 0 : true)) {
    violate(report, invariant::single_scheduler_epoch, "", "running scheduler has no coordinator epoch");
  }

  std::map<WorkId, std::uint32_t> exclusive_per_work;
  std::map<WorkId, std::uint32_t> active_per_work;
  std::map<std::pair<WorkId, AgentId>, std::uint32_t> active_per_work_agent;
  std::uint64_t active_total = 0;

  for (const auto& entry : impl.assignments) {
    const AssignmentRecord& record = entry.second;
    const AssignmentBinding& binding = record.binding;
    const std::string subject = binding.id.to_string();
    if (!check(binding.generation.value() != 0)) {
      violate(report, invariant::assignment_generation_monotonic, subject,
              "assignment generation is zero");
    }
    const auto work_entry = impl.work.find(binding.work);
    if (!check(work_entry != impl.work.end())) {
      violate(report, invariant::assignment_references_known_work, subject,
              "assignment references an unknown work item");
    } else if (!check(work_entry->second.request.generation >= binding.work_generation)) {
      violate(report, invariant::work_generation_monotonic, subject,
              "work generation is below a bound assignment generation");
    }
    const auto agent_entry = impl.agents.find(binding.agent);
    if (!check(agent_entry != impl.agents.end())) {
      violate(report, invariant::assignment_references_known_agent, subject,
              "assignment references an unknown agent");
    } else if (!check(agent_entry->second.descriptor.generation >= binding.agent_generation)) {
      violate(report, invariant::agent_generation_monotonic, subject,
              "agent generation is below a bound assignment generation");
    }
    const bool active = assignment_state_is_active(record.state);
    if (active) {
      ++active_total;
      ++active_per_work[binding.work];
      ++active_per_work_agent[{binding.work, binding.agent}];
      if (binding.exclusive) {
        ++exclusive_per_work[binding.work];
      }
      const auto lease_entry = impl.leases.find(binding.lease);
      if (!check(lease_entry != impl.leases.end())) {
        violate(report, invariant::active_assignment_has_current_lease, subject,
                "active assignment has no lease record");
      }
      if (agent_entry != impl.agents.end() && internal::boot_is_fenced(impl, binding.boot)) {
        violate(report, invariant::fenced_boot_has_no_authority, subject,
                "active assignment references a fenced incarnation");
      }
      if (work_entry != impl.work.end() && work_is_terminal(work_entry->second.lifecycle)) {
        violate(report, invariant::no_dispatchable_cancelled_work, subject,
                "active assignment references terminal work");
      }
      if (agent_entry != impl.agents.end() &&
          (agent_entry->second.lifecycle == AgentLifecycle::Retired ||
           agent_entry->second.lifecycle == AgentLifecycle::Lost)) {
        violate(report, invariant::no_work_for_retired_agent, subject,
                "active assignment references a retired or lost agent");
      }
      const WorkRecord* work_record = work_entry == impl.work.end() ? nullptr : &work_entry->second;
      if (work_record != nullptr) {
        if (!check(work_record->request.requirements.max_parallel >= active_per_work[binding.work])) {
          violate(report, invariant::parallel_fanout_bounded, subject,
                  "active fan-out exceeds the declared maximum");
        }
        if (binding.exclusive && work_record->request.mode != SchedulingMode::Exclusive) {
          violate(report, invariant::exclusive_work_single_assignment, subject,
                  "assignment is exclusive but the work item is not");
        }
      }
    }
    if (assignment_state_is_terminal(record.state) && record.lease_current) {
      violate(report, invariant::lease_does_not_resurrect, subject,
              "terminal assignment still holds a current lease");
    }
    if (assignment_state_is_dispatchable(record.state)) {
      if (binding.scheduler_epoch != impl.scheduler_epoch) {
        violate(report, invariant::no_stale_authority_dispatchable, subject,
                "dispatchable assignment carries a stale scheduler epoch");
      }
      if (binding.coordinator_epoch != impl.coordinator_epoch) {
        violate(report, invariant::no_stale_authority_dispatchable, subject,
                "dispatchable assignment carries a stale coordinator epoch");
      }
      if (binding.policy != impl.policy.id || binding.policy_generation != impl.policy.generation) {
        violate(report, invariant::no_stale_authority_dispatchable, subject,
                "dispatchable assignment carries a stale policy generation");
      }
      if (agent_entry != impl.agents.end()) {
        if (agent_entry->second.descriptor.boot != binding.boot) {
          violate(report, invariant::dispatchable_boot_current, subject,
                  "dispatchable assignment does not reference the current incarnation");
        }
        if (agent_entry->second.profile.generation != binding.capability_generation) {
          violate(report, invariant::no_stale_authority_dispatchable, subject,
                  "dispatchable assignment carries a stale capability generation");
        }
      }
      if (impl.shutting_down) {
        violate(report, invariant::shutdown_blocks_new_assignment, subject,
                "dispatchable assignment exists after admission closed");
      }
    }
  }

  for (const auto& entry : exclusive_per_work) {
    if (!check(entry.second <= 1)) {
      violate(report, invariant::exclusive_assignment_uniqueness, entry.first.to_string(),
              "more than one active exclusive assignment for the same work generation");
    }
  }
  for (const auto& entry : active_per_work_agent) {
    const auto work_entry = impl.work.find(entry.first.first);
    if (work_entry == impl.work.end()) {
      continue;
    }
    if (!check(entry.second <= work_entry->second.request.requirements.max_assignments_per_agent)) {
      violate(report, invariant::parallel_fanout_bounded,
              entry.first.first.to_string() + "/" + entry.first.second.to_string(),
              "agent holds more than the permitted assignments for this work");
    }
  }
  if (!check(active_total <= impl.limits.max_active_assignments)) {
    violate(report, invariant::capacity_not_exceeded, "",
            "active assignment count exceeds max_active_assignments");
  }

  for (const auto& entry : impl.work) {
    const WorkRecord& record = entry.second;
    const std::string subject = entry.first.to_string();
    if (!check(record.request.generation.value() != 0)) {
      violate(report, invariant::work_generation_monotonic, subject, "work generation is zero");
    }
    if (!check(record.request.id == entry.first)) {
      violate(report, invariant::index_matches_canonical, subject, "work map key does not match record id");
    }
    const std::uint32_t canonical = canonical_active_on_work(impl, entry.first);
    if (!check(record.current_assignments == canonical)) {
      violate(report, invariant::queue_accounting_agrees, subject,
              "work assignment accounting disagrees with canonical assignments");
    }
    if (record.lifecycle == WorkLifecycle::Admitted && canonical != 0) {
      violate(report, invariant::work_lifecycle_agrees, subject,
              "admitted work has active assignments");
    }
    if (record.lifecycle == WorkLifecycle::Assigned && canonical == 0) {
      violate(report, invariant::work_lifecycle_agrees, subject,
              "assigned work has no active assignment");
    }
    if (work_is_terminal(record.lifecycle) && canonical != 0) {
      violate(report, invariant::work_lifecycle_agrees, subject, "terminal work has active assignments");
    }
    if (impl.queues.find(record.request.queue) == impl.queues.end()) {
      violate(report, invariant::index_matches_canonical, subject, "work references an unknown queue");
    }
    if (record.last_assignment_generation.value() != 0) {
      bool seen = false;
      for (const auto& assignment_entry : impl.assignments) {
        if (assignment_entry.second.binding.work == entry.first &&
            assignment_entry.second.binding.generation == record.last_assignment_generation) {
          seen = true;
          break;
        }
      }
      if (!check(seen)) {
        violate(report, invariant::assignment_generation_monotonic, subject,
                "last assignment generation does not correspond to a stored assignment");
      }
    }
  }

  for (const auto& entry : impl.agents) {
    const AgentRecord& record = entry.second;
    const std::string subject = entry.first.to_string();
    if (!check(record.descriptor.id == entry.first)) {
      violate(report, invariant::index_matches_canonical, subject, "agent map key does not match record id");
    }
    if (!check(record.descriptor.generation.value() != 0)) {
      violate(report, invariant::agent_generation_monotonic, subject, "agent generation is zero");
    }
    const std::uint32_t canonical = canonical_active_on_agent(impl, entry.first);
    if (!check(record.active_assignments == canonical)) {
      violate(report, invariant::agent_accounting_agrees, subject,
              "agent assignment accounting disagrees with canonical assignments");
    }
    if (!check(record.active_assignments <= record.descriptor.max_concurrency)) {
      violate(report, invariant::capacity_not_exceeded, subject,
              "agent active assignments exceed declared concurrency");
    }
    if (internal::boot_is_fenced(impl, record.descriptor.boot) &&
        (record.lifecycle == AgentLifecycle::Ready || record.lifecycle == AgentLifecycle::Busy)) {
      violate(report, invariant::fenced_boot_has_no_authority, subject,
              "fenced incarnation still reports an accepting lifecycle");
    }
    if (impl.recovered && record.recovered &&
        (record.capability_current || record.health_current || record.availability_current ||
         record.load_current)) {
      violate(report, invariant::recovered_evidence_not_current, subject,
              "recovered dynamic evidence is marked current");
    }
  }

  // Derived indexes must agree with a canonical scan.
  {
    std::map<std::string, std::set<AgentId>> capabilities;
    std::map<AgentLifecycle, std::set<AgentId>> lifecycles;
    std::map<TenantId, std::set<AgentId>> tenants;
    std::map<PlacementDomainId, std::set<AgentId>> domains;
    for (const auto& entry : impl.agents) {
      const AgentRecord& record = entry.second;
      lifecycles[record.lifecycle].insert(entry.first);
      tenants[record.descriptor.tenant].insert(entry.first);
      if (!record.descriptor.placement_domain.is_zero()) {
        domains[record.descriptor.placement_domain].insert(entry.first);
      }
      if (record.capability_current) {
        for (const CapabilityEvidence& evidence : record.profile.capabilities) {
          if (capability_state_is_usable(evidence.state)) {
            capabilities[evidence.name].insert(entry.first);
          }
        }
      }
    }
    auto compare = [&](const auto& index, const auto& expected, std::string_view label) {
      for (const auto& entry : index) {
        const auto found = expected.find(entry.first);
        const std::set<AgentId> empty;
        const std::set<AgentId>& other = found == expected.end() ? empty : found->second;
        if (entry.second != other) {
          violate(report, invariant::index_matches_canonical, std::string(label),
                  "indexed agent set disagrees with canonical scan");
        }
      }
      for (const auto& entry : expected) {
        if (index.find(entry.first) == index.end() && !entry.second.empty()) {
          violate(report, invariant::index_matches_canonical, std::string(label),
                  "canonical agent set is missing from the index");
        }
      }
    };
    ++report.checks_run;
    compare(impl.index_agents_by_lifecycle, lifecycles, "agents_by_lifecycle");
    compare(impl.index_agents_by_tenant, tenants, "agents_by_tenant");
    compare(impl.index_agents_by_domain, domains, "agents_by_domain");
    compare(impl.index_agents_by_capability, capabilities, "agents_by_capability");
  }
  {
    std::map<QueueId, std::set<WorkId>> by_queue;
    std::map<WorkLifecycle, std::set<WorkId>> by_lifecycle;
    for (const auto& entry : impl.work) {
      by_queue[entry.second.request.queue].insert(entry.first);
      by_lifecycle[entry.second.lifecycle].insert(entry.first);
    }
    auto compare_work = [&](const auto& index, const auto& expected, std::string_view label) {
      for (const auto& entry : index) {
        const auto found = expected.find(entry.first);
        const std::set<WorkId> empty;
        const std::set<WorkId>& other = found == expected.end() ? empty : found->second;
        if (entry.second != other) {
          violate(report, invariant::index_matches_canonical, std::string(label),
                  "indexed work set disagrees with canonical scan");
        }
      }
      for (const auto& entry : expected) {
        if (index.find(entry.first) == index.end() && !entry.second.empty()) {
          violate(report, invariant::index_matches_canonical, std::string(label),
                  "canonical work set is missing from the index");
        }
      }
    };
    ++report.checks_run;
    compare_work(impl.index_work_by_queue, by_queue, "work_by_queue");
    compare_work(impl.index_work_by_lifecycle, by_lifecycle, "work_by_lifecycle");
  }
  {
    std::map<AgentId, std::set<AssignmentId>> by_agent;
    std::map<WorkId, std::set<AssignmentId>> by_work;
    for (const auto& entry : impl.assignments) {
      if (!assignment_state_is_active(entry.second.state)) {
        continue;
      }
      by_agent[entry.second.binding.agent].insert(entry.first);
      by_work[entry.second.binding.work].insert(entry.first);
    }
    auto compare_assignments = [&](const auto& index, const auto& expected, std::string_view label) {
      for (const auto& entry : index) {
        const auto found = expected.find(entry.first);
        const std::set<AssignmentId> empty;
        const std::set<AssignmentId>& other = found == expected.end() ? empty : found->second;
        if (entry.second != other) {
          violate(report, invariant::index_matches_canonical, std::string(label),
                  "indexed assignment set disagrees with canonical scan");
        }
      }
      for (const auto& entry : expected) {
        if (index.find(entry.first) == index.end() && !entry.second.empty()) {
          violate(report, invariant::index_matches_canonical, std::string(label),
                  "canonical assignment set is missing from the index");
        }
      }
    };
    ++report.checks_run;
    compare_assignments(impl.index_assignments_by_agent, by_agent, "assignments_by_agent");
    compare_assignments(impl.index_assignments_by_work, by_work, "assignments_by_work");
  }

  if (!check(impl.assignments.size() <= static_cast<std::size_t>(impl.limits.max_active_assignments) +
                                           impl.limits.max_historical_assignments)) {
    violate(report, invariant::capacity_not_exceeded, "", "assignment history exceeds configured bounds");
  }
  if (!check(impl.leases.size() <= impl.limits.max_leases)) {
    violate(report, invariant::capacity_not_exceeded, "", "lease count exceeds max_leases");
  }
  if (!check(impl.fenced_boots.size() <= impl.limits.max_fenced_boots)) {
    violate(report, invariant::capacity_not_exceeded, "", "fenced boot count exceeds max_fenced_boots");
  }

  const Digest256 first = internal::compute_state_digest(impl);
  const Digest256 second = internal::compute_state_digest(impl);
  if (!check(first == second)) {
    violate(report, invariant::digest_matches_canonical, "", "state digest is not deterministic");
  }
  const bool empty_state = impl.agents.empty() && impl.work.empty() && impl.assignments.empty() &&
                          impl.queues.empty() && impl.leases.empty() && impl.fenced_boots.empty();
  if (!check(empty_state ? first.is_zero() : !first.is_zero())) {
    violate(report, invariant::digest_matches_canonical, "",
            empty_state ? "empty state produced a non-zero digest"
                        : "non-empty state produced a zero digest");
  }
  return report;
}

}  // namespace agent_scheduler
