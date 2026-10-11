#ifndef MODERN_LEVELDB_ENGINE_SNAPSHOT_REGISTRY_H_
#define MODERN_LEVELDB_ENGINE_SNAPSHOT_REGISTRY_H_

#include <cstddef>
#include <optional>
#include <set>

#include "format/internal_key.h"

namespace modern_leveldb {

// Tracks every active snapshot sequence, including duplicate registrations.
// It is not thread-safe; DatabaseEngine uses it with the database mutex held.
class SnapshotRegistry final {
public:
    void Register(SequenceNumber sequence);
    // Requires one matching registration.
    void Release(SequenceNumber sequence);

    [[nodiscard]] std::size_t size() const noexcept { return sequences_.size(); }
    [[nodiscard]] std::optional<SequenceNumber> oldest() const noexcept;
    [[nodiscard]] SequenceNumber OldestOr(SequenceNumber fallback) const noexcept;

private:
    std::multiset<SequenceNumber> sequences_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_SNAPSHOT_REGISTRY_H_
