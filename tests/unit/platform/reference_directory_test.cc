#include "support/reference_directory.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>

namespace modern_leveldb::test_support {
namespace {

TEST(ReferenceDirectoryTest, RejectsUnicodeCandidatesAndCreatesInTheSelectedAsciiRoot) {
    TemporaryDirectory unicode;
    const auto non_ascii = unicode.path() / std::filesystem::path{u8"\u6570\u636e"};
    ASSERT_TRUE(std::filesystem::create_directory(non_ascii));
    const std::array<std::filesystem::path, 2> candidates{non_ascii, ReferenceFixtureRoot()};
    const auto selected = SelectAsciiReferenceRoot(candidates);
    TemporaryDirectory fixture(selected);
    EXPECT_TRUE(std::filesystem::equivalent(fixture.path().parent_path(), selected));
    EXPECT_TRUE(std::filesystem::is_directory(fixture.path()));
}

TEST(ReferenceDirectoryTest, MissingOrOnlyUnicodeRootsFailExplicitly) {
    TemporaryDirectory directory;
    const auto non_ascii = directory.path() / std::filesystem::path{u8"\u6570\u636e"};
    ASSERT_TRUE(std::filesystem::create_directory(non_ascii));
    const std::array<std::filesystem::path, 2> candidates{non_ascii, directory.path() / "missing"};
    EXPECT_THROW(static_cast<void>(SelectAsciiReferenceRoot(candidates)), std::runtime_error);
}

}  // namespace
}  // namespace modern_leveldb::test_support
