#include "metadata/version_set.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "base/result_internal.h"
#include "metadata/filenames.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"

namespace modern_leveldb {
namespace {

Result<std::size_t> ReadCurrentChunk(SequentialFile& file, MutableByteView output) {
    const Result<std::size_t> read = file.Read(output);
    if (!read.has_value()) {
        return std::unexpected(read.error());
    }
    if (*read > output.size()) {
        return std::unexpected(Error::Io("CURRENT returned an oversized read"));
    }
    return *read;
}

Result<std::string> ReadCurrentContents(SequentialFile& file) {
    std::string contents;
    std::array<std::byte, 64> buffer{};
    while (true) {
        Result<std::size_t> read = ReadCurrentChunk(file, buffer);
        if (!read.has_value()) {
            return std::unexpected(std::move(read).error());
        }
        if (*read == 0) {
            return contents;
        }
        contents.append(AsStringView(ByteView(buffer).first(*read)));
    }
}

Result<std::string> ReadCurrent(FileSystem& file_system, const std::filesystem::path& directory) {
    Result<std::unique_ptr<SequentialFile>> file =
        file_system.OpenSequential(CurrentFileName(directory));
    if (!file.has_value()) {
        return std::unexpected(std::move(file).error());
    }
    return ReadCurrentContents(**file);
}

Status WriteCurrentTemporary(WritableFile& file, ByteView contents) {
    const Status appended = file.Append(contents);
    if (!appended.has_value()) {
        return appended;
    }
    const Status synced = file.Sync();
    if (!synced.has_value()) {
        return synced;
    }
    const Status closed = file.Close();
    if (!closed.has_value()) {
        return closed;
    }
    return {};
}

struct SelectedManifest {
    std::uint64_t number;
    std::unique_ptr<SequentialFile> file;
};

Result<SelectedManifest> OpenSelectedManifest(FileSystem& file_system,
                                              const std::filesystem::path& directory) {
    const Result<std::string> current = ReadCurrent(file_system, directory);
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    const Result<std::uint64_t> number = ParseCurrentFileContents(*current);
    if (!number.has_value()) {
        return std::unexpected(number.error());
    }
    if (*number >= FileNumberLimit) {
        return std::unexpected(
            Error::Corruption("CURRENT names a MANIFEST beyond the file number limit"));
    }
    Result<std::unique_ptr<SequentialFile>> file =
        file_system.OpenSequential(DescriptorFileName(directory, *number));
    if (!file.has_value()) {
        if (file.error().code() == ErrorCode::NotFound) {
            return std::unexpected(Error::Corruption("CURRENT names a missing MANIFEST: " +
                                                     std::string(file.error().message())));
        }
        return std::unexpected(std::move(file).error());
    }
    SelectedManifest selected{.number = *number, .file = std::move(*file)};
    return selected;
}

Status CheckFileNumberCapacity(std::uint64_t next_file_number) {
    if (next_file_number > FileNumberLimit) {
        return std::unexpected(Error::InvalidArgument("file numbers are exhausted"));
    }
    return {};
}

Status CheckComparatorName(const std::optional<std::string>& name, const Comparator& comparator) {
    if (name.has_value() && *name != comparator.Name()) {
        return std::unexpected(Error::InvalidArgument("edit names comparator " + *name));
    }
    return {};
}

Status CheckCurrentLogNumber(const std::optional<std::uint64_t>& edited_log_number,
                             std::uint64_t current_log_number, std::uint64_t next_file_number) {
    const std::uint64_t log_number = edited_log_number.value_or(current_log_number);
    if (log_number < current_log_number || log_number >= next_file_number) {
        return std::unexpected(Error::InvalidArgument(
            "edit's log number " + std::to_string(log_number) + " is below the current one or " +
            "not below the next file number"));
    }
    return {};
}

Status CheckPreviousLogNumber(const std::optional<std::uint64_t>& edited_log_number,
                              std::uint64_t next_file_number) {
    if (edited_log_number.value_or(0) >= next_file_number) {
        return std::unexpected(
            Error::InvalidArgument("edit's previous log number is not below the next file number"));
    }
    return {};
}

Status CheckAddedFileNumbers(const VersionEdit& edit, std::uint64_t next_file_number) {
    for (const NewFile& added : edit.new_files()) {
        if (added.file.number >= next_file_number) {
            return std::unexpected(
                Error::InvalidArgument("edit adds file " + std::to_string(added.file.number) +
                                       ", which is not below the next file number"));
        }
    }
    return {};
}

void ApplyCompactPointerEdits(const VersionEdit& edit,
                              std::array<std::optional<InternalKey>, NumLevels>& compact_pointers) {
    for (const CompactPointer& pointer : edit.compact_pointers()) {
        compact_pointers[pointer.level] = pointer.key;
    }
}

Error InManifest(std::uint64_t offset, const Error& error) {
    return Error::Corruption("MANIFEST record at offset " + std::to_string(offset) + ": " +
                             std::string(error.message()));
}

void AddPresentSnapshotPointer(VersionEdit& snapshot, std::uint32_t level,
                               const std::optional<InternalKey>& pointer) {
    if (pointer.has_value()) {
        static_cast<void>(snapshot.AddCompactPointer(level, *pointer));
    }
}

void AddSnapshotFiles(VersionEdit& snapshot, std::uint32_t level,
                      std::span<const Version::File> files) {
    for (const Version::File& file : files) {
        static_cast<void>(snapshot.AddFile(level, *file));
    }
}

// Records the comparator name, the compact pointers, and every file.
VersionEdit Snapshot(const Comparator& user_comparator,
                     std::span<const std::optional<InternalKey>, NumLevels> compact_pointers,
                     const Version& version) {
    VersionEdit snapshot;
    snapshot.SetComparatorName(user_comparator.Name());
    for (std::uint32_t level = 0; level < NumLevels; ++level) {
        AddPresentSnapshotPointer(snapshot, level, compact_pointers[level]);
        AddSnapshotFiles(snapshot, level, version.files(level));
    }
    return snapshot;
}

void CollectLevelFileNumbers(std::span<const Version::File> files, std::set<std::uint64_t>& live) {
    for (const Version::File& file : files) {
        live.insert(file->number);
    }
}

void CollectVersionFileNumbers(const Version& version, std::set<std::uint64_t>& live) {
    for (std::uint32_t level = 0; level < NumLevels; ++level) {
        CollectLevelFileNumbers(version.files(level), live);
    }
}

}  // namespace

VersionSet::VersionSet(FileSystem& file_system, std::filesystem::path directory,
                       const InternalKeyComparator& comparator) noexcept
    : file_system_(&file_system), directory_(std::move(directory)), comparator_(&comparator) {}

Result<std::unique_ptr<VersionSet>> VersionSet::Create(FileSystem& file_system,
                                                       std::filesystem::path directory,
                                                       const InternalKeyComparator& comparator) {
    std::unique_ptr<VersionSet> set(new VersionSet(file_system, std::move(directory), comparator));
    set->Install(std::make_shared<const Version>());
    const Status created = set->LogAndApply(VersionEdit());
    if (!created.has_value()) {
        return std::unexpected(created.error());
    }
    return set;
}

struct VersionSet::ManifestRecovery {
    explicit ManifestRecovery(const InternalKeyComparator& comparator)
        : builder(comparator, Version()) {}

    Status Replay(WalReader& reader, VersionSet& set) {
        while (true) {
            const WalReadResult event = reader.ReadNext();
            if (!event.has_value()) {
                return std::unexpected(event.error());
            }
            if (!event->has_value()) {
                return {};
            }
            if (const auto* corruption = std::get_if<WalCorruption>(&**event)) {
                return std::unexpected(InManifest(corruption->offset, corruption->error));
            }
            Status applied = ApplyRecord(std::get<WalLogicalRecord>(**event), set);
            if (!applied.has_value()) {
                return applied;
            }
        }
    }

    Status ApplyRecord(const WalLogicalRecord& record, VersionSet& set) {
        const Result<VersionEdit> edit = VersionEdit::Decode(record.data);
        if (!edit.has_value()) {
            return std::unexpected(InManifest(record.offset, edit.error()));
        }
        Status compatible = CheckComparator(*edit, set.comparator_->user_comparator());
        if (!compatible.has_value()) {
            return compatible;
        }
        const Status applied = builder.Apply(*edit);
        if (!applied.has_value()) {
            return std::unexpected(InManifest(record.offset, applied.error()));
        }
        ApplyCompactPointerEdits(*edit, set.compact_pointers_);
        return AccumulateCounters(*edit, record.offset);
    }

    static Status CheckComparator(const VersionEdit& edit, const Comparator& comparator) {
        const std::optional<std::string>& name = edit.comparator_name();
        if (name.has_value() && *name != comparator.Name()) {
            return std::unexpected(Error::InvalidArgument("MANIFEST comparator " + *name +
                                                          " does not match " +
                                                          std::string(comparator.Name())));
        }
        return {};
    }

    Status AccumulateCounters(const VersionEdit& edit, std::uint64_t offset) {
        IncludeAddedFileNumbers(edit);
        Status log = AccumulateLogNumber(edit, offset);
        if (!log.has_value()) {
            return log;
        }
        AccumulateSparseCounters(edit);
        largest_number = std::max({largest_number, log_number.value_or(0), prev_log_number});
        return {};
    }

    void IncludeAddedFileNumbers(const VersionEdit& edit) {
        for (const NewFile& added : edit.new_files()) {
            largest_number = std::max(largest_number, added.file.number);
        }
    }

    Status AccumulateLogNumber(const VersionEdit& edit, std::uint64_t offset) {
        if (edit.log_number().has_value()) {
            if (log_number.has_value() && *edit.log_number() < *log_number) {
                return std::unexpected(
                    InManifest(offset, Error::Corruption("log number decreases")));
            }
            log_number = edit.log_number();
        }
        return {};
    }

    void AccumulateSparseCounters(const VersionEdit& edit) {
        if (edit.next_file_number().has_value()) {
            next_file = edit.next_file_number();
        }
        if (edit.last_sequence().has_value()) {
            last_sequence = edit.last_sequence();
        }
        prev_log_number = edit.prev_log_number().value_or(prev_log_number);
    }

    Status ValidateCounters() const {
        Status required = RequireRecoveredCounterFields();
        if (!required.has_value()) {
            return required;
        }
        const std::uint64_t next = next_file.value();
        return ValidateRecoveredCounterBounds(next);
    }

    Status RequireRecoveredCounterFields() const {
        if (!next_file.has_value()) {
            return std::unexpected(Error::Corruption("MANIFEST has no next file number"));
        }
        if (!log_number.has_value()) {
            return std::unexpected(Error::Corruption("MANIFEST has no log number"));
        }
        if (!last_sequence.has_value()) {
            return std::unexpected(Error::Corruption("MANIFEST has no last sequence"));
        }
        return {};
    }

    Status ValidateRecoveredCounterBounds(std::uint64_t next) const {
        if (next > FileNumberLimit) {
            return std::unexpected(
                Error::Corruption("MANIFEST's next file number is beyond the file number limit"));
        }
        if (largest_number >= next) {
            return std::unexpected(Error::Corruption("MANIFEST names file or log number " +
                                                     std::to_string(largest_number) +
                                                     ", which is not below its next file number"));
        }
        return {};
    }

    Status Finish(VersionSet& set, std::uint64_t selected_number) {
        Status valid = ValidateCounters();
        if (!valid.has_value()) {
            return valid;
        }
        Result<Version> version = builder.Build();
        if (!version.has_value()) {
            return std::unexpected(
                Error::Corruption("MANIFEST: " + std::string(version.error().message())));
        }
        PublishRecoveredCounters(set, selected_number);
        set.Install(std::make_shared<const Version>(std::move(*version)));
        return {};
    }

    void PublishRecoveredCounters(VersionSet& set, std::uint64_t selected_number) {
        set.manifest_file_number_ = std::max(next_file.value(), selected_number + 1);
        set.next_file_number_ = set.manifest_file_number_ + 1;
        set.last_sequence_ = last_sequence.value();
        set.log_number_ = log_number.value();
        set.prev_log_number_ = prev_log_number;
    }

    VersionBuilder builder;
    std::optional<std::uint64_t> next_file;
    std::optional<std::uint64_t> log_number;
    std::optional<SequenceNumber> last_sequence;
    std::uint64_t prev_log_number = 0;
    // The largest log, previous log, or file number that any record names.
    std::uint64_t largest_number = 0;
};

Result<std::unique_ptr<VersionSet>> VersionSet::Recover(FileSystem& file_system,
                                                        std::filesystem::path directory,
                                                        const InternalKeyComparator& comparator) {
    Result<SelectedManifest> selected = OpenSelectedManifest(file_system, directory);
    if (!selected.has_value()) {
        return std::unexpected(std::move(selected).error());
    }
    std::unique_ptr<VersionSet> set(new VersionSet(file_system, std::move(directory), comparator));
    ManifestRecovery recovery(comparator);
    WalReader reader(std::move(selected->file));
    Status replayed = recovery.Replay(reader, *set);
    if (!replayed.has_value()) {
        return std::unexpected(std::move(replayed).error());
    }
    Status finished = recovery.Finish(*set, selected->number);
    if (!finished.has_value()) {
        return std::unexpected(std::move(finished).error());
    }
    return set;
}

std::set<std::uint64_t> VersionSet::LiveFiles() const {
    std::set<std::uint64_t> live;
    for (const std::weak_ptr<const Version>& held : versions_) {
        const std::shared_ptr<const Version> version = held.lock();
        if (version == nullptr) {
            continue;
        }
        CollectVersionFileNumbers(*version, live);
    }
    return live;
}

Status VersionSet::MarkFileNumberUsed(std::uint64_t number) {
    if (number >= FileNumberLimit) {
        return std::unexpected(Error::InvalidArgument("file number " + std::to_string(number) +
                                                      " is beyond the file number limit"));
    }
    next_file_number_ = std::max(next_file_number_, number + 1);
    return {};
}

Status VersionSet::LogAndApply(VersionEdit edit) {
    if (failure_.has_value()) {
        return std::unexpected(*failure_);
    }
    const Status valid = Validate(edit);
    if (!valid.has_value()) {
        return valid;
    }
    VersionBuilder builder(*comparator_, *current_);
    const Status applied = builder.Apply(edit);
    if (!applied.has_value()) {
        return applied;
    }
    Result<Version> version = builder.Build();
    if (!version.has_value()) {
        return std::unexpected(std::move(version).error());
    }
    return ApplyPrepared(std::move(edit), std::move(*version), nullptr);
}

Status VersionSet::LogAndApply(VersionEdit edit, std::unique_lock<std::mutex>& lock) {
    if (failure_.has_value()) {
        return std::unexpected(*failure_);
    }
    const Status valid = Validate(edit);
    if (!valid.has_value()) {
        return valid;
    }
    Version version = VersionBuilder::BuildTrusted(*comparator_, *current_, edit);
    return ApplyPrepared(std::move(edit), std::move(version), &lock);
}

Status VersionSet::ApplyPrepared(VersionEdit edit, Version version,
                                 std::unique_lock<std::mutex>* lock) {
    const std::uint64_t log_number = edit.log_number().value_or(log_number_);
    const std::uint64_t prev_log_number = edit.prev_log_number().value_or(prev_log_number_);
    PrepareRuntimeEditCounters(edit, log_number, prev_log_number);
    std::array<std::optional<InternalKey>, NumLevels> compact_pointers = compact_pointers_;
    ApplyCompactPointerEdits(edit, compact_pointers);
    std::optional<std::vector<std::byte>> new_manifest_snapshot;
    PrepareManifestSnapshot(new_manifest_snapshot);
    // Prepare ownership/capacity before I/O: after the durable edit succeeds,
    // publishing its topology must not need another allocation.
    auto candidate = std::make_shared<const Version>(std::move(version));
    ReserveInstall();

    Status written = WritePrepared(edit, new_manifest_snapshot, lock);
    if (!written.has_value()) {
        failure_ = written.error();
        return written;
    }
    // Persist first, publish second. Readers keep the old Version during I/O;
    // an uncertain write failure must not authorize cleanup using the candidate.
    PublishPreparedEdit(compact_pointers, candidate, log_number, prev_log_number);
    return {};
}

void VersionSet::PrepareRuntimeEditCounters(VersionEdit& edit, std::uint64_t log_number,
                                            std::uint64_t prev_log_number) const {
    edit.SetLogNumber(log_number);
    edit.SetPrevLogNumber(prev_log_number);
    static_cast<void>(edit.SetNextFileNumber(next_file_number_));
    static_cast<void>(edit.SetLastSequence(last_sequence_));
}

void VersionSet::PrepareManifestSnapshot(
    std::optional<std::vector<std::byte>>& new_manifest_snapshot) const {
    if (manifest_ == nullptr) {
        new_manifest_snapshot =
            Snapshot(comparator_->user_comparator(), compact_pointers_, *current_).Encode();
    }
}

void VersionSet::PublishPreparedEdit(
    std::array<std::optional<InternalKey>, NumLevels>& compact_pointers,
    std::shared_ptr<const Version>& candidate, std::uint64_t log_number,
    std::uint64_t prev_log_number) {
    compact_pointers_ = std::move(compact_pointers);
    log_number_ = log_number;
    prev_log_number_ = prev_log_number;
    InstallPrepared(std::move(candidate));
}

Status VersionSet::WritePrepared(const VersionEdit& edit,
                                 const std::optional<std::vector<std::byte>>& new_manifest_snapshot,
                                 std::unique_lock<std::mutex>* lock) {
    Status written;
    if (lock == nullptr) {
        written = Write(edit, new_manifest_snapshot);
    } else {
        lock->unlock();
        try {
            written = Write(edit, new_manifest_snapshot);
        } catch (...) {
            lock->lock();
            throw;
        }
        lock->lock();
    }
    return written;
}

Status VersionSet::Validate(const VersionEdit& edit) const {
    Status capacity = CheckFileNumberCapacity(next_file_number_);
    if (!capacity.has_value()) {
        return std::unexpected(std::move(capacity).error());
    }
    Status comparator = CheckComparatorName(edit.comparator_name(), comparator_->user_comparator());
    if (!comparator.has_value()) {
        return std::unexpected(std::move(comparator).error());
    }
    Status log = CheckCurrentLogNumber(edit.log_number(), log_number_, next_file_number_);
    if (!log.has_value()) {
        return std::unexpected(std::move(log).error());
    }
    Status previous = CheckPreviousLogNumber(edit.prev_log_number(), next_file_number_);
    if (!previous.has_value()) {
        return std::unexpected(std::move(previous).error());
    }
    return CheckAddedFileNumbers(edit, next_file_number_);
}

Status VersionSet::Write(const VersionEdit& edit,
                         const std::optional<std::vector<std::byte>>& new_manifest_snapshot) {
    std::unique_ptr<WalWriter> created;
    if (manifest_ == nullptr) {
        Result<std::unique_ptr<WalWriter>> writer = CreateManifest(*new_manifest_snapshot);
        if (!writer.has_value()) {
            return std::unexpected(std::move(writer).error());
        }
        created = std::move(*writer);
    }
    WalWriter& manifest = created != nullptr ? *created : *manifest_;
    const Status appended = manifest.AddRecord(edit.Encode());
    if (!appended.has_value()) {
        return appended;
    }
    const Status synced = manifest.Sync();
    if (!synced.has_value()) {
        return synced;
    }
    if (created == nullptr) {
        return {};
    }
    // Persist the new MANIFEST's name before CURRENT can durably refer to it.
    // The native Windows backend retains only its explicit weak namespace policy.
    const Status directory = file_system_->SyncDirectory(directory_);
    if (!directory.has_value()) {
        return directory;
    }
    const Status installed = InstallCurrent();
    if (!installed.has_value()) {
        return installed;
    }
    manifest_ = std::move(created);
    return {};
}

Result<std::unique_ptr<WalWriter>> VersionSet::CreateManifest(ByteView snapshot) {
    Result<std::unique_ptr<WritableFile>> file =
        file_system_->OpenWritable(DescriptorFileName(directory_, manifest_file_number_));
    if (!file.has_value()) {
        return std::unexpected(std::move(file).error());
    }
    auto created = std::make_unique<WalWriter>(std::move(*file));
    const Status appended = created->AddRecord(snapshot);
    if (!appended.has_value()) {
        return std::unexpected(appended.error());
    }
    return created;
}

// Selects metadata through a synced temporary file, rename, then namespace barrier.
// File bytes and the selected name are separate persistence obligations.
Status VersionSet::InstallCurrent() const {
    const std::filesystem::path temporary = TempFileName(directory_, manifest_file_number_);
    Result<std::unique_ptr<WritableFile>> file = file_system_->OpenWritable(temporary);
    if (!file.has_value()) {
        return std::unexpected(std::move(file).error());
    }
    const std::string contents = CurrentFileContents(manifest_file_number_);
    Status persisted = WriteCurrentTemporary(**file, AsBytes(contents));
    if (!persisted.has_value()) {
        return std::unexpected(std::move(persisted).error());
    }
    const Status renamed = file_system_->RenameFile(temporary, CurrentFileName(directory_));
    if (!renamed.has_value()) {
        return renamed;
    }
    return file_system_->SyncDirectory(directory_);
}

void VersionSet::ReserveInstall() {
    read_pinned_versions_.reserve(read_pinned_versions_.size() + 1);
    versions_.reserve(versions_.size() + 1);
}

void VersionSet::InstallPrepared(std::shared_ptr<const Version> version) noexcept {
    RetainPinnedCurrent();
    std::erase_if(versions_,
                  [](const std::weak_ptr<const Version>& held) { return held.expired(); });
    RegisterCurrentVersion(version);
}

void VersionSet::RetainPinnedCurrent() noexcept {
    if (current_ != nullptr && current_->read_pins_ != 0) {
        read_pinned_versions_.push_back(current_);
    }
}

void VersionSet::RegisterCurrentVersion(std::shared_ptr<const Version>& version) noexcept {
    versions_.emplace_back(version);
    current_ = std::move(version);
}

void VersionSet::Install(std::shared_ptr<const Version> version) {
    ReserveInstall();
    InstallPrepared(std::move(version));
}

void VersionSet::ReleaseRead(const Version& version) noexcept {
    --version.read_pins_;
    if (version.read_pins_ != 0 || current_.get() == &version) {
        return;
    }
    std::erase_if(read_pinned_versions_, [&](const std::shared_ptr<const Version>& held) {
        return held.get() == &version;
    });
}

}  // namespace modern_leveldb
