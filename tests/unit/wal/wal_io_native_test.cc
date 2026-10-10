#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "support/native_file_system.h"
#include "support/temporary_directory.h"
#include "wal/wal_io.h"

namespace modern_leveldb {
namespace {

TEST(WalIoNativeTest, WritesSyncsClosesAndReopensARealFile) {
    test_support::TemporaryDirectory directory;
    auto file_system = test_support::CopiedFileSystem();
    const std::filesystem::path path = directory.path() / "000001.log";
    auto writable = file_system.OpenWritable(path);
    ASSERT_TRUE(writable.has_value());
    WalWriter writer(std::move(*writable));
    ASSERT_TRUE(writer.AddRecord(AsBytes("first")).has_value());
    const std::vector<std::byte> large(70'000, std::byte{0x5a});
    ASSERT_TRUE(writer.AddRecord(large).has_value());
    ASSERT_TRUE(writer.Sync().has_value());
    ASSERT_TRUE(writer.Close().has_value());
    auto sequential = file_system.OpenSequential(path);
    ASSERT_TRUE(sequential.has_value());
    WalReader reader(std::move(*sequential));
    auto first = reader.ReadNext();
    ASSERT_TRUE(first.has_value() && first->has_value());
    const auto* first_record = std::get_if<WalLogicalRecord>(&**first);
    ASSERT_NE(first_record, nullptr);
    EXPECT_EQ(AsStringView(first_record->data), "first");
    auto second = reader.ReadNext();
    ASSERT_TRUE(second.has_value() && second->has_value());
    const auto* second_record = std::get_if<WalLogicalRecord>(&**second);
    ASSERT_NE(second_record, nullptr);
    EXPECT_EQ(ByteView(second_record->data).size(), large.size());
    EXPECT_TRUE(std::ranges::equal(second_record->data, large));
    const auto eof = reader.ReadNext();
    ASSERT_TRUE(eof.has_value());
    EXPECT_FALSE(eof->has_value());
}

}  // namespace
}  // namespace modern_leveldb
