#include "engine/iterators.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "metadata/version_edit.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

class MemTableIterator final : public InternalIterator {
 public:
  explicit MemTableIterator(std::shared_ptr<const MemTable> memtable) noexcept
      : memtable_(std::move(memtable)), iterator_(*memtable_) {}

  [[nodiscard]] bool valid() const noexcept override { return iterator_.valid(); }
  [[nodiscard]] ByteView key() const noexcept override { return iterator_.key(); }
  [[nodiscard]] ByteView value() const noexcept override { return iterator_.value(); }

  [[nodiscard]] Status SeekToFirst() override {
    iterator_.SeekToFirst();
    return {};
  }
  [[nodiscard]] Status SeekToLast() override {
    iterator_.SeekToLast();
    return {};
  }
  [[nodiscard]] Status Seek(ByteView target) override {
    Status sought = iterator_.Seek(target);
    if (!sought.has_value()) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 needs a target over 4 GiB
      Invalidate();             // GCOVR_EXCL_LINE: needs a target over 4 GiB
    }
    return sought;
  }
  [[nodiscard]] Status Next() override {
    iterator_.Next();
    return {};
  }
  [[nodiscard]] Status Prev() override {
    iterator_.Prev();
    return {};
  }

 private:
  // GCOVR_EXCL_START: only a failed seek, which needs a target over 4 GiB, reaches this function
  // Leaves the iterator past the last entry.
  void Invalidate() noexcept {
    iterator_.SeekToLast();
    if (iterator_.valid()) {
      iterator_.Next();
    }
  }
  // GCOVR_EXCL_STOP

  std::shared_ptr<const MemTable> memtable_;
  MemTable::Iterator iterator_;
};

// Holds an open table only while it is positioned in it.
class LevelIterator final : public InternalIterator {
 public:
  LevelIterator(std::shared_ptr<const Version> version, std::span<const Version::File> files,
                TableCache& table_cache, const InternalKeyComparator& comparator,
                const TableReadOptions& options) noexcept
      : version_(std::move(version)),
        files_(files),
        table_cache_(&table_cache),
        comparator_(&comparator),
        options_(options) {}

  [[nodiscard]] bool valid() const noexcept override {
    assert(!entries_.has_value() || entries_->valid());
    return entries_.has_value();
  }
  [[nodiscard]] ByteView key() const noexcept override { return entries_->key(); }
  [[nodiscard]] ByteView value() const noexcept override { return entries_->value(); }

  [[nodiscard]] Status SeekToFirst() override {
    if (files_.empty()) {
      Close();
      return {};
    }
    return Enter(0, Edge::First);
  }

  [[nodiscard]] Status SeekToLast() override {
    if (files_.empty()) {
      Close();
      return {};
    }
    return Enter(files_.size() - 1, Edge::Last);
  }

  [[nodiscard]] Status Seek(ByteView target) override {
    // The first file whose largest key is not before the target.
    const auto found = std::ranges::partition_point(files_, [&](const Version::File& file) {
      return comparator_->Compare(file->largest.encoded(), target) < 0;
    });
    if (found == files_.end()) {
      Close();
      return {};
    }
    const auto index = static_cast<std::size_t>(found - files_.begin());
    const Status opened = Open(index);
    if (!opened.has_value()) {
      return opened;
    }
    const Status sought = entries_->Seek(target);
    if (!sought.has_value()) {
      return Fail(sought.error());
    }
    if (entries_->valid()) {
      return {};
    }
    // The seek passed the table's last entry, unless the table has none.
    const Status first = entries_->SeekToFirst();
    if (!first.has_value()) {
      return Fail(first.error());
    }
    if (!entries_->valid()) {
      return Fail(EmptyTable(index));
    }
    return EnterAfter(index);
  }

  [[nodiscard]] Status Next() override {
    assert(valid());
    const Status moved = entries_->Next();
    if (!moved.has_value()) {
      return Fail(moved.error());
    }
    if (entries_->valid()) {
      return {};
    }
    return EnterAfter(file_);
  }

  [[nodiscard]] Status Prev() override {
    assert(valid());
    const Status moved = entries_->Prev();
    if (!moved.has_value()) {
      return Fail(moved.error());
    }
    if (entries_->valid()) {
      return {};
    }
    if (file_ == 0) {
      Close();
      return {};
    }
    return Enter(file_ - 1, Edge::Last);
  }

 private:
  enum class Edge { First, Last };

  [[nodiscard]] Error EmptyTable(std::size_t index) const {
    return Error::Corruption("table file " + std::to_string(files_[index]->number) +
                             " has no entries");
  }

  // Positions the iterator at the first entry of the file after the one at
  // `index`, or past the end.
  [[nodiscard]] Status EnterAfter(std::size_t index) {
    if (index + 1 == files_.size()) {
      Close();
      return {};
    }
    return Enter(index + 1, Edge::First);
  }

  [[nodiscard]] Status Enter(std::size_t index, Edge edge) {
    const Status opened = Open(index);
    if (!opened.has_value()) {
      return opened;
    }
    Status positioned;
    if (edge == Edge::First) {
      positioned = entries_->SeekToFirst();
    } else {
      positioned = entries_->SeekToLast();
    }
    if (!positioned.has_value()) {
      return Fail(positioned.error());
    }
    if (!entries_->valid()) {
      return Fail(EmptyTable(index));
    }
    return {};
  }

  // Opens the table of the file at `index` unless it is open.
  [[nodiscard]] Status Open(std::size_t index) {
    if (entries_.has_value() && file_ == index) {
      return {};
    }
    Close();
    Result<TableCache::Handle> table =
        table_cache_->Find(files_[index]->number, files_[index]->file_size);
    if (!table.has_value()) {
      return std::unexpected(std::move(table).error());
    }
    table_.emplace(std::move(*table));
    entries_.emplace(**table_, options_);
    file_ = index;
    return {};
  }

  [[nodiscard]] Status Fail(Error error) {
    Close();
    return std::unexpected(std::move(error));
  }

  void Close() noexcept {
    entries_.reset();
    table_.reset();
  }

  std::shared_ptr<const Version> version_;
  std::span<const Version::File> files_;
  TableCache* table_cache_;
  const InternalKeyComparator* comparator_;
  TableReadOptions options_;
  // The table is declared before its iterator, which it outlives.
  std::optional<TableCache::Handle> table_;
  std::optional<Table::Iterator> entries_;
  std::size_t file_ = 0;
};

class MergingIterator final : public InternalIterator {
 public:
  MergingIterator(const InternalKeyComparator& comparator,
                  std::vector<std::unique_ptr<InternalIterator>> children) noexcept
      : comparator_(&comparator), children_(std::move(children)) {}

  [[nodiscard]] bool valid() const noexcept override { return current_ != nullptr; }
  [[nodiscard]] ByteView key() const noexcept override { return current_->key(); }
  [[nodiscard]] ByteView value() const noexcept override { return current_->value(); }

  [[nodiscard]] Status SeekToFirst() override {
    for (const std::unique_ptr<InternalIterator>& child : children_) {
      const Status moved = child->SeekToFirst();
      if (!moved.has_value()) {
        return Fail(moved.error());
      }
    }
    forward_ = true;
    FindSmallest();
    return {};
  }

  [[nodiscard]] Status SeekToLast() override {
    for (const std::unique_ptr<InternalIterator>& child : children_) {
      const Status moved = child->SeekToLast();
      if (!moved.has_value()) {
        return Fail(moved.error());
      }
    }
    forward_ = false;
    FindLargest();
    return {};
  }

  [[nodiscard]] Status Seek(ByteView target) override {
    for (const std::unique_ptr<InternalIterator>& child : children_) {
      const Status moved = child->Seek(target);
      if (!moved.has_value()) {
        return Fail(moved.error());
      }
    }
    forward_ = true;
    FindSmallest();
    return {};
  }

  [[nodiscard]] Status Next() override {
    assert(valid());
    if (!forward_) {
      // Position every other child after the current key.
      for (const std::unique_ptr<InternalIterator>& child : children_) {
        if (child.get() == current_) {
          continue;
        }
        const Status sought = child->Seek(key());
        if (!sought.has_value()) {
          return Fail(sought.error());
        }
        if (child->valid() && comparator_->Compare(key(), child->key()) == 0) {
          const Status moved = child->Next();
          if (!moved.has_value()) {
            return Fail(moved.error());
          }
        }
      }
      forward_ = true;
    }
    const Status moved = current_->Next();
    if (!moved.has_value()) {
      return Fail(moved.error());
    }
    FindSmallest();
    return {};
  }

  [[nodiscard]] Status Prev() override {
    assert(valid());
    if (forward_) {
      // Position every other child before the current key.
      for (const std::unique_ptr<InternalIterator>& child : children_) {
        if (child.get() == current_) {
          continue;
        }
        const Status sought = child->Seek(key());
        if (!sought.has_value()) {
          return Fail(sought.error());
        }
        Status moved;
        if (child->valid()) {
          moved = child->Prev();
        } else {
          moved = child->SeekToLast();
        }
        if (!moved.has_value()) {
          return Fail(moved.error());
        }
      }
      forward_ = false;
    }
    const Status moved = current_->Prev();
    if (!moved.has_value()) {
      return Fail(moved.error());
    }
    FindLargest();
    return {};
  }

 private:
  void FindSmallest() noexcept {
    InternalIterator* smallest = nullptr;
    for (const std::unique_ptr<InternalIterator>& child : children_) {
      if (child->valid() &&
          (smallest == nullptr || comparator_->Compare(child->key(), smallest->key()) < 0)) {
        smallest = child.get();
      }
    }
    current_ = smallest;
  }

  // Scans from the last child, so that it wins among equal keys.
  void FindLargest() noexcept {
    InternalIterator* largest = nullptr;
    for (auto child = children_.rbegin(); child != children_.rend(); ++child) {
      if ((*child)->valid() &&
          (largest == nullptr || comparator_->Compare((*child)->key(), largest->key()) > 0)) {
        largest = child->get();
      }
    }
    current_ = largest;
  }

  [[nodiscard]] Status Fail(Error error) noexcept {
    current_ = nullptr;
    return std::unexpected(std::move(error));
  }

  const InternalKeyComparator* comparator_;
  std::vector<std::unique_ptr<InternalIterator>> children_;
  InternalIterator* current_ = nullptr;
  bool forward_ = true;
};

}  // namespace

std::unique_ptr<InternalIterator> NewMemTableIterator(std::shared_ptr<const MemTable> memtable) {
  assert(memtable != nullptr);
  return std::make_unique<MemTableIterator>(std::move(memtable));
}

std::unique_ptr<InternalIterator> NewLevelIterator(std::shared_ptr<const Version> version,
                                                   std::span<const Version::File> files,
                                                   TableCache& table_cache,
                                                   const InternalKeyComparator& comparator,
                                                   const TableReadOptions& options) {
  return std::make_unique<LevelIterator>(std::move(version), files, table_cache, comparator,
                                         options);
}

std::unique_ptr<InternalIterator> NewMergingIterator(
    const InternalKeyComparator& comparator,
    std::vector<std::unique_ptr<InternalIterator>> children) {
  return std::make_unique<MergingIterator>(comparator, std::move(children));
}

std::unique_ptr<InternalIterator> NewInternalIterator(std::shared_ptr<const MemTable> memtable,
                                                      std::shared_ptr<const MemTable> immutable,
                                                      std::shared_ptr<const Version> version,
                                                      TableCache& table_cache,
                                                      const InternalKeyComparator& comparator,
                                                      const TableReadOptions& options) {
  assert(version != nullptr);
  std::vector<std::unique_ptr<InternalIterator>> children;
  children.push_back(NewMemTableIterator(std::move(memtable)));
  if (immutable != nullptr) {
    children.push_back(NewMemTableIterator(std::move(immutable)));
  }
  const std::span<const Version::File> level0 = version->files(0);
  for (std::size_t index = 0; index < level0.size(); ++index) {
    children.push_back(
        NewLevelIterator(version, level0.subspan(index, 1), table_cache, comparator, options));
  }
  for (std::uint32_t level = 1; level < NumLevels; ++level) {
    if (!version->files(level).empty()) {
      children.push_back(
          NewLevelIterator(version, version->files(level), table_cache, comparator, options));
    }
  }
  return NewMergingIterator(comparator, std::move(children));
}

}  // namespace modern_leveldb
