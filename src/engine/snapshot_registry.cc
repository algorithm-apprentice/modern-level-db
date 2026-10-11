#include "engine/snapshot_registry.h"

#include <cassert>
#include <utility>

namespace modern_leveldb {

SnapshotRegistry::~SnapshotRegistry() {
    assert(size_ == 0);  // GCOVR_EXCL_BR_WITHOUT_HIT: registrations retain the engine
}

SnapshotRegistry::Registration::Registration(Node& node) noexcept
    : node_(&node), sequence_(node.sequence) {}

SnapshotRegistry::Registration::Registration(Registration&& source) noexcept
    : node_(std::exchange(source.node_, nullptr)), sequence_(source.sequence_) {}

SnapshotRegistry::Registration::~Registration() {
    assert(node_ == nullptr);  // GCOVR_EXCL_BR_WITHOUT_HIT: release-before-destruction contract
}

SnapshotRegistry::Registration SnapshotRegistry::Register(SequenceNumber sequence) {
    assert(newest_ == nullptr ||
           newest_->sequence <=
               sequence);  // GCOVR_EXCL_BR_WITHOUT_HIT: monotonic database sequence
    auto node = std::make_unique<Node>(*this, sequence);
    Node* const registered = node.get();
    registered->previous = newest_;
    if (newest_ != nullptr) {
        newest_->next = std::move(node);
    } else {
        oldest_ = std::move(node);
    }
    newest_ = registered;
    ++size_;
    return Registration(*registered);
}

void SnapshotRegistry::Release(Registration& registration) noexcept {
    Node* const node = registration.node_;
    assert(node != nullptr);         // GCOVR_EXCL_BR_WITHOUT_HIT: live-registration precondition
    assert(node->registry == this);  // GCOVR_EXCL_BR_WITHOUT_HIT: registration-owner precondition

    std::unique_ptr<Node>* owner = node->previous != nullptr ? &node->previous->next : &oldest_;
    std::unique_ptr<Node> removed = std::move(*owner);
    assert(removed.get() == node);  // GCOVR_EXCL_BR_WITHOUT_HIT: owning-link invariant
    *owner = std::move(removed->next);
    if (*owner != nullptr) {
        (*owner)->previous = removed->previous;
    } else {
        newest_ = removed->previous;
    }
    removed->registry = nullptr;
    removed->previous = nullptr;
    registration.node_ = nullptr;
    --size_;
}

std::optional<SequenceNumber> SnapshotRegistry::oldest() const noexcept {
    if (oldest_ == nullptr) {
        return std::nullopt;
    }
    return oldest_->sequence;
}

SequenceNumber SnapshotRegistry::OldestOr(SequenceNumber fallback) const noexcept {
    return oldest_ == nullptr ? fallback : oldest_->sequence;
}

}  // namespace modern_leveldb
