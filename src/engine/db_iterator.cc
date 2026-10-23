#include "engine/db_iterator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "modern_leveldb/base/coding.h"

namespace modern_leveldb {
namespace {

ByteView UserKey(ByteView internal_key) noexcept {
    return internal_key.first(internal_key.size() - InternalKeyTrailerSize);
}

void Assign(std::vector<std::byte>& destination, ByteView source) {
    destination.assign(source.begin(), source.end());
}

void Assign(std::string& destination, ByteView source) { destination.assign(AsStringView(source)); }

}  // namespace

DbIterator::DbIterator(std::unique_ptr<InternalIterator> internal,
                       const Comparator& user_comparator, SequenceNumber sequence,
                       ReadSampling sampling) noexcept
    : internal_(std::move(internal)),
      user_comparator_(&user_comparator),
      sequence_(sequence),
      sampling_(std::move(sampling)) {}

void DbIterator::CountRead(ByteView key, ByteView value) {
    if (!sampling_.sample) {
        return;
    }
    const std::uint64_t bytes = key.size() + value.size();
    if (!bytes_until_sample_.has_value()) {
        bytes_until_sample_ = sampling_.next_period();
    }
    while (*bytes_until_sample_ < bytes) {
        *bytes_until_sample_ += sampling_.next_period();
        sampling_.sample(key);
    }
    *bytes_until_sample_ -= bytes;
}

ByteView DbIterator::key() const noexcept {
    if (forward_) {
        return UserKey(internal_->key());
    }
    return AsBytes(saved_key_);
}

ByteView DbIterator::value() const noexcept {
    if (forward_) {
        return internal_->value();
    }
    return saved_value_;
}

Status DbIterator::SeekToFirst() {
    valid_ = false;
    forward_ = true;
    saved_value_.clear();
    const Status moved = internal_->SeekToFirst();
    if (!moved.has_value()) {
        return Fail(moved.error());
    }
    return FindNextUserEntry(false);
}

Status DbIterator::SeekToLast() {
    valid_ = false;
    forward_ = false;
    saved_value_.clear();
    const Status moved = internal_->SeekToLast();
    if (!moved.has_value()) {
        return Fail(moved.error());
    }
    return FindPrevUserEntry();
}

Status DbIterator::Seek(const ByteView& user_key) {
    valid_ = false;
    forward_ = true;
    saved_value_.clear();
    // The newest entry of the user key that the sequence sees sorts first.
    Assign(saved_key_, user_key);
    std::array<std::byte, InternalKeyTrailerSize> trailer;
    EncodeFixed64(trailer, PackInternalKeyTrailer(sequence_, SeekValueKind));
    saved_key_.append(AsStringView(trailer));
    const Status moved = internal_->Seek(AsBytes(saved_key_));
    if (!moved.has_value()) {
        return Fail(moved.error());
    }
    return FindNextUserEntry(false);
}

Status DbIterator::Next() {
    valid_ = false;
    Status moved = AdvanceForNext();
    if (!moved.has_value()) {
        return Fail(std::move(moved).error());
    }
    return FindNextUserEntry(true);
}

Status DbIterator::AdvanceForNext() {
    if (forward_) {
        Assign(saved_key_, UserKey(internal_->key()));
        const Status moved = internal_->Next();
        if (!moved.has_value()) {
            return std::unexpected(moved.error());
        }
    } else {
        // The internal iterator is before the saved key's entries.
        forward_ = true;
        Status moved;
        if (internal_->valid()) {
            moved = internal_->Next();
        } else {
            moved = internal_->SeekToFirst();
        }
        if (!moved.has_value()) {
            return std::unexpected(moved.error());
        }
    }
    return {};
}

Status DbIterator::Prev() {
    valid_ = false;
    if (forward_) {
        Result<bool> positioned = PositionBeforeCurrentUser();
        if (!positioned.has_value()) {
            return Fail(std::move(positioned).error());
        }
        if (!*positioned) {
            return {};
        }
    }
    return FindPrevUserEntry();
}

Result<bool> DbIterator::PositionBeforeCurrentUser() {
    Assign(saved_key_, UserKey(internal_->key()));
    while (true) {
        const Status moved = internal_->Prev();
        if (!moved.has_value()) {
            return std::unexpected(moved.error());
        }
        if (!internal_->valid()) {
            valid_ = false;
            saved_key_.clear();
            saved_value_.clear();
            return false;
        }
        const Result<InternalKeyView> parsed = ParseInternalKey(internal_->key());
        if (!parsed.has_value()) {
            return std::unexpected(parsed.error());
        }
        if (user_comparator_->Compare(parsed->user_key, AsBytes(saved_key_)) < 0) {
            break;
        }
    }
    forward_ = false;
    return true;
}

// While skipping, saved_key_ holds the user key whose entries are skipped.
Status DbIterator::FindNextUserEntry(bool skipping) {
    while (internal_->valid()) {
        const Result<InternalKeyView> parsed = ParseInternalKey(internal_->key());
        if (!parsed.has_value()) {
            return Fail(parsed.error());
        }
        CountRead(internal_->key(), internal_->value());
        if (ConsiderNextEntry(*parsed, skipping)) {
            return {};
        }
        const Status moved = internal_->Next();
        if (!moved.has_value()) {
            return Fail(moved.error());
        }
    }
    valid_ = false;
    saved_key_.clear();
    return {};
}

bool DbIterator::ConsiderNextEntry(const InternalKeyView& entry, bool& skipping) {
    if (entry.sequence <= sequence_) {
        if (entry.kind == ValueKind::Deletion) {
            Assign(saved_key_, entry.user_key);
            skipping = true;
        } else if (!skipping ||
                   user_comparator_->Compare(entry.user_key, AsBytes(saved_key_)) > 0) {
            valid_ = true;
            saved_key_.clear();
            return true;
        }
    }
    return false;
}

// Walks back over the entries of each user key, keeping the newest visible
// one, until an earlier user key has a visible entry.
Status DbIterator::FindPrevUserEntry() {
    ValueKind kind = ValueKind::Deletion;
    while (internal_->valid()) {
        const Result<InternalKeyView> parsed = ParseInternalKey(internal_->key());
        if (!parsed.has_value()) {
            return Fail(parsed.error());
        }
        CountRead(internal_->key(), internal_->value());
        if (ConsiderPreviousEntry(*parsed, kind)) {
            break;
        }
        const Status moved = internal_->Prev();
        if (!moved.has_value()) {
            return Fail(moved.error());
        }
    }
    FinishReverseScan(kind);
    return {};
}

bool DbIterator::ConsiderPreviousEntry(const InternalKeyView& entry, ValueKind& kind) {
    if (entry.sequence <= sequence_) {
        if (kind != ValueKind::Deletion &&
            user_comparator_->Compare(entry.user_key, AsBytes(saved_key_)) < 0) {
            return true;
        }
        kind = entry.kind;
        if (kind == ValueKind::Deletion) {
            saved_key_.clear();
            saved_value_.clear();
        } else {
            Assign(saved_key_, entry.user_key);
            Assign(saved_value_, internal_->value());
        }
    }
    return false;
}

void DbIterator::FinishReverseScan(ValueKind kind) {
    if (kind == ValueKind::Deletion) {
        valid_ = false;
        forward_ = true;
        saved_key_.clear();
        saved_value_.clear();
        return;
    }
    valid_ = true;
}

Status DbIterator::Fail(Error error) {
    valid_ = false;
    forward_ = true;
    saved_key_.clear();
    saved_value_.clear();
    return std::unexpected(std::move(error));
}

}  // namespace modern_leveldb
