#ifndef MODERN_LEVELDB_TABLE_BLOCK_H_
#define MODERN_LEVELDB_TABLE_BLOCK_H_

#include <cstddef>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

// An immutable sorted block whose structure and key order were validated when
// it was created, so iteration cannot fail.
class Block final {
 private:
  // The validated structure of the contents. It views the contents buffer,
  // which moving the owning block does not relocate.
  struct Layout {
    ByteView contents;
    const Comparator* comparator;
    // Offset of the restart array, which follows the entries.
    std::size_t entries_end;
    std::size_t restart_count;

    [[nodiscard]] std::size_t RestartPoint(std::size_t index) const noexcept;
    [[nodiscard]] ByteView RestartKey(std::size_t index) const;
  };

 public:
  class Iterator;

  // The comparator must outlive the block.
  [[nodiscard]] static Result<Block> Create(std::vector<std::byte> contents,
                                            const Comparator& comparator);
  static Result<Block> Create(std::vector<std::byte> contents,
                              const Comparator&& comparator) = delete;

  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;
  // Iterators remain valid when their block is moved.
  Block(Block&&) noexcept = default;
  Block& operator=(Block&&) = delete;
  ~Block() = default;

  [[nodiscard]] bool empty() const noexcept { return layout_.entries_end == 0; }
  [[nodiscard]] std::size_t size() const noexcept { return contents_.size(); }

 private:
  Block(std::vector<std::byte> contents, const Comparator& comparator, std::size_t entries_end,
        std::size_t restart_count) noexcept;

  [[nodiscard]] Status Validate() const;

  std::vector<std::byte> contents_;
  Layout layout_;
};

// Reads a block, which must outlive the iterator. A new iterator is not
// positioned. Keys and values remain valid until the iterator moves. If an
// operation throws, the iterator remains consistent at an unspecified position.
class Block::Iterator final {
 public:
  explicit Iterator(const Block& block) noexcept;
  explicit Iterator(Block&&) = delete;
  explicit Iterator(const Block&&) = delete;

  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;
  Iterator(Iterator&&) = delete;
  Iterator& operator=(Iterator&&) = delete;
  ~Iterator() = default;

  [[nodiscard]] bool valid() const noexcept { return current_ < layout_.entries_end; }
  [[nodiscard]] ByteView key() const noexcept;
  [[nodiscard]] ByteView value() const noexcept;

  void SeekToFirst();
  void SeekToLast();
  // Positions at the first key that is not less than the target.
  void Seek(ByteView target);
  void Next();
  void Prev();

 private:
  // Leaves the iterator invalid until the next entry is parsed.
  void SeekToRestartPoint(std::size_t index) noexcept;
  // Parses the entry at next_, which must precede the restart array.
  void ParseEntry();
  // Parses the entry at next_, or invalidates the iterator at the end.
  bool ParseNextEntry();
  void Invalidate() noexcept;

  Layout layout_;
  // Offset of the current entry; the end of the entries when invalid.
  std::size_t current_;
  // Offset just past the current entry.
  std::size_t next_;
  // Index of the restart point at or before the current entry.
  std::size_t restart_index_;
  std::vector<std::byte> key_;
  ByteView value_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_TABLE_BLOCK_H_
