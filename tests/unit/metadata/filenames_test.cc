#include "metadata/filenames.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

constexpr std::uint64_t MaxNumber = std::numeric_limits<std::uint64_t>::max();

static_assert(noexcept(ParseFileName(std::string_view{})));

void ExpectParsed(std::string_view name, FileType type, std::uint64_t number) {
  SCOPED_TRACE(name);
  const std::optional<ParsedFileName> parsed = ParseFileName(name);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->type, type);
  EXPECT_EQ(parsed->number, number);
}

TEST(FileNamesTest, GeneratesLevelDbNamesInsideTheDirectory) {
  const std::filesystem::path directory = "db";

  EXPECT_EQ(LogFileName(directory, 5), directory / "000005.log");
  EXPECT_EQ(TableFileName(directory, 200), directory / "000200.ldb");
  EXPECT_EQ(DescriptorFileName(directory, 1), directory / "MANIFEST-000001");
  EXPECT_EQ(TempFileName(directory, 999), directory / "000999.dbtmp");
  EXPECT_EQ(CurrentFileName(directory), directory / "CURRENT");
  EXPECT_EQ(LockFileName(directory), directory / "LOCK");
}

TEST(FileNamesTest, PadsNumbersToAtLeastSixDigits) {
  const std::filesystem::path directory = "db";

  EXPECT_EQ(LogFileName(directory, 1), directory / "000001.log");
  EXPECT_EQ(LogFileName(directory, 999'999), directory / "999999.log");
  EXPECT_EQ(LogFileName(directory, 1'000'000), directory / "1000000.log");
  EXPECT_EQ(LogFileName(directory, MaxNumber), directory / "18446744073709551615.log");
}

TEST(FileNamesTest, ParsesCanonicalNames) {
  ExpectParsed("000100.log", FileType::Log, 100);
  ExpectParsed("000001.ldb", FileType::Table, 1);
  ExpectParsed("MANIFEST-000002", FileType::Descriptor, 2);
  ExpectParsed("000007.dbtmp", FileType::Temp, 7);
  ExpectParsed("CURRENT", FileType::Current, 0);
  ExpectParsed("LOCK", FileType::Lock, 0);
  ExpectParsed("999999.log", FileType::Log, 999'999);
  ExpectParsed("1000000.ldb", FileType::Table, 1'000'000);
  ExpectParsed("18446744073709551615.log", FileType::Log, MaxNumber);
  ExpectParsed("MANIFEST-18446744073709551615", FileType::Descriptor, MaxNumber);
}

TEST(FileNamesTest, RoundTripsEveryGeneratedName) {
  const std::filesystem::path directory = "db";
  constexpr std::array<std::uint64_t, 5> Numbers{1, 42, 999'999, 1'000'000, MaxNumber};

  for (const std::uint64_t number : Numbers) {
    SCOPED_TRACE(number);
    const std::array<std::pair<std::filesystem::path, FileType>, 4> generated{{
        {LogFileName(directory, number), FileType::Log},
        {TableFileName(directory, number), FileType::Table},
        {DescriptorFileName(directory, number), FileType::Descriptor},
        {TempFileName(directory, number), FileType::Temp},
    }};
    for (const auto& [path, type] : generated) {
      SCOPED_TRACE(path.string());
      EXPECT_EQ(path.parent_path(), directory);
      const std::optional<ParsedFileName> parsed = ParseFileName(path.filename().string());
      ASSERT_TRUE(parsed.has_value());
      EXPECT_EQ(parsed->type, type);
      EXPECT_EQ(parsed->number, number);
    }
  }

  ExpectParsed(CurrentFileName(directory).filename().string(), FileType::Current, 0);
  ExpectParsed(LockFileName(directory).filename().string(), FileType::Lock, 0);
}

TEST(FileNamesTest, RejectsLevelDbErrorCases) {
  constexpr std::array<std::string_view, 21> Names{
      "",
      "foo",
      "foo-dx-100.log",
      ".log",
      "manifest",
      "CURREN",
      "CURRENTX",
      "MANIFES",
      "MANIFEST",
      "MANIFEST-",
      "XMANIFEST-3",
      "MANIFEST-3x",
      "LOC",
      "LOCKx",
      "LO",
      "LOGx",
      "18446744073709551616.log",
      "184467440737095516150.log",
      "100",
      "100.",
      "100.lop",
  };

  for (const std::string_view name : Names) {
    EXPECT_FALSE(ParseFileName(name).has_value()) << name;
  }
}

TEST(FileNamesTest, RejectsNamesLevelDbAcceptsButNeverGenerates) {
  constexpr std::array<std::string_view, 16> Names{
      "100.log",
      "0.log",
      "000000.log",
      "0000100.log",
      "01000000.ldb",
      "018446744073709551615.log",
      "MANIFEST-2",
      "MANIFEST-000000",
      "+00100.log",
      " 000100.log",
      "000100.log ",
      "000100.sst",
      "LOG",
      "LOG.old",
      "db/000100.log",
      "000100.log.dbtmp",
  };

  for (const std::string_view name : Names) {
    EXPECT_FALSE(ParseFileName(name).has_value()) << name;
  }
}

TEST(CurrentFileTest, EncodesLevelDbContents) {
  EXPECT_EQ(CurrentFileContents(5), "MANIFEST-000005\n");
  EXPECT_EQ(CurrentFileContents(1'234'567), "MANIFEST-1234567\n");
  EXPECT_EQ(CurrentFileContents(MaxNumber), "MANIFEST-18446744073709551615\n");
}

TEST(CurrentFileTest, ParsesCanonicalContents) {
  constexpr std::array<std::uint64_t, 5> Numbers{1, 5, 999'999, 1'000'000, MaxNumber};

  for (const std::uint64_t number : Numbers) {
    SCOPED_TRACE(number);
    const Result<std::uint64_t> parsed = ParseCurrentFileContents(CurrentFileContents(number));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, number);
  }
}

TEST(CurrentFileTest, RejectsMalformedContents) {
  constexpr std::array<std::string_view, 12> Contents{
      "",
      "\n",
      "MANIFEST-000005",
      "MANIFEST-000005\n\n",
      "MANIFEST-000005\r\n",
      "MANIFEST-5\n",
      "MANIFEST-000000\n",
      " MANIFEST-000005\n",
      "../MANIFEST-000005\n",
      "db/MANIFEST-000005\n",
      "000005.log\n",
      "CURRENT\n",
  };

  for (const std::string_view contents : Contents) {
    SCOPED_TRACE(contents);
    const Result<std::uint64_t> parsed = ParseCurrentFileContents(contents);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code(), ErrorCode::Corruption);
  }
}

}  // namespace
}  // namespace modern_leveldb
