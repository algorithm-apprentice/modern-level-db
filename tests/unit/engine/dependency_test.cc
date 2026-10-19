#include "engine/dependency.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "modern_leveldb/background_executor.h"
#include "modern_leveldb/clock.h"
#include "modern_leveldb/file_system.h"
#include "table/block_cache.h"

namespace modern_leveldb {
namespace {

struct Lifetime {
    std::array<int, 4> created{};
    std::array<int, 4> destroyed{};
    std::array<bool, 4> alive{};
    bool state_alive = false;
    bool mutex_held = false;
};

struct Resource {
    Lifetime& lifetime;
    std::size_t index;
    bool check_executor_order = false;

    Resource(Lifetime& observed, std::size_t slot, bool check_order = false)
        : lifetime(observed), index(slot), check_executor_order(check_order) {
        ++lifetime.created[index];
        lifetime.alive[index] = true;
    }

    ~Resource() {
        if (index < 3) {
            EXPECT_FALSE(lifetime.state_alive);
        } else if (check_executor_order) {
            EXPECT_TRUE(lifetime.state_alive);
            EXPECT_FALSE(lifetime.mutex_held);
        }
        ++lifetime.destroyed[index];
        lifetime.alive[index] = false;
    }
};

static_assert(!std::is_copy_constructible_v<Dependency<Resource>>);
static_assert(!std::is_copy_assignable_v<Dependency<Resource>>);
static_assert(std::is_nothrow_move_constructible_v<Dependency<Resource>>);
static_assert(!std::is_move_assignable_v<Dependency<Resource>>);
static_assert(noexcept(OwnDependency(std::declval<std::unique_ptr<Resource>>())));
static_assert(noexcept(BorrowDependency(std::declval<Resource&>())));

TEST(DependencyTest, DeletesOwnedObjectsExactlyOnce) {
    Lifetime lifetime;
    {
        auto dependency = OwnDependency(std::make_unique<Resource>(lifetime, 0));
        EXPECT_TRUE(lifetime.alive[0]);
        EXPECT_EQ(dependency.get()->index, 0U);
    }
    EXPECT_EQ(lifetime.destroyed[0], 1);
}

TEST(DependencyTest, DoesNotDeleteBorrowedObjects) {
    Lifetime lifetime;
    {
        Resource resource(lifetime, 0);
        {
            auto dependency = BorrowDependency(resource);
            EXPECT_EQ(dependency.get(), &resource);
        }
        EXPECT_EQ(lifetime.destroyed[0], 0);
        EXPECT_TRUE(lifetime.alive[0]);
    }
    EXPECT_EQ(lifetime.destroyed[0], 1);
}

TEST(DependencyTest, MovesOwningPointersWithoutDuplicatingOwnership) {
    Lifetime lifetime;
    {
        auto first = OwnDependency(std::make_unique<Resource>(lifetime, 0));
        Resource* pointer = first.get();
        auto second = std::move(first);
        EXPECT_EQ(first.get(), nullptr);
        EXPECT_EQ(second.get(), pointer);
        EXPECT_EQ(lifetime.destroyed[0], 0);
    }
    EXPECT_EQ(lifetime.destroyed[0], 1);
}

TEST(DependencyTest, MovesBorrowedPointersWithoutTakingOwnership) {
    Lifetime lifetime;
    {
        Resource borrowed(lifetime, 0);
        {
            auto first = BorrowDependency(borrowed);
            auto second = std::move(first);
            EXPECT_EQ(first.get(), nullptr);
            EXPECT_EQ(second.get(), &borrowed);
        }
        EXPECT_EQ(lifetime.destroyed[0], 0);
        EXPECT_TRUE(lifetime.alive[0]);
    }
    EXPECT_EQ(lifetime.destroyed[0], 1);
}

template <typename T>
void ExpectEmptyOwner() {
    auto first = OwnDependency(std::unique_ptr<T>{});
    EXPECT_EQ(first.get(), nullptr);
    auto second = std::move(first);
    EXPECT_EQ(first.get(), nullptr);
    EXPECT_EQ(second.get(), nullptr);
}

TEST(DependencyTest, PreservesEmptyOwnersAndMovedFromStatesForEveryServiceKind) {
    ExpectEmptyOwner<FileSystem>();
    ExpectEmptyOwner<BlockCache>();
    ExpectEmptyOwner<Clock>();
    ExpectEmptyOwner<BackgroundExecutor>();
}

struct ResourceSelection {
    Lifetime& lifetime;
    std::array<std::unique_ptr<Resource>, 4>& supplied;
    unsigned owned_mask;
    std::size_t failure;

    Dependency<Resource> Prepare(std::size_t index) {
        if (index == failure) {
            throw std::bad_alloc();
        }
        if ((owned_mask & (1U << index)) != 0) {
            return OwnDependency(std::make_unique<Resource>(lifetime, index, index == 3));
        }
        return BorrowDependency(*supplied[index]);
    }
};

struct DependentState {
    Lifetime& lifetime;

    explicit DependentState(Lifetime& observed) : lifetime(observed) {
        lifetime.state_alive = true;
    }
    ~DependentState() {
        EXPECT_TRUE(lifetime.alive[0]);
        EXPECT_TRUE(lifetime.alive[1]);
        EXPECT_TRUE(lifetime.alive[2]);
        lifetime.state_alive = false;
    }
};

struct UnlockOnExit {
    Lifetime& lifetime;
    ~UnlockOnExit() { lifetime.mutex_held = false; }
};

struct ComposedDependencies {
    Dependency<Resource> file_system;
    Dependency<Resource> cache;
    Dependency<Resource> clock;
    DependentState state;
    Dependency<Resource> executor;

    explicit ComposedDependencies(ResourceSelection& selection)
        : file_system(selection.Prepare(0)),
          cache(selection.Prepare(1)),
          clock(selection.Prepare(2)),
          state(selection.lifetime),
          executor(selection.Prepare(3)) {
        if (selection.failure == 4) {
            throw std::bad_alloc();
        }
    }

    ~ComposedDependencies() {
        state.lifetime.mutex_held = true;
        UnlockOnExit unlock{state.lifetime};
    }
};

TEST(DependencyTest, PreservesEveryOwnershipMixtureDuringConstructionAndDestruction) {
    for (unsigned mask = 0; mask < 16; ++mask) {
        for (std::size_t failure = 0; failure < 6; ++failure) {
            SCOPED_TRACE(testing::Message() << "mask=" << mask << " failure=" << failure);
            Lifetime lifetime;
            std::array<std::unique_ptr<Resource>, 4> supplied;
            for (std::size_t index = 0; index < supplied.size(); ++index) {
                if ((mask & (1U << index)) == 0) {
                    supplied[index] = std::make_unique<Resource>(lifetime, index);
                }
            }
            ResourceSelection selection{lifetime, supplied, mask, failure};
            if (failure < 5) {
                EXPECT_THROW(static_cast<void>(ComposedDependencies(selection)), std::bad_alloc);
            } else {
                ComposedDependencies dependencies(selection);
                EXPECT_TRUE(lifetime.state_alive);
                for (bool alive : lifetime.alive) {
                    EXPECT_TRUE(alive);
                }
            }
            EXPECT_FALSE(lifetime.state_alive);
            EXPECT_FALSE(lifetime.mutex_held);
            for (std::size_t index = 0; index < supplied.size(); ++index) {
                if ((mask & (1U << index)) == 0) {
                    EXPECT_EQ(lifetime.destroyed[index], 0);
                    EXPECT_TRUE(lifetime.alive[index]);
                } else {
                    EXPECT_EQ(lifetime.destroyed[index], lifetime.created[index]);
                    EXPECT_FALSE(lifetime.alive[index]);
                }
                supplied[index].reset();
                EXPECT_EQ(lifetime.destroyed[index], lifetime.created[index]);
            }
        }
    }
}

}  // namespace
}  // namespace modern_leveldb
