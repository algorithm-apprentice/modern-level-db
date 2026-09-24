#ifndef MODERN_LEVELDB_TABLE_TABLE_BUILDER_H_
#define MODERN_LEVELDB_TABLE_TABLE_BUILDER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "table/block_builder.h"
#include "table/block_format.h"
#include "table/bloom_filter.h"
#include "table/filter_block.h"

namespace modern_leveldb {

struct TableBuilderOptions {
  // A data block is written once its size estimate reaches this size.
  std::size_t block_size = 4 * 1024;
  // Must be at least one.
  std::uint32_t restart_interval = 16;
  // Without a policy, the table has no filter block.
  std::optional<BloomFilterPolicy> filter_policy;
};

// Writes one SSTable of internal keys, which must strictly increase under the
// comparator. The first error is kept, and later calls return it without
// further writes. Destroying the builder without Finish abandons the table;
// the caller deletes the file. If a call throws, which only allocation failure
// causes, the builder must not be used again.
class TableBuilder final {
 public:
  // The comparator must outlive the builder. A null file makes every call
  // return InvalidArgument.
  TableBuilder(std::unique_ptr<WritableFile> file, const InternalKeyComparator& comparator,
               const TableBuilderOptions& options);
  TableBuilder(std::unique_ptr<WritableFile> file, const InternalKeyComparator&& comparator,
               const TableBuilderOptions& options) = delete;

  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;
  TableBuilder(TableBuilder&&) = delete;
  TableBuilder& operator=(TableBuilder&&) = delete;
  ~TableBuilder() = default;

  [[nodiscard]] Status Add(ByteView internal_key, ByteView value);

  // Writes the remaining blocks and the footer, then syncs and closes the
  // file. After an earlier error it only closes the file. Returns the first
  // error. Every call after Finish returns InvalidArgument.
  [[nodiscard]] Status Finish();

  [[nodiscard]] std::uint64_t entry_count() const noexcept { return entry_count_; }

  // Bytes appended so far: the written data blocks before Finish, and the
  // whole table after it succeeds.
  [[nodiscard]] std::uint64_t file_size() const noexcept { return file_size_; }

 private:
  // Keeps the first error and reports whether status succeeded.
  bool Record(Status status);
  [[nodiscard]] Status FirstError() const;
  void AddPendingIndexEntry();
  void WriteDataBlock();
  void WriteBlock(ByteView contents, BlockHandle& handle);
  void WriteTail();

  std::unique_ptr<WritableFile> file_;
  const InternalKeyComparator* comparator_;
  std::size_t block_size_;
  std::uint32_t restart_interval_;
  BlockBuilder data_block_;
  BlockBuilder index_block_;
  std::optional<FilterBlockBuilder> filter_block_;
  // The metaindex key of the filter block.
  std::string filter_key_;
  std::vector<std::byte> last_key_;
  // The last written data block, whose index entry waits for the next key.
  BlockHandle pending_handle_{};
  bool pending_index_entry_ = false;
  std::uint64_t entry_count_ = 0;
  std::uint64_t file_size_ = 0;
  std::optional<Error> first_error_;
  bool finished_ = false;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_TABLE_TABLE_BUILDER_H_
