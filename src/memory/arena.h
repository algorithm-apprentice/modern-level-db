#ifndef MODERN_LEVELDB_MEMORY_ARENA_H_
#define MODERN_LEVELDB_MEMORY_ARENA_H_

#include <cstddef>
#include <memory>
#include <vector>

#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb {

class Arena final {
 public:
  Arena() noexcept = default;

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&&) = delete;
  Arena& operator=(Arena&&) = delete;

  ~Arena() = default;

  [[nodiscard]] MutableByteView Allocate(std::size_t bytes);
  [[nodiscard]] MutableByteView AllocateAligned(std::size_t bytes);

  [[nodiscard]] std::size_t memory_usage() const noexcept { return memory_usage_; }

 private:
  static constexpr std::size_t BlockSize = 4U * 1'024U;
  static constexpr std::size_t DedicatedAllocationThreshold = BlockSize / 4U;

  [[nodiscard]] std::byte* AllocateFallback(std::size_t bytes);
  [[nodiscard]] std::byte* AllocateBlock(std::size_t bytes);

  std::byte* allocation_pointer_ = nullptr;
  std::size_t bytes_remaining_ = 0;
  std::vector<std::unique_ptr<std::byte[]>> blocks_;
  std::size_t memory_usage_ = 0;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_MEMORY_ARENA_H_
