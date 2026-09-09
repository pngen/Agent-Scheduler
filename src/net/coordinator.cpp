// Agent Scheduler — coordinator server: framed TCP front end for the scheduler.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/net/coordinator.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "agent_scheduler/json.hpp"
#include "agent_scheduler/version.hpp"

namespace agent_scheduler::net {
namespace {

[[nodiscard]] Frame make_frame(MessageType type, std::uint64_t correlation) {
  Frame frame;
  frame.header.version = static_cast<std::uint16_t>(protocol_version);
  frame.header.type = static_cast<std::uint16_t>(type);
  frame.header.correlation = correlation;
  return frame;
}

template <class Message>
[[nodiscard]] Frame encode_message(MessageType type,
                                   std::uint64_t correlation,
                                   const Message& message,
                                   const ResourceLimits& limits) {
  Frame frame = make_frame(type, correlation);
  ByteWriter writer;
  encode(writer, message, limits);
  frame.payload = writer.take();
  if (frame.payload.size() > limits.max_payload_size) {
    throw DecodeError("encoded message exceeds the configured maximum payload size");
  }
  return frame;
}

void write_error_json(JsonWriter& writer, const SchedulerError& error) {
  writer.begin_object();
  writer.key("code").value(to_string(error.code));
  writer.key("operation").value(error.operation);
  writer.key("subject").value(error.subject);
  writer.key("outcome").value(to_string(error.outcome));
  writer.key("current_generation").value(error.current_generation);
  writer.key("expected_generation").value(error.expected_generation);
  writer.key("detail").value(error.detail);
  writer.key("factors");
  writer.begin_array();
  for (const std::string& factor : error.factors) {
    writer.value(factor);
  }
  writer.end_array();
  writer.end_object();
}

}  // namespace

CoordinatorServer::CoordinatorServer(AgentScheduler& scheduler, CoordinatorOptions options)
    : scheduler_(scheduler), options_(std::move(options)) {
  options_.limits = scheduler_.limits();
}

CoordinatorServer::~CoordinatorServer() { (void)stop(); }

MutationResult CoordinatorServer::start() {
  if (running_.load(std::memory_order_acquire)) {
    return MutationResult::success();
  }
  if (const auto init = initialize_network(); !init.ok()) {
    return init;
  }
  if (options_.load_on_start && !options_.persistence_path.empty()) {
    std::error_code error;
    if (std::filesystem::exists(options_.persistence_path, error) && !error) {
      const RecoveryResult recovery = scheduler_.load(options_.persistence_path);
      if (!recovery.ok()) {
        return MutationResult::failure(*recovery.error);
      }
    }
  }
  if (const auto started = scheduler_.start(); !started.ok()) {
    return started;
  }
  if (const auto listening = listener_.listen_loopback(options_.port, options_.limits.max_accept_backlog);
      !listening.ok()) {
    return listening;
  }
  port_.store(listener_.bound_port().value_or(0), std::memory_order_relaxed);
  running_.store(true, std::memory_order_release);
  scheduler_.set_shutdown_notifier([this]() {
    std::vector<std::shared_ptr<Session>> sessions;
    {
      const std::lock_guard<std::mutex> guard(sessions_mutex_);
      for (auto& entry : sessions_) {
        sessions.push_back(entry.second);
      }
    }
    for (const auto& session : sessions) {
      if (options_.notify_sessions_on_stop) {
        Frame notice = make_frame(MessageType::ShutdownNotice, 0);
        (void)session->enqueue(std::move(notice));
      }
      session->request_close();
    }
  });
  accept_thread_ = std::thread([this]() { accept_loop(); });
  reap_thread_ = std::thread([this]() { reap_loop(); });
  return MutationResult::success();
}

MutationResult CoordinatorServer::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return MutationResult::success();
  }
  listener_.close();
  (void)scheduler_.shutdown();
  std::vector<std::shared_ptr<Session>> sessions;
  {
    const std::lock_guard<std::mutex> guard(sessions_mutex_);
    for (auto& entry : sessions_) {
      sessions.push_back(std::move(entry.second));
    }
    sessions_.clear();
    agent_sessions_.clear();
  }
  for (const auto& session : sessions) {
    session->request_close();
  }
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }
  for (const auto& session : sessions) {
    session->join();
  }
  {
    const std::lock_guard<std::mutex> guard(reap_mutex_);
    reap_stopping_ = true;
  }
  reap_cv_.notify_all();
  if (reap_thread_.joinable()) {
    reap_thread_.join();
  }
  if (options_.persist_on_stop && !options_.persistence_path.empty()) {
    const MutationResult saved = scheduler_.save(options_.persistence_path);
    if (!saved.ok()) {
      return saved;
    }
  }
  return MutationResult::success();
}

std::size_t CoordinatorServer::connection_count() const {
  const std::lock_guard<std::mutex> guard(sessions_mutex_);
  return sessions_.size();
}

void CoordinatorServer::accept_loop() {
  while (running_.load(std::memory_order_acquire)) {
    std::optional<Socket> accepted = listener_.accept_one();
    if (!accepted.has_value()) {
      if (!running_.load(std::memory_order_acquire)) {
        break;
      }
      continue;
    }
    {
      const std::lock_guard<std::mutex> guard(sessions_mutex_);
      if (sessions_.size() >= options_.limits.max_connections) {
        accepted->close();
        rejected_frames_.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
    }
    const std::uint64_t id = next_session_id_.fetch_add(1, std::memory_order_relaxed);
    SessionOptions session_options;
    session_options.max_frame_size = options_.limits.max_frame_size;
    session_options.max_payload_size = options_.limits.max_payload_size;
    session_options.max_send_queue = options_.limits.max_send_queue;
    session_options.protocol_version = static_cast<std::uint16_t>(protocol_version);
    auto session = std::make_shared<Session>(id, TcpStream{std::move(*accepted)}, session_options, *this);
    {
      const std::lock_guard<std::mutex> guard(sessions_mutex_);
      sessions_.emplace(id, session);
    }
    session->start();
  }
}

void CoordinatorServer::reap_loop() {
  std::deque<std::shared_ptr<Session>> pending;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(reap_mutex_);
      reap_cv_.wait(lock, [this]() { return reap_stopping_ || !reap_queue_.empty(); });
      if (reap_queue_.empty() && reap_stopping_) {
        return;
      }
      pending.swap(reap_queue_);
    }
    // Destruction happens outside the reaper lock and on a thread that is never the
    // session's own reader or writer thread.
    pending.clear();
    reap_cv_.notify_all();
  }
}

void CoordinatorServer::forget_agent_session(AgentId agent, AgentBootId boot) {
  const std::lock_guard<std::mutex> guard(sessions_mutex_);
  const auto found = agent_sessions_.find(agent);
  if (found != agent_sessions_.end() && found->second.first == boot) {
    agent_sessions_.erase(found);
  }
}

bool CoordinatorServer::push_assignment(AgentId agent,
                                        AgentBootId boot,
                                        const AssignWorkMessage& message) {
  std::shared_ptr<Session> session;
  {
    const std::lock_guard<std::mutex> guard(sessions_mutex_);
    const auto found = agent_sessions_.find(agent);
    if (found == agent_sessions_.end() || found->second.first != boot) {
      return false;
    }
    const auto session_entry = sessions_.find(found->second.second);
    if (session_entry == sessions_.end()) {
      return false;
    }
    session = session_entry->second;
  }
  try {
    return session->enqueue(encode_message(MessageType::AssignWork, 0, message, options_.limits));
  } catch (const std::exception&) {
    return false;
  }
}

void CoordinatorServer::on_closed(Session& session, std::string reason) {
  std::shared_ptr<Session> keep_alive;
  std::optional<std::pair<AgentId, AgentBootId>> bound;
  {
    const std::lock_guard<std::mutex> guard(sessions_mutex_);
    const auto found = sessions_.find(session.id());
    if (found != sessions_.end()) {
      keep_alive = found->second;
      sessions_.erase(found);
    }
    bound = session.bound_agent();
    if (bound.has_value()) {
      const auto agent_entry = agent_sessions_.find(bound->first);
      if (agent_entry != agent_sessions_.end() && agent_entry->second.first == bound->second &&
          agent_entry->second.second == session.id()) {
        agent_sessions_.erase(agent_entry);
      }
    }
  }
  if (keep_alive) {
    // Hand ownership to the reaper thread: this call runs on the session's own reader
    // thread, which must never be asked to join itself.
    std::unique_lock<std::mutex> lock(reap_mutex_);
    reap_cv_.wait(lock, [this]() {
      return reap_queue_.size() < options_.limits.max_connections || reap_stopping_;
    });
    reap_queue_.push_back(std::move(keep_alive));
    lock.unlock();
    reap_cv_.notify_all();
  }
  if (bound.has_value()) {
    (void)scheduler_.declare_agent_lost(bound->first, bound->second,
                                        reason.empty() ? std::string("session closed") : reason);
  }
}

void CoordinatorServer::send_result(std::shared_ptr<Session> session,
                                    MessageType type,
                                    std::uint64_t correlation,
                                    const MutationResult& result,
                                    ScheduleOutcome outcome) {
  ResultMessage message;
  message.request_type = type;
  message.ok = result.ok();
  message.outcome = outcome;
  if (result.error.has_value()) {
    message.error = *result.error;
  } else {
    message.error = SchedulerError{};
    message.error.outcome = outcome;
  }
  try {
    (void)session->enqueue(encode_message(type, correlation, message, options_.limits));
  } catch (const std::exception&) {
    rejected_frames_.fetch_add(1, std::memory_order_relaxed);
  }
}

void CoordinatorServer::send_error(std::shared_ptr<Session> session,
                                   std::uint64_t correlation,
                                   ErrorCode code,
                                   std::string operation,
                                   std::string subject,
                                   std::string detail) {
  ErrorMessage message;
  message.code = code;
  message.operation = std::move(operation);
  message.subject = std::move(subject);
  message.detail = std::move(detail);
  try {
    (void)session->enqueue(encode_message(MessageType::Error, correlation, message, options_.limits));
  } catch (const std::exception&) {
    rejected_frames_.fetch_add(1, std::memory_order_relaxed);
  }
}

std::string CoordinatorServer::query_json(const QueryMessage& query) const {
  JsonWriter writer;
  switch (query.kind) {
    case QueryKind::Version: {
      writer.begin_object();
      writer.key("product").value(std::string(product_name));
      writer.key("version").value(std::string(version_string));
      writer.key("build").value(build_info());
      writer.key("protocol_version").value(static_cast<std::uint64_t>(protocol_version));
      writer.key("persistence_format_version")
          .value(static_cast<std::uint64_t>(persistence_format_version));
      writer.end_object();
      break;
    }
    case QueryKind::Summary: {
      const SchedulerSummary summary = scheduler_.summary();
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
      writer.key("agents_ready").value(static_cast<std::uint64_t>(summary.agents_ready));
      writer.key("agents_busy").value(static_cast<std::uint64_t>(summary.agents_busy));
      writer.key("agents_draining").value(static_cast<std::uint64_t>(summary.agents_draining));
      writer.key("agents_lost").value(static_cast<std::uint64_t>(summary.agents_lost));
      writer.key("agents_retired").value(static_cast<std::uint64_t>(summary.agents_retired));
      writer.key("queues").value(static_cast<std::uint64_t>(summary.queue_count));
      writer.key("work_admitted").value(static_cast<std::uint64_t>(summary.work_admitted));
      writer.key("work_assigned").value(static_cast<std::uint64_t>(summary.work_assigned));
      writer.key("work_executing").value(static_cast<std::uint64_t>(summary.work_executing));
      writer.key("work_completed").value(static_cast<std::uint64_t>(summary.work_completed));
      writer.key("work_cancelled").value(static_cast<std::uint64_t>(summary.work_cancelled));
      writer.key("active_assignments").value(static_cast<std::uint64_t>(summary.active_assignments));
      writer.key("dispatchable_assignments")
          .value(static_cast<std::uint64_t>(summary.dispatchable_assignments));
      writer.key("fenced_boots").value(static_cast<std::uint64_t>(summary.fenced_boots));
      writer.key("scheduling_rounds").value(summary.scheduling_rounds);
      writer.key("state_revision").value(summary.state_revision);
      writer.key("state_digest").value(summary.state_digest.to_string());
      writer.end_object();
      break;
    }
    case QueryKind::Agents: {
      writer.begin_array();
      for (const AgentStatus& agent : scheduler_.all_agents()) {
        if (!query.agent.is_zero() && agent.id != query.agent) {
          continue;
        }
        writer.begin_object();
        writer.key("id").value(agent.id.to_string());
        writer.key("generation").value(agent.generation.value());
        writer.key("boot").value(agent.boot.to_string());
        writer.key("registration_generation").value(agent.registration_generation.value());
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
        writer.key("capabilities");
        writer.begin_array();
        for (const CapabilityEvidence& evidence : agent.capabilities) {
          writer.begin_object();
          writer.key("name").value(evidence.name);
          writer.key("state").value(to_string(evidence.state));
          writer.key("quality").value(static_cast<std::uint64_t>(evidence.quality));
          writer.key("generation").value(evidence.generation.value());
          writer.end_object();
        }
        writer.end_array();
        writer.end_object();
      }
      writer.end_array();
      break;
    }
    case QueryKind::Work: {
      writer.begin_array();
      for (const WorkStatus& work : scheduler_.all_work()) {
        if (!query.work.is_zero() && work.id != query.work) {
          continue;
        }
        writer.begin_object();
        writer.key("id").value(work.id.to_string());
        writer.key("generation").value(work.generation.value());
        writer.key("queue").value(work.queue.to_string());
        writer.key("lifecycle").value(to_string(work.lifecycle));
        writer.key("priority").value(static_cast<std::uint64_t>(work.priority));
        writer.key("fairness_class").value(work.fairness_class);
        writer.key("current_assignments").value(static_cast<std::uint64_t>(work.current_assignments));
        writer.key("max_parallel").value(static_cast<std::uint64_t>(work.max_parallel));
        writer.key("bypass_count").value(static_cast<std::uint64_t>(work.bypass_count));
        writer.key("starvation_rounds").value(static_cast<std::uint64_t>(work.starvation_rounds));
        writer.key("last_assignment_generation").value(work.last_assignment_generation.value());
        writer.key("reason").value(work.reason);
        writer.end_object();
      }
      writer.end_array();
      break;
    }
    case QueryKind::Assignments: {
      writer.begin_array();
      for (const Assignment& assignment : scheduler_.all_assignments()) {
        const AssignmentBinding& binding = assignment.binding;
        if (!query.work.is_zero() && binding.work != query.work) {
          continue;
        }
        if (!query.assignment.is_zero() && binding.id != query.assignment) {
          continue;
        }
        if (!query.agent.is_zero() && binding.agent != query.agent) {
          continue;
        }
        writer.begin_object();
        writer.key("id").value(binding.id.to_string());
        writer.key("generation").value(binding.generation.value());
        writer.key("work").value(binding.work.to_string());
        writer.key("work_generation").value(binding.work_generation.value());
        writer.key("agent").value(binding.agent.to_string());
        writer.key("agent_generation").value(binding.agent_generation.value());
        writer.key("boot").value(binding.boot.to_string());
        writer.key("scheduler_epoch").value(binding.scheduler_epoch.value());
        writer.key("coordinator_epoch").value(binding.coordinator_epoch.value());
        writer.key("policy_generation").value(binding.policy_generation.value());
        writer.key("capability_generation").value(binding.capability_generation.value());
        writer.key("lease").value(binding.lease.to_string());
        writer.key("dispatch").value(binding.dispatch.to_string());
        writer.key("state").value(to_string(assignment.state));
        writer.key("invalidation").value(to_string(assignment.invalidation));
        writer.key("lease_current").value(assignment.lease_current);
        writer.key("replica_index").value(static_cast<std::uint64_t>(binding.replica_index));
        writer.key("exclusive").value(binding.exclusive);
        writer.key("reason").value(assignment.reason);
        writer.end_object();
      }
      writer.end_array();
      break;
    }
    case QueryKind::Leases: {
      writer.begin_array();
      for (const Lease& lease : scheduler_.leases()) {
        if (!query.assignment.is_zero() && lease.assignment != query.assignment) {
          continue;
        }
        if (!query.agent.is_zero() && lease.agent != query.agent) {
          continue;
        }
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
      break;
    }
    case QueryKind::Fenced: {
      writer.begin_array();
      for (const FencedBoot& fenced : scheduler_.fenced_boots()) {
        writer.begin_object();
        writer.key("agent").value(fenced.agent.to_string());
        writer.key("boot").value(fenced.boot.to_string());
        writer.key("generation").value(fenced.generation.value());
        writer.key("reason").value(fenced.reason);
        writer.end_object();
      }
      writer.end_array();
      break;
    }
    case QueryKind::Invariants: {
      const InvariantReport report = scheduler_.check_invariants();
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
      break;
    }
    case QueryKind::Fairness: {
      const FairnessSnapshot fairness = scheduler_.fairness();
      writer.begin_object();
      writer.key("scheduling_rounds").value(fairness.scheduling_rounds);
      writer.key("bounded_starvation").value(fairness.bounded_starvation);
      writer.key("max_priority_bypass").value(static_cast<std::uint64_t>(fairness.max_priority_bypass));
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
      break;
    }
    case QueryKind::Capabilities: {
      writer.begin_array();
      for (const AgentStatus& agent : scheduler_.all_agents()) {
        if (!query.agent.is_zero() && agent.id != query.agent) {
          continue;
        }
        writer.begin_object();
        writer.key("agent").value(agent.id.to_string());
        writer.key("boot").value(agent.boot.to_string());
        writer.key("capability_generation").value(agent.capability_generation.value());
        writer.key("current").value(agent.evidence_current);
        writer.key("capabilities");
        writer.begin_array();
        for (const CapabilityEvidence& evidence : agent.capabilities) {
          writer.begin_object();
          writer.key("name").value(evidence.name);
          writer.key("state").value(to_string(evidence.state));
          writer.key("quality").value(static_cast<std::uint64_t>(evidence.quality));
          writer.end_object();
        }
        writer.end_array();
        writer.end_object();
      }
      writer.end_array();
      break;
    }
    case QueryKind::Queues: {
      writer.begin_array();
      for (const WorkStatus& work : scheduler_.all_work()) {
        writer.begin_object();
        writer.key("queue").value(work.queue.to_string());
        writer.key("work").value(work.id.to_string());
        writer.key("lifecycle").value(to_string(work.lifecycle));
        writer.end_object();
      }
      writer.end_array();
      break;
    }
    case QueryKind::Policy: {
      const PolicySnapshot policy = scheduler_.policy();
      writer.begin_object();
      writer.key("id").value(policy.id.to_string());
      writer.key("generation").value(policy.generation.value());
      writer.key("aging_rounds_per_step").value(static_cast<std::uint64_t>(policy.aging_rounds_per_step));
      writer.key("max_priority_bypass").value(static_cast<std::uint64_t>(policy.max_priority_bypass));
      writer.key("starvation_alert_rounds")
          .value(static_cast<std::uint64_t>(policy.starvation_alert_rounds));
      writer.key("allow_unbounded_starvation").value(policy.allow_unbounded_starvation);
      writer.key("assignment_lease_ttl_ms").value(policy.assignment_lease_ttl_ms);
      writer.key("registration_lease_ttl_ms").value(policy.registration_lease_ttl_ms);
      writer.key("ranking_weights");
      writer.begin_array();
      for (const RankingWeight& weight : policy.ranking_weights) {
        writer.begin_object();
        writer.key("factor").value(weight.factor);
        writer.key("weight").value(static_cast<std::uint64_t>(weight.weight));
        writer.end_object();
      }
      writer.end_array();
      writer.key("fairness_class_weights");
      writer.begin_array();
      for (const QueueClassWeight& weight : policy.fairness_class_weights) {
        writer.begin_object();
        writer.key("class").value(weight.fairness_class);
        writer.key("weight").value(static_cast<std::uint64_t>(weight.weight));
        writer.end_object();
      }
      writer.end_array();
      writer.end_object();
      break;
    }
  }
  return writer.str();
}

void CoordinatorServer::on_frame(Session& session, const Frame& frame) {
  std::shared_ptr<Session> shared;
  {
    const std::lock_guard<std::mutex> guard(sessions_mutex_);
    const auto found = sessions_.find(session.id());
    if (found == sessions_.end()) {
      return;
    }
    shared = found->second;
  }
  handle_frame(std::move(shared), frame);
}

void CoordinatorServer::handle_frame(std::shared_ptr<Session> session, const Frame& frame) {
  frames_processed_.fetch_add(1, std::memory_order_relaxed);
  const auto type = static_cast<MessageType>(frame.header.type);
  const std::uint64_t correlation = frame.header.correlation;
  ByteReader reader(std::span<const std::uint8_t>(frame.payload.data(), frame.payload.size()));
  try {
    switch (type) {
      case MessageType::Hello: {
        HelloMessage message;
        decode(reader, message, options_.limits);
        reader.require_exhausted();
        ResultMessage result;
        result.request_type = type;
        result.ok = message.protocol_version == protocol_version;
        result.outcome = result.ok ? ScheduleOutcome::NoChange : ScheduleOutcome::RejectInvalidRequest;
        if (!result.ok) {
          result.error = make_error(ErrorCode::ProtocolError, "hello", "",
                                    "unsupported protocol version", ScheduleOutcome::RejectInvalidRequest);
        }
        (void)session->enqueue(encode_message(MessageType::HelloResult, correlation, result, options_.limits));
        if (!result.ok) {
          protocol_violations_.fetch_add(1, std::memory_order_relaxed);
          session->request_close();
        }
        return;
      }
      case MessageType::RegisterAgent: {
        RegisterAgentMessage message;
        decode(reader, message, options_.limits);
        reader.require_exhausted();
        const MutationResult result = scheduler_.register_agent(message.descriptor);
        if (result.ok()) {
          session->bind_agent(message.descriptor.id, message.descriptor.boot);
          {
            const std::lock_guard<std::mutex> guard(sessions_mutex_);
            agent_sessions_[message.descriptor.id] = {message.descriptor.boot, session->id()};
          }
          // An incarnation that (re-)registers re-opens the question of every pending
          // assignment bound to it, including assignments restored by recovery.
          (void)scheduler_.revalidate_assignments();
          run_scheduling_pass();
        }
        send_result(session, MessageType::RegisterResult, correlation, result,
                    result.ok() ? ScheduleOutcome::NoChange : ScheduleOutcome::RejectInvalidRequest);
        return;
      }
      case MessageType::PublishCapabilities: {
        PublishCapabilitiesMessage message;
        decode(reader, message, options_.limits);
        reader.require_exhausted();
        const MutationResult result = scheduler_.publish_capabilities(message.agent, message.boot, message.profile);
        send_result(session, MessageType::PublishCapabilitiesResult, correlation, result,
                    result.ok() ? ScheduleOutcome::NoChange : ScheduleOutcome::RejectInvalidRequest);
        if (result.ok()) {
          run_scheduling_pass();
        }
        return;
      }
      case MessageType::PublishHealth: {
        PublishHealthMessage message;
        decode(reader, message, options_.limits);
        reader.require_exhausted();
        const MutationResult result = scheduler_.publish_health(message.agent, message.boot, message.observation);
        send_result(session, MessageType::PublishHealthResult, correlation, result,
                    result.ok() ? ScheduleOutcome::NoChange : ScheduleOutcome::RejectInvalidRequest);
        if (result.ok()) {
          run_scheduling_pass();
        }
        return;
      }
      case MessageType::PublishAvailability: {
        PublishAvailabilityMessage message;
        decode(reader, message, options_.limits);
        reader.require_exhausted();
        const MutationResult result =
            scheduler_.publish_availability(message.agent, message.boot, message.observation);
        send_result(session, MessageType::PublishAvailabilityResult, correlation, result,
                    result.ok() ? ScheduleOutcome::NoChange : ScheduleOutcome::RejectInvalidRequest);
        if (result.ok()) {
          run_scheduling_pass();
        }
        return;
      }
      case MessageType::PublishLoad: {
        PublishLoadMessage message;
        decode(reader, message, options_.limits);
        reader.require_exhausted();
        const MutationResult result = scheduler_.publish_load(message.agent, message.boot, message.observation);
        send_result(session, MessageType::PublishLoadResult, correlation, result,
                    result.ok() ? ScheduleOutcome::NoChange : ScheduleOutcome::RejectInvalidRequest);
        if (result.ok()) {
          run_scheduling_pass();
        }
        return;
      }
      case MessageType::Heartbeat: {
        HeartbeatMessage message;
        decode(reader, message, options_.limits);
        reader.require_exhausted();
        const MutationResult result =
            scheduler_.heartbeat(message.agent, message.boot, message.registration_generation);
        send_result(session, MessageType::HeartbeatResult, correlation, result,
                    result.ok() ? ScheduleOutcome::NoChange : ScheduleOutcome::RejectInvalidRequest);
        return;
      }
      case MessageType::SubmitWork: {
        SubmitWorkMessage message;
        decode(reader, message, options_.limits);
        reader.require_exhausted();
        const MutationResult result = scheduler_.submit_work(message.request);
        send_result(session, MessageType::SubmitWorkResult, correlation, result,
                    result.ok() ? ScheduleOutcome::NoChange : ScheduleOutcome::RejectInvalidRequest);
        if (result.ok()) {
          run_scheduling_pass();
        }
        return;
      }
      case MessageType::CancelWork: {
        CancelWorkMessage message;
        decode(reader, message, options_.limits);
        reader.require_exhausted();
        const MutationResult result = scheduler_.cancel_work(message.work, message.generation, message.reason);
        send_result(session, MessageType::CancelWorkResult, correlation, result,
                    result.ok() ? ScheduleOutcome::RejectCancelled : ScheduleOutcome::RejectInvalidRequest);
        return;
      }
      case MessageType::SupersedeWork: {
        SupersedeWorkMessage message;
        decode(reader, message, options_.limits);
        reader.require_exhausted();
        const MutationResult result =
            scheduler_.supersede_work(message.work, message.expected_generation, message.replacement);
        send_result(session, MessageType::SupersedeWorkResult, correlation, result,
                    result.ok() ? ScheduleOutcome::NoChange : ScheduleOutcome::RejectInvalidRequest);
        if (result.ok()) {
          run_scheduling_pass();
        }
        return;
      }
      case MessageType::AssignmentAck: {
        AssignmentAckMessage message;
        decode(reader, message, options_.limits);
        reader.require_exhausted();
        MutationResult result;
        if (message.accepted) {
          result = scheduler_.acknowledge(message.binding.id, message.binding.generation,
                                          message.binding.dispatch, message.binding.dispatch_generation,
                                          message.binding.boot);
        } else {
          result = scheduler_.reject_assignment(message.binding.id, message.binding.generation,
                                                message.binding.boot, message.reason);
        }
        send_result(session, MessageType::AssignmentAck, correlation, result,
                    result.ok() ? ScheduleOutcome::NoChange : ScheduleOutcome::RejectInvalidRequest);
        if (result.ok()) {
          run_scheduling_pass();
        }
        return;
      }
      case MessageType::AssignmentReject: {
        AssignmentObservationMessage message;
        decode(reader, message, options_.limits);
        reader.require_exhausted();
        const MutationResult result = scheduler_.reject_assignment(
            message.binding.id, message.binding.generation, message.binding.boot, message.reason);
        send_result(session, MessageType::AssignmentReject, correlation, result,
                    result.ok() ? ScheduleOutcome::NoChange : ScheduleOutcome::RejectInvalidRequest);
        if (result.ok()) {
          run_scheduling_pass();
        }
        return;
      }
      case MessageType::WorkStarted: {
        AssignmentObservationMessage message;
        decode(reader, message, options_.limits);
        reader.require_exhausted();
        const MutationResult result = scheduler_.mark_executing(
            message.binding.id, message.binding.generation, message.binding.boot);
        send_result(session, MessageType::WorkStarted, correlation, result,
                    result.ok() ? ScheduleOutcome::NoChange : ScheduleOutcome::RejectInvalidRequest);
        return;
      }
      case MessageType::WorkReleased:
      case MessageType::WorkCompleted:
      case MessageType::WorkFailed: {
        AssignmentObservationMessage message;
        decode(reader, message, options_.limits);
        reader.require_exhausted();
        MutationResult result;
        if (type == MessageType::WorkReleased) {
          result = scheduler_.release(message.binding.id, message.binding.generation, message.binding.boot,
                                      message.reason);
        } else if (type == MessageType::WorkCompleted) {
          result = scheduler_.complete_assignment(message.binding.id, message.binding.generation,
                                                  message.binding.boot);
        } else {
          result = scheduler_.fail_assignment(message.binding.id, message.binding.generation,
                                              message.binding.boot, message.reason);
        }
        send_result(session, type, correlation, result,
                    result.ok() ? ScheduleOutcome::NoChange : ScheduleOutcome::RejectInvalidRequest);
        if (result.ok()) {
          run_scheduling_pass();
        }
        return;
      }
      case MessageType::DrainAgent: {
        DrainAgentMessage message;
        decode(reader, message, options_.limits);
        reader.require_exhausted();
        const MutationResult result = scheduler_.drain_agent(message.agent, message.boot, message.force);
        send_result(session, MessageType::DrainAgentResult, correlation, result,
                    result.ok() ? ScheduleOutcome::RejectDrain : ScheduleOutcome::RejectInvalidRequest);
        return;
      }
      case MessageType::DeregisterAgent: {
        DeregisterAgentMessage message;
        decode(reader, message, options_.limits);
        reader.require_exhausted();
        const MutationResult result = scheduler_.deregister_agent(message.agent, message.boot, message.force);
        if (result.ok()) {
          forget_agent_session(message.agent, message.boot);
        }
        send_result(session, MessageType::DeregisterAgentResult, correlation, result,
                    result.ok() ? ScheduleOutcome::NoChange : ScheduleOutcome::RejectInvalidRequest);
        return;
      }
      case MessageType::DispatchAssignment: {
        DispatchAssignmentMessage message;
        decode(reader, message, options_.limits);
        reader.require_exhausted();
        const DispatchResult dispatch_result = scheduler_.dispatch(message.assignment, message.generation);
        ResultMessage result;
        result.request_type = type;
        result.ok = dispatch_result.dispatched();
        result.outcome = dispatch_result.outcome;
        if (!dispatch_result.dispatched()) {
          result.error = make_error(ErrorCode::StaleGeneration, "dispatch_assignment",
                                    message.assignment.to_string(), dispatch_result.detail,
                                    dispatch_result.outcome);
        }
        (void)session->enqueue(
            encode_message(MessageType::DispatchAssignmentResult, correlation, result, options_.limits));
        return;
      }
      case MessageType::Query: {
        QueryMessage message;
        decode(reader, message, options_.limits);
        reader.require_exhausted();
        QueryResultMessage result;
        result.kind = message.kind;
        result.json = query_json(message);
        (void)session->enqueue(encode_message(MessageType::QueryResult, correlation, result, options_.limits));
        return;
      }
      default:
        protocol_violations_.fetch_add(1, std::memory_order_relaxed);
        send_error(session, correlation, ErrorCode::ProtocolError, "handle_frame",
                   std::to_string(frame.header.type), "message type is not accepted from this peer");
        return;
    }
  } catch (const DecodeError& error) {
    protocol_violations_.fetch_add(1, std::memory_order_relaxed);
    send_error(session, correlation, ErrorCode::DecodeError, "handle_frame",
               to_string(type), error.what());
  } catch (const std::exception& error) {
    protocol_violations_.fetch_add(1, std::memory_order_relaxed);
    send_error(session, correlation, ErrorCode::Internal, "handle_frame", to_string(type), error.what());
  }
}

void CoordinatorServer::run_scheduling_pass() {
  if (!options_.auto_dispatch) {
    return;
  }
  ScheduleRequest request;
  request.max_assignments = options_.limits.max_batch_assignments;
  const BatchDecision batch = scheduler_.schedule_batch(request);
  for (const ScheduleDecision& decision : batch.decisions) {
    if (!decision.assignment.has_value()) {
      continue;
    }
    const AssignmentBinding& binding = decision.assignment.value();
    const DispatchResult dispatch = scheduler_.dispatch(binding.id, binding.generation);
    if (!dispatch.dispatched()) {
      continue;
    }
    AssignWorkMessage message;
    message.binding = dispatch.handoff.value_or(binding);
    const auto work = scheduler_.work_status(binding.work);
    if (work.has_value()) {
      message.kind = work->kind;
    }
    message.payload_ref = std::string{};
    (void)push_assignment(binding.agent, binding.boot, message);
  }
}

}  // namespace agent_scheduler::net
