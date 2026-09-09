// Agent Scheduler — durable persistence integrity.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include <fstream>
#include <string>
#include <vector>

#include "scheduler_fixture.hpp"
#include "test_support.hpp"

using namespace agent_scheduler;
using namespace as_fixture;

namespace {

[[nodiscard]] std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream),
                                   std::istreambuf_iterator<char>());
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void write_byte_at(const std::filesystem::path& path, std::size_t offset, std::uint8_t value) {
  std::vector<std::uint8_t> bytes = read_bytes(path);
  AS_REQUIRE(offset < bytes.size());
  bytes[offset] = value;
  write_bytes(path, bytes);
}

[[nodiscard]] std::unique_ptr<AgentScheduler> populated() {
  auto scheduler = std::make_unique<AgentScheduler>(test_options());
  for (std::uint64_t index = 1; index <= 3; ++index) {
    const AgentDescriptor descriptor = make_agent(index, index * 7, 2);
    AS_CHECK(register_full(*scheduler, descriptor, {{"coding", 900}}).ok());
  }
  for (std::uint64_t index = 1; index <= 4; ++index) {
    AS_CHECK(scheduler->submit_work(make_work(100 + index, {"coding"}, static_cast<std::uint32_t>(index * 10))).ok());
  }
  const BatchDecision batch = scheduler->schedule_batch(ScheduleRequest{});
  AS_CHECK(batch.assigned >= 1);
  return scheduler;
}

}  // namespace

AS_TEST(persistence, round_trip_preserves_durable_semantics) {
  ScratchDirectory scratch("persist_round_trip");
  const std::filesystem::path path = scratch.file("state.asstate");
  const auto source = populated();
  const SchedulerSummary before = source->summary();
  AS_CHECK(source->save(path).ok());

  PersistenceHeaderInfo header;
  AS_CHECK(read_persistence_header(path, header).ok());
  AS_CHECK(header.format_version == PersistenceFormat::version);
  AS_CHECK(header.payload_bytes > 0);
  AS_CHECK(header.record_count > 0);
  AS_CHECK(!header.semantic_digest.is_zero());

  SchedulerOptions options = test_options();
  options.auto_start = false;
  AgentScheduler recovered(options);
  const RecoveryResult result = recovered.load(path);
  AS_CHECK(result.ok());
  AS_CHECK(result.agents_loaded == 3);
  AS_CHECK(result.work_loaded == 4);
  AS_CHECK(result.assignments_loaded == before.active_assignments + before.historical_assignments);
  AS_CHECK(result.stored_digest == result.computed_digest);
  AS_CHECK(result.new_scheduler_epoch.value() == result.previous_scheduler_epoch.value() + 1);
  const SchedulerSummary after = recovered.summary();
  AS_CHECK(after.agent_count == before.agent_count);
  AS_CHECK(after.work_admitted == before.work_admitted);
  AS_CHECK(after.queue_count == before.queue_count);
  AS_CHECK(recovered.check_invariants().ok());
}

AS_TEST(persistence, every_integrity_field_is_enforced) {
  ScratchDirectory scratch("persist_integrity");
  const std::filesystem::path path = scratch.file("state.asstate");
  const auto source = populated();
  AS_CHECK(source->save(path).ok());
  const std::vector<std::uint8_t> good = read_bytes(path);
  AS_REQUIRE(good.size() > PersistenceFormat::header_bytes + PersistenceFormat::trailer_bytes);

  SchedulerOptions options = test_options();
  options.auto_start = false;

  {  // Bad magic.
    const std::filesystem::path copy = scratch.file("bad_magic.asstate");
    write_bytes(copy, good);
    write_byte_at(copy, 0, static_cast<std::uint8_t>('X'));
    AgentScheduler scheduler(options);
    const RecoveryResult result = scheduler.load(copy);
    AS_CHECK(!result.ok());
    AS_CHECK(result.error->code == ErrorCode::PersistenceFormat);
  }
  {  // Unsupported version must be rejected before the payload is interpreted.
    const std::filesystem::path copy = scratch.file("bad_version.asstate");
    write_bytes(copy, good);
    write_byte_at(copy, 8, 99);
    AgentScheduler scheduler(options);
    const RecoveryResult result = scheduler.load(copy);
    AS_CHECK(!result.ok());
    AS_CHECK(result.error->code == ErrorCode::PersistenceUnsupportedVersion);
  }
  {  // Header checksum.
    const std::filesystem::path copy = scratch.file("bad_header_crc.asstate");
    write_bytes(copy, good);
    write_byte_at(copy, 16, static_cast<std::uint8_t>(good[16] ^ 0xFFu));
    AgentScheduler scheduler(options);
    AS_CHECK(!scheduler.load(copy).ok());
  }
  {  // Payload checksum.
    const std::filesystem::path copy = scratch.file("bad_payload_crc.asstate");
    write_bytes(copy, good);
    write_byte_at(copy, good.size() - PersistenceFormat::trailer_bytes - 1,
                  static_cast<std::uint8_t>(good[good.size() - PersistenceFormat::trailer_bytes - 1] ^ 0xFFu));
    AgentScheduler scheduler(options);
    const RecoveryResult result = scheduler.load(copy);
    AS_CHECK(!result.ok());
    AS_CHECK(result.error->code == ErrorCode::PersistenceIntegrity);
  }
  {  // Truncation.
    const std::filesystem::path copy = scratch.file("truncated.asstate");
    write_bytes(copy, std::vector<std::uint8_t>(good.begin(), good.end() - 5));
    AgentScheduler scheduler(options);
    AS_CHECK(!scheduler.load(copy).ok());
  }
  {  // Trailing garbage.
    const std::filesystem::path copy = scratch.file("garbage.asstate");
    std::vector<std::uint8_t> extended = good;
    extended.push_back(0x42);
    write_bytes(copy, extended);
    AgentScheduler scheduler(options);
    const RecoveryResult result = scheduler.load(copy);
    AS_CHECK(!result.ok());
    AS_CHECK(result.error->code == ErrorCode::PersistenceIntegrity);
  }
  {  // Trailer magic.
    const std::filesystem::path copy = scratch.file("bad_trailer.asstate");
    write_bytes(copy, good);
    write_byte_at(copy, good.size() - PersistenceFormat::trailer_bytes, static_cast<std::uint8_t>('Z'));
    AgentScheduler scheduler(options);
    AS_CHECK(!scheduler.load(copy).ok());
  }
  {  // Semantic digest mismatch: rewrite the stored digest field.
    const std::filesystem::path copy = scratch.file("bad_digest.asstate");
    write_bytes(copy, good);
    write_byte_at(copy, 40, static_cast<std::uint8_t>(good[40] ^ 0x01u));
    // Repair the header checksum so only the semantic digest is wrong.
    std::vector<std::uint8_t> bytes = read_bytes(copy);
    std::uint32_t zero = 0;
    std::memcpy(bytes.data() + 36, &zero, sizeof(zero));
    const std::uint32_t header_crc = crc32(bytes);
    std::memcpy(bytes.data() + 36, &header_crc, sizeof(header_crc));
    write_bytes(copy, bytes);
    AgentScheduler scheduler(options);
    const RecoveryResult result = scheduler.load(copy);
    AS_CHECK(!result.ok());
    AS_CHECK(result.error->code == ErrorCode::PersistenceIntegrity);
  }
}

AS_TEST(persistence, atomic_replacement_leaves_no_temporary_files) {
  ScratchDirectory scratch("persist_atomic");
  const std::filesystem::path path = scratch.file("state.asstate");
  const auto source = populated();
  AS_CHECK(source->save(path).ok());
  std::size_t entries = 0;
  for (const auto& entry : std::filesystem::directory_iterator(scratch.path())) {
    (void)entry;
    ++entries;
  }
  AS_CHECK(entries == 1);
  AS_CHECK(source->save(path).ok());
  entries = 0;
  for (const auto& entry : std::filesystem::directory_iterator(scratch.path())) {
    (void)entry;
    ++entries;
  }
  AS_CHECK(entries == 1);
}

AS_TEST(persistence, io_failures_are_reported_without_corrupting_state) {
  ScratchDirectory scratch("persist_io");
  const auto source = populated();
  const std::filesystem::path missing = scratch.file("nested").string() + "/state.asstate";
  const MutationResult result = source->save(missing);
  AS_CHECK(!result.ok());
  AS_CHECK(result.error->code == ErrorCode::PersistenceIo);
  const std::filesystem::path absent = scratch.file("absent.asstate");
  SchedulerOptions options = test_options();
  options.auto_start = false;
  AgentScheduler scheduler(options);
  const RecoveryResult load = scheduler.load(absent);
  AS_CHECK(!load.ok());
  AS_CHECK(load.error->code == ErrorCode::PersistenceIo);
  AS_CHECK(scheduler.summary().agent_count == 0);
}

AS_TEST(persistence, hostile_paths_are_handled) {
  ScratchDirectory scratch("persist_paths");
  const std::filesystem::path spaced = scratch.path() / "directory with spaces";
  std::filesystem::create_directories(spaced);
  const auto source = populated();
  AS_CHECK(source->save(spaced / "state with spaces.asstate").ok());
  const std::filesystem::path unicode = scratch.path() / "\u00fcnicode-\u00e5gent";
  std::filesystem::create_directories(unicode);
  AS_CHECK(source->save(unicode / "state.asstate").ok());
  SchedulerOptions options = test_options();
  options.auto_start = false;
  AgentScheduler scheduler(options);
  AS_CHECK(scheduler.load(unicode / "state.asstate").ok());
}

AS_TEST(persistence, save_is_rejected_above_the_configured_byte_limit) {
  ScratchDirectory scratch("persist_limit");
  const auto source = populated();
  PersistOptions options;
  options.max_bytes = 64;
  const MutationResult result = source->save(scratch.file("state.asstate"), options);
  AS_CHECK(!result.ok());
  AS_CHECK(result.error->code == ErrorCode::LimitExceeded);
}

AS_TEST(persistence, load_into_a_running_scheduler_is_rejected) {
  ScratchDirectory scratch("persist_running");
  const std::filesystem::path path = scratch.file("state.asstate");
  const auto source = populated();
  AS_CHECK(source->save(path).ok());
  AgentScheduler running(test_options());
  const RecoveryResult result = running.load(path);
  AS_CHECK(!result.ok());
  AS_CHECK(result.error->code == ErrorCode::AlreadyStarted);
}

AS_TEST(persistence, deterministic_encoding_produces_identical_bytes) {
  ScratchDirectory scratch("persist_deterministic");
  const std::filesystem::path first_path = scratch.file("first.asstate");
  const std::filesystem::path second_path = scratch.file("second.asstate");
  const auto source = populated();
  AS_CHECK(source->save(first_path).ok());
  AS_CHECK(source->save(second_path).ok());
  AS_CHECK(read_bytes(first_path) == read_bytes(second_path));
}
