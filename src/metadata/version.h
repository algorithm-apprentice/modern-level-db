#ifndef MODERN_LEVELDB_METADATA_VERSION_H_
#define MODERN_LEVELDB_METADATA_VERSION_H_

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

#include "format/internal_key.h"
#include "metadata/version_edit.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

// The live table files of every level. Each level lists its files by smallest
// internal key, with the file number breaking ties, and files in levels above 0
// do not overlap. Versions are immutable and share file metadata.
class Version final {
 public:
  using File = std::shared_ptr<const FileMetadata>;

  Version() = default;

  // Requires a level below NumLevels.
  [[nodiscard]] std::span<const File> files(std::uint32_t level) const noexcept;

 private:
  friend class VersionBuilder;

  std::array<std::vector<File>, NumLevels> files_;
};

// Applies version edits to a base version.
class VersionBuilder final {
 public:
  VersionBuilder(const InternalKeyComparator& comparator, const Version& base);
  VersionBuilder(const InternalKeyComparator&& comparator, const Version& base) = delete;

  // Removes the edit's deleted files, then adds its new files. Returns
  // InvalidArgument for a deleted file that is not live in its level, a new
  // file whose number is live in any level, or a new file whose smallest key
  // follows its largest; the builder may then only be destroyed.
  [[nodiscard]] Status Apply(const VersionEdit& edit);

  // Returns the version of the applied edits, or InvalidArgument if two files
  // in a level above 0 overlap.
  [[nodiscard]] Result<Version> Build() const;

 private:
  [[nodiscard]] bool IsLive(std::uint64_t number) const;

  const InternalKeyComparator* comparator_;
  std::array<std::map<std::uint64_t, Version::File>, NumLevels> levels_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_METADATA_VERSION_H_
