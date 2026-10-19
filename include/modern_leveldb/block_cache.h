#ifndef MODERN_LEVELDB_BLOCK_CACHE_H_
#define MODERN_LEVELDB_BLOCK_CACHE_H_

#include <cstddef>
#include <memory>

namespace modern_leveldb {

// Opaque decoded-block storage; shared owners can serve several databases.
class BlockCache;

// Capacity is an eviction target, not a strict allocation limit. Zero disables
// cache retention. Pinned blocks can postpone eviction.
[[nodiscard]] std::shared_ptr<BlockCache> CreateBlockCache(std::size_t capacity);

// Concurrent observation of retained block charges, excluding unrelated
// allocator/cache metadata overhead.
[[nodiscard]] std::size_t BlockCacheCharge(const BlockCache& cache);

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_BLOCK_CACHE_H_
