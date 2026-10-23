#ifndef MODERN_LEVELDB_BLOCK_CACHE_H_
#define MODERN_LEVELDB_BLOCK_CACHE_H_

#include <cstddef>
#include <memory>

#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

enum class BlockCachePolicy {
    Automatic,
    Lru,
    Clock,
};

struct BlockCacheOptions {
    std::size_t capacity = 0;
    BlockCachePolicy policy = BlockCachePolicy::Automatic;
};

// Opaque decoded-block storage; shared owners can serve several databases.
class BlockCache;

// Capacity is an eviction target, not a strict allocation limit. Zero disables
// cache retention. Pinned blocks can postpone eviction.
[[nodiscard]] std::shared_ptr<BlockCache> CreateBlockCache(std::size_t capacity);

// Explicit named policy selection. Automatic currently retains LRU; an invalid
// policy is an error. Allocation failures retain the ordinary throwing model.
[[nodiscard]] Result<std::shared_ptr<BlockCache>> CreateBlockCacheWithOptions(
    const BlockCacheOptions& options);

// Concurrent observation of retained block charges, excluding unrelated
// allocator/cache metadata overhead.
[[nodiscard]] std::size_t BlockCacheCharge(const BlockCache& cache);

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_BLOCK_CACHE_H_
