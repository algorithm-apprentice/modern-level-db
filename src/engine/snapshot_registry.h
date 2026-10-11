#ifndef MODERN_LEVELDB_ENGINE_SNAPSHOT_REGISTRY_H_
#define MODERN_LEVELDB_ENGINE_SNAPSHOT_REGISTRY_H_

#include <cstddef>
#include <memory>
#include <optional>

#include "format/internal_key.h"

namespace modern_leveldb {

// Tracks every active snapshot registration in nondecreasing sequence order.
// It is not thread-safe; DatabaseEngine uses it with the database mutex held.
class SnapshotRegistry final {
    struct Node;

public:
    SnapshotRegistry() = default;
    SnapshotRegistry(const SnapshotRegistry&) = delete;
    SnapshotRegistry& operator=(const SnapshotRegistry&) = delete;
    SnapshotRegistry(SnapshotRegistry&&) = delete;
    SnapshotRegistry& operator=(SnapshotRegistry&&) = delete;
    ~SnapshotRegistry();

    class Registration final {
    public:
        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;
        Registration(Registration&& source) noexcept;
        Registration& operator=(Registration&&) = delete;
        ~Registration();

        [[nodiscard]] SequenceNumber sequence() const noexcept { return sequence_; }

    private:
        friend class SnapshotRegistry;

        explicit Registration(Node& node) noexcept;

        Node* node_;
        SequenceNumber sequence_;
    };

    // Requires a sequence not below the newest active registration.
    [[nodiscard]] Registration Register(SequenceNumber sequence);
    // Requires a live registration returned by this registry.
    void Release(Registration& registration) noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::optional<SequenceNumber> oldest() const noexcept;
    [[nodiscard]] SequenceNumber OldestOr(SequenceNumber fallback) const noexcept;

private:
    struct Node {
        Node(SnapshotRegistry& owner, SequenceNumber value) noexcept
            : registry(&owner), sequence(value) {}

        SnapshotRegistry* registry;
        SequenceNumber sequence;
        Node* previous = nullptr;
        std::unique_ptr<Node> next;
    };

    std::unique_ptr<Node> oldest_;
    Node* newest_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_SNAPSHOT_REGISTRY_H_
