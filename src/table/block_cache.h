#ifndef MODERN_LEVELDB_TABLE_BLOCK_CACHE_H_
#define MODERN_LEVELDB_TABLE_BLOCK_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

#include "cache/sharded_lru_cache.h"
#include "modern_leveldb/block_cache.h"
#include "table/block.h"
#include "table/clock_block_cache.h"

namespace modern_leveldb {

// The public cache is opaque; typed block operations stay inside the table layer.
class BlockCache final {
    using Lru = ShardedLruCache<Block>;
    // Monostate is construction-only; the selected backend never changes.
    using Backends = std::variant<std::monostate, Lru, ClockBlockCache>;

public:
    class Handle final {
    public:
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
        Handle(Handle&&) noexcept = default;
        Handle& operator=(Handle&&) noexcept = default;
        ~Handle() = default;

        [[nodiscard]] const Block& value() const noexcept {
            return std::visit([](const auto& pin) -> const Block& { return pin.value(); }, pin_);
        }
        [[nodiscard]] const Block* operator->() const noexcept { return &value(); }
        [[nodiscard]] const Block& operator*() const noexcept { return value(); }

    private:
        friend class BlockCache;
        explicit Handle(Lru::Handle pin) noexcept
            : pin_(std::in_place_type<Lru::Handle>, std::move(pin)) {}
        explicit Handle(ClockBlockCache::Handle pin) noexcept
            : pin_(std::in_place_type<ClockBlockCache::Handle>, std::move(pin)) {}

        std::variant<Lru::Handle, ClockBlockCache::Handle> pin_;
    };

    explicit BlockCache(std::size_t capacity) : BlockCache(capacity, BlockCachePolicy::Automatic) {}
    // Policy is validated by the public factory or established by an internal caller.
    BlockCache(std::size_t capacity, BlockCachePolicy policy) {
        if (policy == BlockCachePolicy::Clock) {
            backends_.emplace<ClockBlockCache>(
                capacity, clock_cache_detail::OwnerFootprint{sizeof(BlockCache)});
        } else {
            backends_.emplace<Lru>(capacity);
        }
    }
    BlockCache(const BlockCache&) = delete;
    BlockCache& operator=(const BlockCache&) = delete;
    BlockCache(BlockCache&&) = delete;
    BlockCache& operator=(BlockCache&&) = delete;
    ~BlockCache() = default;

    [[nodiscard]] Result<Handle> Insert(const ByteView& key, std::unique_ptr<const Block> value,
                                        std::size_t charge) {
        if (auto* lru = std::get_if<Lru>(&backends_)) {
            return lru->Insert(key, std::move(value), charge).transform([](Lru::Handle pin) {
                return Handle(std::move(pin));
            });
        }
        return std::get<ClockBlockCache>(backends_)
            .Insert(ClockKey(key), std::move(value), charge)
            .transform([](ClockBlockCache::Handle pin) { return Handle(std::move(pin)); });
    }

    [[nodiscard]] std::optional<Handle> Lookup(const ByteView& key) {
        if (auto* lru = std::get_if<Lru>(&backends_)) {
            auto found = lru->Lookup(key);
            if (!found.has_value()) {
                return std::nullopt;
            }
            return Handle(std::move(*found));
        }
        auto found = std::get<ClockBlockCache>(backends_).Lookup(ClockKey(key));
        if (!found.has_value()) {
            return std::nullopt;
        }
        return Handle(std::move(*found));
    }
    [[nodiscard]] std::uint64_t NewId() noexcept {
        if (auto* lru = std::get_if<Lru>(&backends_)) {
            return lru->NewId();
        }
        return std::get<ClockBlockCache>(backends_).NewId();
    }
    [[nodiscard]] std::size_t total_charge() const {
        if (const auto* lru = std::get_if<Lru>(&backends_)) {
            return lru->total_charge();
        }
        return std::get<ClockBlockCache>(backends_).total_charge();
    }
    [[nodiscard]] std::optional<ClockCacheStats> GetClockStats() const {
        if (const auto* clock = std::get_if<ClockBlockCache>(&backends_)) {
            return clock->GetStats();
        }
        return std::nullopt;
    }

private:
    // Table's fixed identity/offset key establishes this shape for CLOCK.
    [[nodiscard]] static ClockBlockCache::Key ClockKey(const ByteView& key) noexcept {
        return ClockBlockCache::Key(key.data(), ClockBlockCache::Key::extent);
    }

    Backends backends_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_TABLE_BLOCK_CACHE_H_
