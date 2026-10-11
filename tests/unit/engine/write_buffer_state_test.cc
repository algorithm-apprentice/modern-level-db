#include "engine/write_buffer_state.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <vector>

#include "modern_leveldb/base/comparator.h"
#include "support/memory_file_system.h"

namespace modern_leveldb {
namespace {

using test_support::MemoryFileSystem;

std::unique_ptr<WalWriter> NewLog(MemoryFileSystem& file_system,
                                  const std::filesystem::path& path) {
    return std::make_unique<WalWriter>(file_system.OpenWritable(path).value());
}

TEST(WriteBufferStateTest, InstallsAndPublishesTheActiveGeneration) {
    MemoryFileSystem file_system;
    WriteBufferState state;
    state.InstallRecovered(NewLog(file_system, "1.log"), 1,
                           std::make_shared<MemTable>(BytewiseComparator()));

    EXPECT_TRUE(state.has_log());
    EXPECT_EQ(state.log_number(), 1U);
    EXPECT_FALSE(state.has_immutable());
    EXPECT_FALSE(state.HasImmutableForCompaction());

    const std::size_t before = state.published_mutable_bytes();
    const std::vector<std::byte> value(66000);
    state.mutable_table().Add(1, ValueKind::Value, {}, value);
    EXPECT_EQ(state.published_mutable_bytes(), before);

    state.PublishMutableMemoryUsage();
    EXPECT_EQ(state.published_mutable_bytes(), state.mutable_table().memory_usage());
    EXPECT_GT(state.published_mutable_bytes(), before);
    EXPECT_TRUE(state.log().Close().has_value());
}

TEST(WriteBufferStateTest, RotatesAndRetainsAReadPinnedImmutableTable) {
    MemoryFileSystem file_system;
    WriteBufferState state;
    auto first = std::make_shared<MemTable>(BytewiseComparator());
    const std::weak_ptr<const MemTable> retained = first;
    state.InstallRecovered(NewLog(file_system, "1.log"), 1, std::move(first));

    {
        MemTable::ReadPin pin = state.mutable_table().PinRead();
        EXPECT_TRUE(state.log().Close().has_value());
        state.InstallRotation(NewLog(file_system, "2.log"), 2,
                              std::make_shared<MemTable>(BytewiseComparator()));

        EXPECT_EQ(state.log_number(), 2U);
        EXPECT_TRUE(state.has_immutable());
        EXPECT_TRUE(state.HasImmutableForCompaction());
        ASSERT_NE(state.immutable_owner(), nullptr);
        EXPECT_EQ(&pin.value(), state.immutable_owner().get());

        state.ClearImmutable();
        EXPECT_FALSE(state.has_immutable());
        EXPECT_FALSE(state.HasImmutableForCompaction());
        EXPECT_FALSE(retained.expired());
    }

    state.ReleaseReadPinnedMemtables();
    EXPECT_TRUE(retained.expired());
    EXPECT_TRUE(state.log().Close().has_value());
}

}  // namespace
}  // namespace modern_leveldb
