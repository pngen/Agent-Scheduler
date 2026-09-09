# Agent Scheduler

**Scheduling authority for persistent autonomous workers.**

Agent Scheduler is a standalone C++20 infrastructure component that answers one systems
question:

> Which persistent autonomous agent should own this work now — under which capability,
> policy, resource, budget, locality, fairness, health, lifecycle, and authority
> constraints — and which assignment remains authoritative as agents restart, disappear,
> recover, become unavailable, or are superseded?

It is not an inference-request scheduler and it is not an agent execution runtime. It
schedules long-lived workers, not one-shot requests, and it owns scheduling authority
only.

```
persistent agent population
  → work admission
  → hard eligibility
  → deterministic ranking
  → authoritative assignment
  → dispatch lease
  → revalidation
  → execution handoff
  → completion/release observation
  → rescheduling or retirement
```

## What Agent Scheduler owns

- Work admission, queue accounting, and fairness accounting.
- Hard eligibility of persistent agents for a work item.
- Deterministic, explainable ranking among hard-eligible agents.
- Authoritative assignment state and dispatch leases, bound to explicit generations.
- Pre-dispatch revalidation and fencing of stale authority.
- Agent lifecycle state, drain, loss, and reincarnation semantics.
- Bounded, versioned, integrity-checked durable persistence and conservative recovery.
- Structured, deterministic explanations for every decision.
- A reference loopback transport, coordinator server, and agent client.

## What Agent Scheduler deliberately does not own

| Adjacent runtime | Boundary |
| --- | --- |
| Agent Runtime | Owns the execution lifecycle of long-running agents. Agent Scheduler only decides who owns work and whether that ownership is still valid. |
| Model Router | Owns model/provider/backend routing. Agent Scheduler consumes a resolved model requirement as a capability name such as `model:...`. |
| Ensemble Fabric | Owns parallel attempts, judges, arbitration, consensus. Agent Scheduler can schedule the participants, never the semantics. |
| Critic Fabric | Owns critique/verifier semantics. Agent Scheduler can schedule a critic-capable worker. |
| Experiment Fabric / Lab Scheduler | Own experiments, hypotheses, metrics, and experiment-specific placement. Agent Scheduler schedules workers generally. |
| Workload Fabric / Execution Fabric | Own durable logical workload and physical attempt authority. Agent Scheduler references those identities through `payload_ref` and never duplicates their lifecycle. |
| Resource Broker / Capacity / Reservation / Cost Governor / SLO Fabric | Own reservations, allocations, capacity modelling, economics, and service contracts. Agent Scheduler consumes verdicts through one narrow adapter. |
| Cluster Fabric / Fabric Scheduler | Own topology, placement, and failure domains. Agent Scheduler consumes a placement domain and topology epoch. |
| Runtime / Hardware Capability Registry | Own canonical capability discovery. Agent Scheduler stores generation-bound evidence with provenance. |
| Artifact Promotion / Autonomous Foundry | Own trust and promotion decisions. Agent Scheduler is a lower scheduling primitive they may consume. |

The core library never links another Summon Software Labs repository. Adjacent runtimes
are consumed only through the replaceable adapter in `agent_scheduler/adapters.hpp`.

## Architecture

```
AgentScheduler (public API, agent_scheduler/scheduler.hpp)
├── canonical state      agents, work, assignments, leases, fenced boots, queues, tenants
├── derived indexes      by capability, lifecycle, tenant, domain, queue, agent, work
├── pipeline             eligibility → ranking → assignment → lease → revalidation → handoff
├── invariants           callable from tests and inspection tooling
├── snapshots            immutable, generation-bound views
├── persistence          versioned, integrity-checked, atomic
└── net/                 framed TCP transport, coordinator server, agent client, control client
```

Canonical state is authoritative. Indexes are acceleration structures that are checked
against a canonical scan by `check_invariants()`. One mutex guards authoritative state;
it is never held across socket I/O, filesystem I/O, thread joins, external callbacks, or
the interlock used by the race suite.

## Agent model

An agent is a persistent autonomous worker capable of accepting work over time. Every
dimension is explicit and independently meaningful:

- identity: `AgentId`, `AgentGeneration`, `AgentBootId` (incarnation),
  `AgentRegistrationGeneration`
- lifecycle: `UNREGISTERED`, `REGISTERING`, `READY`, `BUSY`, `DRAINING`,
  `UNAVAILABLE`, `REVALIDATION_REQUIRED`, `LOST`, `RETIRED`
- health: `UNKNOWN`, `HEALTHY`, `DEGRADED`, `UNHEALTHY`
- readiness, reachability, availability: each `UNKNOWN`-capable
- load: canonical active assignments (derived from assignment state) plus advisory
  reported load with its own generation and freshness
- capability profile: generation-bound evidence with provenance
- locality: placement domain and topology epoch
- scope: tenant, namespace, policy labels
- cost/SLO/resource evidence references

`READY` must be earned. Registration alone produces `REGISTERING`; the incarnation
becomes `READY` only after current capability, health, and availability evidence has
been published for that same `AgentBootId`. A connected process is not proof of
readiness, and a healthy agent may still be incompatible, unauthorized, or at capacity.

## Capability model

Capabilities are free-form namespaced identifiers (`coding`, `tool:shell`,
`platform:windows`, `language:cpp`, `accelerator:cuda`, `model:...`), so
heterogeneous worker families are supported without a hard-coded taxonomy. Each piece of
evidence carries a state, a quality value, a generation, the originating `AgentBootId`,
an observation time, and provenance text.

States are `UNKNOWN`, `DECLARED`, `OBSERVED`, `VERIFIED`, `REVOKED`, and
`STALE`. A hard requirement defaults to a minimum state of `OBSERVED`: a bare claim is
not enough. `UNKNOWN`, `REVOKED`, and `STALE` never satisfy a hard requirement, and
evidence from a superseded incarnation is rejected rather than silently adopted.

## Work model

A work item is scheduling metadata only; the payload is opaque.

- identity and generation: `WorkId`, `WorkGeneration`
- queue and queue generation, priority, fairness class
- required and preferred capabilities with minimum state and minimum quality
- tenant/namespace scope and required policy labels
- affinity, anti-affinity, placement-domain restriction, topology epoch
- health/reachability/readiness/lease requirements
- optional external feasibility requirements (resource, budget, SLO, reservation)
- warm-state keys, deadline, earliest start
- `Exclusive` or explicitly `Parallel` mode with bounded `max_parallel` and
  `max_assignments_per_agent`

By default one logical work item has exactly one current authoritative assignment.
Parallel execution is a declared policy, never an accidental duplicate.

## Scheduling pipeline

```
admission → structural validation → authority validation → candidate discovery
→ hard eligibility → policy → capability → lifecycle/readiness → resource/budget/SLO
→ locality/affinity → deterministic factor evaluation → deterministic ranking
→ assignment creation → lease creation → pre-dispatch revalidation → handoff
→ observation → completion/release/cancellation/supersession
```

Filtering and scoring are separate. Every rejected candidate carries a specific
`ScheduleOutcome` and a safe detail string; every eligible candidate carries named
factors. `ScheduleDecision` exposes the candidate set, the ranking order, the selected
agent, the tie-break reason, the rejection histogram, and the bound generations.

Ranking uses named integer factors with policy weights:
`load_headroom`, `remaining_capacity`, `capability_quality`,
`capability_completeness`, `warm_state_affinity`, `locality_match`,
`health_quality`, `readiness`, `queue_age`, `fairness_deficit`,
`dispatch_latency`, `cost_evidence`, `slo_headroom`, `resource_headroom`,
`reliability_history`, `anti_concentration`, `failure_domain_diversity`,
`handoff_cost`.

An unknown factor contributes zero and is reported as `UNKNOWN`, so missing evidence can
never improve a candidate's rank. Ties are broken by evidence coverage and then by agent
identity, so identical authoritative state always produces an identical winner, identical
candidate ordering, and — because assignment identity is derived from its bound authority
— identical assignment identity.

## Hard eligibility before ranking

Hard predicates execute first and `UNKNOWN` never passes one. A favorable score cannot
rescue an invalid agent, and priority cannot bypass capability, lifecycle, authority,
policy, resource, budget, tenancy, health, or compatibility requirements. Hard rejections
include stale scheduler or coordinator epoch, stale agent boot or generation, stale work
generation, fenced or retired or draining or lost incarnations, missing or insufficient
capability evidence, stale capability generations, unhealthy or unreachable agents,
insufficient capacity, external feasibility refusal, tenant or policy violation,
placement-domain violation, affinity or anti-affinity violation, cancelled or superseded
work, and assignment conflicts.

## Fairness

Fairness is explicit rather than accidental queue order. Each admissible work item tracks
a bypass count and starvation rounds; the effective priority includes bounded aging
credit; and a per-fairness-class bypass ceiling guarantees that a continuously eligible
item runs within `ceiling + (admissible − 1)` scheduling rounds. Priority cannot cause
unbounded starvation unless a policy explicitly sets `allow_unbounded_starvation`.
Fairness state lives on the work item, so it survives agent restarts, reincarnations, and
scheduler recovery, and cancelled or superseded work leaves fairness accounting
immediately.

## Assignment authority, leases, and fencing

An assignment binds the complete authority that makes it dispatchable:
`AssignmentId`, `AssignmentGeneration`, work and work generation, agent, agent
generation and `AgentBootId`, scheduler and scheduler epoch, coordinator epoch, policy
generation, capability profile and generation, external feasibility generations, queue
generation, lease and dispatch identity with their generations, placement domain,
topology epoch, and replica index.

Before handoff the scheduler revalidates every one of those. A decision that once won
does not remain executable on stale evidence. Terminal assignment states can never be
revived; lease expiry removes dispatch authority without assuming process death;
acknowledgements and observations are accepted only from the current, unfenced
incarnation and only for the exact dispatch identity.

Outcomes include `ASSIGNED`, `NO_CHANGE`, `DEFERRED`, `NO_ELIGIBLE_AGENT`,
`REVALIDATION_REQUIRED`, `SHUTTING_DOWN`, and the specific rejections
`REJECT_STALE_SCHEDULER_EPOCH`, `REJECT_STALE_COORDINATOR_EPOCH`,
`REJECT_STALE_AGENT_BOOT`, `REJECT_STALE_AGENT_GENERATION`,
`REJECT_STALE_WORK_GENERATION`, `REJECT_STALE_ASSIGNMENT`, `REJECT_STALE_POLICY`,
`REJECT_STALE_CAPABILITY`, `REJECT_RESOURCE_FEASIBILITY`, `REJECT_BUDGET`,
`REJECT_SLO`, `REJECT_POLICY`, `REJECT_CANCELLED`, `REJECT_SUPERSEDED`,
`REJECT_CONFLICT`, `REJECT_CAPABILITY`, `REJECT_FENCED`, `REJECT_DRAIN`,
`REJECT_LEASE_EXPIRED`, `REJECT_LOCALITY`, `REJECT_AFFINITY`, `REJECT_TENANT`,
`REJECT_HEALTH`, `REJECT_REACHABILITY`, `REJECT_READINESS`, `REJECT_CAPACITY`,
`REJECT_EXPIRED`, `REJECT_DEADLINE`, `REJECT_LIMIT_EXCEEDED`,
`REJECT_INVALID_REQUEST`.

## Persistence and recovery

Durable state is written in a single versioned container:

- 8-byte magic `AGSDCHST`, explicit `format_version` (currently 1)
- 72-byte header carrying payload length, record count, payload CRC-32, header CRC-32,
  and a 256-bit SHA-256 semantic digest of the payload
- canonical little-endian payload with bounded, length-checked fields
- 16-byte trailer with magic and a trailer checksum
- atomic replacement through a temporary sibling, optional flush to stable storage, and
  optional re-read verification before the rename

Corruption, truncation, trailing garbage, unknown section tags, out-of-range enum values,
oversized collections, and unsupported format versions are rejected before any state is
mutated; the payload is decoded into candidate state first. Unsupported versions are
rejected before the payload is interpreted at all.

Recovery is deliberately conservative. After a scheduler restart the durable queues,
agent identities, work metadata, assignment history, policy, and fenced incarnations are
restored, the scheduler epoch advances, the process coordinator epoch advances on start,
and every dynamic observation — capability evidence, health, availability, load, leases —
is invalidated. Restored assignments enter `REVALIDATION_REQUIRED`: they keep their
exclusive slot (so recovery cannot create a duplicate owner) but carry no dispatch
authority. Surviving incarnations must re-register and republish current evidence;
assignments bound to a previous coordinator epoch are invalidated with
`COORDINATOR_EPOCH_CHANGED` rather than silently revived.

## Distributed reference architecture

```
Agent Scheduler Coordinator  (agent_scheduler_coordinator)
   ↕ framed TCP on 127.0.0.1
Agent Worker A / B / C       (agent_scheduler_agent)
```

The reference transport is Winsock TCP loopback with a strict framed protocol: magic,
protocol version, message type, correlation id, payload length, header CRC-32, payload
CRC-32, bounded payloads, and unknown-type rejection. Decoding is strict and consumes the
payload exactly. Each session has a bounded outbound queue and a dedicated writer thread,
so frames are never interleaved; session destruction is deferred to a reaper thread so a
session never joins itself. Incompatible protocol versions are rejected explicitly.

## REAL, SYNTHETIC, and UNSUPPORTED

**REAL** — independently executed and verified in this environment:

- independent operating-system processes for the coordinator and every agent
- real loopback TCP sockets between those processes
- real `TerminateProcess` kill of a running agent, followed by real session-loss
  detection through the control path
- real coordinator process termination and a genuinely fresh coordinator process
- real filesystem persistence with integrity checks and atomic replacement
- real installed-package consumption by an out-of-tree `find_package` consumer
- real local CUDA evidence on the installed RTX 5090 when the optional probe is enabled

**SYNTHETIC** — deterministic logical profiles, never physical infrastructure:

- large agent populations, many queues and tenants, capability churn, synthetic deaths
  and reincarnations, synthetic placement domains and failure domains
- heterogeneous model/tool fleets expressed as capability names
- scale and fairness workloads generated by `SyntheticLaboratory`

**UNSUPPORTED** — not claimed and not implemented:

- physical multi-node networking, remote agents, or multi-host placement
- multi-GPU execution, remote GPU execution, MIG, NVLink, NVSwitch, RDMA, GPUDirect,
  or DPU offload
- any model/provider/tool integration beyond capability-name consumption

## Build

Requirements: CMake 3.20 or newer, a C++20 compiler, and — for the reference transport —
Windows with Winsock.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

Options (all default to ON at top level except benchmarks, ASan, and CUDA):

| Option | Effect |
| --- | --- |
| `AGENT_SCHEDULER_BUILD_TESTS` | build the test suite |
| `AGENT_SCHEDULER_BUILD_TOOLS` | build inspection and control tools |
| `AGENT_SCHEDULER_BUILD_EXAMPLES` | build runnable examples |
| `AGENT_SCHEDULER_BUILD_BENCHMARKS` | build the benchmark suite |
| `AGENT_SCHEDULER_ENABLE_ASAN` | instrument first-party targets with AddressSanitizer |
| `AGENT_SCHEDULER_ENABLE_CUDA` | build the optional real CUDA evidence probe |

First-party targets compile with `/W4 /WX /permissive- /utf-8` on MSVC and
`-Wall -Wextra -Wpedantic -Werror` plus conversion, shadowing, and old-style-cast
warnings elsewhere. Warnings are fixed at the source; no warning is globally suppressed.

## Install

```powershell
cmake --install build --config Release --prefix C:/labs/agent_scheduler
```

The install tree contains the static library, all public headers, the tools (when built),
`LICENSE`, `NOTICE`, and the CMake package files.

## Using the installed package

```cmake
cmake_minimum_required(VERSION 3.20)
project(consumer LANGUAGES CXX)
find_package(agent_scheduler 1.0.0 REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE agent_scheduler::agent_scheduler)
```

```cpp
#include <cstdio>
#include "agent_scheduler/scheduler.hpp"

int main() {
  agent_scheduler::SchedulerOptions options;
  options.scheduler_id = agent_scheduler::SchedulerId{1};
  agent_scheduler::AgentScheduler scheduler(options);
  // register agents, publish evidence, submit work, schedule, dispatch, inspect
  std::printf("%s\n", scheduler.summary().state_digest.to_string().c_str());
  return scheduler.check_invariants().ok() ? 0 : 1;
}
```

The installed package is self-contained: the source tree is not needed after
installation, and no other Summon Software Labs repository is required to build or run
the core.

## Tools

| Tool | Purpose |
| --- | --- |
| `agent_scheduler_inspect` | inspect a durable state file: `--validate`, `--summary`, `--agents`, `--queues`, `--assignments`, `--leases`, `--fenced`, `--capabilities`, `--fairness`, `--invariants`, `--policy`, `--json`, plus `--synthetic --seed N` |
| `agent_scheduler_ctl` | drive a running coordinator: `--query <kind>`, `--submit-work`, `--cancel-work`, `--dispatch`, `--drain-agent`, `--deregister-agent` |
| `agent_scheduler_coordinator` | run the coordinator: `--port`, `--state`, `--run-ms`, `--stop-file`, `--no-load`, `--no-persist` |
| `agent_scheduler_agent` | run one persistent agent process: `--port`, `--agent-id`, `--generation`, `--boot`, `--concurrency`, `--capability`, `--work-ms` |

All tools are non-interactive: no prompts, no dialogs, no visible helper windows, and
non-zero exit codes preserved on failure.

## Examples

| Example | Demonstrates |
| --- | --- |
| `embedded_scheduler` | register agents, publish evidence, submit work, schedule, dispatch, inspect |
| `heterogeneous_capabilities` | heterogeneous profiles, hard eligibility, deterministic ranking |
| `fairness` | competing queues and classes under sustained contention, bounded starvation |
| `reincarnation` | agent death, stale-assignment rejection, reincarnation, fencing |
| `distributed_coordinator` | run a coordinator that independent agent processes attach to |

## Tests

```powershell
ctest --test-dir build -C Release --output-on-failure
```

| Test | Coverage |
| --- | --- |
| `test_core` | lifecycle, readiness, admission, eligibility, ranking determinism, explanations, drain, lease expiry, parallel fan-out |
| `test_capability` | capability states, generations, provenance, minimum quality, hostile profiles |
| `test_fairness` | priority preference, bounded starvation, bypass ceiling, fairness survival, cancellation |
| `test_assignment` | dispatch revalidation, dispatch identity, terminal states, revalidation, independent authority |
| `test_persistence` | round trip, every integrity field, atomic replacement, I/O failure, hostile paths, byte limits |
| `test_recovery` | restart authority, conservative recovery, durable semantics, epoch rejection, repeated recovery |
| `test_invariants` | invariant suite, index/canonical agreement under churn, monotonic generations, shutdown |
| `test_property` | randomized sequences, differential eligibility against an independent reference, save/load properties, insertion-order independence |
| `test_races` | twelve barrier-driven races with one deterministic legal outcome each |
| `test_protocol` | frame codec, malformed frames, split and pipelined frames, message round trips, strict decoding |
| `test_limits` | checked arithmetic, every configured bound, hostile numeric and text input, instance independence |
| `test_synthetic` | deterministic population and run reproducibility, 1000-agent scale, infeasible work |
| `test_shutdown` | idempotent shutdown, committed operations, notifier re-entry, concurrent query/mutation |
| `multiprocess` | the real multiprocess proof described above |

No test or validation command uses a timeout. A hanging test is treated as a defect and
diagnosed.

## Benchmarks

```powershell
cmake -S . -B build-bench -G "Visual Studio 17 2022" -A x64 -DAGENT_SCHEDULER_BUILD_BENCHMARKS=ON
cmake --build build-bench --config Release --parallel
build-bench/Release/agent_scheduler_bench.exe
```

The benchmark measures completed operations — agent registration, capability publication,
work admission, candidate discovery and hard-eligibility filtering, a complete scheduling
pass, assignment commit, assignment release, snapshot creation, invariant checking,
persistence save and load including real durability work, and explanation construction —
over a deterministic synthetic ladder with explicit seeds and correctness guards.
Populations are synthetic logical profiles in a single process; no physical scale or
multi-node capability is implied. `--quick` runs a small ladder, `--json` emits
deterministic JSON.

## Resource limits

Every externally influenced growth dimension is bounded in `ResourceLimits` and validated
before allocation:

| Limit | Default |
| --- | --- |
| `max_agents` | 200000 |
| `max_queues` | 4096 |
| `max_work_items` | 500000 |
| `max_active_assignments` | 200000 |
| `max_historical_assignments` | 500000 |
| `max_leases` | 200000 |
| `max_fenced_boots` | 500000 |
| `max_tenants` | 8192 |
| `max_capabilities_per_agent` | 256 |
| `max_policy_labels` | 64 |
| `max_capability_requirements` | 128 |
| `max_affinity_entries` | 256 |
| `max_warm_state_keys` | 64 |
| `max_explanation_factors` | 64 |
| `max_candidates_examined` | 200000 |
| `max_retained_history` | 100000 |
| `max_batch_assignments` | 100000 |
| `max_string_size` | 512 |
| `max_identifier_size` | 64 |
| `max_frame_size` / `max_payload_size` | 1 MiB |
| `max_connections` | 64 |
| `max_session_threads` | 160 |
| `max_send_queue` | 4096 |
| `max_persistence_bytes` | 1 GiB |

## Limitations

- The reference transport targets Winsock and therefore Windows; the core scheduler,
  persistence, and codec are platform-neutral, but the shipped network layer is not.
- The reference transport is a loopback control plane without TLS, mutual authentication,
  or encryption. Its trust boundary is the local machine and the processes on it.
- Correlation identifiers are chosen by the requesting peer and echoed verbatim. The
  coordinator keeps no per-correlation state, so duplicate or reordered correlation ids
  cannot corrupt authoritative state; duplicated mutations are resolved by the idempotency
  and generation rules of the scheduler itself.
- Persistence uses monotonic process-local timestamps; lease and freshness comparisons are
  meaningful within a process, and recovery invalidates them rather than reinterpreting
  them across restarts.
- The scheduler does not enforce execution cancellation it cannot observe. When execution
  has already crossed into Agent Runtime or Execution Fabric, the scheduler reports the
  scheduling state accurately instead of pretending the physical work stopped.
- Fairness is proven under the shipped weighted bounded-starvation contract; a policy that
  explicitly opts into `allow_unbounded_starvation` removes that guarantee by design.
- The optional CUDA probe proves a real local device only. It does not imply multi-GPU,
  remote-GPU, or multi-node capability.
- The benchmark harness measures a single process over synthetic populations.

## Project layout

```
CMakeLists.txt              build, install, export, package config
cmake/                      package config template
include/agent_scheduler/    public headers (core and net/)
src/                        implementation, internal headers under src/internal/
cuda/                       optional real CUDA evidence probe
tools/                      inspect, control, coordinator, and agent executables
examples/                   runnable examples
tests/                      unit, property, race, protocol, and multiprocess suites
benchmarks/                 benchmark suite
LICENSE, NOTICE             Apache License 2.0 and attribution
```

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
