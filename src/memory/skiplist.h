#ifndef MODERN_LEVELDB_MEMORY_SKIPLIST_H_
#define MODERN_LEVELDB_MEMORY_SKIPLIST_H_

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

#include "memory/arena.h"

namespace modern_leveldb {

template <typename Entry>
concept ArenaCompatibleSkipListEntry =
    std::is_default_constructible_v<Entry> && std::is_trivially_copy_constructible_v<Entry> &&
    std::is_trivially_destructible_v<Entry> && alignof(Entry) <= Arena::Alignment;

// Arena-backed ordered index with one externally serialized writer and concurrent
// readers. Entries remain immutable after publication; nodes live until arena teardown.
// The comparator, arena, and list must outlive all traversals. Compare must support
// concurrent calls; its borrowed state is not protected by the link atomics.
// Any storage referenced by an Entry must also remain alive and unchanged.
// Map-like callers use a pointer or handle Entry whose referenced record contains
// both the logical key and value; the SkipList itself is an ordered set of Entries.
// See docs/learning/08-cpp-ownership-errors-and-concurrency.md.
template <ArenaCompatibleSkipListEntry Entry, typename Compare>
class SkipList final {
private:
    struct Node;
    using Link = std::atomic<Node*>;

public:
    explicit SkipList(const Compare& compare, Arena& arena)
        : compare_(compare), arena_(arena), head_(NewNode(Entry{}, MaxHeight)) {}

    SkipList(Compare&&, Arena&) = delete;
    SkipList(const Compare&&, Arena&) = delete;
    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;
    SkipList(SkipList&&) = delete;
    SkipList& operator=(SkipList&&) = delete;
    ~SkipList() = default;

    [[nodiscard]] bool Insert(Entry entry) {
        std::array<Node*, MaxHeight> predecessors{};
        Node* existing = FindGreaterOrEqual(entry, predecessors.data());
        if (existing != nullptr && Equal(existing->entry, entry)) {
            return false;
        }
        InsertAfterSearch(entry, predecessors);
        return true;
    }

    // Requires a unique entry, as established by reserved memtable write sequences.
    // Duplicate insertion is a caller error, not a recoverable result here.
    void InsertTrusted(Entry entry) {
        std::array<Node*, MaxHeight> predecessors{};
        Node* existing = FindGreaterOrEqual(entry, predecessors.data());
        assert(existing == nullptr || !Equal(existing->entry, entry));
        (void)existing;
        InsertAfterSearch(entry, predecessors);
    }

    [[nodiscard]] bool Contains(const Entry& entry) const {
        Node* node = FindGreaterOrEqual(entry, nullptr);
        return node != nullptr && Equal(node->entry, entry);
    }

    class Iterator final {
    public:
        explicit Iterator(const SkipList& list) noexcept : list_(&list) {}

        Iterator(const Iterator&) = delete;
        Iterator& operator=(const Iterator&) = delete;
        Iterator(Iterator&&) = delete;
        Iterator& operator=(Iterator&&) = delete;
        ~Iterator() = default;

        [[nodiscard]] bool valid() const noexcept { return node_ != nullptr; }

        [[nodiscard]] const Entry& entry() const {
            assert(valid());
            return node_->entry;
        }

        void Next() {
            assert(valid());
            node_ = node_->Next(0);
        }

        void Prev() {
            assert(valid());
            node_ = list_->FindLessThan(node_->entry);
            if (node_ == list_->head_) {
                node_ = nullptr;
            }
        }

        void Seek(const Entry& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }

        void SeekToFirst() { node_ = list_->head_->Next(0); }

        void SeekToLast() {
            node_ = list_->FindLast();
            if (node_ == list_->head_) {
                node_ = nullptr;
            }
        }

    private:
        const SkipList* list_;
        Node* node_ = nullptr;
    };

private:
    static constexpr int MaxHeight = 12;
    static constexpr std::uint32_t Branching = 4;
    static constexpr std::uint32_t RandomModulus = 2'147'483'647U;
    static constexpr std::uint64_t RandomMultiplier = 16'807U;

    struct Node {
        explicit Node(Entry node_entry) noexcept : entry(node_entry) {}

        [[nodiscard]] Node* Next(int level) const noexcept {
            return LinkAt(level).load(std::memory_order_acquire);
        }

        [[nodiscard]] Node* NextRelaxed(int level) const noexcept {
            return LinkAt(level).load(std::memory_order_relaxed);
        }

        void SetNext(int level, Node* node) noexcept {
            LinkAt(level).store(node, std::memory_order_release);
        }

        void SetNextRelaxed(int level, Node* node) noexcept {
            LinkAt(level).store(node, std::memory_order_relaxed);
        }

        [[nodiscard]] Link& LinkAt(int level) const noexcept {
            assert(level >= 0);
            auto* base = reinterpret_cast<std::byte*>(const_cast<Node*>(this));
            std::byte* address =
                base + LinksOffset() + static_cast<std::size_t>(level) * sizeof(Link);
            return *std::launder(reinterpret_cast<Link*>(address));
        }

        const Entry entry;
    };

    // Nodes and their variable-height trailing links are constructed in Arena
    // storage and reclaimed without individual destruction.
    static_assert(std::is_trivially_destructible_v<Link>);
    static_assert(alignof(Link) <= Arena::Alignment);
    static_assert(sizeof(Link) % alignof(Link) == 0);
    static_assert(alignof(Node) <= Arena::Alignment);
    static_assert((alignof(Link) & (alignof(Link) - 1U)) == 0U);

    static constexpr std::size_t LinksOffset() noexcept {
        // Variable-height links follow the fixed node, aligned and constructed
        // separately. Trivial destruction permits whole-arena reclamation.
        return (sizeof(Node) + alignof(Link) - 1U) & ~(alignof(Link) - 1U);
    }

    void InsertAfterSearch(Entry entry, std::array<Node*, MaxHeight>& predecessors) {
        const int height = RandomHeight();
        const int current_height = max_height_.load(std::memory_order_relaxed);
        if (height > current_height) {
            for (int level = current_height; level < height; ++level) {
                predecessors[static_cast<std::size_t>(level)] = head_;
            }
        }

        Node* node = NewNode(entry, height);
        if (height > current_height) {
            // Height is only a search hint. A reader seeing the higher level before
            // its first link is published finds the initialized null head link and descends.
            max_height_.store(height, std::memory_order_relaxed);
        }
        for (int level = 0; level < height; ++level) {
            Node* predecessor = predecessors[static_cast<std::size_t>(level)];
            node->SetNextRelaxed(level, predecessor->NextRelaxed(level));
            // Release-publication pairs with readers' acquire loads: the key and
            // outgoing link are initialized before the node becomes reachable.
            predecessor->SetNext(level, node);
        }
    }

    [[nodiscard]] Node* NewNode(Entry entry, int height) {
        MutableByteView storage =
            arena_.AllocateAligned(LinksOffset() + sizeof(Link) * static_cast<std::size_t>(height));
        Node* node = std::construct_at(reinterpret_cast<Node*>(storage.data()), entry);
        for (int level = 0; level < height; ++level) {
            std::byte* address =
                storage.data() + LinksOffset() + static_cast<std::size_t>(level) * sizeof(Link);
            (void)std::construct_at(reinterpret_cast<Link*>(address), nullptr);
        }
        return node;
    }

    [[nodiscard]] std::uint32_t NextRandom() noexcept {
        const std::uint64_t product = random_seed_ * RandomMultiplier;
        random_seed_ = static_cast<std::uint32_t>((product >> 31U) + (product & RandomModulus));
        if (random_seed_ > RandomModulus) {
            random_seed_ -= RandomModulus;
        }
        return random_seed_;
    }

    [[nodiscard]] int RandomHeight() noexcept {
        int height = 1;
        while (height < MaxHeight && NextRandom() % Branching == 0U) {
            ++height;
        }
        return height;
    }

    [[nodiscard]] bool Equal(const Entry& left, const Entry& right) const {
        return compare_(left, right) == 0;
    }

    [[nodiscard]] bool EntryIsAfterNode(const Entry& entry, Node* node) const {
        return node != nullptr && compare_(node->entry, entry) < 0;
    }

    [[nodiscard]] Node* FindGreaterOrEqual(const Entry& entry, Node** predecessors) const {
        Node* node = head_;
        int level = max_height_.load(std::memory_order_relaxed) - 1;
        while (true) {
            Node* next = node->Next(level);
            if (EntryIsAfterNode(entry, next)) {
                node = next;
            } else {
                if (predecessors != nullptr) {
                    predecessors[level] = node;
                }
                if (level == 0) {
                    return next;
                }
                --level;
            }
        }
    }

    [[nodiscard]] Node* FindLessThan(const Entry& entry) const {
        Node* node = head_;
        int level = max_height_.load(std::memory_order_relaxed) - 1;
        while (true) {
            Node* next = node->Next(level);
            if (next == nullptr || compare_(next->entry, entry) >= 0) {
                if (level == 0) {
                    return node;
                }
                --level;
            } else {
                node = next;
            }
        }
    }

    [[nodiscard]] Node* FindLast() const {
        Node* node = head_;
        int level = max_height_.load(std::memory_order_relaxed) - 1;
        while (true) {
            Node* next = node->Next(level);
            if (next == nullptr) {
                if (level == 0) {
                    return node;
                }
                --level;
            } else {
                node = next;
            }
        }
    }

    const Compare& compare_;
    Arena& arena_;
    Node* const head_;
    std::atomic<int> max_height_{1};
    // Writer-private height selection; atomic links do not serialize writers.
    std::uint32_t random_seed_ = 0xdeadbeefU & 0x7fffffffU;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_MEMORY_SKIPLIST_H_
