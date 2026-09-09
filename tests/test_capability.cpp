// Agent Scheduler — capability evidence and generation semantics.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "scheduler_fixture.hpp"
#include "test_support.hpp"

using namespace agent_scheduler;
using namespace as_fixture;

namespace {

struct Heterogeneous {
  std::unique_ptr<AgentScheduler> scheduler;
  AgentDescriptor coding;
  AgentDescriptor research;
  AgentDescriptor cuda;
};

[[nodiscard]] Heterogeneous make_heterogeneous() {
  Heterogeneous population{std::make_unique<AgentScheduler>(test_options()), make_agent(1, 1), make_agent(2, 2),
                           make_agent(3, 3)};
  population.coding.display_name = "coding-cpp-windows-repository";
  population.research.display_name = "research-network-document";
  population.cuda.display_name = "coding-cuda-cpp-windows";
  AS_CHECK(register_full(*population.scheduler, population.coding,
                         {{"coding", 900}, {"language:cpp", 900}, {"platform:windows", 900},
                          {"tool:repository", 900}})
               .ok());
  AS_CHECK(register_full(*population.scheduler, population.research,
                         {{"research", 900}, {"tool:network", 900}, {"tool:document", 900}})
               .ok());
  AS_CHECK(register_full(*population.scheduler, population.cuda,
                         {{"coding", 800}, {"language:cpp", 800}, {"platform:windows", 800},
                          {"accelerator:cuda", 900}})
               .ok());
  return population;
}

}  // namespace

AS_TEST(capability, hard_requirements_precede_ranking) {
  Heterogeneous population = make_heterogeneous();
  AS_CHECK(population.scheduler->
               submit_work(make_work(100, {"coding", "language:cpp", "platform:windows"}))
               .ok());
  const ScheduleDecision decision = population.scheduler->schedule(WorkId{100}, WorkGeneration{1});
  AS_CHECK(decision.outcome == ScheduleOutcome::Assigned);
  AS_CHECK(decision.eligible_count == 2);
  for (const CandidateEvaluation& candidate : decision.candidates) {
    if (candidate.agent == population.research.id) {
      AS_CHECK(candidate.rejection == ScheduleOutcome::RejectCapability);
    }
  }
}

AS_TEST(capability, unknown_requirement_rejects_every_agent) {
  Heterogeneous population = make_heterogeneous();
  AS_CHECK(population.scheduler->submit_work(make_work(100, {"capability:nonexistent"})).ok());
  const ScheduleDecision decision = population.scheduler->schedule(WorkId{100}, WorkGeneration{1});
  AS_CHECK(decision.outcome == ScheduleOutcome::NoEligibleAgent);
  AS_CHECK(decision.eligible_count == 0);
  AS_REQUIRE(decision.rejection_summary.size() == 1);
  AS_CHECK(decision.rejection_summary.front().find("REJECT_CAPABILITY=3") != std::string::npos);
}

AS_TEST(capability, preferred_capability_only_affects_eligible_candidates) {
  Heterogeneous population = make_heterogeneous();
  WorkRequest request = make_work(100, {"coding"});
  CapabilityRequirement preferred;
  preferred.name = "accelerator:cuda";
  preferred.mode = CapabilityRequirementMode::Preferred;
  preferred.minimum_state = CapabilityState::Observed;
  request.requirements.capabilities.push_back(preferred);
  canonicalize(request);
  AS_CHECK(population.scheduler->submit_work(request).ok());
  const ScheduleDecision decision = population.scheduler->schedule(WorkId{100}, WorkGeneration{1});
  AS_CHECK(decision.outcome == ScheduleOutcome::Assigned);
  AS_CHECK(decision.selected_agent == population.cuda.id);
  for (const CandidateEvaluation& candidate : decision.candidates) {
    if (candidate.agent == population.research.id) {
      AS_CHECK(candidate.rejection == ScheduleOutcome::RejectCapability);
      AS_CHECK(candidate.factors.empty());
    }
  }
}

AS_TEST(capability, minimum_quality_is_enforced) {
  Heterogeneous population = make_heterogeneous();
  WorkRequest request = make_work(100, {"accelerator:cuda"});
  request.requirements.capabilities.front().minimum_quality = 950;
  canonicalize(request);
  AS_CHECK(population.scheduler->submit_work(request).ok());
  const ScheduleDecision decision = population.scheduler->schedule(WorkId{100}, WorkGeneration{1});
  AS_CHECK(decision.outcome == ScheduleOutcome::NoEligibleAgent);
}

AS_TEST(capability, revoked_and_stale_evidence_never_satisfies_a_requirement) {
  Heterogeneous population = make_heterogeneous();
  CapabilityProfile revoked = make_profile(population.cuda, {{"coding", 900}, {"accelerator:cuda", 900}},
                                           900, AgentCapabilityGeneration{2});
  revoked.capabilities.back().state = CapabilityState::Revoked;
  AS_CHECK(population.scheduler->publish_capabilities(population.cuda.id, population.cuda.boot, revoked).ok());

  AS_CHECK(population.scheduler->submit_work(make_work(100, {"accelerator:cuda"})).ok());
  const ScheduleDecision decision = population.scheduler->schedule(WorkId{100}, WorkGeneration{1});
  AS_CHECK(decision.outcome == ScheduleOutcome::NoEligibleAgent);
  AS_REQUIRE(decision.candidates.size() == 3);
  for (const CandidateEvaluation& candidate : decision.candidates) {
    AS_CHECK(candidate.rejection == ScheduleOutcome::RejectCapability ||
             candidate.rejection == ScheduleOutcome::RejectStaleCapability);
  }
}

AS_TEST(capability, stale_capability_generation_is_rejected) {
  Heterogeneous population = make_heterogeneous();
  AS_CHECK(population.scheduler->
               publish_capabilities(population.coding.id, population.coding.boot,
                                     make_profile(population.coding, {{"coding", 900}}, 900,
                                                  AgentCapabilityGeneration{3}))
               .ok());
  const MutationResult stale = population.scheduler->publish_capabilities(
      population.coding.id, population.coding.boot,
      make_profile(population.coding, {{"coding", 100}}, 100, AgentCapabilityGeneration{2}));
  AS_CHECK(!stale.ok());
  AS_CHECK(stale.error->outcome == ScheduleOutcome::RejectStaleCapability);
  const MutationResult conflicting = population.scheduler->publish_capabilities(
      population.coding.id, population.coding.boot,
      make_profile(population.coding, {{"coding", 100}}, 100, AgentCapabilityGeneration{3}));
  AS_CHECK(!conflicting.ok());
  AS_CHECK(conflicting.error->outcome == ScheduleOutcome::RejectConflict);
}

AS_TEST(capability, capability_generation_change_invalidates_a_bound_assignment) {
  Heterogeneous population = make_heterogeneous();
  AS_CHECK(population.scheduler->submit_work(make_work(100, {"accelerator:cuda"})).ok());
  const ScheduleDecision decision = population.scheduler->schedule(WorkId{100}, WorkGeneration{1});
  AS_REQUIRE(decision.assignment.has_value());
  AS_CHECK(decision.selected_agent == population.cuda.id);

  AS_CHECK(population.scheduler->
               publish_capabilities(population.cuda.id, population.cuda.boot,
                                     make_profile(population.cuda,
                                                  {{"coding", 900}, {"accelerator:cuda", 900}}, 900,
                                                  AgentCapabilityGeneration{2}))
               .ok());
  const DispatchResult dispatch =
      population.scheduler->dispatch(decision.assignment->id, decision.assignment->generation);
  AS_CHECK(!dispatch.dispatched());
  AS_CHECK(dispatch.outcome == ScheduleOutcome::RejectStaleCapability ||
           dispatch.outcome == ScheduleOutcome::RevalidationRequired);
  AS_CHECK(population.scheduler->assignment(decision.assignment->id)->state ==
           AssignmentState::RevalidationRequired);
  const RevalidationReport report = population.scheduler->revalidate_assignments();
  AS_CHECK(report.invalidated == 1);
  AS_CHECK(population.scheduler->assignment(decision.assignment->id)->state == AssignmentState::Invalidated);
}

AS_TEST(capability, evidence_from_a_foreign_boot_is_rejected) {
  Heterogeneous population = make_heterogeneous();
  CapabilityProfile foreign = make_profile(population.coding, {{"coding", 900}}, 900,
                                           AgentCapabilityGeneration{2});
  foreign.boot = AgentBootId{derive(4242, 1)};
  const MutationResult result = population.scheduler->publish_capabilities(
      population.coding.id, population.coding.boot, foreign);
  AS_CHECK(!result.ok());
  AS_CHECK(result.error->outcome == ScheduleOutcome::RejectStaleAgentBoot);

  CapabilityProfile correct_boot = make_profile(population.coding, {{"coding", 900}}, 900,
                                                AgentCapabilityGeneration{2});
  AS_CHECK(population.scheduler->
               publish_capabilities(population.coding.id, population.coding.boot, correct_boot)
               .ok());
  CapabilityProfile wrong_evidence_boot = correct_boot;
  wrong_evidence_boot.generation = AgentCapabilityGeneration{3};
  wrong_evidence_boot.capabilities.front().boot = AgentBootId{derive(4242, 2)};
  const MutationResult rejected = population.scheduler->publish_capabilities(
      population.coding.id, population.coding.boot, wrong_evidence_boot);
  AS_CHECK(!rejected.ok());
  AS_CHECK(rejected.error->outcome == ScheduleOutcome::RejectStaleAgentBoot);
  AS_CHECK(population.scheduler->submit_work(make_work(100, {"coding"})).ok());
  const ScheduleDecision decision = population.scheduler->schedule(WorkId{100}, WorkGeneration{1});
  AS_CHECK(decision.outcome == ScheduleOutcome::Assigned);
}

AS_TEST(capability, capability_profile_validation_rejects_hostile_input) {
  Heterogeneous population = make_heterogeneous();
  CapabilityProfile oversized = make_profile(population.coding, {{"coding", 900}}, 900,
                                             AgentCapabilityGeneration{4});
  oversized.capabilities.front().name = std::string(300, 'x');
  AS_CHECK(!population.scheduler->publish_capabilities(population.coding.id, population.coding.boot, oversized).ok());

  CapabilityProfile control_chars = make_profile(population.coding, {{"coding", 900}}, 900,
                                                 AgentCapabilityGeneration{4});
  control_chars.capabilities.front().name = std::string("bad\nname");
  AS_CHECK(!population.scheduler->publish_capabilities(population.coding.id, population.coding.boot, control_chars).ok());

  CapabilityProfile duplicated = make_profile(population.coding, {{"coding", 900}, {"coding", 900}}, 900,
                                              AgentCapabilityGeneration{4});
  AS_CHECK(!population.scheduler->publish_capabilities(population.coding.id, population.coding.boot, duplicated).ok());

  CapabilityProfile bad_quality = make_profile(population.coding, {{"coding", 900}}, 900,
                                               AgentCapabilityGeneration{4});
  bad_quality.capabilities.front().quality = 5000;
  AS_CHECK(!population.scheduler->publish_capabilities(population.coding.id, population.coding.boot, bad_quality).ok());
}
