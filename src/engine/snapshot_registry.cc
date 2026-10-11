#include "engine/snapshot_registry.h"

#include <cassert>

namespace modern_leveldb {

void SnapshotRegistry::Register(SequenceNumber sequence) { sequences_.insert(sequence); }

void SnapshotRegistry::Release(SequenceNumber sequence) {
    const auto found = sequences_.find(sequence);
    assert(found != sequences_.end());  // GCOVR_EXCL_BR_WITHOUT_HIT: registered precondition
    sequences_.erase(found);
}

std::optional<SequenceNumber> SnapshotRegistry::oldest() const noexcept {
    if (sequences_.empty()) {
        return std::nullopt;
    }
    return *sequences_.begin();
}

SequenceNumber SnapshotRegistry::OldestOr(SequenceNumber fallback) const noexcept {
    return sequences_.empty() ? fallback : *sequences_.begin();
}

}  // namespace modern_leveldb
