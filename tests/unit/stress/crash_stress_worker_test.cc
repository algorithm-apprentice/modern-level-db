#include "stress/crash_stress_worker.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "modern_leveldb/db.h"
#include "support/temporary_directory.h"

namespace modern_leveldb::stress {
namespace {

void WriteJournal(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create crash journal fixture");
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) {
        throw std::runtime_error("cannot write crash journal fixture");
    }
}

TEST(CrashStressWorkerArgumentsTest, ParsesModesDefaultsAndNativePaths) {
#if defined(_WIN32)
    const std::filesystem::path database = L"db-\u6570\u636e";
    const std::filesystem::path journal = L"journal-\U0001f680";
#else
    const std::filesystem::path database = "db-\xe6\x95\xb0\xe6\x8d\xae";
    const std::filesystem::path journal = "journal-\xf0\x9f\x9a\x80";
#endif
    const std::vector<std::filesystem::path> write{
        "--mode", "write-hold", "--db", database, "--index", "42", "--seed", "17",
        "--compression", "zstd",
    };
    const CrashStressWorkerArguments parsed_write =
        ParseCrashStressWorkerArguments(write);
    EXPECT_EQ(parsed_write.mode, CrashStressWorkerMode::WriteHold);
    EXPECT_EQ(parsed_write.database, database);
    EXPECT_EQ(parsed_write.index, 42U);
    EXPECT_EQ(parsed_write.seed, 17U);
    EXPECT_EQ(parsed_write.compression, Compression::Zstd);

    const std::vector<std::filesystem::path> verify{
        "--mode", "verify-prefix", "--db", database, "--journal", journal, "--seed", "9",
    };
    const CrashStressWorkerArguments parsed_verify =
        ParseCrashStressWorkerArguments(verify);
    EXPECT_EQ(parsed_verify.mode, CrashStressWorkerMode::VerifyPrefix);
    EXPECT_EQ(parsed_verify.journal, journal);
    EXPECT_EQ(parsed_verify.compression, Compression::None);

    const std::array<std::filesystem::path, 2> identity{"--mode", "identity"};
    EXPECT_EQ(ParseCrashStressWorkerArguments(identity).mode,
              CrashStressWorkerMode::Identity);
    const std::array<std::filesystem::path, 4> initialize{
        "--mode", "initialize", "--db", database,
    };
    EXPECT_EQ(ParseCrashStressWorkerArguments(initialize).mode,
              CrashStressWorkerMode::Initialize);
}

TEST(CrashStressWorkerArgumentsTest, RejectsInvalidModeSpecificArguments) {
    const std::vector<std::vector<std::filesystem::path>> invalid{
        {},
        {"--mode", "unknown"},
        {"--mode", "write-hold", "--db", "db"},
        {"--mode", "verify-prefix", "--db", "db"},
        {"--mode", "verify-optional", "--db", "db"},
        {"--mode", "identity", "--db", "db"},
        {"--mode", "write-hold", "--db", "db", "--index", "x"},
        {"--mode", "write-hold", "--db", "db", "--index", "1", "--index", "2"},
        {"--mode", "write-hold", "--db", "db", "--index", "1", "--compression", "brotli"},
    };
    for (const auto& arguments : invalid) {
        EXPECT_THROW(static_cast<void>(ParseCrashStressWorkerArguments(arguments)),
                     std::invalid_argument);
    }
}

TEST(CrashStressWorkerTest, ValuesAndProtocolRecordsAreDeterministic) {
    const std::string first = CrashStressValue(17, 3);
    const std::string same = CrashStressValue(17, 3);
    const std::string different_seed = CrashStressValue(18, 3);
    const std::string different_index = CrashStressValue(17, 4);

    EXPECT_EQ(first, same);
    EXPECT_NE(first, different_seed);
    EXPECT_NE(first, different_index);
    EXPECT_EQ(first.size(), CrashStressValueSize);
    EXPECT_EQ(CrashStressAcknowledgement(3), "ack\t3\n");
    EXPECT_EQ(CrashStressReadiness(3), "ready\t3\n");
}

TEST(CrashStressWorkerTest, ParsesOnlyContiguousAcknowledgementJournals) {
    test_support::TemporaryDirectory directory;
    const auto journal = directory.path() / "acknowledged.log";
    WriteJournal(journal, "format=modern-leveldb-crash-journal-v1\nack\t0\nack\t1\nack\t2\n");

    const CrashStressJournal parsed = ParseCrashStressJournal(journal);

    EXPECT_EQ(parsed.acknowledged, 3U);

    for (const std::string_view malformed : {
             "",
             "format=wrong\n",
             "format=modern-leveldb-crash-journal-v1\nack\t1\n",
             "format=modern-leveldb-crash-journal-v1\nack\t0\nack\t0\n",
             "format=modern-leveldb-crash-journal-v1\nack\t0\ntail\n",
             "format=modern-leveldb-crash-journal-v1\nack\tx\n",
         }) {
        WriteJournal(journal, malformed);
        EXPECT_THROW(static_cast<void>(ParseCrashStressJournal(journal)),
                     std::runtime_error);
    }
}

TEST(CrashStressWorkerTest, VerifiesAcknowledgedPrefixAgainstNativeDatabase) {
    test_support::TemporaryDirectory directory;
    const auto database = directory.path() / "db";
    const auto journal = directory.path() / "acknowledged.log";
    for (std::size_t index = 0; index != 3; ++index) {
        WriteCrashStressBatch(database, 17, index, Compression::Snappy);
    }
    WriteJournal(journal, "format=modern-leveldb-crash-journal-v1\nack\t0\nack\t1\nack\t2\n");

    EXPECT_EQ(VerifyCrashStressPrefix(database, journal, 17, Compression::Snappy), 3U);

    WriteJournal(journal,
                 "format=modern-leveldb-crash-journal-v1\nack\t0\nack\t1\nack\t2\nack\t3\n");
    EXPECT_THROW(
        static_cast<void>(VerifyCrashStressPrefix(database, journal, 17, Compression::Snappy)),
        std::runtime_error);
}

TEST(CrashStressWorkerTest, OptionalBatchAcceptsOnlyAbsentOrCompleteState) {
    test_support::TemporaryDirectory directory;
    const auto absent = directory.path() / "absent";
    InitializeCrashStressDatabase(absent, Compression::None);
    EXPECT_EQ(VerifyOptionalCrashStressBatch(absent, 301, 0, Compression::None),
              OptionalCrashBatchState::Absent);

    const auto complete = directory.path() / "complete";
    WriteCrashStressBatch(complete, 301, 0, Compression::None);
    EXPECT_EQ(VerifyOptionalCrashStressBatch(complete, 301, 0, Compression::None),
              OptionalCrashBatchState::Complete);

    const auto partial = directory.path() / "partial";
    Options options;
    options.create_if_missing = true;
#if defined(_WIN32)
    options.allow_weak_namespace_durability = true;
#endif
    {
        auto opened = Database::Open(options, partial);
        ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
        Database database = std::move(*opened);
        ASSERT_TRUE(database
                        .Put(AsBytes(CrashStressLatestKey), AsBytes(CrashStressValue(301, 0)),
                             {.sync = true})
                        .has_value());
    }
    EXPECT_THROW(
        static_cast<void>(VerifyOptionalCrashStressBatch(partial, 301, 0, Compression::None)),
        std::runtime_error);
}

TEST(CrashStressWorkerTest, FreshInflightDatabaseCanUseANonzeroOperationIndex) {
    test_support::TemporaryDirectory directory;
    const auto database_path = directory.path() / "inflight";
    InitializeCrashStressDatabase(database_path, Compression::None);

    {
        Database database =
            OpenCrashStressDatabase(database_path, Compression::None, false);
        ApplyCrashStressBatch(database, 17, 9);
    }

    EXPECT_EQ(VerifyOptionalCrashStressBatch(database_path, 17, 9, Compression::None),
              OptionalCrashBatchState::Complete);
}

}  // namespace
}  // namespace modern_leveldb::stress
