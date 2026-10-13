#include "stress/linearizability.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace modern_leveldb::stress {
namespace {

RegisterOperation Store(std::size_t id, std::string value, std::uint64_t invoke,
                        std::uint64_t complete) {
    return {
        .id = id,
        .kind = RegisterOperationKind::Store,
        .value = std::move(value),
        .observed = std::nullopt,
        .invoke = invoke,
        .complete = complete,
    };
}

RegisterOperation Delete(std::size_t id, std::uint64_t invoke, std::uint64_t complete) {
    return {
        .id = id,
        .kind = RegisterOperationKind::Delete,
        .value = std::nullopt,
        .observed = std::nullopt,
        .invoke = invoke,
        .complete = complete,
    };
}

RegisterOperation Read(std::size_t id, std::optional<std::string> observed,
                       std::uint64_t invoke, std::uint64_t complete) {
    return {
        .id = id,
        .kind = RegisterOperationKind::Read,
        .value = std::nullopt,
        .observed = std::move(observed),
        .invoke = invoke,
        .complete = complete,
    };
}

std::optional<std::string> Replay(
    std::optional<std::string> state, const std::vector<RegisterOperation>& operations,
    const LinearizationWitness& witness) {
    for (const std::size_t id : witness.operation_ids) {
        const auto selected =
            std::ranges::find(operations, id, &RegisterOperation::id);
        if (selected == operations.end()) {
            throw std::runtime_error("witness names an unknown operation");
        }
        switch (selected->kind) {
            case RegisterOperationKind::Store:
                state = selected->value;
                break;
            case RegisterOperationKind::Delete:
                state.reset();
                break;
            case RegisterOperationKind::Read:
                if (state != selected->observed) {
                    throw std::runtime_error("witness read does not match state");
                }
                break;
        }
    }
    return state;
}

TEST(LinearizabilityTest, FindsSequentialWitnessAndFinalState) {
    const std::vector operations{
        Store(10, "a", 1, 2),
        Read(11, "a", 3, 4),
        Delete(12, 5, 6),
        Read(13, std::nullopt, 7, 8),
    };

    const auto witness = FindRegisterLinearization(std::nullopt, operations);

    ASSERT_TRUE(witness.has_value());
    EXPECT_EQ(witness->operation_ids, (std::vector<std::size_t>{10, 11, 12, 13}));
    EXPECT_FALSE(witness->final_value.has_value());
    EXPECT_EQ(Replay(std::nullopt, operations, *witness), witness->final_value);
}

TEST(LinearizabilityTest, OverlappingWritesCanLinearizeInEitherOrder) {
    const std::vector operations{
        Store(1, "a", 1, 4),
        Store(2, "b", 2, 3),
        Read(3, "a", 5, 6),
    };

    const auto witness = FindRegisterLinearization(std::nullopt, operations);

    ASSERT_TRUE(witness.has_value());
    EXPECT_EQ(witness->final_value, "a");
    EXPECT_EQ(Replay(std::nullopt, operations, *witness), witness->final_value);
}

TEST(LinearizabilityTest, OverlappingReadCanObserveTheEarlierState) {
    const std::vector operations{
        Store(1, "new", 1, 4),
        Read(2, "old", 2, 3),
    };

    const auto witness = FindRegisterLinearization(std::string("old"), operations);

    ASSERT_TRUE(witness.has_value());
    EXPECT_EQ(witness->final_value, "new");
}

TEST(LinearizabilityTest, RealTimePrecedenceRejectsAnOldRead) {
    const std::vector operations{
        Store(1, "new", 1, 2),
        Read(2, "old", 3, 4),
    };

    EXPECT_FALSE(
        FindRegisterLinearization(std::string("old"), operations).has_value());
}

TEST(LinearizabilityTest, BacktracksAcrossAmbiguousOverlaps) {
    const std::vector operations{
        Store(1, "a", 1, 6),
        Store(2, "b", 2, 5),
        Read(3, "b", 3, 4),
        Read(4, "a", 7, 8),
    };

    const auto witness = FindRegisterLinearization(std::nullopt, operations);

    ASSERT_TRUE(witness.has_value());
    EXPECT_EQ(witness->final_value, "a");
    EXPECT_EQ(Replay(std::nullopt, operations, *witness), witness->final_value);
}

TEST(LinearizabilityTest, RejectsMalformedAndOversizedHistories) {
    const std::vector duplicate_ids{
        Store(1, "a", 1, 2),
        Store(1, "b", 3, 4),
    };
    const std::vector duplicate_ticks{
        Store(1, "a", 1, 3),
        Read(2, "a", 2, 3),
    };
    const std::vector reversed_interval{
        Store(1, "a", 2, 1),
    };
    const std::vector malformed_store{
        RegisterOperation{.id = 1,
                          .kind = RegisterOperationKind::Store,
                          .value = std::nullopt,
                          .observed = std::string("a"),
                          .invoke = 1,
                          .complete = 2},
    };
    std::vector<RegisterOperation> oversized;
    for (std::size_t index = 0; index != MaxRegisterOperations + 1; ++index) {
        oversized.push_back(Store(index, std::to_string(index), index * 2, index * 2 + 1));
    }

    const std::array<const std::vector<RegisterOperation>*, 5> invalid{
        &duplicate_ids,
        &duplicate_ticks,
        &reversed_interval,
        &malformed_store,
        &oversized,
    };
    for (const auto* history : invalid) {
        EXPECT_THROW(static_cast<void>(FindRegisterLinearization(std::nullopt, *history)),
                     std::invalid_argument);
    }
}

}  // namespace
}  // namespace modern_leveldb::stress
