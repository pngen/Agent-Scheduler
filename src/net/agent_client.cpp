// Agent Scheduler — reference agent process client.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "agent_scheduler/net/agent_client.hpp"

#include <chrono>
#include <condition_variable>
#include <string>
#include <thread>
#include <utility>

#include "agent_scheduler/clock.hpp"
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
[[nodiscard]] std::vector<std::uint8_t> encode_payload(const Message& message,
                                                       const ResourceLimits& limits) {
  ByteWriter writer;
  encode(writer, message, limits);
  return writer.take();
}

}  // namespace

AgentClient::AgentClient(AgentClientOptions options) : options_(std::move(options)) {}

AgentClient::~AgentClient() { stop(); }

MutationResult AgentClient::connect_and_register() {
  if (session_ != nullptr) {
    return MutationResult::success();
  }
  if (const auto init = initialize_network(); !init.ok()) {
    return init;
  }
  TcpStream stream;
  if (const auto connected = connect_loopback(options_.port, stream); !connected.ok()) {
    return connected;
  }
  SessionOptions session_options;
  session_options.max_frame_size = options_.limits.max_frame_size;
  session_options.max_payload_size = options_.limits.max_payload_size;
  session_options.max_send_queue = options_.limits.max_send_queue;
  session_options.protocol_version = static_cast<std::uint16_t>(protocol_version);
  session_ = std::make_unique<Session>(1, std::move(stream), session_options, *this);
  session_->start();

  HelloMessage hello;
  hello.protocol_version = protocol_version;
  hello.role = "agent";
  hello.build = build_info();
  hello.nonce = process_nonce();
  if (const auto result = send_and_wait(MessageType::Hello, encode_payload(hello, options_.limits),
                                        MessageType::HelloResult);
      !result.ok()) {
    return result;
  }

  RegisterAgentMessage registration;
  registration.descriptor = options_.descriptor;
  const MutationResult registered =
      send_and_wait(MessageType::RegisterAgent, encode_payload(registration, options_.limits),
                    MessageType::RegisterResult);
  if (!registered.ok()) {
    return registered;
  }
  return publish_capabilities();
}

MutationResult AgentClient::publish_capabilities() {
  PublishCapabilitiesMessage message;
  message.agent = options_.descriptor.id;
  message.boot = options_.descriptor.boot;
  message.profile = options_.profile;
  return send_and_wait(MessageType::PublishCapabilities, encode_payload(message, options_.limits),
                       MessageType::PublishCapabilitiesResult);
}

MutationResult AgentClient::publish_health() {
  PublishHealthMessage message;
  message.agent = options_.descriptor.id;
  message.boot = options_.descriptor.boot;
  message.observation = options_.health;
  return send_and_wait(MessageType::PublishHealth, encode_payload(message, options_.limits),
                       MessageType::PublishHealthResult);
}

MutationResult AgentClient::publish_availability() {
  PublishAvailabilityMessage message;
  message.agent = options_.descriptor.id;
  message.boot = options_.descriptor.boot;
  message.observation = options_.availability;
  return send_and_wait(MessageType::PublishAvailability, encode_payload(message, options_.limits),
                       MessageType::PublishAvailabilityResult);
}

MutationResult AgentClient::publish_load() {
  PublishLoadMessage message;
  message.agent = options_.descriptor.id;
  message.boot = options_.descriptor.boot;
  message.observation = options_.load;
  return send_and_wait(MessageType::PublishLoad, encode_payload(message, options_.limits),
                       MessageType::PublishLoadResult);
}

MutationResult AgentClient::heartbeat() {
  HeartbeatMessage message;
  message.agent = options_.descriptor.id;
  message.boot = options_.descriptor.boot;
  message.registration_generation = options_.descriptor.registration_generation;
  return send_and_wait(MessageType::Heartbeat, encode_payload(message, options_.limits),
                       MessageType::HeartbeatResult);
}

MutationResult AgentClient::send_and_wait(MessageType type,
                                          std::vector<std::uint8_t> payload,
                                          MessageType expected_reply) {
  if (session_ == nullptr) {
    return MutationResult::failure(make_error(ErrorCode::NotStarted, "send", to_string(type),
                                              "session is not connected", ScheduleOutcome::NoChange));
  }
  const std::uint64_t correlation = next_correlation_.fetch_add(1, std::memory_order_relaxed);
  Frame frame = make_frame(type, correlation);
  frame.payload = std::move(payload);
  if (!session_->enqueue(std::move(frame))) {
    return MutationResult::failure(make_error(ErrorCode::Internal, "send", to_string(type),
                                              "send queue is saturated", ScheduleOutcome::NoChange));
  }
  std::unique_lock<std::mutex> lock(reply_mutex_);
  reply_cv_.wait(lock, [this, correlation]() {
    return (last_correlation_ == correlation && last_result_.has_value()) || stopped_.load() ||
           (session_ != nullptr && session_->closed());
  });
  if (last_correlation_ == correlation && last_result_.has_value()) {
    const ResultMessage reply = *last_result_;
    last_result_.reset();
    if (reply.ok) {
      return MutationResult::success();
    }
    return MutationResult::failure(reply.error);
  }
  (void)expected_reply;
  return MutationResult::failure(make_error(ErrorCode::Internal, "send", to_string(type),
                                            "session closed before a reply arrived",
                                            ScheduleOutcome::NoChange));
}

void AgentClient::report_observation(MessageType type, const AssignmentBinding& binding, std::string reason) {
  AssignmentObservationMessage message;
  message.binding = binding;
  message.reason = std::move(reason);
  Frame frame = make_frame(type, 0);
  ByteWriter writer;
  encode(writer, message, options_.limits);
  frame.payload = writer.take();
  (void)session_->enqueue(std::move(frame));
}

void AgentClient::on_frame(Session& session, const Frame& frame) {
  (void)session;
  frames_handled_.fetch_add(1, std::memory_order_relaxed);
  handle_frame(frame);
}

void AgentClient::handle_frame(const Frame& frame) {
  const auto type = static_cast<MessageType>(frame.header.type);
  ByteReader reader(std::span<const std::uint8_t>(frame.payload.data(), frame.payload.size()));
  try {
    switch (type) {
      case MessageType::HelloResult:
      case MessageType::RegisterResult:
      case MessageType::PublishCapabilitiesResult:
      case MessageType::PublishHealthResult:
      case MessageType::PublishAvailabilityResult:
      case MessageType::PublishLoadResult:
      case MessageType::HeartbeatResult:
      case MessageType::SubmitWorkResult:
      case MessageType::CancelWorkResult:
      case MessageType::SupersedeWorkResult:
      case MessageType::DrainAgentResult:
      case MessageType::DeregisterAgentResult:
      case MessageType::DispatchAssignmentResult: {
        ResultMessage message;
        decode(reader, message, options_.limits);
        reader.require_exhausted();
        const std::lock_guard<std::mutex> guard(reply_mutex_);
        last_result_ = message;
        last_correlation_ = frame.header.correlation;
        reply_cv_.notify_all();
        return;
      }
      case MessageType::AssignWork: {
        AssignWorkMessage message;
        decode(reader, message, options_.limits);
        reader.require_exhausted();
        {
          const std::lock_guard<std::mutex> guard(mutex_);
          assignments_.push_back(message);
        }
        if (on_assignment) {
          on_assignment(message);
        }
        if (options_.auto_ack) {
          AssignmentAckMessage ack;
          ack.binding = message.binding;
          ack.accepted = true;
          ack.reason = "accepted";
          Frame reply = make_frame(MessageType::AssignmentAck, 0);
          ByteWriter writer;
          encode(writer, ack, options_.limits);
          reply.payload = writer.take();
          (void)session_->enqueue(std::move(reply));
        }
        if (options_.auto_complete) {
          if (options_.simulated_work_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options_.simulated_work_ms));
          }
          report_observation(MessageType::WorkStarted, message.binding, "started");
          report_observation(MessageType::WorkCompleted, message.binding, "completed");
        }
        return;
      }
      case MessageType::ShutdownNotice: {
        if (on_shutdown) {
          on_shutdown();
        }
        return;
      }
      case MessageType::Error: {
        ErrorMessage message;
        decode(reader, message, options_.limits);
        reader.require_exhausted();
        return;
      }
      default:
        return;
    }
  } catch (const std::exception&) {
    return;
  }
}

void AgentClient::on_closed(Session& session, std::string reason) {
  (void)session;
  (void)reason;
  {
    const std::lock_guard<std::mutex> guard(reply_mutex_);
    reply_cv_.notify_all();
  }
  if (on_drain) {
    on_drain();
  }
}

std::size_t AgentClient::pump() {
  if (session_ == nullptr) {
    return 0;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return static_cast<std::size_t>(frames_handled_.load(std::memory_order_relaxed));
}

std::vector<AssignWorkMessage> AgentClient::received_assignments() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return assignments_;
}

MutationResult AgentClient::run(std::atomic<bool>& stop_flag) {
  if (const auto connected = connect_and_register(); !connected.ok()) {
    return connected;
  }
  if (const auto published = publish_health(); !published.ok()) {
    return published;
  }
  if (const auto published = publish_availability(); !published.ok()) {
    return published;
  }
  if (const auto published = publish_load(); !published.ok()) {
    return published;
  }
  const std::uint64_t start = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
  std::uint64_t last_heartbeat = start;
  while (!stop_flag.load() && !stopped_.load()) {
    if (session_ == nullptr || session_->closed()) {
      break;
    }
    const std::uint64_t now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (now - last_heartbeat >= options_.heartbeat_interval_ms) {
      (void)heartbeat();
      last_heartbeat = now;
    }
    if (options_.run_duration_ms != 0 && now - start >= options_.run_duration_ms) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return MutationResult::success();
}

void AgentClient::stop() {
  if (stopped_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  if (session_ != nullptr) {
    session_->request_close();
    session_->join();
    session_.reset();
  }
  {
    const std::lock_guard<std::mutex> guard(reply_mutex_);
    reply_cv_.notify_all();
  }
}

}  // namespace agent_scheduler::net
