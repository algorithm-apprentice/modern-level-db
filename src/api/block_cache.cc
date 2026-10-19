#include "modern_leveldb/block_cache.h"

#include <cstddef>
#include <memory>

#include "table/block_cache.h"

namespace modern_leveldb {

std::shared_ptr<BlockCache> CreateBlockCache(std::size_t capacity) {
    return std::make_shared<BlockCache>(capacity);
}

std::size_t BlockCacheCharge(const BlockCache& cache) { return cache.total_charge(); }

}  // namespace modern_leveldb
