#ifndef MODERN_LEVELDB_ENGINE_SNAPSHOT_REGISTRY_H_
#define MODERN_LEVELDB_ENGINE_SNAPSHOT_REGISTRY_H_

#include <cstddef>
#include <list>
#include <optional>

#include "format/internal_key.h"

namespace modern_leveldb {

// Tracks every active snapshot sequence, including duplicate registrations.
// It is not thread-safe; DatabaseEngine uses it with the database mutex held.
class SnapshotRegistry final {
    using Sequences = std::list<SequenceNumber>;

public:
    SnapshotRegistry() = default;
    SnapshotRegistry(const SnapshotRegistry&) = delete;
    SnapshotRegistry& operator=(const SnapshotRegistry&) = delete;
    SnapshotRegistry(SnapshotRegistry&&) = delete;
    SnapshotRegistry& operator=(SnapshotRegistry&&) = delete;
    ~SnapshotRegistry() = default;

    class Registration final {
    public:
        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;
        Registration(Registration&& source) noexcept;
        Registration& operator=(Registration&&) = delete;
        ~Registration() = default;

        [[nodiscard]] SequenceNumber sequence() const noexcept;

    private:
        friend class SnapshotRegistry;

        Registration(SnapshotRegistry& owner, Sequences::iterator position) noexcept
            : owner_(&owner), sequence_(*position), position_(position) {}

        SnapshotRegistry* owner_;
        SequenceNumber sequence_;
        std::optional<Sequences::iterator> position_;
    };

    // Requires a sequence not below the newest active registration.
    [[nodiscard]] Registration Register(SequenceNumber sequence);
    // Requires a live registration returned by this registry.
    void Release(Registration& registration) noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return sequences_.size(); }
    [[nodiscard]] std::optional<SequenceNumber> oldest() const noexcept;
    [[nodiscard]] SequenceNumber OldestOr(SequenceNumber fallback) const noexcept;

private:
    Sequences sequences_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_SNAPSHOT_REGISTRY_H_
