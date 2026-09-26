#include <gtest/gtest.h>

#include <type_traits>
#include <utility>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/db.h"

namespace modern_leveldb {
namespace {

static_assert(!std::is_copy_constructible_v<Database>);
static_assert(!std::is_copy_assignable_v<Database>);
static_assert(std::is_nothrow_move_constructible_v<Database>);
static_assert(std::is_nothrow_move_assignable_v<Database>);
static_assert(!std::is_copy_constructible_v<Iterator>);
static_assert(!std::is_copy_assignable_v<Iterator>);
static_assert(std::is_nothrow_move_constructible_v<Iterator>);
static_assert(std::is_nothrow_move_assignable_v<Iterator>);
static_assert(!std::is_copy_constructible_v<Snapshot>);
static_assert(!std::is_copy_assignable_v<Snapshot>);
static_assert(std::is_nothrow_move_constructible_v<Snapshot>);
static_assert(std::is_nothrow_move_assignable_v<Snapshot>);
static_assert(std::is_copy_constructible_v<WriteBatch>);
static_assert(std::is_copy_assignable_v<WriteBatch>);
static_assert(std::is_nothrow_move_constructible_v<WriteBatch>);
static_assert(std::is_nothrow_move_assignable_v<WriteBatch>);
static_assert(static_cast<int>(Compression::None) == 0);
static_assert(static_cast<int>(Compression::Snappy) == 1);
static_assert(static_cast<int>(Compression::Zstd) == 2);

TEST(PublicOptionsTest, DefaultsToSnappyCompression) {
  const Options options;
  EXPECT_EQ(options.compression, Compression::Snappy);
  EXPECT_EQ(options.zstd_compression_level, 1);
}

TEST(PublicWriteBatchTest, OwnsCopiesAppendsAndClearsOperations) {
  WriteBatch batch;
  EXPECT_EQ(batch.ApproximateSize(), 12U);
  ASSERT_TRUE(batch.Put(AsBytes("a"), AsBytes("1")).has_value());
  ASSERT_TRUE(batch.Delete(AsBytes("b")).has_value());
  const std::size_t original_size = batch.ApproximateSize();
  EXPECT_GT(original_size, 12U);

  WriteBatch copy = batch;
  ASSERT_TRUE(copy.Append(batch).has_value());
  EXPECT_EQ(copy.ApproximateSize(), original_size * 2U - 12U);
  EXPECT_EQ(batch.ApproximateSize(), original_size);

  copy.Clear();
  EXPECT_EQ(copy.ApproximateSize(), 12U);
}

TEST(PublicWriteBatchTest, SupportsCopyMoveAndMovedFromStates) {
  WriteBatch original;
  ASSERT_TRUE(original.Put(AsBytes("a"), AsBytes("1")).has_value());

  WriteBatch copied;
  copied = original;
  const WriteBatch* same = &copied;
  copied = *same;
  EXPECT_EQ(copied.ApproximateSize(), original.ApproximateSize());

  WriteBatch moved = std::move(original);
  EXPECT_EQ(original.ApproximateSize(), 0U);
  EXPECT_FALSE(original.Put(AsBytes("b"), AsBytes("2")).has_value());
  EXPECT_FALSE(original.Delete(AsBytes("b")).has_value());
  EXPECT_FALSE(original.Append(moved).has_value());
  EXPECT_FALSE(moved.Append(original).has_value());
  original.Clear();

  WriteBatch copied_moved_from = original;
  EXPECT_EQ(copied_moved_from.ApproximateSize(), 0U);
  copied = original;
  EXPECT_EQ(copied.ApproximateSize(), 0U);

  WriteBatch assigned;
  assigned = std::move(moved);
  EXPECT_GT(assigned.ApproximateSize(), 12U);
}

}  // namespace
}  // namespace modern_leveldb
