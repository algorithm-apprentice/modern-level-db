#include "stress/stress.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "support/temporary_directory.h"
#include "support/text_file.h"

namespace modern_leveldb::stress {
namespace {

StressArguments SmallRun(const std::filesystem::path& root, std::string_view name) {
    StressArguments result;
    result.database = root / (std::string(name) + "-db");
    result.output = root / (std::string(name) + "-output");
    result.seed = 17;
    result.operations = 96;
    result.compression = Compression::None;
    return result;
}

class ThrowAtOperation final : public StressObserver {
public:
    explicit ThrowAtOperation(std::size_t selected) : selected_(selected) {}

    void BeforeOperation(const test_support::ModelOperation& operation) override {
        if (operation.index == selected_) {
            throw std::runtime_error("injected stress observer failure");
        }
    }

private:
    std::size_t selected_;
};

TEST(StressArgumentsTest, ParsesDefaultsOverridesAndNativePaths) {
#if defined(_WIN32)
    const std::filesystem::path database = L"db-\u6570\u636e";
    const std::filesystem::path output = L"out-\U0001f680";
#else
    const std::filesystem::path database = "db-\xe6\x95\xb0\xe6\x8d\xae";
    const std::filesystem::path output = "out-\xf0\x9f\x9a\x80";
#endif
    const std::vector<std::filesystem::path> defaults{
        "--db", database, "--output", output,
    };
    const StressArguments parsed_defaults = ParseStressArguments(defaults);
    EXPECT_EQ(parsed_defaults.database, database);
    EXPECT_EQ(parsed_defaults.output, output);
    EXPECT_EQ(parsed_defaults.seed, 301U);
    EXPECT_EQ(parsed_defaults.operations, 100'000U);
    EXPECT_EQ(parsed_defaults.compression, Compression::None);

    const std::vector<std::filesystem::path> overrides{
        "--db",          database, "--output",     output, "--seed",
        "18446744073709551615", "--operations", "42", "--compression", "zstd",
    };
    const StressArguments parsed_overrides = ParseStressArguments(overrides);
    EXPECT_EQ(parsed_overrides.seed, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(parsed_overrides.operations, 42U);
    EXPECT_EQ(parsed_overrides.compression, Compression::Zstd);
}

TEST(StressArgumentsTest, RejectsMissingDuplicateAndInvalidOptions) {
    const std::vector<std::vector<std::filesystem::path>> invalid{
        {},
        {"--db", "db"},
        {"--output", "out"},
        {"--db", "db", "--output", "out", "--seed"},
        {"--db", "", "--output", "out"},
        {"--db", "db", "--output", ""},
        {"--db", "db", "--output", "out", "--seed", "x"},
        {"--db", "db", "--db", "other", "--output", "out"},
        {"--db", "db", "--output", "out", "--output", "other"},
        {"--db", "db", "--output", "out", "--seed", "1", "--seed", "2"},
        {"--db", "db", "--output", "out", "--operations", "1", "--operations", "2"},
        {"--db", "db", "--output", "out", "--compression", "none", "--compression", "zstd"},
        {"--db", "db", "--output", "out", "--operations", "0"},
        {"--db", "db", "--output", "out", "--operations", "18446744073709551616"},
        {"--db", "db", "--output", "out", "--compression", "brotli"},
        {"--db", "db", "--output", "out", "--unknown", "1"},
    };

    for (const auto& arguments : invalid) {
        EXPECT_THROW(static_cast<void>(ParseStressArguments(arguments)), std::invalid_argument);
    }

    const std::array<std::filesystem::path, 1> help_arguments{"--help"};
    const StressArguments help = ParseStressArguments(help_arguments);
    EXPECT_TRUE(help.help);
}

TEST(StressRunTest, RejectsExistingEqualAndNestedPathsWithoutMutation) {
    test_support::TemporaryDirectory root;
    const StressBuildIdentity identity{"test-revision", false};

    StressArguments existing_database = SmallRun(root.path(), "existing-database");
    std::filesystem::create_directory(existing_database.database);
    EXPECT_THROW(static_cast<void>(RunStress(existing_database, identity)),
                 std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(existing_database.output));

    StressArguments existing_output = SmallRun(root.path(), "existing-output");
    std::filesystem::create_directory(existing_output.output);
    EXPECT_THROW(static_cast<void>(RunStress(existing_output, identity)), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(existing_output.database));

    StressArguments equal = SmallRun(root.path(), "equal");
    equal.output = equal.database;
    EXPECT_THROW(static_cast<void>(RunStress(equal, identity)), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(equal.database));

    StressArguments nested = SmallRun(root.path(), "nested");
    nested.output = nested.database / "artifacts";
    EXPECT_THROW(static_cast<void>(RunStress(nested, identity)), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(nested.database));

    StressArguments missing_parent = SmallRun(root.path(), "missing-parent");
    missing_parent.database = root.path() / "missing" / "database";
    EXPECT_THROW(static_cast<void>(RunStress(missing_parent, identity)),
                 std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(missing_parent.output));
}

TEST(StressRunTest, SameSeedProducesIdenticalSemanticArtifacts) {
    test_support::TemporaryDirectory root;
    const StressBuildIdentity identity{"test-revision", false};
    const StressArguments first = SmallRun(root.path(), "first");
    const StressArguments second = SmallRun(root.path(), "second");

    const StressSummary first_summary = RunStress(first, identity);
    const StressSummary second_summary = RunStress(second, identity);

    EXPECT_EQ(first_summary, second_summary);
    EXPECT_EQ(first_summary.operations, first.operations);
    const std::string run = test_support::ReadTextFile(first.output / "run.txt");
    EXPECT_EQ(run, test_support::ReadTextFile(second.output / "run.txt"));
    EXPECT_NE(run.find("revision=test-revision"), std::string::npos);
    EXPECT_NE(run.find("dirty=false"), std::string::npos);
    EXPECT_NE(run.find("write_buffer_size=65536"), std::string::npos);
    EXPECT_EQ(test_support::ReadTextFile(first.output / "operations.log"),
              test_support::ReadTextFile(second.output / "operations.log"));
    EXPECT_EQ(test_support::ReadTextFile(first.output / "final-state.txt"),
              test_support::ReadTextFile(second.output / "final-state.txt"));
    EXPECT_EQ(test_support::ReadTextFile(first.output / "completion.txt"),
              test_support::ReadTextFile(second.output / "completion.txt"));
    EXPECT_FALSE(std::filesystem::exists(first.output / "failure.txt"));
}

TEST(StressRunTest, FailurePreservesAttemptAndDoesNotPublishCompletion) {
    test_support::TemporaryDirectory root;
    const StressBuildIdentity identity{"test-revision", false};
    const StressArguments arguments = SmallRun(root.path(), "failure");
    ThrowAtOperation observer(3);

    EXPECT_THROW(static_cast<void>(RunStress(arguments, identity, &observer)), std::runtime_error);

    const std::string operations =
        test_support::ReadTextFile(arguments.output / "operations.log");
    EXPECT_NE(operations.find("\n0\t"), std::string::npos);
    EXPECT_NE(operations.find("\n3\t"), std::string::npos);
    EXPECT_EQ(operations.find("\n4\t"), std::string::npos);
    EXPECT_NE(test_support::ReadTextFile(arguments.output / "failure.txt")
                  .find("injected stress observer failure"),
              std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(arguments.output / "completion.txt"));
    EXPECT_FALSE(std::filesystem::exists(arguments.output / "final-state.txt"));
}

TEST(StressRunTest, PreservesNativeUnicodePaths) {
    test_support::TemporaryDirectory root;
    const StressBuildIdentity identity{"test-revision", false};
    StressArguments arguments;
#if defined(_WIN32)
    arguments.database = root.path() / L"db-\u6570\u636e-\U0001f680";
    arguments.output = root.path() / L"out-\u8bc1\u636e-\U0001f680";
#else
    arguments.database = root.path() / "db-\xe6\x95\xb0\xe6\x8d\xae-\xf0\x9f\x9a\x80";
    arguments.output = root.path() / "out-\xe8\xaf\x81\xe6\x8d\xae-\xf0\x9f\x9a\x80";
#endif
    arguments.operations = 16;

    const StressSummary summary = RunStress(arguments, identity);

    EXPECT_EQ(summary.operations, 16U);
    EXPECT_TRUE(std::filesystem::exists(arguments.database));
    EXPECT_TRUE(std::filesystem::exists(arguments.output / "completion.txt"));
}

TEST(StressRunTest, UsageStatesLocalOnlySafetyBoundary) {
    const std::string usage = StressUsage();

    EXPECT_NE(usage.find("disposable"), std::string::npos);
    EXPECT_NE(usage.find("--db"), std::string::npos);
    EXPECT_NE(usage.find("--output"), std::string::npos);
    EXPECT_NE(usage.find("--operations"), std::string::npos);
}

}  // namespace
}  // namespace modern_leveldb::stress
