#ifndef MODERN_LEVELDB_DB_H_
#define MODERN_LEVELDB_DB_H_

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "modern_leveldb/iterator.h"
#include "modern_leveldb/options.h"
#include "modern_leveldb/snapshot.h"
#include "modern_leveldb/write_batch.h"

namespace modern_leveldb {

namespace detail {
class DatabaseState;
}

class Database final {
 public:
  [[nodiscard]] static Result<Database> Open(Options options, std::filesystem::path directory);

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  Database(Database&& source) noexcept;
  Database& operator=(Database&& source) noexcept;
  ~Database();

  [[nodiscard]] Status Put(ByteView key, ByteView value, const WriteOptions& options = {});
  [[nodiscard]] Status Delete(ByteView key, const WriteOptions& options = {});
  // Copies the batch, so the caller may share or inspect it during the call.
  [[nodiscard]] Status Write(const WriteBatch& batch, const WriteOptions& options = {});
  // Exclusively borrows the batch until this call returns. No thread may read,
  // copy, mutate, or submit it concurrently; its public contents are unchanged.
  [[nodiscard]] Status WriteExclusive(WriteBatch& batch, const WriteOptions& options = {});
  [[nodiscard]] Result<bool> Get(ByteView key, std::vector<std::byte>& value,
                                 const ReadOptions& options = {});
  [[nodiscard]] Result<std::optional<std::vector<std::byte>>> Get(ByteView key,
                                                                  const ReadOptions& options = {});
  [[nodiscard]] Result<Iterator> NewIterator(const ReadOptions& options = {});
  [[nodiscard]] Result<Snapshot> GetSnapshot();

 private:
  explicit Database(std::shared_ptr<detail::DatabaseState> state) noexcept;

  std::shared_ptr<detail::DatabaseState> state_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_DB_H_
