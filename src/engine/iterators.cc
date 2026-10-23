#include "engine/iterators.h"

#include <algorithm>
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
    [[nodiscard]] Status Seek(const ByteView& target) override {
        iterator_.Seek(target);
        return {};
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

    [[nodiscard]] bool valid() const noexcept override { return entries_.has_value(); }
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

    [[nodiscard]] Status Seek(const ByteView& target) override {
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
        return SeekInOpenFile(target, index);
    }

    [[nodiscard]] Status Next() override {
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

    [[nodiscard]] Status SeekInOpenFile(const ByteView& target, std::size_t index) {
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

class IteratorChild final {
public:
    explicit IteratorChild(std::unique_ptr<InternalIterator> iterator) noexcept
        : iterator_(std::move(iterator)) {
        Refresh();
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] ByteView key() const noexcept { return key_; }
    [[nodiscard]] ByteView value() const noexcept { return iterator_->value(); }

    [[nodiscard]] Status SeekToFirst() {
        const Status moved = iterator_->SeekToFirst();
        Refresh();
        return moved;
    }
    [[nodiscard]] Status SeekToLast() {
        const Status moved = iterator_->SeekToLast();
        Refresh();
        return moved;
    }
    [[nodiscard]] Status Seek(const ByteView& target) {
        const Status moved = iterator_->Seek(target);
        Refresh();
        return moved;
    }
    [[nodiscard]] Status Next() {
        const Status moved = iterator_->Next();
        Refresh();
        return moved;
    }
    [[nodiscard]] Status Prev() {
        const Status moved = iterator_->Prev();
        Refresh();
        return moved;
    }

private:
    void Refresh() noexcept {
        valid_ = iterator_->valid();
        if (valid_) {
            key_ = iterator_->key();
        }
    }

    std::unique_ptr<InternalIterator> iterator_;
    bool valid_ = false;
    ByteView key_;
};

class MergingIterator final : public InternalIterator {
public:
    MergingIterator(const InternalKeyComparator& comparator,
                    std::vector<std::unique_ptr<InternalIterator>> children)
        : comparator_(&comparator) {
        children_.reserve(children.size());
        for (std::unique_ptr<InternalIterator>& child : children) {
            children_.emplace_back(std::move(child));
        }
    }

    [[nodiscard]] bool valid() const noexcept override { return current_ != nullptr; }
    [[nodiscard]] ByteView key() const noexcept override { return current_->key(); }
    [[nodiscard]] ByteView value() const noexcept override { return current_->value(); }

    [[nodiscard]] Status SeekToFirst() override {
        current_ = nullptr;
        for (IteratorChild& child : children_) {
            const Status moved = child.SeekToFirst();
            if (!moved.has_value()) {
                return Fail(moved.error());
            }
        }
        forward_ = true;
        FindSmallest();
        return {};
    }

    [[nodiscard]] Status SeekToLast() override {
        current_ = nullptr;
        for (IteratorChild& child : children_) {
            const Status moved = child.SeekToLast();
            if (!moved.has_value()) {
                return Fail(moved.error());
            }
        }
        forward_ = false;
        FindLargest();
        return {};
    }

    [[nodiscard]] Status Seek(const ByteView& target) override {
        current_ = nullptr;
        for (IteratorChild& child : children_) {
            const Status moved = child.Seek(target);
            if (!moved.has_value()) {
                return Fail(moved.error());
            }
        }
        forward_ = true;
        FindSmallest();
        return {};
    }

    [[nodiscard]] Status Next() override {
        IteratorChild* selected = std::exchange(current_, nullptr);
        if (!forward_) {
            Status turned = TurnForward(*selected);
            if (!turned.has_value()) {
                return turned;
            }
        }
        const Status moved = selected->Next();
        if (!moved.has_value()) {
            return Fail(moved.error());
        }
        FindSmallest();
        return {};
    }

    [[nodiscard]] Status Prev() override {
        IteratorChild* selected = std::exchange(current_, nullptr);
        if (forward_) {
            Status turned = TurnBackward(*selected);
            if (!turned.has_value()) {
                return turned;
            }
        }
        const Status moved = selected->Prev();
        if (!moved.has_value()) {
            return Fail(moved.error());
        }
        FindLargest();
        return {};
    }

private:
    Status TurnForward(IteratorChild& selected) {
        for (IteratorChild& child : children_) {
            if (&child == &selected) {
                continue;
            }
            Status positioned = PositionAfterCurrent(child, selected.key());
            if (!positioned.has_value()) {
                return positioned;
            }
        }
        forward_ = true;
        return {};
    }

    Status PositionAfterCurrent(IteratorChild& child, ByteView key) {
        const Status sought = child.Seek(key);
        if (!sought.has_value()) {
            return Fail(sought.error());
        }
        if (child.valid() && comparator_->Compare(key, child.key()) == 0) {
            const Status moved = child.Next();
            if (!moved.has_value()) {
                return Fail(moved.error());
            }
        }
        return {};
    }

    Status TurnBackward(IteratorChild& selected) {
        for (IteratorChild& child : children_) {
            if (&child == &selected) {
                continue;
            }
            Status positioned = PositionBeforeCurrent(child, selected.key());
            if (!positioned.has_value()) {
                return positioned;
            }
        }
        forward_ = false;
        return {};
    }

    Status PositionBeforeCurrent(IteratorChild& child, ByteView key) {
        const Status sought = child.Seek(key);
        if (!sought.has_value()) {
            return Fail(sought.error());
        }
        Status moved;
        if (child.valid()) {
            moved = child.Prev();
        } else {
            moved = child.SeekToLast();
        }
        if (!moved.has_value()) {
            return Fail(moved.error());
        }
        return {};
    }

    void FindSmallest() noexcept {
        IteratorChild* smallest = nullptr;
        for (IteratorChild& child : children_) {
            if (child.valid() &&
                (smallest == nullptr || comparator_->Compare(child.key(), smallest->key()) < 0)) {
                smallest = &child;
            }
        }
        current_ = smallest;
    }

    // Scans from the last child, so that it wins among equal keys.
    void FindLargest() noexcept {
        IteratorChild* largest = nullptr;
        for (auto child = children_.rbegin(); child != children_.rend(); ++child) {
            if (child->valid() &&
                (largest == nullptr || comparator_->Compare(child->key(), largest->key()) > 0)) {
                largest = &*child;
            }
        }
        current_ = largest;
    }

    [[nodiscard]] Status Fail(Error error) noexcept {
        current_ = nullptr;
        return std::unexpected(std::move(error));
    }

    const InternalKeyComparator* comparator_;
    // The fixed child list keeps the selected wrapper's address stable.
    std::vector<IteratorChild> children_;
    // A null selection hides cached views while a fallible move is in progress.
    IteratorChild* current_ = nullptr;
    bool forward_ = true;
};

}  // namespace

std::unique_ptr<InternalIterator> NewMemTableIterator(std::shared_ptr<const MemTable> memtable) {
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

void AppendLevelIterators(std::shared_ptr<const Version> version, std::uint32_t level,
                          std::span<const Version::File> files, TableCache& table_cache,
                          const InternalKeyComparator& comparator, const TableReadOptions& options,
                          std::vector<std::unique_ptr<InternalIterator>>& children) {
    if (level == 0) {
        for (std::size_t index = 0; index < files.size(); ++index) {
            children.push_back(NewLevelIterator(version, files.subspan(index, 1), table_cache,
                                                comparator, options));
        }
    } else if (!files.empty()) {
        children.push_back(
            NewLevelIterator(std::move(version), files, table_cache, comparator, options));
    }
}

std::unique_ptr<InternalIterator> NewMergingIterator(
    const InternalKeyComparator& comparator,
    std::vector<std::unique_ptr<InternalIterator>> children) {
    if (children.size() == 1) {
        return std::move(children.front());
    }
    return std::make_unique<MergingIterator>(comparator, std::move(children));
}

std::unique_ptr<InternalIterator> NewInternalIterator(std::shared_ptr<const MemTable> memtable,
                                                      std::shared_ptr<const MemTable> immutable,
                                                      std::shared_ptr<const Version> version,
                                                      TableCache& table_cache,
                                                      const InternalKeyComparator& comparator,
                                                      const TableReadOptions& options) {
    std::vector<std::unique_ptr<InternalIterator>> children;
    children.push_back(NewMemTableIterator(std::move(memtable)));
    if (immutable != nullptr) {
        children.push_back(NewMemTableIterator(std::move(immutable)));
    }
    for (std::uint32_t level = 0; level < NumLevels; ++level) {
        AppendLevelIterators(version, level, version->files(level), table_cache, comparator,
                             options, children);
    }
    return NewMergingIterator(comparator, std::move(children));
}

}  // namespace modern_leveldb
