#ifndef MODERN_LEVELDB_ENGINE_READ_DIAGNOSTICS_H_
#define MODERN_LEVELDB_ENGINE_READ_DIAGNOSTICS_H_

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace modern_leveldb::read_diagnostics {

enum class Counter : std::size_t {
  Gets,
  MutableHits,
  ImmutableHits,
  SstableHits,
  Deletions,
  Misses,
  Level0Candidates,
  DeeperCandidates,
  FilesSearched,
  TableCacheHits,
  TableCacheMisses,
  BlockCacheHits,
  BlockCacheMisses,
  RandomReadCalls,
  RandomReadRequestedBytes,
  RandomReadReturnedBytes,
  MappedViewBlocks,
  MappedViewBytes,
  CopiedReadBlocks,
  CopiedReadBytes,
  StoredBlocks,
  StoredBlockBytes,
  DecodedBlocks,
  DecodedBlockBytes,
  DecompressedBlocks,
  ValidationEntries,
  RestartEntriesDecoded,
  IndexEntriesDecoded,
  DataEntriesDecoded,
  InternalKeyComparisons,
  ResultBytes,
  Count,
};

enum class Stage : std::size_t {
  Get,
  CandidateSelection,
  TableCacheLookup,
  BlockCacheLookup,
  RandomRead,
  StoredBlockDecode,
  BlockConstruction,
  IndexSeek,
  DataSeek,
  ResultCopy,
  Count,
};

enum class BlockRole {
  Other,
  Index,
  Data,
};

inline constexpr std::array<std::string_view, static_cast<std::size_t>(Counter::Count)>
    CounterNames{
        "gets",
        "mutable_hits",
        "immutable_hits",
        "sstable_hits",
        "deletions",
        "misses",
        "level0_candidates",
        "deeper_candidates",
        "files_searched",
        "table_cache_hits",
        "table_cache_misses",
        "block_cache_hits",
        "block_cache_misses",
        "random_read_calls",
        "random_read_requested_bytes",
        "random_read_returned_bytes",
        "mapped_view_blocks",
        "mapped_view_bytes",
        "copied_read_blocks",
        "copied_read_bytes",
        "stored_blocks",
        "stored_block_bytes",
        "decoded_blocks",
        "decoded_block_bytes",
        "decompressed_blocks",
        "validation_entries",
        "restart_entries_decoded",
        "index_entries_decoded",
        "data_entries_decoded",
        "internal_key_comparisons",
        "result_bytes",
};

inline constexpr std::array<std::string_view, static_cast<std::size_t>(Stage::Count)> StageNames{
    "get",         "candidate_selection", "table_cache_lookup", "block_cache_lookup",
    "random_read", "stored_block_decode", "block_construction", "index_seek",
    "data_seek",   "result_copy",
};

struct StageTotal {
  std::uint64_t events = 0;
  std::uint64_t nanoseconds = 0;
};

enum class FileOpenReason : std::size_t {
  Mapped,
  Disabled,
  MissingExpectedSize,
  EmptyFile,
  SizeMismatch,
  SizeUnrepresentable,
  CountBudgetExhausted,
  ByteBudgetExhausted,
  StatFailed,
  MmapFailed,
  Count,
};

inline constexpr std::array<std::string_view, static_cast<std::size_t>(FileOpenReason::Count)>
    FileOpenReasonNames{
        "mapped",
        "disabled",
        "missing_expected_size",
        "empty_file",
        "size_mismatch",
        "size_unrepresentable",
        "count_budget_exhausted",
        "byte_budget_exhausted",
        "stat_failed",
        "mmap_failed",
};

struct FileOpenTotal {
  std::uint64_t files = 0;
  std::uint64_t bytes = 0;
};

struct SetupSnapshot {
  std::array<FileOpenTotal, static_cast<std::size_t>(FileOpenReason::Count)> file_opens{};
};

struct Snapshot {
  std::array<std::uint64_t, static_cast<std::size_t>(Counter::Count)> counters{};
  std::array<StageTotal, static_cast<std::size_t>(Stage::Count)> stages{};
  std::uint64_t sample_seed = 0;
  std::uint64_t sample_denominator = 0;
  std::uint64_t sampled_gets = 0;
};

// Installs one foreground-thread setup collection epoch.
class SetupSession final {
 public:
  explicit SetupSession(SetupSnapshot& snapshot) noexcept;
  SetupSession(const SetupSession&) = delete;
  SetupSession& operator=(const SetupSession&) = delete;
  ~SetupSession();
};

// Installs one foreground-thread collection epoch. The snapshot must outlive
// the session, and sessions must not nest on one thread.
class Session final {
 public:
  Session(Snapshot& snapshot, std::uint64_t sample_seed, std::uint64_t sample_denominator) noexcept;
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  ~Session();
};

// Marks one public Get and deterministically selects it for stage timing.
class GetScope final {
 public:
  GetScope() noexcept;
  GetScope(const GetScope&) = delete;
  GetScope& operator=(const GetScope&) = delete;
  ~GetScope();

 private:
  bool active_ = false;
  bool sampled_ = false;
  std::chrono::steady_clock::time_point started_{};
};

// Accumulates an inclusive duration only for a sampled Get.
class StageScope final {
 public:
  explicit StageScope(Stage stage) noexcept;
  StageScope(const StageScope&) = delete;
  StageScope& operator=(const StageScope&) = delete;
  ~StageScope();

 private:
  Stage stage_;
  bool active_ = false;
  std::chrono::steady_clock::time_point started_{};
};

// Attributes trusted block-entry decoding to the active index or data seek.
class BlockRoleScope final {
 public:
  explicit BlockRoleScope(BlockRole role) noexcept;
  BlockRoleScope(const BlockRoleScope&) = delete;
  BlockRoleScope& operator=(const BlockRoleScope&) = delete;
  ~BlockRoleScope();

 private:
  bool active_ = false;
  BlockRole previous_ = BlockRole::Other;
};

void Add(Counter counter, std::uint64_t amount = 1) noexcept;
void RecordDecodedEntry(bool restart = false) noexcept;
void RecordFileOpen(FileOpenReason reason, std::uint64_t bytes) noexcept;

}  // namespace modern_leveldb::read_diagnostics

#endif  // MODERN_LEVELDB_ENGINE_READ_DIAGNOSTICS_H_
