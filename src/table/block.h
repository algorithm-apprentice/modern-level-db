#ifndef MODERN_LEVELDB_TABLE_BLOCK_H_
#define MODERN_LEVELDB_TABLE_BLOCK_H_

#include <cstddef>
#include <functional>
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
class Block final {
 private:
  struct Layout {
    ByteView contents;
    std::size_t entries_end;
    std::size_t restart_count;

    [[nodiscard]] std::size_t RestartPoint(std::size_t index) const noexcept;
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

  BlockContents contents_;
  Layout layout_;
};

// Reads a block, which and whose comparator must outlive the iterator. A new
// iterator is not positioned. Keys and values remain valid until it moves.
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
  ~Iterator() = default;

  [[nodiscard]] bool valid() const noexcept {
    return !error_.has_value() && current_ < layout_.entries_end;
  }
  [[nodiscard]] ByteView key() const noexcept;
  [[nodiscard]] ByteView value() const noexcept;

  [[nodiscard]] Status SeekToFirst();
  [[nodiscard]] Status SeekToLast();
  [[nodiscard]] Status Seek(ByteView target);
  [[nodiscard]] Status Next();
  [[nodiscard]] Status Prev();

 private:
  void BeginPositioning() noexcept;
  [[nodiscard]] bool RestartPoint(std::size_t index, std::size_t& offset) const noexcept;
  [[nodiscard]] Status RestartKey(std::size_t index, ByteView& key);
  [[nodiscard]] Status SeekToRestartPoint(std::size_t index);
  [[nodiscard]] Status ParseNextEntry();
  [[nodiscard]] Status Corruption();
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
  ByteView value_;
  std::optional<Error> error_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_TABLE_BLOCK_H_
