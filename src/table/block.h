#ifndef MODERN_LEVELDB_TABLE_BLOCK_H_
#define MODERN_LEVELDB_TABLE_BLOCK_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"
#include "table/block_format.h"

namespace modern_leveldb {

enum class BlockKeyFormat {
    Arbitrary,
    Internal,
};

// An immutable encoded block. Construction validates only the restart-array
// region; iterators validate entries lazily as they reach them.
// Entries store shared-prefix length, key-suffix length, value length, suffix,
// and value. Restart entries store full keys; the trailing fixed32 offsets/count
// permit bounded reconstruction. See docs/learning/05-tables-filters-and-caches.md.
class Block final {
private:
    struct Layout {
        ByteView contents;
        std::size_t entries_end;
        std::size_t restart_count;

        [[nodiscard]] std::size_t RestartPoint(std::size_t index) const noexcept;
    };

    struct DecodedEntry {
        std::uint32_t shared;
        ByteView key_delta;
        ByteView value;
        std::size_t header_size;
    };

public:
    class Iterator;
    using EntryVisitor = std::function<Status(ByteView key, ByteView value)>;

    [[nodiscard]] static Result<Block> Create(std::vector<std::byte> contents);
    [[nodiscard]] static Result<Block> Create(BlockContents contents);

    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    // Iterators remain valid when their block is moved.
    Block(Block&&) noexcept = default;
    Block& operator=(Block&&) = delete;
    ~Block() = default;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return contents_.data().size(); }
    [[nodiscard]] bool cacheable() const noexcept { return contents_.cacheable(); }

    // Table-open validation for index/metaindex blocks. It walks physical entries
    // from byte zero and validates complete restart topology without comparing
    // key order.
    [[nodiscard]] Status ValidateEntries(const EntryVisitor& visitor) const;

private:
    Block(BlockContents contents, std::size_t entries_end, std::size_t restart_count) noexcept;
    [[nodiscard]] static const std::byte* DecodeEntry(const std::byte* input,
                                                      const std::byte* limit, std::uint32_t& shared,
                                                      std::uint32_t& non_shared,
                                                      std::uint32_t& value_size) noexcept;
    [[nodiscard]] static std::optional<DecodedEntry> ReadEntry(
        const Layout& layout, std::size_t offset, std::size_t previous_key_size) noexcept;
    static void ReconstructKey(std::string& key, const DecodedEntry& entry);
    [[nodiscard]] static Status ValidateRestartEntry(const Layout& layout, std::size_t offset,
                                                     std::uint32_t shared,
                                                     std::size_t& restart_index);

    BlockContents contents_;
    Layout layout_;
};

// Requires sorted keys under the borrowed comparator. Backing block storage and
// comparator must outlive the iterator, including after an owning block is moved.
// A new iterator is not positioned. Keys and values remain valid until it moves.
// Complete keys borrow block bytes; prefix-compressed keys use reusable storage.
// Reverse traversal lazily caches a restart interval and borrows its decoded keys.
// Positioning calls recover from an earlier corruption and start over.
class Block::Iterator final {
public:
    Iterator(const Block& block, const Comparator& comparator,
             BlockKeyFormat format = BlockKeyFormat::Arbitrary) noexcept;
    Iterator(const Block& block, const Comparator&& comparator,
             BlockKeyFormat format = BlockKeyFormat::Arbitrary) = delete;
    Iterator(Block&& block, const Comparator& comparator,
             BlockKeyFormat format = BlockKeyFormat::Arbitrary) = delete;
    Iterator(const Block&& block, const Comparator& comparator,
             BlockKeyFormat format = BlockKeyFormat::Arbitrary) = delete;

    Iterator(const Iterator&) = delete;
    Iterator& operator=(const Iterator&) = delete;
    Iterator(Iterator&&) = delete;
    Iterator& operator=(Iterator&&) = delete;
    ~Iterator();

    [[nodiscard]] bool valid() const noexcept { return current_ < layout_.entries_end; }
    [[nodiscard]] ByteView key() const noexcept;
    [[nodiscard]] ByteView value() const noexcept;

    [[nodiscard]] Status SeekToFirst();
    [[nodiscard]] Status SeekToLast();
    [[nodiscard]] Status Seek(ByteView target);
    [[nodiscard]] Status Next();
    [[nodiscard]] Status Prev();

private:
    struct ReverseCache;

    void BeginPositioning() noexcept;
    [[nodiscard]] bool FindRestart(ByteView target, std::size_t left, std::size_t right,
                                   std::size_t& index) noexcept;
    [[nodiscard]] bool RestoreCheckpoint(std::size_t index, int current_key_compare) noexcept;
    void ScanToLowerBound(ByteView target);
    [[nodiscard]] bool FindPreviousRestart(std::size_t original) noexcept;
    void ScanToPreviousEntry(std::size_t original);
    void ScanToLastEntry();
    [[nodiscard]] bool RestartPoint(std::size_t index, std::size_t& offset) const noexcept;
    [[nodiscard]] bool RestartKey(std::size_t index, ByteView& key) noexcept;
    [[nodiscard]] bool SeekToRestartPoint(std::size_t index) noexcept;
    // False means end or corruption; CurrentStatus distinguishes them at the boundary.
    bool ParseNextEntry();
    [[nodiscard]] bool CommitEntry(const std::byte* key, std::uint32_t shared,
                                   std::uint32_t non_shared, std::uint32_t value_size);
    [[nodiscard]] bool AdvanceRestartIndex() noexcept;
    bool Corruption() noexcept;
    [[nodiscard]] Status CurrentStatus() const;
    [[nodiscard]] bool AcceptsKeySize(std::size_t size) const noexcept;
    [[nodiscard]] int Compare(ByteView left, ByteView right) const noexcept;
    void Invalidate() noexcept;

    Layout layout_;
    const Comparator* comparator_;
    BlockKeyFormat format_;
    std::size_t current_;
    std::size_t next_;
    std::size_t restart_index_;
    std::string key_;
    // A non-null pointer selects borrowed bytes, including a complete empty key.
    ByteView borrowed_key_;
    ByteView value_;
    std::unique_ptr<ReverseCache> reverse_cache_;
    // Only uninterrupted cached Prev calls share one checked replay context.
    bool reverse_cache_active_ = false;
    // Corruption invalidates the position before setting this error-only latch.
    bool corrupted_ = false;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_TABLE_BLOCK_H_
