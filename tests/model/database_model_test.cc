#include "support/database_model.h"

#include <gtest/gtest.h>

#include "support/temporary_directory.h"

namespace modern_leveldb {
namespace {

TEST(DatabaseModelTest, MatchesSeededBinarySnapshotTraces) {
  for (const Compression compression :
       {Compression::None, Compression::Snappy, Compression::Zstd}) {
    for (const std::uint64_t seed : {1U, 17U, 301U}) {
      SCOPED_TRACE(testing::Message() << "compression=" << static_cast<int>(compression));
      test_support::TemporaryDirectory directory;
      Options options;
      options.create_if_missing = true;
      options.write_buffer_size = 64 * 1024;
      options.compression = compression;
      options.bloom_bits_per_key = 10;
      test_support::ModernClient database(directory.path(), options);
      const test_support::Model expected = test_support::RunModel(seed, 250, database);
      database.Reopen();
      EXPECT_EQ(database.Scan(false), test_support::ExpectedEntries(expected, false));
    }
  }
}

}  // namespace
}  // namespace modern_leveldb
