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

template <typename Key>
concept ArenaCompatibleSkipListKey =
    std::is_default_constructible_v<Key> && std::is_trivially_copy_constructible_v<Key> &&
    std::is_trivially_destructible_v<Key> && alignof(Key) <= alignof(std::max_align_t);

template <ArenaCompatibleSkipListKey Key, typename Compare>
class SkipList final {
 private:
  struct Node;
  using Link = std::atomic<Node*>;

 public:
  explicit SkipList(const Compare& compare, Arena& arena)
      : compare_(compare), arena_(arena), head_(NewNode(Key{}, MaxHeight)) {
    static_assert(std::is_trivially_destructible_v<Link>);
    static_assert(alignof(Link) <= alignof(std::max_align_t));
    static_assert(sizeof(Link) % alignof(Link) == 0);
    static_assert(alignof(Node) <= alignof(std::max_align_t));
  }

  SkipList(Compare&&, Arena&) = delete;
  SkipList(const Compare&&, Arena&) = delete;
  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;
  SkipList(SkipList&&) = delete;
  SkipList& operator=(SkipList&&) = delete;
  ~SkipList() = default;

  [[nodiscard]] bool Insert(Key key) {
    std::array<Node*, MaxHeight> predecessors{};
    Node* existing = FindGreaterOrEqual(key, predecessors.data());
    if (existing != nullptr && Equal(existing->key, key)) {
      return false;
    }

    const int height = RandomHeight();
    const int current_height = max_height_.load(std::memory_order_relaxed);
    if (height > current_height) {
      for (int level = current_height; level < height; ++level) {
        predecessors[static_cast<std::size_t>(level)] = head_;
      }
    }

    Node* node = NewNode(key, height);
    if (height > current_height) {
      max_height_.store(height, std::memory_order_relaxed);
    }
    for (int level = 0; level < height; ++level) {
      Node* predecessor = predecessors[static_cast<std::size_t>(level)];
      node->SetNextRelaxed(level, predecessor->NextRelaxed(level));
      predecessor->SetNext(level, node);
    }
    return true;
  }

  [[nodiscard]] bool Contains(const Key& key) const {
    Node* node = FindGreaterOrEqual(key, nullptr);
    return node != nullptr && Equal(node->key, key);
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

    [[nodiscard]] const Key& key() const {
      assert(valid());
      return node_->key;
    }

    void Next() {
      assert(valid());
      node_ = node_->Next(0);
    }

    void Prev() {
      assert(valid());
      node_ = list_->FindLessThan(node_->key);
      if (node_ == list_->head_) {
        node_ = nullptr;
      }
    }

    void Seek(const Key& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }

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
    Node(Key node_key, int node_height, std::byte* node_links) noexcept
        : key(node_key), height(node_height), links(node_links) {}

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
      assert(level < height);
      std::byte* address = links + static_cast<std::size_t>(level) * sizeof(Link);
      return *std::launder(reinterpret_cast<Link*>(address));
    }

    const Key key;
    const int height;
    std::byte* const links;
  };

  [[nodiscard]] Node* NewNode(Key key, int height) {
    MutableByteView link_storage =
        arena_.AllocateAligned(sizeof(Link) * static_cast<std::size_t>(height));
    for (int level = 0; level < height; ++level) {
      std::byte* address = link_storage.data() + static_cast<std::size_t>(level) * sizeof(Link);
      (void)std::construct_at(reinterpret_cast<Link*>(address), nullptr);
    }

    MutableByteView node_storage = arena_.AllocateAligned(sizeof(Node));
    return std::construct_at(reinterpret_cast<Node*>(node_storage.data()), key, height,
                             link_storage.data());
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

  [[nodiscard]] bool Equal(const Key& left, const Key& right) const {
    return compare_(left, right) == 0;
  }

  [[nodiscard]] bool KeyIsAfterNode(const Key& key, Node* node) const {
    return node != nullptr && compare_(node->key, key) < 0;
  }

  [[nodiscard]] Node* FindGreaterOrEqual(const Key& key, Node** predecessors) const {
    Node* node = head_;
    int level = max_height_.load(std::memory_order_relaxed) - 1;
    while (true) {
      Node* next = node->Next(level);
      if (KeyIsAfterNode(key, next)) {
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

  [[nodiscard]] Node* FindLessThan(const Key& key) const {
    Node* node = head_;
    int level = max_height_.load(std::memory_order_relaxed) - 1;
    while (true) {
      Node* next = node->Next(level);
      if (next == nullptr || compare_(next->key, key) >= 0) {
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
  std::uint32_t random_seed_ = 0xdeadbeefU & 0x7fffffffU;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_MEMORY_SKIPLIST_H_
