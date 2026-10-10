#ifndef MODERN_LEVELDB_MEMORY_ARENA_H_
#define MODERN_LEVELDB_MEMORY_ARENA_H_

#include <cstddef>
#include <memory>
#include <vector>

#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb {

// Monotonic byte storage: allocations remain stable until arena destruction,
// when all blocks are reclaimed together. Individual entries cannot be freed.
// Allocation and accounting need one writer or external synchronization.
// See docs/learning/03-memory-and-mvcc.md.
class Arena final {
public:
    static constexpr std::size_t Alignment = sizeof(void*) > std::size_t{8} ? sizeof(void*)
                                                                            : std::size_t{8};

    Arena() noexcept = default;

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;

    ~Arena() = default;

    [[nodiscard]] MutableByteView Allocate(std::size_t bytes);
    [[nodiscard]] MutableByteView AllocateAligned(std::size_t bytes);

    // Reserved block bytes plus per-block pointer accounting, not live payload
    // size or total process memory. Must not race with allocation.
    [[nodiscard]] std::size_t memory_usage() const noexcept { return memory_usage_; }

private:
    static constexpr std::size_t BlockSize = 4U * 1'024U;
    static constexpr std::size_t DedicatedAllocationThreshold = BlockSize / 4U;

    [[nodiscard]] std::byte* AllocateFromCurrentBlock(std::size_t bytes,
                                                      std::size_t padding = 0) noexcept;
    [[nodiscard]] std::byte* AllocateFallback(std::size_t bytes);
    [[nodiscard]] std::byte* AllocateBlock(std::size_t bytes);

    std::byte* allocation_pointer_ = nullptr;
    std::size_t bytes_remaining_ = 0;
    std::vector<std::unique_ptr<std::byte[]>> blocks_;
    std::size_t memory_usage_ = 0;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_MEMORY_ARENA_H_
