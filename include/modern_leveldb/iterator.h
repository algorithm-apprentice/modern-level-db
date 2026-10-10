#ifndef MODERN_LEVELDB_ITERATOR_H_
#define MODERN_LEVELDB_ITERATOR_H_

#include <memory>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

class Database;

// Bidirectional view of visible user keys in comparator order at a fixed sequence.
// Retains its engine and read sources. Do not operate on, move, or destroy the
// same iterator concurrently. See docs/learning/06-reads-and-iterators.md.
class Iterator final {
public:
    Iterator(const Iterator&) = delete;
    Iterator& operator=(const Iterator&) = delete;
    Iterator(Iterator&& source) noexcept;
    Iterator& operator=(Iterator&& source) noexcept;
    ~Iterator();

    // Initially false; reaching either end is successful but leaves no position.
    [[nodiscard]] bool valid() const noexcept;
    // Require valid(). Borrowed views expire on movement or destruction; copy
    // them before advancing when values must be retained.
    [[nodiscard]] ByteView key() const noexcept;
    [[nodiscard]] ByteView value() const noexcept;

    // Seeks establish a position; Seek chooses the first user key >= its target,
    // whose length follows the database key representation. A failed move leaves
    // the iterator invalid. A later seek can start over.
    [[nodiscard]] Status SeekToFirst();
    [[nodiscard]] Status SeekToLast();
    [[nodiscard]] Status Seek(ByteView key);
    // Require valid(); these are not seek operations on an unpositioned iterator.
    [[nodiscard]] Status Next();
    [[nodiscard]] Status Prev();

private:
    class Impl;

    explicit Iterator(std::unique_ptr<Impl> impl) noexcept;

    friend class Database;

    std::unique_ptr<Impl> impl_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ITERATOR_H_
