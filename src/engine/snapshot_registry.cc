#include "engine/snapshot_registry.h"

namespace modern_leveldb {

SnapshotRegistry::SnapshotRegistry() noexcept : sentinel_(0) {
    sentinel_.previous_ = &sentinel_;
    sentinel_.next_ = &sentinel_;
}

SnapshotRegistry::Registration* SnapshotRegistry::Register(SequenceNumber sequence) {
    Registration* const registration = new Registration(sequence);
    registration->previous_ = sentinel_.previous_;
    registration->next_ = &sentinel_;
    registration->previous_->next_ = registration;
    sentinel_.previous_ = registration;
    ++size_;
    return registration;
}

void SnapshotRegistry::Release(Registration*& registration) noexcept {
    registration->previous_->next_ = registration->next_;
    registration->next_->previous_ = registration->previous_;
    Registration* const released = registration;
    registration = nullptr;
    delete released;  // GCOVR_EXCL_LINE: GCC attributes trivial private destruction elsewhere
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
