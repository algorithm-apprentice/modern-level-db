#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "modern_leveldb/db.h"

namespace modern_leveldb {
namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("modern-leveldb-windows-api-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        if (!std::filesystem::create_directory(path_)) {
            throw std::runtime_error("could not create the Windows API fixture");
        }
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

Options OpeningOptions() {
    Options options;
    options.allow_weak_namespace_durability = true;
    return options;
}

Options CreatingOptions() {
    Options options = OpeningOptions();
    options.create_if_missing = true;
    options.write_buffer_size = 64 * 1'024;
    return options;
}

std::map<std::filesystem::path, std::vector<char>> Inventory(
    const std::filesystem::path& directory) {
    std::map<std::filesystem::path, std::vector<char>> result;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        std::ifstream input(entry.path(), std::ios::binary);
        result.emplace(entry.path().filename(),
                       std::vector<char>(std::istreambuf_iterator<char>(input),
                                         std::istreambuf_iterator<char>()));
    }
    return result;
}

TEST(PublicWindowsDatabaseTest, RequiresConsentBeforeCreatingOrLockingADatabase) {
    TemporaryDirectory directory;
    for (const bool sync_creation : {false, true}) {
        Options options;
        options.create_if_missing = true;
        options.sync_wal_creation = sync_creation;
        const auto path = directory.path() / (sync_creation ? "strong" : "no-wal-sync");
        const auto rejected = Database::Open(options, path);
        ASSERT_FALSE(rejected.has_value());
        EXPECT_EQ(rejected.error().code(), ErrorCode::NotSupported);
        EXPECT_FALSE(std::filesystem::exists(path));
    }
    EXPECT_TRUE(Inventory(directory.path()).empty());
}

TEST(PublicWindowsDatabaseTest, ExplicitConsentCreatesSyncsAndReopensARealDatabase) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "db";
    {
        auto database = Database::Open(CreatingOptions(), path);
        ASSERT_TRUE(database.has_value()) << database.error().ToString();
        ASSERT_TRUE(database->Put(AsBytes("a"), AsBytes("value"), {.sync = true}).has_value());
        WriteBatch batch;
        ASSERT_TRUE(batch.Put(AsBytes("b"), AsBytes("batch")).has_value());
        ASSERT_TRUE(batch.Delete(AsBytes("a")).has_value());
        ASSERT_TRUE(database->Write(batch, {.sync = true}).has_value());
        const auto state = database->GetState();
        ASSERT_TRUE(state.has_value());
        EXPECT_EQ(state->last_sequence, 3U);
    }
    const auto before = Inventory(path);
    ASSERT_FALSE(before.empty());
    const auto strict = Database::Open({}, path);
    ASSERT_FALSE(strict.has_value());
    EXPECT_EQ(strict.error().code(), ErrorCode::NotSupported);
    EXPECT_EQ(Inventory(path), before);
    auto reopened = Database::Open(OpeningOptions(), path);
    ASSERT_TRUE(reopened.has_value()) << reopened.error().ToString();
    const auto value = reopened->Get(AsBytes("b"));
    ASSERT_TRUE(value.has_value() && value->has_value());
    EXPECT_EQ(AsStringView(**value), "batch");
    EXPECT_FALSE(reopened->Get(AsBytes("a"))->has_value());
}

TEST(PublicWindowsDatabaseTest, UnicodeRecoveryIgnoresUnrelatedNativeNames) {
    TemporaryDirectory directory;
    const auto path = directory.path() / std::filesystem::path{u8"\u6570\u636e-\U0001f4be"};
    ASSERT_TRUE(std::filesystem::create_directory(path));
    const auto foreign = path / std::filesystem::path{u8"\U0001f4be-\u9644\u4ef6.txt"};
    {
        std::ofstream output(foreign, std::ios::binary);
        ASSERT_TRUE(output.is_open());
        output << "not a database file";
    }
    {
        auto database = Database::Open(CreatingOptions(), path);
        ASSERT_TRUE(database.has_value()) << database.error().ToString();
        const std::string large(80 * 1'024, 'x');
        ASSERT_TRUE(database->Put(AsBytes("large"), AsBytes(large), {.sync = true}).has_value());
        ASSERT_TRUE(
            database->Put(AsBytes("trigger"), AsBytes("flush"), {.sync = true}).has_value());
    }
    auto reopened = Database::Open(OpeningOptions(), path);
    ASSERT_TRUE(reopened.has_value()) << reopened.error().ToString();
    const auto value = reopened->Get(AsBytes("large"));
    ASSERT_TRUE(value.has_value() && value->has_value());
    EXPECT_EQ((*value)->size(), 80U * 1'024U);
    EXPECT_TRUE(std::filesystem::exists(foreign));
}

TEST(PublicWindowsDatabaseTest, UnicodeFailuresRemainTypedAndUnsupportedLocationsStayClosed) {
    TemporaryDirectory directory;
    const auto path = directory.path() / std::filesystem::path{u8"\u7f3a\u5931-\U0001f4be"};
    const auto missing = Database::Open(OpeningOptions(), path);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(missing.error().message().find("\xe7\xbc\xba\xe5\xa4\xb1"), std::string_view::npos);
    const auto network = Database::Open(CreatingOptions(), L"\\\\server\\share\\db");
    ASSERT_FALSE(network.has_value());
    EXPECT_EQ(network.error().code(), ErrorCode::NotSupported);
}

class CurrentDirectoryGuard {
public:
    CurrentDirectoryGuard() : saved_(std::filesystem::current_path()) {}
    CurrentDirectoryGuard(const CurrentDirectoryGuard&) = delete;
    CurrentDirectoryGuard& operator=(const CurrentDirectoryGuard&) = delete;
    ~CurrentDirectoryGuard() {
        std::error_code error;
        std::filesystem::current_path(saved_, error);
        if (error) {
            std::cerr << "current-directory restoration failed: " << error.message() << '\n';
        }
    }

private:
    std::filesystem::path saved_;
};

TEST(PublicWindowsDatabaseTest, FreezesRelativePathsBeforeBackgroundAndRecoveryWork) {
    TemporaryDirectory directory;
    const auto first = directory.path() / "first";
    const auto second = directory.path() / "second";
    ASSERT_TRUE(std::filesystem::create_directory(first));
    ASSERT_TRUE(std::filesystem::create_directory(second));
    CurrentDirectoryGuard restore;
    std::filesystem::current_path(first);
    {
        auto database = Database::Open(CreatingOptions(), "db");
        ASSERT_TRUE(database.has_value()) << database.error().ToString();
        std::filesystem::current_path(second);
        const std::string large(80 * 1'024, 'x');
        ASSERT_TRUE(database->Put(AsBytes("large"), AsBytes(large), {.sync = true}).has_value());
        ASSERT_TRUE(database->Put(AsBytes("trigger"), AsBytes("flush")).has_value());
    }
    EXPECT_FALSE(std::filesystem::exists(second / "db"));
    auto reopened = Database::Open(OpeningOptions(), first / "db");
    ASSERT_TRUE(reopened.has_value()) << reopened.error().ToString();
    const auto value = reopened->Get(AsBytes("large"));
    ASSERT_TRUE(value.has_value() && value->has_value());
    EXPECT_EQ((*value)->size(), 80U * 1'024U);
}

}  // namespace
}  // namespace modern_leveldb
