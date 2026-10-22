#include "engine/table_cache.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include "metadata/filenames.h"
#include "modern_leveldb/base/coding.h"

namespace modern_leveldb {
namespace table_cache_detail {

bool ReuseShard::enabled(std::size_t capacity) const noexcept {
    return capacity != 0 && opens.load(std::memory_order_acquire) <= capacity;
}

void ReuseShard::RecordOpen(std::size_t capacity) noexcept {
    auto count = opens.load(std::memory_order_relaxed);
    while (count <= capacity &&
           !opens.compare_exchange_weak(count, count + 1, std::memory_order_release,
                                        std::memory_order_relaxed)) {
    }
}

void ReuseShard::Invalidate(std::size_t capacity) noexcept {
    CompleteInvalidation(BeginInvalidation(capacity), capacity);
}

std::uint64_t ReuseShard::BeginInvalidation(std::size_t capacity) noexcept {
    constexpr auto Terminal = std::numeric_limits<std::uint64_t>::max();
    const auto observed = generation.load(std::memory_order_relaxed);
    if (observed >= Terminal - 1) {
        opens.store(capacity + 1, std::memory_order_release);
    }
    return observed;
}

void ReuseShard::CompleteInvalidation(std::uint64_t observed, std::size_t capacity) noexcept {
    constexpr auto Terminal = std::numeric_limits<std::uint64_t>::max();
    for (;;) {
        if (observed == Terminal) {
            return;
        }
        if (generation.compare_exchange_weak(observed, observed + 1, std::memory_order_release,
                                             std::memory_order_relaxed)) {
            return;
        }
        observed = BeginInvalidation(capacity);
    }
}

}  // namespace table_cache_detail

namespace {

struct ReaderHint {
    std::weak_ptr<const std::byte> identity;
    std::uint64_t file_number = 0;
    std::uint64_t generation = 0;
    std::weak_ptr<const Table> table;
};

constexpr std::size_t HintCount = 64;
thread_local std::array<ReaderHint, HintCount> ReaderHints;

std::array<std::byte, sizeof(std::uint64_t)> CacheKey(std::uint64_t file_number) noexcept {
    std::array<std::byte, sizeof(std::uint64_t)> key{};
    EncodeFixed64(key, file_number);
    return key;
}

}  // namespace

TableCache::TableCache(FileSystem& file_system, std::filesystem::path directory,
                       const InternalKeyComparator& comparator, const TableOptions& options,
                       std::size_t capacity, bool reuse_readers)
    : file_system_(&file_system),
      directory_(std::move(directory)),
      comparator_(&comparator),
      options_(options),
      reuse_readers_(reuse_readers),
      shard_capacity_(ShardedLruCacheCore::ShardCapacity(capacity)),
      identity_(std::make_shared<const std::byte>(std::byte{0})),
      tables_(capacity) {}

Result<TableCache::Handle> TableCache::Find(std::uint64_t file_number, std::uint64_t file_size) {
    const auto key = CacheKey(file_number);
    auto cached = tables_.Lookup(key);
    if (cached.has_value()) {
        return Handle(std::move(*cached));
    }

    Result<std::unique_ptr<Table>> table = OpenTable(file_number, file_size);
    if (!table.has_value()) {
        return std::unexpected(std::move(table).error());
    }
    // Tables have charge one, so accounting saturation would require SIZE_MAX
    // tables in one shard.
    TableOwner owner(std::move(*table));
    auto holder = std::make_unique<const TableOwner>(std::move(owner));
    return tables_.Insert(key, std::move(holder), 1).transform([](Tables::Handle pin) {
        return Handle(std::move(pin));
    });
}

Result<TableLookupKind> TableCache::Get(std::uint64_t file_number, std::uint64_t file_size,
                                        const LookupKey& key, std::vector<std::byte>& value,
                                        const TableReadOptions& options) {
    ReaderHint* hint = nullptr;
    table_cache_detail::ReuseShard* shard = nullptr;
    std::uint64_t generation = 0;
    if (reuse_readers_) {
        const auto hash = ShardedLruCacheCore::HashKey(CacheKey(file_number));
        shard = &reuse_[ShardedLruCacheCore::ShardIndex(hash)];
        if (shard->enabled(shard_capacity_)) {
            generation = shard->generation.load(std::memory_order_acquire);
            const auto identity_hash = reinterpret_cast<std::uintptr_t>(identity_.get()) >> 4U;
            hint = &ReaderHints[(hash ^ identity_hash) & (HintCount - 1)];
            if (hint->file_number == file_number && hint->generation == generation &&
                !hint->identity.owner_before(identity_) &&
                !identity_.owner_before(hint->identity)) {
                if (const auto table = hint->table.lock()) {
                    return table->Get(key, value, options);
                }
            }
        }
    }
    auto found = Find(file_number, file_size);
    if (!found.has_value()) {
        return std::unexpected(std::move(found).error());
    }
    if (hint != nullptr && shard->enabled(shard_capacity_) &&
        shard->generation.load(std::memory_order_acquire) == generation) {
        hint->identity = identity_;
        hint->file_number = file_number;
        hint->generation = generation;
        hint->table = found->pin_.value();
    }
    return (*found)->Get(key, value, options);
}

Result<std::unique_ptr<Table>> TableCache::OpenTable(std::uint64_t file_number,
                                                     std::uint64_t file_size) {
    Result<std::unique_ptr<RandomAccessFile>> file =
        file_system_->OpenRandomAccess(TableFileName(directory_, file_number), file_size);
    if (!file.has_value()) {
        return std::unexpected(std::move(file).error());
    }
    Result<std::unique_ptr<Table>> table =
        Table::Open(std::move(*file), file_size, *comparator_, options_);
    if (!table.has_value()) {
        return std::unexpected(std::move(table).error());
    }
    const auto hash = ShardedLruCacheCore::HashKey(CacheKey(file_number));
    reuse_[ShardedLruCacheCore::ShardIndex(hash)].RecordOpen(shard_capacity_);
    return table;
}

void TableCache::Evict(std::uint64_t file_number) {
    const auto key = CacheKey(file_number);
    tables_.Erase(key);
    const auto hash = ShardedLruCacheCore::HashKey(key);
    reuse_[ShardedLruCacheCore::ShardIndex(hash)].Invalidate(shard_capacity_);
}

}  // namespace modern_leveldb
