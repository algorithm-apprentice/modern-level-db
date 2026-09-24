#include "engine/seek_statistics.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/compaction_picker.h"
#include "engine/lookup.h"
#include "format/internal_key.h"
#include "metadata/version.h"
#include "metadata/version_edit.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

static_assert(ReadBytesPeriod == std::uint64_t{1} << 20U);

// A file of the size, whose keys the statistics never read.
FileMetadata File(std::uint64_t number, std::string_view smallest, std::string_view largest,
                  std::uint64_t size) {
  return FileMetadata{
      .number = number,
      .file_size = size,
      .smallest = InternalKey::Create(AsBytes(smallest), 100, ValueKind::Value).value(),
      .largest = InternalKey::Create(AsBytes(largest), 1, ValueKind::Value).value()};
}

TEST(ReadSamplingPeriodsTest, DrawsLevelDbsPeriods) {
  // The periods that LevelDB's Random(seed).Uniform(2 << 20) draws.
  const std::function<std::uint64_t()> first = ReadSamplingPeriods(1);
  EXPECT_EQ(first(), 16807U);
  EXPECT_EQ(first(), 1456881U);
  EXPECT_EQ(first(), 1551577U);
  EXPECT_EQ(first(), 1379370U);
  const std::function<std::uint64_t()> other = ReadSamplingPeriods(12345);
  EXPECT_EQ(other(), 1961519U);
  EXPECT_EQ(other(), 22016U);
  EXPECT_EQ(other(), 938176U);
  EXPECT_EQ(other(), 1551224U);
}

class SeekStatisticsTest : public testing::Test {
 protected:
  // Applies an edit that removes and adds the files to the base version.
  std::shared_ptr<const Version> Next(
      const Version& base, std::initializer_list<DeletedFile> removed,
      std::initializer_list<std::pair<std::uint32_t, FileMetadata>> added) const {
    VersionEdit edit;
    for (const DeletedFile& file : removed) {
      EXPECT_TRUE(edit.RemoveFile(file.level, file.number).has_value());
    }
    for (const auto& [level, file] : added) {
      EXPECT_TRUE(edit.AddFile(level, file).has_value());
    }
    VersionBuilder builder(comparator_, base);
    EXPECT_TRUE(builder.Apply(edit).has_value());
    Result<Version> version = builder.Build();
    EXPECT_TRUE(version.has_value());
    return std::make_shared<const Version>(version.has_value() ? std::move(*version) : Version());
  }

  std::shared_ptr<const Version> Make(
      std::initializer_list<std::pair<std::uint32_t, FileMetadata>> files) const {
    return Next(Version(), {}, files);
  }

  static SeekCharge ChargeOf(const std::shared_ptr<const Version>& version, std::uint32_t level,
                             std::size_t index) {
    return SeekCharge{.level = level, .file = version->files(level)[index]};
  }

  // Charges the file `times` times and returns how many charges recorded it.
  int Charge(const std::shared_ptr<const Version>& version,
             const std::shared_ptr<const Version>& current, const SeekCharge& charge, int times) {
    int recorded = 0;
    for (int time = 0; time < times; ++time) {
      recorded += statistics_.Charge(version, current, charge) ? 1 : 0;
    }
    return recorded;
  }

  // "number@level" for the current version's file to compact, or "none".
  std::string Recorded(const std::shared_ptr<const Version>& current) const {
    const std::optional<SeekCompaction> compaction = statistics_.FileToCompact(current);
    return compaction.has_value()
               ? std::to_string(compaction->file->number) + "@" + std::to_string(compaction->level)
               : "none";
  }

  InternalKeyComparator comparator_{BytewiseComparator()};
  SeekStatistics statistics_;
};

TEST_F(SeekStatisticsTest, RecordsAFileOnceItsBudgetRunsOut) {
  // A file of 150 times 16 KiB allows 150 seeks.
  const auto version = Make({{1, File(10, "a", "b", 150 * 16384)}});
  const SeekCharge charge = ChargeOf(version, 1, 0);

  EXPECT_EQ(Charge(version, version, charge, 149), 0);
  EXPECT_EQ(Recorded(version), "none");
  EXPECT_TRUE(statistics_.Charge(version, version, charge));
  EXPECT_EQ(Recorded(version), "10@1");
  const std::optional<SeekCompaction> compaction = statistics_.FileToCompact(version);
  ASSERT_TRUE(compaction.has_value());
  EXPECT_EQ(compaction->file, charge.file);
  // It is recorded once, and stays recorded while its version is current.
  EXPECT_FALSE(statistics_.Charge(version, version, charge));
  statistics_.Retain(*version);
  EXPECT_EQ(Recorded(version), "10@1");
}

TEST_F(SeekStatisticsTest, DividesTheFileSizeInto16KiBSeeks) {
  // One byte less than 150 times 16 KiB allows 149 seeks.
  const auto version = Make({{1, File(10, "a", "b", 150 * 16384 - 1)}});
  EXPECT_EQ(Charge(version, version, ChargeOf(version, 1, 0), 148), 0);
  EXPECT_TRUE(statistics_.Charge(version, version, ChargeOf(version, 1, 0)));
}

TEST_F(SeekStatisticsTest, AllowsEveryFileAHundredSeeks) {
  const auto version =
      Make({{1, File(10, "a", "b", 1000)}, {1, File(11, "c", "d", 101 * 16384 - 1)}});

  EXPECT_EQ(Charge(version, version, ChargeOf(version, 1, 0), 99), 0);
  EXPECT_TRUE(statistics_.Charge(version, version, ChargeOf(version, 1, 0)));
  // Once a file is recorded, another that runs out is not.
  EXPECT_EQ(Charge(version, version, ChargeOf(version, 1, 1), 100), 0);
  EXPECT_EQ(Recorded(version), "10@1");
}

TEST_F(SeekStatisticsTest, RecordsOnlyForTheCurrentVersion) {
  const auto old_version = Make({{1, File(10, "a", "b", 1000)}});
  const auto current = Next(*old_version, {}, {{2, File(20, "a", "z", 1000)}});
  const SeekCharge charge = ChargeOf(old_version, 1, 0);

  // A read that used an older version runs the budget out without recording.
  EXPECT_EQ(Charge(old_version, current, charge, 100), 0);
  EXPECT_EQ(Recorded(current), "none");
  EXPECT_EQ(Recorded(old_version), "none");
  // The current version shares the file, whose next charge records it.
  EXPECT_TRUE(statistics_.Charge(current, current, charge));
  EXPECT_EQ(Recorded(current), "10@1");
  // Another version's file to compact is ignored.
  EXPECT_EQ(Recorded(old_version), "none");
}

TEST_F(SeekStatisticsTest, KeepsABudgetPerMetadataObject) {
  const auto first = Make({{1, File(10, "a", "b", 1000)}});
  // The second version shares file 10's metadata.
  const auto second = Next(*first, {}, {{3, File(20, "a", "z", 1000)}});
  EXPECT_EQ(Charge(first, first, ChargeOf(first, 1, 0), 99), 0);
  EXPECT_TRUE(statistics_.Charge(second, second, ChargeOf(second, 1, 0)));

  // A trivial move adds new metadata for the file, which starts a new budget.
  const auto third = Next(*second, {{.level = 1, .number = 10}}, {{2, File(10, "a", "b", 1000)}});
  statistics_.Retain(*third);
  EXPECT_EQ(Charge(third, third, ChargeOf(third, 2, 0), 99), 0);
  EXPECT_TRUE(statistics_.Charge(third, third, ChargeOf(third, 2, 0)));
  EXPECT_EQ(Recorded(third), "10@2");
}

TEST_F(SeekStatisticsTest, RetainForgetsFilesAndRecordsOfOtherVersions) {
  const auto first = Make({{1, File(10, "a", "b", 1000)}, {1, File(11, "c", "d", 1000)}});
  EXPECT_EQ(Charge(first, first, ChargeOf(first, 1, 0), 99), 0);
  EXPECT_EQ(Charge(first, first, ChargeOf(first, 1, 1), 100), 1);
  EXPECT_EQ(Recorded(first), "11@1");

  // The current version lacks file 10, so its budget is forgotten, and file
  // 11's record belonged to the first version.
  const auto second = Next(*first, {{.level = 1, .number = 10}}, {});
  statistics_.Retain(*second);
  EXPECT_EQ(Recorded(first), "none");
  EXPECT_EQ(Recorded(second), "none");
  // A version that holds file 10's metadata again finds a new budget.
  const auto third = Next(*first, {}, {{2, File(30, "a", "z", 1000)}});
  EXPECT_FALSE(statistics_.Charge(third, third, ChargeOf(third, 1, 0)));
  EXPECT_EQ(Recorded(third), "none");
}

TEST_F(SeekStatisticsTest, RecordsAnExhaustedFileInTheNextVersionThatHoldsIt) {
  const auto first = Make({{1, File(10, "a", "b", 1000)}, {1, File(11, "c", "d", 1000)}});
  EXPECT_EQ(Charge(first, first, ChargeOf(first, 1, 0), 100), 1);
  // File 11 runs out, and keeps being charged, while file 10 is recorded.
  EXPECT_EQ(Charge(first, first, ChargeOf(first, 1, 1), 150), 0);

  const auto second = Next(*first, {}, {{2, File(20, "a", "z", 1000)}});
  statistics_.Retain(*second);
  EXPECT_EQ(Recorded(second), "none");
  EXPECT_TRUE(statistics_.Charge(second, second, ChargeOf(second, 1, 1)));
  EXPECT_EQ(Recorded(second), "11@1");
}

}  // namespace
}  // namespace modern_leveldb
