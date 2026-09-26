#ifndef MODERN_LEVELDB_ITERATOR_H_
#define MODERN_LEVELDB_ITERATOR_H_

#include <memory>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

class Database;

class Iterator final {
 public:
  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;
  Iterator(Iterator&& source) noexcept;
  Iterator& operator=(Iterator&& source) noexcept;
  ~Iterator();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] ByteView key() const noexcept;
  [[nodiscard]] ByteView value() const noexcept;

  [[nodiscard]] Status SeekToFirst();
  [[nodiscard]] Status SeekToLast();
  [[nodiscard]] Status Seek(ByteView key);
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
