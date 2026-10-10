#ifndef MODERN_LEVELDB_TABLE_TABLE_H_
#define MODERN_LEVELDB_TABLE_TABLE_H_

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "cache/sharded_lru_cache.h"
#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "table/block.h"
#include "table/block_format.h"
#include "table/bloom_filter.h"
#include "table/filter_block.h"

namespace modern_leveldb {

using BlockCache = ShardedLruCache<Block>;

struct TableOptions {
    // Used only if the table was built with the same policy. A policy requires a
    // user comparator that considers keys equal only when their bytes are equal.
    std::optional<BloomFilterPolicy> filter_policy;
    // Not owned; must outlive every table that uses it.
    BlockCache* block_cache = nullptr;
    // Production tables contain internal keys. Direct format tests may select
    // arbitrary bytewise keys explicitly.
    BlockKeyFormat block_key_format = BlockKeyFormat::Internal;
};

struct TableReadOptions {
    // Whether blocks read from the file enter the block cache.
    bool fill_cache = true;
};

enum class TableLookupKind {
    Missing,
    Value,
    Deletion,
};

// An immutable SSTable of internal keys. Its const members are safe for
// concurrent calls.
// Open checks the footer and index topology. Metaindex/filter validation requires
// a configured filter policy; data entries are checked only when read. The owned
// file also retains any borrowed mapped block bytes.
class Table final {
private:
    class BlockReference;

public:
    class Iterator;

    // The comparator and optional block cache must outlive the table. External
    // mutation/truncation of its backing file is outside the immutable-file contract.
    [[nodiscard]] static Result<std::unique_ptr<Table>> Open(
        std::unique_ptr<RandomAccessFile> file, std::uint64_t file_size,
        const InternalKeyComparator& comparator, const TableOptions& options);
    static Result<std::unique_ptr<Table>> Open(std::unique_ptr<RandomAccessFile> file,
                                               std::uint64_t file_size,
                                               const InternalKeyComparator&& comparator,
                                               const TableOptions& options) = delete;

    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;
    Table(Table&&) = delete;
    Table& operator=(Table&&) = delete;
    ~Table() = default;

    // Returns the first entry not less than the lookup key if it has the
    // lookup's user key: the newest version visible at the lookup's sequence.
    [[nodiscard]] Result<TableLookupKind> Get(const LookupKey& key, std::vector<std::byte>& value,
                                              const TableReadOptions& options = {}) const;

private:
    Table(std::unique_ptr<RandomAccessFile> file, std::uint64_t blocks_end,
          const InternalKeyComparator& comparator, BlockKeyFormat block_key_format, Block index,
          std::optional<FilterBlockReader> filter, BlockCache* block_cache,
          std::uint64_t cache_id) noexcept;

    [[nodiscard]] Result<BlockReference> ReadDataBlock(BlockHandle handle,
                                                       const TableReadOptions& options) const;

    std::unique_ptr<RandomAccessFile> file_;
    // Offset of the footer, which follows every block.
    std::uint64_t blocks_end_;
    const InternalKeyComparator* comparator_;
    const Comparator* block_comparator_;
    BlockKeyFormat block_key_format_;
    Block index_;
    std::optional<FilterBlockReader> filter_;
    BlockCache* block_cache_;
    std::uint64_t cache_id_;
};

// Keeps a data block alive: through its cache handle, or by owning it.
class Table::BlockReference final {
public:
    explicit BlockReference(BlockCache::Handle handle) noexcept : handle_(std::move(handle)) {}
    explicit BlockReference(std::unique_ptr<const Block> block) noexcept
        : owned_(std::move(block)) {}

    [[nodiscard]] const Block& block() const noexcept {
        return handle_.has_value() ? handle_->value() : *owned_;
    }

private:
    std::optional<BlockCache::Handle> handle_;
    std::unique_ptr<const Block> owned_;
};

// Reads a table, which must outlive the iterator. A new iterator is not
// positioned. Keys and values remain valid until the iterator moves. A failed
// positioning call leaves the iterator invalid.
class Table::Iterator final {
public:
    explicit Iterator(const Table& table, const TableReadOptions& options = {}) noexcept;
    explicit Iterator(Table&&, const TableReadOptions& = {}) = delete;
    explicit Iterator(const Table&&, const TableReadOptions& = {}) = delete;

    Iterator(const Iterator&) = delete;
    Iterator& operator=(const Iterator&) = delete;
    Iterator(Iterator&&) = delete;
    Iterator& operator=(Iterator&&) = delete;
    ~Iterator() = default;

    // Every positioning call leaves a data block iterator only at a valid entry.
    [[nodiscard]] bool valid() const noexcept {
        assert(!data_.has_value() || data_->valid());
        return data_.has_value();
    }
    [[nodiscard]] ByteView key() const noexcept;
    [[nodiscard]] ByteView value() const noexcept;

    [[nodiscard]] Status SeekToFirst();
    [[nodiscard]] Status SeekToLast();
    // Positions at the first entry not less than the target.
    [[nodiscard]] Status Seek(ByteView target);
    [[nodiscard]] Status Next();
    [[nodiscard]] Status Prev();

private:
    enum class Edge {
        First,
        Last,
    };

    // Reads the data block at the index position, or leaves the iterator
    // invalid if the index is.
    [[nodiscard]] Status LoadBlock();
    // Loads the block at the index position and positions at one of its edges.
    [[nodiscard]] Status EnterBlock(Edge edge);
    [[nodiscard]] Status Fail(Error error);

    const Table* table_;
    TableReadOptions options_;
    Block::Iterator index_;
    // The current data block outlives its iterator, which is declared after it.
    std::optional<BlockReference> block_;
    std::optional<Block::Iterator> data_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_TABLE_TABLE_H_
