#ifndef MODERN_LEVELDB_ENGINE_SNAPSHOT_REGISTRY_H_
#define MODERN_LEVELDB_ENGINE_SNAPSHOT_REGISTRY_H_

#include <cstddef>
#include <optional>

#include "format/internal_key.h"

namespace modern_leveldb {

// Tracks active snapshots in a nondecreasing intrusive list. The registry alone
// creates and deletes nodes. It is not thread-safe; DatabaseEngine uses it with
// the database mutex held.
class SnapshotRegistry final {
public:
    class Registration final {
    public:
        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;
        Registration(Registration&&) = delete;
        Registration& operator=(Registration&&) = delete;

        [[nodiscard]] SequenceNumber sequence() const noexcept { return sequence_; }

    private:
        friend class SnapshotRegistry;

        Registration(SnapshotRegistry& registry, SequenceNumber sequence) noexcept
            : registry_(&registry), sequence_(sequence) {}
        ~Registration() = default;

        SnapshotRegistry* registry_;
        SequenceNumber sequence_;
        Registration* previous_ = nullptr;
        Registration* next_ = nullptr;
    };

    SnapshotRegistry() noexcept;
    SnapshotRegistry(const SnapshotRegistry&) = delete;
    SnapshotRegistry& operator=(const SnapshotRegistry&) = delete;
    SnapshotRegistry(SnapshotRegistry&&) = delete;
    SnapshotRegistry& operator=(SnapshotRegistry&&) = delete;
    ~SnapshotRegistry();

    // Requires a sequence not below the newest active registration.
    [[nodiscard]] Registration* Register(SequenceNumber sequence);
    // Requires a live registration returned by this registry. Clears the handle.
    void Release(Registration*& registration) noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::optional<SequenceNumber> oldest() const noexcept;
    [[nodiscard]] SequenceNumber OldestOr(SequenceNumber fallback) const noexcept;

private:
    Registration sentinel_;
    std::size_t size_ = 0;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_SNAPSHOT_REGISTRY_H_
