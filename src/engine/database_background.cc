#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/result_internal.h"
#include "engine/compaction.h"
#include "engine/compaction_picker.h"
#include "engine/database.h"
#include "engine/flush.h"
#include "metadata/filenames.h"
#include "metadata/version.h"
#include "metadata/version_edit.h"
#include "platform/file_system.h"

namespace modern_leveldb {
namespace {

bool KeepFile(const ParsedFileName& file, const std::set<std::uint64_t>& live,
              std::uint64_t log_number, std::uint64_t manifest_number) {
    switch (file.type) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/5 default of an exhaustive switch
        case FileType::Log:
            return file.number >= log_number;
        case FileType::Descriptor:
            return file.number >= manifest_number;
        case FileType::Table:
        case FileType::Temp:
            return live.contains(file.number);
        case FileType::Lock:
        case FileType::Current:
            return true;
    }
    // GCOVR_EXCL_START: ParseNativeFileName returns an exhaustive FileType
    return true;
    // GCOVR_EXCL_STOP
}

}  // namespace

Status DatabaseEngine::WaitForBackgroundWork() {
    std::unique_lock lock(mutex_);
    background_finished_.wait(lock, [this] { return !background_scheduled_; });
    if (sticky_error_.has_value()) {
        return StickyError();
    }
    return {};
}

bool DatabaseEngine::NeedsCompaction() const {
    const Version& current = *versions_->current_raw();
    if (ScoreCompaction(current).score >= 1) {
        return true;
    }
    return seek_statistics_.HasFileToCompact(current);
}

void DatabaseEngine::MaybeScheduleBackgroundWork() {
    if (background_scheduled_ || closing_ || sticky_error_.has_value()) {
        return;
    }
    if (!write_buffers_.has_immutable() && !NeedsCompaction()) {
        return;
    }
    background_scheduled_ = true;
    Status scheduled;
    try {
        scheduled = executor_.get()->Schedule([this](std::stop_token) { BackgroundCall(); });
    } catch (...) {
        scheduled =
            std::unexpected(Error::Aborted("scheduling background work threw an exception"));
    }
    if (!scheduled.has_value()) {
        background_scheduled_ = false;
        RecordStickyError(scheduled.error());
    }
}

void DatabaseEngine::BackgroundCall() {
    std::unique_lock lock(mutex_);
    if (!closing_ && !sticky_error_.has_value()) {
        try {
            if (write_buffers_.has_immutable()) {
                FlushImmutable(lock);
            } else {
                BackgroundCompaction(lock);
            }
        } catch (...) {
            if (!lock.owns_lock()) {
                lock.lock();
            }
            RecordStickyError(Error::Aborted("background work stopped with an exception"));
        }
    }
    background_scheduled_ = false;
    MaybeScheduleBackgroundWork();
    background_finished_.notify_all();
}

void DatabaseEngine::FlushImmutable(std::unique_lock<std::mutex>& lock) {
    const std::shared_ptr<const Version> base = versions_->current();
    const std::uint64_t number = versions_->NewFileNumber();
    // Retain the output number before unlocked I/O: cleanup must not mistake
    // an unfinished/uninstalled table for an obsolete file.
    pending_outputs_.insert(number);
    const std::shared_ptr<const MemTable> immutable = write_buffers_.immutable_owner();
    const std::uint64_t log_number = write_buffers_.log_number();
    lock.unlock();
    FlushOptions options;
    options.table_options = policy_.table_options;
    options.target_file_size = policy_.max_file_size;
    Result<VersionEdit> edit =
        modern_leveldb::FlushMemTable(*file_system_.get(), directory_, comparator_, options,
                                      table_cache_, *immutable, number, *base, log_number);
    lock.lock();
    const Status applied =
        ApplyBackgroundEdit(std::move(edit), lock, "the database closed during a flush");
    if (!applied.has_value()) {
        // Recorded first, so that no cleanup removes a table that the edit may
        // still reference.
        RecordStickyError(applied.error());
        pending_outputs_.erase(number);
        return;
    }
    pending_outputs_.erase(number);
    write_buffers_.ClearImmutable();
    seek_statistics_.Retain(*versions_->current());
    background_finished_.notify_all();
    RemoveObsoleteFiles(lock);
}

void DatabaseEngine::BackgroundCompaction(std::unique_lock<std::mutex>& lock) {
    std::vector<std::uint64_t> outputs;
    const bool compacted = Compact(lock, outputs);
    for (const std::uint64_t number : outputs) {
        pending_outputs_.erase(number);
    }
    if (compacted) {
        RemoveObsoleteFiles(lock);
    }
}

bool DatabaseEngine::Compact(std::unique_lock<std::mutex>& lock,
                             std::vector<std::uint64_t>& outputs) {
    const std::shared_ptr<const Version> current = versions_->current();
    const std::optional<Compaction> compaction =
        PickCompaction(current, comparator_, versions_->compact_pointers(),
                       seek_statistics_.FileToCompact(*current), policy_.max_file_size);
    // The need that scheduled this task remains, since only background work
    // installs versions or drops the file to compact.
    if (IsTrivialMove(*compaction, policy_.max_file_size)) {
        InstallTrivialCompaction(*compaction, lock);
        return false;
    }
    RewriteCompaction(*compaction, lock, outputs);
    return true;
}

CompactionHooks DatabaseEngine::MakeCompactionHooks(std::vector<std::uint64_t>& outputs) {
    CompactionHooks hooks;
    hooks.new_file_number = [this, &outputs] {
        const std::lock_guard guard(mutex_);
        const std::uint64_t number = versions_->NewFileNumber();
        pending_outputs_.insert(number);
        outputs.push_back(number);
        return number;
    };
    hooks.before_entry = [this] { return BeforeCompactionEntry(); };
    return hooks;
}

void DatabaseEngine::RewriteCompaction(const Compaction& compaction,
                                       std::unique_lock<std::mutex>& lock,
                                       std::vector<std::uint64_t>& outputs) {
    const SequenceNumber smallest_snapshot = snapshots_.OldestOr(versions_->last_sequence());
    const std::unique_ptr<InternalIterator> input =
        NewCompactionIterator(compaction, table_cache_, comparator_);
    CompactionOptions options;
    options.table_options = policy_.table_options;
    options.target_file_size = policy_.max_file_size;
    CompactionHooks hooks = MakeCompactionHooks(outputs);
    lock.unlock();
    Result<VersionEdit> edit =
        RunCompaction(*file_system_.get(), directory_, comparator_, options, table_cache_,
                      compaction, *input, smallest_snapshot, hooks);
    lock.lock();
    const Status applied =
        ApplyBackgroundEdit(std::move(edit), lock, "the database closed during a compaction");
    FinishCompaction(applied);
}

void DatabaseEngine::InstallTrivialCompaction(const Compaction& compaction,
                                              std::unique_lock<std::mutex>& lock) {
    VersionEdit edit = CompactionEdit(compaction);
    // The file is valid at the next level, which nothing there overlaps.
    static_cast<void>(edit.AddFile(compaction.level + 1, *compaction.inputs[0].front()));
    FinishCompaction(versions_->LogAndApply(std::move(edit), lock));
}

Status DatabaseEngine::ApplyBackgroundEdit(Result<VersionEdit> edit,
                                           std::unique_lock<std::mutex>& lock,
                                           std::string_view closing_error) {
    if (!edit.has_value()) {
        return std::unexpected(std::move(edit).error());
    }
    if (closing_) {
        // Closing can begin after the last per-entry check; do not install a
        // completed output into an engine already shutting down.
        return std::unexpected(Error::Aborted(std::string(closing_error)));
    }
    return versions_->LogAndApply(std::move(*edit), lock);
}

void DatabaseEngine::FinishCompaction(const Status& applied) {
    if (!applied.has_value()) {
        RecordStickyError(applied.error());
        return;
    }
    seek_statistics_.Retain(*versions_->current());
}

Status DatabaseEngine::BeforeCompactionEntry() {
    if (closing_) {
        return std::unexpected(Error::Aborted("the database is closing"));
    }
    if (!write_buffers_.HasImmutableForCompaction()) {
        return {};
    }
    std::unique_lock lock(mutex_);
    // Only background work, which this is, drops the immutable memtable.
    FlushImmutable(lock);
    if (sticky_error_.has_value()) {
        return StickyError();
    }
    return {};
}

void DatabaseEngine::RemoveObsoleteFiles(std::unique_lock<std::mutex>& lock) {
    // Uncertain metadata failure forbids reclamation: either durable topology
    // may still need the files. Otherwise retain all pinned Versions and outputs.
    if (sticky_error_.has_value()) {
        return;
    }
    const Result<std::vector<std::filesystem::path>> obsolete = CollectObsoleteFiles();
    if (!obsolete.has_value()) {
        return;
    }
    lock.unlock();
    for (const std::filesystem::path& path : *obsolete) {
        static_cast<void>(file_system_.get()->RemoveFile(path));
    }
    lock.lock();
}

Result<std::vector<std::filesystem::path>> DatabaseEngine::CollectObsoleteFiles() {
    // Recovery and every flush set the previous log number to zero.
    std::set<std::uint64_t> live = versions_->LiveFiles();
    live.insert(pending_outputs_.begin(), pending_outputs_.end());
    const Result<std::vector<std::filesystem::path>> children =
        file_system_.get()->ListDirectory(directory_);
    if (!children.has_value()) {
        return std::unexpected(children.error());
    }
    std::vector<std::filesystem::path> obsolete;
    for (const std::filesystem::path& child : *children) {
        const std::optional<ParsedFileName> parsed = ParseNativeFileName(child);
        if (!parsed.has_value()) {
            continue;
        }
        if (KeepFile(*parsed, live, versions_->log_number(), versions_->manifest_file_number())) {
            continue;
        }
        if (parsed->type == FileType::Table) {
            table_cache_.Evict(parsed->number);
        }
        obsolete.push_back(directory_ / child.filename());
    }
    return obsolete;
}

void DatabaseEngine::RecordStickyError(Error error) {
    // Keeps the first error.
    sticky_error_ = sticky_error_.value_or(std::move(error));
    background_finished_.notify_all();
}

std::unexpected<Error> DatabaseEngine::StickyError() const {
    return std::unexpected(*sticky_error_);
}

}  // namespace modern_leveldb
