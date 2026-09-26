#ifndef MODERN_LEVELDB_WRITE_BATCH_H_
#define MODERN_LEVELDB_WRITE_BATCH_H_

#include <cstddef>
#include <memory>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

class Database;

class WriteBatch final {
 public:
  WriteBatch();
  WriteBatch(const WriteBatch& source);
  WriteBatch& operator=(const WriteBatch& source);
  WriteBatch(WriteBatch&& source) noexcept;
  WriteBatch& operator=(WriteBatch&& source) noexcept;
  ~WriteBatch();

  [[nodiscard]] Status Put(ByteView key, ByteView value);
  [[nodiscard]] Status Delete(ByteView key);
  [[nodiscard]] Status Append(const WriteBatch& source);
  void Clear() noexcept;
  [[nodiscard]] std::size_t ApproximateSize() const noexcept;

 private:
  class Impl;

  friend class Database;

  std::unique_ptr<Impl> impl_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_WRITE_BATCH_H_
