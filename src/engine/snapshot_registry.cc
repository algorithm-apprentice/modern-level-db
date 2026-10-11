#include "engine/snapshot_registry.h"

#include <cassert>

namespace modern_leveldb {

SnapshotRegistry::SnapshotRegistry() noexcept : sentinel_(*this, 0) {
    sentinel_.previous_ = &sentinel_;
    sentinel_.next_ = &sentinel_;
}

SnapshotRegistry::~SnapshotRegistry() {
    assert(size_ == 0);  // GCOVR_EXCL_BR_WITHOUT_HIT: registrations retain the engine
}

SnapshotRegistry::Registration* SnapshotRegistry::Register(SequenceNumber sequence) {
    assert(size_ == 0 || sentinel_.previous_->sequence_ <=
                             sequence);  // GCOVR_EXCL_BR_WITHOUT_HIT: monotonic database sequence
    Registration* const registration = new Registration(*this, sequence);
    registration->previous_ = sentinel_.previous_;
    registration->next_ = &sentinel_;
    registration->previous_->next_ = registration;
    sentinel_.previous_ = registration;
    ++size_;
    return registration;
}

void SnapshotRegistry::Release(Registration*& registration) noexcept {
    assert(registration != nullptr);  // GCOVR_EXCL_BR_WITHOUT_HIT: live handle
    assert(registration->registry_ ==
           this);  // GCOVR_EXCL_BR_WITHOUT_HIT: registration-owner precondition
    registration->previous_->next_ = registration->next_;
    registration->next_->previous_ = registration->previous_;
    registration->registry_ = nullptr;
    Registration* const released = registration;
    registration = nullptr;
    delete released;
    --size_;
}

std::optional<SequenceNumber> SnapshotRegistry::oldest() const noexcept {
    if (size_ == 0) {
        return std::nullopt;
    }
    return sentinel_.next_->sequence_;
}

SequenceNumber SnapshotRegistry::OldestOr(SequenceNumber fallback) const noexcept {
    return size_ == 0 ? fallback : sentinel_.next_->sequence_;
}

}  // namespace modern_leveldb
