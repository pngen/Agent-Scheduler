// Agent Scheduler — persistence file I/O and integrity verification.
// Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
#include "internal/persistence_io.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>

#include "agent_scheduler/clock.hpp"
#include "agent_scheduler/digest.hpp"

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#endif

namespace agent_scheduler {
namespace {

constexpr std::uint32_t kHeaderCrcOffset = 36;

[[nodiscard]] MutationResult io_error(ErrorCode code,
                                      std::string operation,
                                      std::string subject,
                                      std::string detail) {
  return MutationResult::failure(
      make_error(code, std::move(operation), std::move(subject), std::move(detail),
                 ScheduleOutcome::RejectInvalidRequest));
}

[[nodiscard]] std::string temp_suffix() {
  static std::atomic<std::uint64_t> counter{0};
  return ".tmp-" + std::to_string(static_cast<unsigned long long>(process_nonce())) + "-" +
         std::to_string(counter.fetch_add(1));
}

[[nodiscard]] bool flush_to_storage(std::FILE* file) noexcept {
#if defined(_WIN32)
  return std::fflush(file) == 0 && _commit(_fileno(file)) == 0;
#else
  return std::fflush(file) == 0 && ::fsync(::fileno(file)) == 0;
#endif
}

}  // namespace

namespace internal {

MutationResult read_all(const std::filesystem::path& path,
                        std::uint64_t max_bytes,
                        std::vector<std::uint8_t>& out) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) {
    return io_error(ErrorCode::PersistenceIo, "load", path.string(),
                    std::string("cannot stat state file: ") + error.message());
  }
  if (size > max_bytes) {
    return io_error(ErrorCode::LimitExceeded, "load", path.string(),
                    "state file exceeds the configured persistence byte limit");
  }
  std::FILE* file = nullptr;
#if defined(_WIN32)
  if (::_wfopen_s(&file, path.c_str(), L"rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.string().c_str(), "rb");
#endif
  if (file == nullptr) {
    return io_error(ErrorCode::PersistenceIo, "load", path.string(), "cannot open state file for reading");
  }
  out.assign(static_cast<std::size_t>(size), 0);
  const std::size_t read = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), file);
  std::fclose(file);
  if (read != out.size()) {
    return io_error(ErrorCode::PersistenceIo, "load", path.string(), "short read while loading state file");
  }
  return MutationResult::success();
}

MutationResult verify_file(const std::vector<std::uint8_t>& file,
                           PersistenceHeaderInfo& info,
                           std::vector<std::uint8_t>& payload) {
  constexpr std::size_t kHeader = PersistenceFormat::header_bytes;
  constexpr std::size_t kTrailer = PersistenceFormat::trailer_bytes;
  if (file.size() < kHeader + kTrailer) {
    return io_error(ErrorCode::PersistenceIntegrity, "load", "",
                    "state file is shorter than the header and trailer");
  }
  if (std::memcmp(file.data(), PersistenceFormat::magic.data(), PersistenceFormat::magic.size()) != 0) {
    return io_error(ErrorCode::PersistenceFormat, "load", "", "state file magic does not match");
  }
  std::uint32_t version = 0;
  std::memcpy(&version, file.data() + 8, sizeof(version));
  if (version != PersistenceFormat::version) {
    return io_error(ErrorCode::PersistenceUnsupportedVersion, "load", "",
                    "state file format version " + std::to_string(version) +
                        " is not supported by this build (expected " +
                        std::to_string(PersistenceFormat::version) + ")");
  }
  std::vector<std::uint8_t> header(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(kHeader));
  std::uint32_t stored_header_crc = 0;
  std::memcpy(&stored_header_crc, header.data() + kHeaderCrcOffset, sizeof(stored_header_crc));
  std::uint32_t zero = 0;
  std::memcpy(header.data() + kHeaderCrcOffset, &zero, sizeof(zero));
  const std::uint32_t computed_header_crc = crc32(header);
  if (computed_header_crc != stored_header_crc) {
    return io_error(ErrorCode::PersistenceIntegrity, "load", "", "state file header checksum mismatch");
  }
  std::uint32_t flags = 0;
  std::uint64_t payload_bytes = 0;
  std::uint64_t record_count = 0;
  std::uint32_t payload_crc = 0;
  std::memcpy(&flags, file.data() + 12, sizeof(flags));
  std::memcpy(&payload_bytes, file.data() + 16, sizeof(payload_bytes));
  std::memcpy(&record_count, file.data() + 24, sizeof(record_count));
  std::memcpy(&payload_crc, file.data() + 32, sizeof(payload_crc));
  const std::uint64_t expected_total = static_cast<std::uint64_t>(kHeader) + payload_bytes + kTrailer;
  if (expected_total != file.size()) {
    return io_error(ErrorCode::PersistenceIntegrity, "load", "",
                    "state file length does not match the declared payload length (truncation or trailing garbage)");
  }
  if (std::memcmp(file.data() + kHeader + payload_bytes, PersistenceFormat::magic.data(),
                  PersistenceFormat::magic.size()) != 0) {
    return io_error(ErrorCode::PersistenceIntegrity, "load", "", "state file trailer magic does not match");
  }
  std::uint32_t stored_trailer_crc = 0;
  std::memcpy(&stored_trailer_crc, file.data() + kHeader + payload_bytes + 8, sizeof(stored_trailer_crc));
  const std::uint32_t computed_trailer_crc =
      crc32(std::span<const std::uint8_t>(file.data(), kHeader + static_cast<std::size_t>(payload_bytes))) ^
      0x5A5A5A5Au;
  if (computed_trailer_crc != stored_trailer_crc) {
    return io_error(ErrorCode::PersistenceIntegrity, "load", "", "state file trailer checksum mismatch");
  }
  payload.assign(file.begin() + static_cast<std::ptrdiff_t>(kHeader),
                 file.begin() + static_cast<std::ptrdiff_t>(kHeader + payload_bytes));
  if (crc32(payload) != payload_crc) {
    return io_error(ErrorCode::PersistenceIntegrity, "load", "", "state file payload checksum mismatch");
  }
  info.format_version = version;
  info.flags = flags;
  info.payload_bytes = payload_bytes;
  info.record_count = record_count;
  info.payload_crc32 = payload_crc;
  info.semantic_digest = Digest256{};
  std::memcpy(info.semantic_digest.bytes.data(), file.data() + 40, 32);
  info.total_bytes = file.size();
  return MutationResult::success();
}

MutationResult write_atomically(const std::filesystem::path& path,
                                const std::vector<std::uint8_t>& bytes,
                                const PersistOptions& options) {
  const std::filesystem::path directory = path.parent_path();
  if (!directory.empty()) {
    std::error_code error;
    if (!std::filesystem::exists(directory, error) || error) {
      return io_error(ErrorCode::PersistenceIo, "save", path.string(),
                      "destination directory does not exist");
    }
  }
  const std::filesystem::path temporary = path.string() + temp_suffix();
  {
    std::FILE* file = nullptr;
#if defined(_WIN32)
    if (::_wfopen_s(&file, temporary.c_str(), L"wb") != 0) {
      file = nullptr;
    }
#else
    file = std::fopen(temporary.string().c_str(), "wb");
#endif
    if (file == nullptr) {
      return io_error(ErrorCode::PersistenceIo, "save", path.string(),
                      "cannot open temporary state file for writing");
    }
    const std::size_t written = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), file);
    const bool flushed = !options.fsync || flush_to_storage(file);
    const int closed = std::fclose(file);
    if (written != bytes.size() || !flushed || closed != 0) {
      (void)std::filesystem::remove(temporary);
      return io_error(ErrorCode::PersistenceIo, "save", path.string(),
                      "failed while writing or flushing the temporary state file");
    }
  }
  if (options.verify_after_write) {
    std::vector<std::uint8_t> reread;
    if (const auto read_result = read_all(temporary, bytes.size() + 1, reread); !read_result.ok()) {
      (void)std::filesystem::remove(temporary);
      return read_result;
    }
    if (reread != bytes) {
      (void)std::filesystem::remove(temporary);
      return io_error(ErrorCode::PersistenceIntegrity, "save", path.string(),
                      "re-read state file does not match the bytes written");
    }
    PersistenceHeaderInfo info;
    std::vector<std::uint8_t> payload;
    if (const auto verify_result = verify_file(reread, info, payload); !verify_result.ok()) {
      (void)std::filesystem::remove(temporary);
      return verify_result;
    }
  }
#if defined(_WIN32)
  if (::MoveFileExW(temporary.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    const DWORD code = ::GetLastError();
    (void)std::filesystem::remove(temporary);
    return io_error(ErrorCode::PersistenceIo, "save", path.string(),
                    "atomic replacement failed with Windows error " + std::to_string(code));
  }
#else
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    (void)std::filesystem::remove(temporary);
    return io_error(ErrorCode::PersistenceIo, "save", path.string(),
                    std::string("atomic replacement failed: ") + error.message());
  }
#endif
  return MutationResult::success();
}

MutationResult remove_if_present(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    return MutationResult::success();
  }
  std::filesystem::remove(path, error);
  if (error) {
    return io_error(ErrorCode::PersistenceIo, "remove", path.string(), error.message());
  }
  return MutationResult::success();
}

}  // namespace internal

MutationResult read_persistence_header(const std::filesystem::path& path, PersistenceHeaderInfo& out) {
  std::vector<std::uint8_t> file;
  if (const auto read_result = internal::read_all(path, 1ull << 40, file); !read_result.ok()) {
    return read_result;
  }
  std::vector<std::uint8_t> payload;
  return internal::verify_file(file, out, payload);
}

}  // namespace agent_scheduler
