#include "engine/compaction.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "engine/iterators.h"
#include "metadata/filenames.h"
#include "metadata/version.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "table/table.h"

namespace modern_leveldb {
namespace {

// LevelDB's MaxGrandParentOverlapBytes, relative to the target file size.
constexpr std::uint64_t GrandparentOverlapFactor = 10;

void Expect(const Status& status) noexcept {
  assert(status.has_value());
  static_cast<void>(status);
}

// Decodes a key that ParseInternalKey and the table builder accepted.
InternalKey Decoded(ByteView key) {
  Result<InternalKey> decoded = InternalKey::Decode(key);
  assert(decoded.has_value());
  return std::move(decoded).value();
}

// Sums the grandparents that the entries pass, as LevelDB's
// Compaction::ShouldStopBefore does.
class GrandparentOverlap {
 public:
  GrandparentOverlap(const InternalKeyComparator& comparator,
                     std::span<const Version::File> grandparents, std::uint64_t limit) noexcept
      : comparator_(comparator), grandparents_(grandparents), limit_(limit) {}

  // Returns whether the output must end before the entry with this key. The
  // grandparents passed before the first entry do not count.
  bool ShouldStopBefore(ByteView key) noexcept {
    while (next_ < grandparents_.size() &&
           comparator_.Compare(key, grandparents_[next_]->largest.encoded()) > 0) {
      if (seen_key_) {
        overlapped_ += grandparents_[next_]->file_size;
      }
      ++next_;
    }
    seen_key_ = true;
    if (overlapped_ > limit_) {
      overlapped_ = 0;
      return true;
    }
    return false;
  }

 private:
  const InternalKeyComparator& comparator_;
  std::span<const Version::File> grandparents_;
  std::uint64_t limit_;
  std::size_t next_ = 0;
  bool seen_key_ = false;
  std::uint64_t overlapped_ = 0;
};

// Answers LevelDB's Compaction::IsBaseLevelForKey for user keys in increasing
// order, scanning each level below the next one once.
class BaseLevel {
 public:
  BaseLevel(const Version& version, std::uint32_t level, const Comparator& user_comparator) noexcept
      : version_(version), first_level_(level + 2), user_comparator_(user_comparator) {}

  // Returns whether no file in the levels below the next one holds the key.
  bool IsBaseLevelForKey(ByteView user_key) noexcept {
    for (std::uint32_t level = first_level_; level < NumLevels; ++level) {
      const std::span<const Version::File> files = version_.files(level);
      std::size_t& next = next_[level];
      while (next < files.size()) {
        const FileMetadata& file = *files[next];
        if (user_comparator_.Compare(user_key, file.largest.user_key()) <= 0) {
          if (user_comparator_.Compare(user_key, file.smallest.user_key()) >= 0) {
            return false;
          }
          break;
        }
        ++next;
      }
    }
    return true;
  }

 private:
  const Version& version_;
  std::uint32_t first_level_;
  const Comparator& user_comparator_;
  std::array<std::size_t, NumLevels> next_{};
};

// Writes the entries that a compaction keeps to verified output tables.
class Outputs {
 public:
  Outputs(FileSystem& file_system, const std::filesystem::path& directory,
          const InternalKeyComparator& comparator, const CompactionOptions& options,
          TableCache& table_cache, const CompactionHooks& hooks) noexcept
      : file_system_(file_system),
        directory_(directory),
        comparator_(comparator),
        options_(options),
        table_cache_(table_cache),
        hooks_(hooks) {}

  [[nodiscard]] bool open() const noexcept { return builder_.has_value(); }
  [[nodiscard]] std::span<const FileMetadata> finished() const noexcept { return finished_; }

  // Adds the entry to the open output, opening one if there is none, and
  // finishes the output once it reaches the target file size.
  [[nodiscard]] Status Add(ByteView key, ByteView value) {
    if (!builder_.has_value()) {
      number_ = hooks_.new_file_number();
      const std::filesystem::path path = TableFileName(directory_, number_);
      Result<std::unique_ptr<WritableFile>> file = file_system_.OpenWritable(path);
      if (!file.has_value()) {
        return std::unexpected(std::move(file).error());
      }
      builder_.emplace(std::move(*file), comparator_, options_.table_options);
      smallest_.assign(key.begin(), key.end());
    }
    largest_.assign(key.begin(), key.end());
    const Status added = builder_->Add(key, value);
    if (!added.has_value()) {
      return added;
    }
    if (builder_->file_size() >= options_.target_file_size) {
      return Finish();
    }
    return {};
  }

  // Finishes the open output and checks that its table opens.
  [[nodiscard]] Status Finish() {
    const Status finished = builder_->Finish();
    const std::uint64_t size = builder_->file_size();
    builder_.reset();
    if (!finished.has_value()) {
      return finished;
    }
    const Result<TableCache::Handle> table = table_cache_.Find(number_, size);
    if (!table.has_value()) {
      return std::unexpected(table.error());
    }
    InternalKey smallest = Decoded(smallest_);
    InternalKey largest = Decoded(largest_);
    FileMetadata output{.number = number_,
                        .file_size = size,
                        .smallest = std::move(smallest),
                        .largest = std::move(largest)};
    finished_.push_back(std::move(output));
    return {};
  }

 private:
  FileSystem& file_system_;
  const std::filesystem::path& directory_;
  const InternalKeyComparator& comparator_;
  const CompactionOptions& options_;
  TableCache& table_cache_;
  const CompactionHooks& hooks_;
  std::optional<TableBuilder> builder_;
  std::uint64_t number_ = 0;
  std::vector<std::byte> smallest_;
  std::vector<std::byte> largest_;
  std::vector<FileMetadata> finished_;
};

}  // namespace

std::unique_ptr<InternalIterator> NewCompactionIterator(const Compaction& compaction,
                                                        TableCache& table_cache,
                                                        const InternalKeyComparator& comparator) {
  TableReadOptions options;
  options.fill_cache = false;
  std::vector<std::unique_ptr<InternalIterator>> children;
  for (std::uint32_t which = 0; which < compaction.inputs.size(); ++which) {
    const std::vector<Version::File>& files = compaction.inputs[which];
    if (compaction.level + which == 0) {
      // Level-0 files may overlap, so each is merged on its own.
      for (const Version::File& file : files) {
        children.push_back(NewLevelIterator(compaction.version, std::span(&file, 1), table_cache,
                                            comparator, options));
      }
    } else if (!files.empty()) {
      children.push_back(
          NewLevelIterator(compaction.version, files, table_cache, comparator, options));
    }
  }
  return NewMergingIterator(comparator, std::move(children));
}

Result<VersionEdit> RunCompaction(FileSystem& file_system, const std::filesystem::path& directory,
                                  const InternalKeyComparator& comparator,
                                  const CompactionOptions& options, TableCache& table_cache,
                                  const Compaction& compaction, InternalIterator& input,
                                  SequenceNumber smallest_snapshot, const CompactionHooks& hooks) {
  const Comparator& user_comparator = comparator.user_comparator();
  GrandparentOverlap grandparents(comparator, compaction.grandparents,
                                  GrandparentOverlapFactor * options.target_file_size);
  BaseLevel base_level(*compaction.version, compaction.level, user_comparator);
  Outputs outputs(file_system, directory, comparator, options, table_cache, hooks);
  std::optional<std::vector<std::byte>> user_key;
  std::optional<SequenceNumber> last_sequence;

  Status moved = input.SeekToFirst();
  while (moved.has_value() && input.valid()) {
    const Status before = hooks.before_entry();
    if (!before.has_value()) {
      return std::unexpected(before.error());
    }
    const ByteView key = input.key();
    // The grandparents pass for every entry, whether it is kept or dropped.
    if (grandparents.ShouldStopBefore(key) && outputs.open()) {
      const Status finished = outputs.Finish();
      if (!finished.has_value()) {
        return std::unexpected(finished.error());
      }
    }
    const Result<ParsedInternalKey> parsed = ParseInternalKey(key);
    if (!parsed.has_value()) {
      return std::unexpected(parsed.error());
    }
    if (!user_key.has_value() || user_comparator.Compare(parsed->user_key, *user_key) != 0) {
      user_key.emplace(parsed->user_key.begin(), parsed->user_key.end());
      last_sequence.reset();
    }
    // Every reader sees the earlier entry of the user key, or the deletion
    // hides nothing that remains below the compaction.
    const bool drop =
        (last_sequence.has_value() && *last_sequence <= smallest_snapshot) ||
        (parsed->kind == ValueKind::Deletion && parsed->sequence <= smallest_snapshot &&
         base_level.IsBaseLevelForKey(parsed->user_key));
    last_sequence = parsed->sequence;
    if (!drop) {
      const Status added = outputs.Add(key, input.value());
      if (!added.has_value()) {
        return std::unexpected(added.error());
      }
    }
    moved = input.Next();
  }
  if (!moved.has_value()) {
    return std::unexpected(moved.error());
  }
  if (outputs.open()) {
    const Status finished = outputs.Finish();
    if (!finished.has_value()) {
      return std::unexpected(finished.error());
    }
  }

  VersionEdit edit = CompactionEdit(compaction);
  if (outputs.finished().empty()) {
    return edit;
  }
  const Status synced = file_system.SyncDirectory(directory);
  if (!synced.has_value()) {
    return std::unexpected(synced.error());
  }
  for (const FileMetadata& output : outputs.finished()) {
    Expect(edit.AddFile(compaction.level + 1, output));
  }
  return edit;
}

}  // namespace modern_leveldb
