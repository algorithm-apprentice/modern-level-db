#ifndef MODERN_LEVELDB_TABLE_CLOCK_BLOCK_CACHE_H_
#define MODERN_LEVELDB_TABLE_CLOCK_BLOCK_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

class Block;

namespace clock_cache_detail {
struct Slot;
struct State;
struct LookupRoot;
}  // namespace clock_cache_detail

struct ClockCacheStats {
    std::size_t metadata_bytes = 0;
    std::size_t metadata_allowance = 0;
    // Maximum old-plus-prepared payload within one shard, not whole-process RSS.
    std::size_t peak_shard_preparation_bytes = 0;
    std::size_t pooled_slots = 0;
    std::size_t indexed_entries = 0;
    std::size_t retained_index_bytes = 0;
    std::size_t standalone_bytes = 0;
    std::size_t peak_standalone_bytes = 0;
    std::size_t erased_bytes = 0;
    std::size_t peak_erased_bytes = 0;
    std::size_t peak_retired_bytes = 0;
    std::size_t retired_bytes = 0;
    std::size_t admission_rejections = 0;
    std::size_t index_refreshes = 0;
};

// A fixed-key optimization cache. Canonical positions and acquired index roots
// remain allocated until destruction; all leases/lookups finish before then.
// Concurrent mutation may cause a miss, never a wrong-key or unpinned hit.
class ClockBlockCache final {
public:
    using Key = std::span<const std::byte, 16>;

    class Handle final {
    public:
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
        Handle(Handle&& source) noexcept;
        Handle& operator=(Handle&& source) noexcept;
        ~Handle();

        [[nodiscard]] const Block& value() const noexcept;
        [[nodiscard]] const Block* operator->() const noexcept { return &value(); }
        [[nodiscard]] const Block& operator*() const noexcept { return value(); }

    private:
        friend class ClockBlockCache;
        friend class ClockBlockCacheTestPeer;
        explicit Handle(clock_cache_detail::Slot& slot) noexcept;
        void Reset() noexcept;

        clock_cache_detail::Slot* slot_ = nullptr;
    };

    explicit ClockBlockCache(std::size_t capacity);
    ClockBlockCache(const ClockBlockCache&) = delete;
    ClockBlockCache& operator=(const ClockBlockCache&) = delete;
    ClockBlockCache(ClockBlockCache&&) = delete;
    ClockBlockCache& operator=(ClockBlockCache&&) = delete;
    ~ClockBlockCache();

    [[nodiscard]] Result<Handle> Insert(Key key, std::unique_ptr<const Block> value,
                                        std::size_t charge);
    [[nodiscard]] std::optional<Handle> Lookup(Key key);
    void Erase(Key key);
    [[nodiscard]] std::size_t total_charge() const;
    [[nodiscard]] ClockCacheStats GetStats() const;
    [[nodiscard]] std::uint64_t NewId() noexcept;

private:
    friend class ClockBlockCacheTestPeer;
    [[nodiscard]] clock_cache_detail::LookupRoot AcquireLookupRoot(Key key) const;
    [[nodiscard]] static std::optional<Handle> LookupInRoot(
        const clock_cache_detail::LookupRoot& root, Key key);

    std::unique_ptr<clock_cache_detail::State> state_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_TABLE_CLOCK_BLOCK_CACHE_H_
