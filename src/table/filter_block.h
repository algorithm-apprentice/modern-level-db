#ifndef MODERN_LEVELDB_TABLE_FILTER_BLOCK_H_
#define MODERN_LEVELDB_TABLE_FILTER_BLOCK_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "table/bloom_filter.h"

namespace modern_leveldb {

// Builds a table's filter block: one filter for each 2 KiB range of data block
// offsets. If a member function throws, which only allocation failure causes,
// the builder must not be used again.
class FilterBlockBuilder final {
 public:
  explicit FilterBlockBuilder(BloomFilterPolicy policy) noexcept : policy_(policy) {}

  FilterBlockBuilder(const FilterBlockBuilder&) = delete;
  FilterBlockBuilder& operator=(const FilterBlockBuilder&) = delete;
  FilterBlockBuilder(FilterBlockBuilder&&) = delete;
  FilterBlockBuilder& operator=(FilterBlockBuilder&&) = delete;
  ~FilterBlockBuilder() = default;

  // Starts the data block at block_offset; offsets must not decrease. Returns
  // InvalidArgument, leaving the builder unchanged, if the finished block
  // would exceed UINT32_MAX bytes.
  [[nodiscard]] Status StartBlock(std::uint64_t block_offset);

  // Adds a key of the current data block. Returns InvalidArgument, leaving the
  // builder unchanged, if the finished block would exceed UINT32_MAX bytes.
  [[nodiscard]] Status AddKey(ByteView key);

  // Returns the filter block, which stays valid until the builder is
  // destroyed. Must be called at most once.
  [[nodiscard]] ByteView Finish();

 private:
  void GenerateFilter();

  BloomFilterPolicy policy_;
  // Flattened keys of the current range and the offset of each key.
  std::vector<std::byte> keys_;
  std::vector<std::size_t> key_starts_;
  // The generated filters, followed by the offset array once finished.
  std::vector<std::byte> result_;
  std::vector<std::uint32_t> filter_offsets_;
  bool finished_ = false;
};

// Answers filter queries from a validated filter block that it owns.
class FilterBlockReader final {
 public:
  [[nodiscard]] static Result<FilterBlockReader> Create(std::vector<std::byte> contents,
                                                        BloomFilterPolicy policy);

  FilterBlockReader(const FilterBlockReader&) = delete;
  FilterBlockReader& operator=(const FilterBlockReader&) = delete;
  FilterBlockReader(FilterBlockReader&&) noexcept = default;
  FilterBlockReader& operator=(FilterBlockReader&&) = delete;
  ~FilterBlockReader() = default;

  // Returns false only if no key of the data block at block_offset can equal
  // key. An offset beyond the last filter may match.
  [[nodiscard]] bool KeyMayMatch(std::uint64_t block_offset, ByteView key) const noexcept;

 private:
  FilterBlockReader(std::vector<std::byte> contents, BloomFilterPolicy policy,
                    std::size_t array_offset, std::size_t filter_count) noexcept;

  // Returns the start of filter index; index filter_count_ yields the array
  // offset, which ends the last filter.
  [[nodiscard]] std::uint32_t FilterOffset(std::size_t index) const noexcept;

  std::vector<std::byte> contents_;
  BloomFilterPolicy policy_;
  std::size_t array_offset_;
  std::size_t filter_count_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_TABLE_FILTER_BLOCK_H_
