#include "support/database_model.h"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <stdexcept>
#include <string>

namespace modern_leveldb::test_support {
namespace {

class ModelClient final {
public:
    void Put(std::string_view key, std::string_view value) {
        current_[std::string(key)] = std::string(value);
    }

    void Delete(std::string_view key) { current_.erase(std::string(key)); }

    void Write(const ModelBatch& operations, bool) {
        for (const auto& [key, value] : operations) {
            if (value.has_value()) {
                current_[key] = *value;
            } else {
                current_.erase(key);
            }
        }
    }

    void SnapshotAt(std::size_t slot) { snapshots_.at(slot) = current_; }

    void Release(std::size_t slot) { snapshots_.at(slot).reset(); }

    std::optional<std::string> Read(std::string_view key, std::size_t slot = SnapshotSlots) const {
        const Model& selected = View(slot);
        const auto entry = selected.find(std::string(key));
        if (entry == selected.end()) {
            return std::nullopt;
        }
        return entry->second;
    }

    Entries Scan(bool reverse, std::size_t slot = SnapshotSlots,
                 std::optional<std::string_view> target = {}) const {
        return ExpectedEntries(View(slot), reverse, target);
    }

    void Reopen() {
        for (auto& snapshot : snapshots_) {
            snapshot.reset();
        }
    }

private:
    const Model& View(std::size_t slot) const {
        if (slot == SnapshotSlots) {
            return current_;
        }
        return snapshots_.at(slot).value();
    }

    Model current_;
    std::array<std::optional<Model>, SnapshotSlots> snapshots_;
};

class ThrowingClient final {
public:
    void Put(std::string_view, std::string_view) { Fail(); }
    void Delete(std::string_view) { Fail(); }
    void Write(const ModelBatch&, bool) { Fail(); }
    void SnapshotAt(std::size_t) { Fail(); }
    void Release(std::size_t) { Fail(); }
    std::optional<std::string> Read(std::string_view, std::size_t = SnapshotSlots) {
        Fail();
    }
    Entries Scan(bool, std::size_t = SnapshotSlots,
                 std::optional<std::string_view> = {}) {
        Fail();
    }
    void Reopen() { Fail(); }

private:
    [[noreturn]] static void Fail() { throw std::runtime_error("injected client failure"); }
};

TEST(DatabaseModelSupportTest, SeededOperationsAreOwningDeterministicAndBounded) {
    ModelWorkload first(301);
    ModelWorkload same(301);
    ModelWorkload different(302);
    bool observed_difference = false;

    for (std::size_t index = 0; index != 100; ++index) {
        const ModelOperation left = first.Next(index);
        const ModelOperation right = same.Next(index);
        const ModelOperation other = different.Next(index);

        EXPECT_EQ(left, right);
        observed_difference = observed_difference || left != other;
        EXPECT_EQ(left.index, index);
        EXPECT_EQ(left.key.size(), 5U);
        EXPECT_LT(left.slot, SnapshotSlots);
        EXPECT_LE(left.value.size(), 2048U);
        if (left.kind == ModelOperationKind::Batch) {
            EXPECT_EQ(left.batch.size(), 3U);
        } else {
            EXPECT_TRUE(left.batch.empty());
        }
    }

    EXPECT_TRUE(observed_difference);
}

TEST(DatabaseModelSupportTest, ObserverSeesEveryOperationInOrder) {
    ModelClient client;
    std::size_t observed = 0;

    const Model result = RunModelObserved(
        17, 64,
        [&](const ModelOperation& operation) {
            EXPECT_EQ(operation.index, observed);
            ++observed;
        },
        client);

    EXPECT_EQ(observed, 64U);
    EXPECT_EQ(client.Scan(false), ExpectedEntries(result, false));
}

TEST(DatabaseModelSupportTest, ClientFailuresReportSeedAndOperation) {
    ThrowingClient client;

    try {
        static_cast<void>(RunModel(991, 1, client));
        FAIL() << "RunModel accepted an injected client failure";
    } catch (const std::runtime_error& error) {
        const std::string message(error.what());
        EXPECT_NE(message.find("seed=991"), std::string::npos);
        EXPECT_NE(message.find("operation=0"), std::string::npos);
        EXPECT_NE(message.find("injected client failure"), std::string::npos);
    }
}

}  // namespace
}  // namespace modern_leveldb::test_support
