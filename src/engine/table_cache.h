#ifndef MODERN_LEVELDB_ENGINE_TABLE_CACHE_H_
#define MODERN_LEVELDB_ENGINE_TABLE_CACHE_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>

#include "cache/sharded_lru_cache.h"
#include "format/internal_key.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "table/table.h"

namespace modern_leveldb {

namespace table_cache_detail {

// Capacity is the shared LRU shard ceiling. Counts close permanently at
// capacity-plus-one; terminal generations are published only after closure.
struct ReuseShard {
    std::atomic<std::size_t> opens{0};
    std::atomic<std::uint64_t> generation{0};

    [[nodiscard]] bool enabled(std::size_t capacity) const noexcept;
    void RecordOpen(std::size_t capacity) noexcept;
    void Invalidate(std::size_t capacity) noexcept;
    // Preparation closes an exhausted gate before any terminal publication.
    // Complete takes an observation returned by Begin for this shard/capacity.
    [[nodiscard]] std::uint64_t BeginInvalidation(std::size_t capacity) noexcept;
    void CompleteInvalidation(std::uint64_t observed, std::size_t capacity) noexcept;
};

}  // namespace table_cache_detail

// Keeps the tables of a database directory open for reuse, by file number.
// The file system, the comparator, and the options' block cache must outlive
// the table cache, and every handle must be released before it is destroyed.
// Find, Get and Evict are safe for concurrent calls.
class TableCache final {
    using TableOwner = std::shared_ptr<const Table>;
    using UniqueTables = ShardedLruCache<Table>;
    using SharedCache = ShardedLruCache<TableOwner>;

    struct SharedTables {
        explicit SharedTables(std::size_t capacity)
            : shard_capacity(ShardedLruCacheCore::ShardCapacity(capacity)),
              identity(std::make_shared<const std::byte>(std::byte{0})),
              tables(capacity) {}

        const std::size_t shard_capacity;
        std::shared_ptr<const std::byte> identity;
        std::array<table_cache_detail::ReuseShard, ShardedLruCacheCore::ShardCount> reuse;
        SharedCache tables;
    };

    // Monostate exists only while this cache's selected backend is constructed.
    using Tables = std::variant<std::monostate, UniqueTables, SharedTables>;

public:
    // Retains the original cache mapping pin, including for iterators/maintenance.
    class Handle final {
    public:
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
        Handle(Handle&&) noexcept = default;
        Handle& operator=(Handle&&) noexcept = default;
        ~Handle() = default;

        [[nodiscard]] const Table& value() const noexcept {
            return std::visit(
                [](const auto& pin) -> const Table& {
                    if constexpr (std::is_same_v<std::remove_cvref_t<decltype(pin)>,
                                                 UniqueTables::Handle>) {
                        return pin.value();
                    } else {
                        return *pin.value();
                    }
                },
                pin_);
        }
        [[nodiscard]] const Table* operator->() const noexcept { return &value(); }
        [[nodiscard]] const Table& operator*() const noexcept { return value(); }

    private:
        friend class TableCache;
        explicit Handle(UniqueTables::Handle pin) noexcept
            : pin_(std::in_place_type<UniqueTables::Handle>, std::move(pin)) {}
        explicit Handle(SharedCache::Handle pin) noexcept
            : pin_(std::in_place_type<SharedCache::Handle>, std::move(pin)) {}
        // Called only for a lease produced by this cache's immutable Shared mode.
        [[nodiscard]] const TableOwner& shared_owner() const noexcept {
            return std::get<SharedCache::Handle>(pin_).value();
        }

        std::variant<UniqueTables::Handle, SharedCache::Handle> pin_;
    };

    // Each of the cache's 16 shards has a capacity of ceil(capacity / 16)
    // tables, which it enforces only when it caches a table, by closing its
    // least recently used tables that no handle holds. The capacity is
    // therefore a target, not a limit on open files.
    TableCache(FileSystem& file_system, std::filesystem::path directory,
               const InternalKeyComparator& comparator, const TableOptions& options,
               std::size_t capacity, bool reuse_readers = false);
    TableCache(FileSystem& file_system, std::filesystem::path directory,
               const InternalKeyComparator&& comparator, const TableOptions& options,
               std::size_t capacity, bool reuse_readers = false) = delete;

    TableCache(const TableCache&) = delete;
    TableCache& operator=(const TableCache&) = delete;
    TableCache(TableCache&&) = delete;
    TableCache& operator=(TableCache&&) = delete;
    ~TableCache() = default;

    // Returns the table of the nonzero file number, opening the table file of
    // that number with the given size unless the table is cached; a cached table
    // ignores the size. Errors are returned unchanged and are not cached.
    [[nodiscard]] Result<Handle> Find(std::uint64_t file_number, std::uint64_t file_size);

    // Reuses only weak per-thread hints when the shard has never exceeded its
    // open-event ceiling. Every fallback keeps its mapping pin through Table Get.
    [[nodiscard]] Result<TableLookupKind> Get(std::uint64_t file_number, std::uint64_t file_size,
                                              const LookupKey& key, std::vector<std::byte>& value,
                                              const TableReadOptions& options = {}) {
        if (auto* unique = std::get_if<UniqueTables>(&tables_)) {
            auto found = FindUnique(*unique, file_number, file_size);
            if (!found.has_value()) {
                return std::unexpected(std::move(found).error());
            }
            return (*found)->Get(key, value, options);
        }
        return GetShared(std::get<SharedTables>(tables_), file_number, file_size, key, value,
                         options);
    }

    // Stops caching the table of the file number. Handles stay valid, and a Find
    // that is opening the file may cache it again, so before deleting a table
    // file the caller must ensure that no Find for it can run.
    void Evict(std::uint64_t file_number);

private:
    // Both Unique point Get and outward Find retain this original typed pin.
    [[nodiscard]] Result<UniqueTables::Handle> FindUnique(UniqueTables& tables,
                                                          std::uint64_t file_number,
                                                          std::uint64_t file_size);
    [[nodiscard]] Result<TableLookupKind> GetShared(SharedTables& shared, std::uint64_t file_number,
                                                    std::uint64_t file_size, const LookupKey& key,
                                                    std::vector<std::byte>& value,
                                                    const TableReadOptions& options);
    [[nodiscard]] Result<std::unique_ptr<Table>> OpenTable(std::uint64_t file_number,
                                                           std::uint64_t file_size,
                                                           SharedTables* shared);

    FileSystem* file_system_;
    std::filesystem::path directory_;
    const InternalKeyComparator* comparator_;
    TableOptions options_;
    Tables tables_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_TABLE_CACHE_H_
