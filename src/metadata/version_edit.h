#ifndef MODERN_LEVELDB_METADATA_VERSION_EDIT_H_
#define MODERN_LEVELDB_METADATA_VERSION_EDIT_H_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

inline constexpr std::uint32_t NumLevels = 7;

struct FileMetadata {
  std::uint64_t number;
  std::uint64_t file_size;
  InternalKey smallest;
  InternalKey largest;
};

struct CompactPointer {
  std::uint32_t level;
  InternalKey key;
};

struct DeletedFile {
  std::uint32_t level;
  std::uint64_t number;

  friend auto operator<=>(const DeletedFile&, const DeletedFile&) = default;
};

struct NewFile {
  std::uint32_t level;
  FileMetadata file;
};

// One MANIFEST record: changes to the live table files and database counters.
// Mutators reject values that the format or later readers cannot represent, so
// every edit is encodable.
class VersionEdit final {
 public:
  [[nodiscard]] Status SetComparatorName(std::string_view name);
  void SetLogNumber(std::uint64_t number) noexcept { log_number_ = number; }
  void SetPrevLogNumber(std::uint64_t number) noexcept { prev_log_number_ = number; }
  [[nodiscard]] Status SetNextFileNumber(std::uint64_t number);
  [[nodiscard]] Status SetLastSequence(SequenceNumber sequence);
  [[nodiscard]] Status AddCompactPointer(std::uint32_t level, InternalKey key);
  [[nodiscard]] Status AddFile(std::uint32_t level, FileMetadata file);
  [[nodiscard]] Status RemoveFile(std::uint32_t level, std::uint64_t number);

  [[nodiscard]] const std::optional<std::string>& comparator_name() const noexcept {
    return comparator_name_;
  }
  [[nodiscard]] std::optional<std::uint64_t> log_number() const noexcept { return log_number_; }
  [[nodiscard]] std::optional<std::uint64_t> prev_log_number() const noexcept {
    return prev_log_number_;
  }
  [[nodiscard]] std::optional<std::uint64_t> next_file_number() const noexcept {
    return next_file_number_;
  }
  [[nodiscard]] std::optional<SequenceNumber> last_sequence() const noexcept {
    return last_sequence_;
  }
  [[nodiscard]] std::span<const CompactPointer> compact_pointers() const noexcept {
    return compact_pointers_;
  }
  [[nodiscard]] const std::set<DeletedFile>& deleted_files() const noexcept {
    return deleted_files_;
  }
  [[nodiscard]] std::span<const NewFile> new_files() const noexcept { return new_files_; }

  // Returns the LevelDB encoding, with fields in LevelDB's canonical order.
  [[nodiscard]] std::vector<std::byte> Encode() const;
  // Accepts fields in any order. A repeated scalar field keeps its last value.
  [[nodiscard]] static Result<VersionEdit> Decode(ByteView encoded);

 private:
  [[nodiscard]] Status DecodeField(std::uint32_t tag, ByteView& input);

  std::optional<std::string> comparator_name_;
  std::optional<std::uint64_t> log_number_;
  std::optional<std::uint64_t> prev_log_number_;
  std::optional<std::uint64_t> next_file_number_;
  std::optional<SequenceNumber> last_sequence_;
  std::vector<CompactPointer> compact_pointers_;
  std::set<DeletedFile> deleted_files_;
  std::vector<NewFile> new_files_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_METADATA_VERSION_EDIT_H_
