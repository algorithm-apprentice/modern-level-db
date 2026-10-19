#ifndef MODERN_LEVELDB_TABLE_BLOCK_CACHE_H_
#define MODERN_LEVELDB_TABLE_BLOCK_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "cache/sharded_lru_cache.h"
#include "modern_leveldb/block_cache.h"
#include "table/block.h"

namespace modern_leveldb {

// The public cache is opaque; typed block operations stay inside the table layer.
class BlockCache final {
public:
    using Handle = ShardedLruCache<Block>::Handle;

    explicit BlockCache(std::size_t capacity) : cache_(capacity) {}
    BlockCache(const BlockCache&) = delete;
    BlockCache& operator=(const BlockCache&) = delete;
    BlockCache(BlockCache&&) = delete;
    BlockCache& operator=(BlockCache&&) = delete;
    ~BlockCache() = default;

    [[nodiscard]] Result<Handle> Insert(ByteView key, std::unique_ptr<const Block> value,
                                        std::size_t charge) {
        return cache_.Insert(key, std::move(value), charge);
    }

    [[nodiscard]] std::optional<Handle> Lookup(ByteView key) { return cache_.Lookup(key); }
    [[nodiscard]] std::uint64_t NewId() noexcept { return cache_.NewId(); }
    [[nodiscard]] std::size_t total_charge() const { return cache_.total_charge(); }

private:
    ShardedLruCache<Block> cache_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_TABLE_BLOCK_CACHE_H_
