#include "engine/snapshot_registry.h"

#include <cassert>
#include <iterator>
#include <utility>

namespace modern_leveldb {

SnapshotRegistry::Registration::Registration(Registration&& source) noexcept
    : owner_(std::exchange(source.owner_, nullptr)),
      sequence_(source.sequence_),
      position_(std::exchange(source.position_, std::nullopt)) {}

SequenceNumber SnapshotRegistry::Registration::sequence() const noexcept { return sequence_; }

SnapshotRegistry::Registration SnapshotRegistry::Register(SequenceNumber sequence) {
    assert(sequences_.empty() ||
           sequences_.back() <= sequence);  // GCOVR_EXCL_BR_WITHOUT_HIT: monotonic DB sequence
    sequences_.push_back(sequence);
    return Registration(*this, std::prev(sequences_.end()));
}

void SnapshotRegistry::Release(Registration& registration) noexcept {
    assert(registration.owner_ ==
           this);  // GCOVR_EXCL_BR_WITHOUT_HIT: registration-owner precondition
    assert(registration.position_.has_value());  // GCOVR_EXCL_BR_WITHOUT_HIT: live token
    sequences_.erase(*registration.position_);
    registration.position_.reset();
    registration.owner_ = nullptr;
}

std::optional<SequenceNumber> SnapshotRegistry::oldest() const noexcept {
    if (sequences_.empty()) {
        return std::nullopt;
    }
    return sequences_.front();
}

SequenceNumber SnapshotRegistry::OldestOr(SequenceNumber fallback) const noexcept {
    return sequences_.empty() ? fallback : sequences_.front();
}

}  // namespace modern_leveldb
