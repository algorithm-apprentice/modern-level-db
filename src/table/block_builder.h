#ifndef MODERN_LEVELDB_TABLE_BLOCK_BUILDER_H_
#define MODERN_LEVELDB_TABLE_BLOCK_BUILDER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

// Builds one sorted block with prefix-compressed keys and restart points.
class BlockBuilder final {
 public:
  // The restart interval must be at least one.
  explicit BlockBuilder(std::uint32_t restart_interval) noexcept;

  BlockBuilder(const BlockBuilder&) = delete;
  BlockBuilder& operator=(const BlockBuilder&) = delete;
  BlockBuilder(BlockBuilder&&) = delete;
  BlockBuilder& operator=(BlockBuilder&&) = delete;
  ~BlockBuilder() = default;

  // Keys must strictly increase under the comparator that will read the
  // block. Returns InvalidArgument, leaving the builder unchanged, if the entry
  // would make the finished block larger than UINT32_MAX bytes. Requires that
  // Finish has not been called since construction or the last Reset. If Add or
  // Finish throws, the builder must be Reset before it is used again.
  [[nodiscard]] Status Add(ByteView key, ByteView value);

  // Appends the restart array and returns the block, which stays valid until
  // Reset or destruction.
  [[nodiscard]] ByteView Finish();

  void Reset() noexcept;

  // Returns the size the block would have if it were finished now.
  [[nodiscard]] std::size_t CurrentSizeEstimate() const noexcept;

  // Reports whether no entry was added since construction or the last Reset.
  [[nodiscard]] bool empty() const noexcept { return entries_since_restart_ == 0; }

 private:
  std::uint32_t restart_interval_;
  // Zero only before the first entry, because every Add increments it after
  // any restart.
  std::uint32_t entries_since_restart_ = 0;
  bool finished_ = false;
  std::vector<std::byte> buffer_;
  // Restart points after the implicit first one at offset zero.
  std::vector<std::uint32_t> restarts_;
  std::vector<std::byte> last_key_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_TABLE_BLOCK_BUILDER_H_
