#include "modern_leveldb/block_cache.h"

#include <cstddef>
#include <expected>
#include <memory>

#include "table/block_cache.h"

namespace modern_leveldb {

std::shared_ptr<BlockCache> CreateBlockCache(std::size_t capacity) {
    return std::make_shared<BlockCache>(capacity);
}

Result<std::shared_ptr<BlockCache>> CreateBlockCacheWithOptions(const BlockCacheOptions& options) {
    switch (options.policy) {
        case BlockCachePolicy::Automatic:
        case BlockCachePolicy::Lru:
        case BlockCachePolicy::Clock:
            return std::make_shared<BlockCache>(options.capacity, options.policy);
    }
    return std::unexpected(Error::InvalidArgument("unknown block cache policy"));
}

std::size_t BlockCacheCharge(const BlockCache& cache) { return cache.total_charge(); }

}  // namespace modern_leveldb
