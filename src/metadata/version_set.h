#ifndef MODERN_LEVELDB_METADATA_VERSION_SET_H_
#define MODERN_LEVELDB_METADATA_VERSION_SET_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <vector>

#include "format/internal_key.h"
#include "metadata/version.h"
#include "metadata/version_edit.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "wal/wal_io.h"

namespace modern_leveldb {

// File numbers stay below this bound: the version set rejects larger numbers
// from CURRENT, the MANIFEST, and MarkFileNumberUsed, and LogAndApply rejects
// every edit once allocation passes the bound, so every MANIFEST it writes is
// recoverable and allocation never overflows.
inline constexpr std::uint64_t FileNumberLimit = std::uint64_t{1} << 63U;

// The current version and the database counters, persisted in the MANIFEST
// that CURRENT names. Methods require external synchronization, and the file
// system and the comparator must outlive the version set.
class VersionSet final {
 public:
  // Writes MANIFEST-000001 for a new, empty database and points CURRENT at it.
  // The directory must exist and must not contain a database.
  [[nodiscard]] static Result<std::unique_ptr<VersionSet>> Create(
      FileSystem& file_system, std::filesystem::path directory,
      const InternalKeyComparator& comparator);
  static Result<std::unique_ptr<VersionSet>> Create(
      FileSystem& file_system, std::filesystem::path directory,
      const InternalKeyComparator&& comparator) = delete;

  // Replays the MANIFEST that CURRENT names; a missing CURRENT is NotFound. The
  // first LogAndApply writes a new MANIFEST.
  [[nodiscard]] static Result<std::unique_ptr<VersionSet>> Recover(
      FileSystem& file_system, std::filesystem::path directory,
      const InternalKeyComparator& comparator);
  static Result<std::unique_ptr<VersionSet>> Recover(
      FileSystem& file_system, std::filesystem::path directory,
      const InternalKeyComparator&& comparator) = delete;

  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;
  VersionSet(VersionSet&&) = delete;
  VersionSet& operator=(VersionSet&&) = delete;
  ~VersionSet() = default;

  [[nodiscard]] std::shared_ptr<const Version> current() const noexcept { return current_; }
  [[nodiscard]] std::span<const std::optional<InternalKey>, NumLevels> compact_pointers()
      const noexcept {
    return compact_pointers_;
  }
  // Returns the numbers of the files in every installed version still held,
  // including the current one.
  [[nodiscard]] std::set<std::uint64_t> LiveFiles() const;

  [[nodiscard]] std::uint64_t manifest_file_number() const noexcept {
    return manifest_file_number_;
  }
  [[nodiscard]] std::uint64_t NewFileNumber() noexcept { return next_file_number_++; }
  // Advances the next file number past a number found on disk. Returns
  // InvalidArgument for a number at or above FileNumberLimit.
  [[nodiscard]] Status MarkFileNumberUsed(std::uint64_t number);

  [[nodiscard]] SequenceNumber last_sequence() const noexcept { return last_sequence_; }
  // Requires a sequence at least the current one and at most MaxSequenceNumber.
  void SetLastSequence(SequenceNumber sequence) noexcept;
  [[nodiscard]] std::uint64_t log_number() const noexcept { return log_number_; }
  [[nodiscard]] std::uint64_t prev_log_number() const noexcept { return prev_log_number_; }

  // Records the edit in the MANIFEST and makes the version it produces current.
  // Returns InvalidArgument, writing nothing, once allocation has passed
  // FileNumberLimit, or for an edit that names another comparator, a log
  // number below the current one or not below the next file number, a previous
  // log number or new file number not below the next file number, or files
  // that the version builder rejects. Fills absent log numbers and sets the
  // next file number and last sequence. After a file operation fails, keeps
  // the previous state and returns that error from every later call.
  [[nodiscard]] Status LogAndApply(VersionEdit edit);

 private:
  // Leaves the version set without a current version until one is installed.
  VersionSet(FileSystem& file_system, std::filesystem::path directory,
             const InternalKeyComparator& comparator) noexcept;

  [[nodiscard]] Status Validate(const VersionEdit& edit) const;
  [[nodiscard]] Status Write(const VersionEdit& edit);
  [[nodiscard]] Status InstallCurrent() const;
  void Install(std::shared_ptr<const Version> version);

  FileSystem* file_system_;
  std::filesystem::path directory_;
  const InternalKeyComparator* comparator_;
  std::shared_ptr<const Version> current_;
  std::vector<std::weak_ptr<const Version>> versions_;
  std::array<std::optional<InternalKey>, NumLevels> compact_pointers_;
  std::uint64_t manifest_file_number_ = 1;
  std::uint64_t next_file_number_ = 2;
  SequenceNumber last_sequence_ = 0;
  std::uint64_t log_number_ = 0;
  std::uint64_t prev_log_number_ = 0;
  std::unique_ptr<WalWriter> manifest_;
  std::optional<Error> failure_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_METADATA_VERSION_SET_H_
