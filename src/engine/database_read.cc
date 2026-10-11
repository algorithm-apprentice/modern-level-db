#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "engine/database.h"
#include "engine/iterators.h"
#include "engine/lookup.h"
#include "metadata/version.h"

namespace modern_leveldb {
namespace {

TableReadOptions ReadOptionsFor(const DatabaseEngineReadOptions& options) {
    TableReadOptions read_options;
    read_options.fill_cache = options.fill_cache;
    return read_options;
}

Result<std::uint64_t> LevelFileBytes(std::span<const Version::File> files, std::uint32_t level) {
    std::uint64_t total = 0;
    for (const Version::File& file : files) {
        if (file->file_size > std::numeric_limits<std::uint64_t>::max() - total) {
            return std::unexpected(
                Error::Aborted("level " + std::to_string(level) + " file bytes exceed uint64"));
        }
        total += file->file_size;
    }
    return total;
}

}  // namespace

// GCOVR_EXCL_START: GCC emits duplicate constructor/destructor ABI clones
DatabaseEngine::ReadSources::ReadSources(DatabaseEngine& engine) noexcept : engine_(&engine) {
    memtable_.emplace(engine.write_buffers_.mutable_table().PinRead());
    if (engine.write_buffers_.has_immutable()) {
        immutable_.emplace(engine.write_buffers_.immutable_owner()->PinRead());
    }
    version_.emplace(engine.versions_->PinCurrent());
}

DatabaseEngine::ReadSources::~ReadSources() {
    version_.reset();
    immutable_.reset();
    memtable_.reset();
    engine_->write_buffers_.ReleaseReadPinnedMemtables();
}
// GCOVR_EXCL_STOP

const MemTable& DatabaseEngine::ReadSources::memtable() const noexcept {
    return memtable_->value();
}

const MemTable* DatabaseEngine::ReadSources::immutable() const noexcept {
    return immutable_.has_value() ? &immutable_->value() : nullptr;
}

const Version& DatabaseEngine::ReadSources::version() const noexcept { return version_->value(); }

Result<bool> DatabaseEngine::Get(ByteView key, std::vector<std::byte>& value,
                                 const DatabaseEngineReadOptions& options) {
    std::unique_lock lock(mutex_);
    const SequenceNumber sequence =
        options.snapshot.has_value() ? *options.snapshot : versions_->last_sequence();
    Result<bool> read;
    {
        ReadSources sources(*this);
        std::optional<SeekCharge> seek;
        lock.unlock();
        try {
            LookupKey lookup_key = LookupKey::CreateTrusted(key, sequence);
            read = LookupValue(sources.memtable(), sources.immutable(), sources.version(),
                               table_cache_, comparator_, lookup_key, value, seek,
                               ReadOptionsFor(options));
        } catch (...) {
            lock.lock();
            throw;
        }
        lock.lock();
        if (seek.has_value() &&
            seek_statistics_.Charge(sources.version(), *versions_->current_raw(), *seek)) {
            MaybeScheduleBackgroundWork();
        }
    }
    if (!read.has_value()) {
        return std::unexpected(std::move(read).error());
    }
    return read;
}

std::unique_ptr<DbIterator> DatabaseEngine::NewIterator(const DatabaseEngineReadOptions& options) {
    std::unique_lock lock(mutex_);
    const SequenceNumber sequence =
        options.snapshot.has_value() ? *options.snapshot : versions_->last_sequence();
    std::unique_ptr<InternalIterator> internal = NewInternalIterator(
        write_buffers_.mutable_owner(), write_buffers_.immutable_owner(), versions_->current(),
        table_cache_, comparator_, ReadOptionsFor(options));
    ReadSampling sampling;
    sampling.next_period = ReadSamplingPeriods(++seed_);
    sampling.sample = [this](ByteView internal_key) { RecordReadSample(internal_key); };
    lock.unlock();
    return std::make_unique<DbIterator>(std::move(internal), comparator_.user_comparator(),
                                        sequence, std::move(sampling));
}

void DatabaseEngine::RecordReadSample(ByteView internal_key) {
    const std::lock_guard lock(mutex_);
    const Version& current = *versions_->current_raw();
    const std::optional<SeekCharge> charge = SampleCharge(current, comparator_, internal_key);
    if (charge.has_value() && seek_statistics_.Charge(current, current, *charge)) {
        MaybeScheduleBackgroundWork();
    }
}

SequenceNumber DatabaseEngine::GetSnapshot() {
    const std::lock_guard lock(mutex_);
    const SequenceNumber sequence = versions_->last_sequence();
    snapshots_.insert(sequence);
    return sequence;
}

void DatabaseEngine::ReleaseSnapshot(SequenceNumber snapshot) {
    const std::lock_guard lock(mutex_);
    const auto found = snapshots_.find(snapshot);
    assert(found != snapshots_.end());
    snapshots_.erase(found);
}

Result<DatabaseEngineState> DatabaseEngine::GetState() {
    const std::lock_guard lock(mutex_);
    DatabaseEngineState state;
    const Version& current = *versions_->current_raw();
    for (std::uint32_t level = 0; level < NumLevels; ++level) {
        const std::span<const Version::File> files = current.files(level);
        Result<std::uint64_t> bytes = LevelFileBytes(files, level);
        if (!bytes.has_value()) {
            return std::unexpected(std::move(bytes).error());
        }
        state.levels[level] = {
            .file_count = files.size(),
            .file_bytes = *bytes,
        };
    }

    state.last_sequence = versions_->last_sequence();
    state.snapshot_count = snapshots_.size();
    if (!snapshots_.empty()) {
        state.oldest_snapshot_sequence = *snapshots_.begin();
    }
    state.write_queue_depth = write_queue_.size();
    state.mutable_memtable_bytes = write_buffers_.published_mutable_bytes();
    if (write_buffers_.has_immutable()) {
        state.immutable_memtable_bytes = write_buffers_.immutable_owner()->memory_usage();
    }
    state.protected_output_count = pending_outputs_.size();
    state.background_work_scheduled = background_scheduled_;
    state.sticky_error = background_error_;
    return state;
}

}  // namespace modern_leveldb
