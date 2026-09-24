#include "metadata/version_set.h"

#include <algorithm>
#include <array>
#include <cassert>
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

#include "metadata/filenames.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"

namespace modern_leveldb {
namespace {

// Checks recording a field that the version set's invariants make valid.
void Expect(const Status& status) noexcept {
  assert(status.has_value());
  static_cast<void>(status);
}

Result<std::string> ReadCurrent(FileSystem& file_system, const std::filesystem::path& directory) {
  Result<std::unique_ptr<SequentialFile>> file =
      file_system.OpenSequential(CurrentFileName(directory));
  if (!file.has_value()) {
    return std::unexpected(std::move(file).error());
  }
  std::string contents;
  std::array<std::byte, 64> buffer{};
  while (true) {
    const Result<std::size_t> read = (*file)->Read(buffer);
    if (!read.has_value()) {
      return std::unexpected(read.error());
    }
    if (*read > buffer.size()) {
      return std::unexpected(Error::Io("CURRENT returned an oversized read"));
    }
    if (*read == 0) {
      return contents;
    }
    contents.append(AsStringView(ByteView(buffer).first(*read)));
  }
}

Error InManifest(std::uint64_t offset, const Error& error) {
  return Error::Corruption("MANIFEST record at offset " + std::to_string(offset) + ": " +
                           std::string(error.message()));
}

// Records the comparator name, the compact pointers, and every file.
VersionEdit Snapshot(const Comparator& user_comparator,
                     std::span<const std::optional<InternalKey>, NumLevels> compact_pointers,
                     const Version& version) {
  VersionEdit snapshot;
  Expect(snapshot.SetComparatorName(user_comparator.Name()));
  for (std::uint32_t level = 0; level < NumLevels; ++level) {
    if (compact_pointers[level].has_value()) {
      Expect(snapshot.AddCompactPointer(level, *compact_pointers[level]));
    }
    for (const Version::File& file : version.files(level)) {
      Expect(snapshot.AddFile(level, *file));
    }
  }
  return snapshot;
}

}  // namespace

VersionSet::VersionSet(FileSystem& file_system, std::filesystem::path directory,
                       const InternalKeyComparator& comparator)
    : file_system_(&file_system), directory_(std::move(directory)), comparator_(&comparator) {
  Install(std::make_shared<const Version>());
}

Result<std::unique_ptr<VersionSet>> VersionSet::Create(FileSystem& file_system,
                                                       std::filesystem::path directory,
                                                       const InternalKeyComparator& comparator) {
  std::unique_ptr<VersionSet> set(new VersionSet(file_system, std::move(directory), comparator));
  const Status created = set->LogAndApply(VersionEdit());
  if (!created.has_value()) {
    return std::unexpected(created.error());
  }
  return set;
}

Result<std::unique_ptr<VersionSet>> VersionSet::Recover(FileSystem& file_system,
                                                        std::filesystem::path directory,
                                                        const InternalKeyComparator& comparator) {
  const Result<std::string> current = ReadCurrent(file_system, directory);
  if (!current.has_value()) {
    return std::unexpected(current.error());
  }
  const Result<std::uint64_t> manifest_number = ParseCurrentFileContents(*current);
  if (!manifest_number.has_value()) {
    return std::unexpected(manifest_number.error());
  }
  if (*manifest_number >= FileNumberLimit) {
    return std::unexpected(
        Error::Corruption("CURRENT names a MANIFEST beyond the file number limit"));
  }
  Result<std::unique_ptr<SequentialFile>> file =
      file_system.OpenSequential(DescriptorFileName(directory, *manifest_number));
  if (!file.has_value()) {
    if (file.error().code() == ErrorCode::NotFound) {
      return std::unexpected(Error::Corruption("CURRENT names a missing MANIFEST: " +
                                               std::string(file.error().message())));
    }
    return std::unexpected(std::move(file).error());
  }

  std::unique_ptr<VersionSet> set(new VersionSet(file_system, std::move(directory), comparator));
  VersionBuilder builder(comparator, Version());
  std::optional<std::uint64_t> next_file;
  std::optional<std::uint64_t> log_number;
  std::optional<SequenceNumber> last_sequence;
  std::uint64_t prev_log_number = 0;
  // The largest log, previous log, or file number that any record names.
  std::uint64_t largest_number = 0;
  WalReader reader(std::move(*file));
  while (true) {
    const WalReadResult event = reader.ReadNext();
    if (!event.has_value()) {
      return std::unexpected(event.error());
    }
    if (!event->has_value()) {
      break;
    }
    if (const auto* corruption = std::get_if<WalCorruption>(&**event)) {
      return std::unexpected(InManifest(corruption->offset, corruption->error));
    }
    const WalLogicalRecord& record = std::get<WalLogicalRecord>(**event);
    const Result<VersionEdit> edit = VersionEdit::Decode(record.data);
    if (!edit.has_value()) {
      return std::unexpected(InManifest(record.offset, edit.error()));
    }
    const std::optional<std::string>& name = edit->comparator_name();
    if (name.has_value() && *name != comparator.user_comparator().Name()) {
      return std::unexpected(
          Error::InvalidArgument("MANIFEST comparator " + *name + " does not match " +
                                 std::string(comparator.user_comparator().Name())));
    }
    const Status applied = builder.Apply(*edit);
    if (!applied.has_value()) {
      return std::unexpected(InManifest(record.offset, applied.error()));
    }
    for (const CompactPointer& pointer : edit->compact_pointers()) {
      set->compact_pointers_[pointer.level] = pointer.key;
    }
    for (const NewFile& added : edit->new_files()) {
      largest_number = std::max(largest_number, added.file.number);
    }
    if (edit->log_number().has_value()) {
      if (log_number.has_value() && *edit->log_number() < *log_number) {
        return std::unexpected(
            InManifest(record.offset, Error::Corruption("log number decreases")));
      }
      log_number = edit->log_number();
    }
    if (edit->next_file_number().has_value()) {
      next_file = edit->next_file_number();
    }
    if (edit->last_sequence().has_value()) {
      last_sequence = edit->last_sequence();
    }
    prev_log_number = edit->prev_log_number().value_or(prev_log_number);
    largest_number = std::max({largest_number, log_number.value_or(0), prev_log_number});
  }

  if (!next_file.has_value()) {
    return std::unexpected(Error::Corruption("MANIFEST has no next file number"));
  }
  if (!log_number.has_value()) {
    return std::unexpected(Error::Corruption("MANIFEST has no log number"));
  }
  if (!last_sequence.has_value()) {
    return std::unexpected(Error::Corruption("MANIFEST has no last sequence"));
  }
  const std::uint64_t next = next_file.value();
  if (next > FileNumberLimit) {
    return std::unexpected(
        Error::Corruption("MANIFEST's next file number is beyond the file number limit"));
  }
  if (largest_number >= next) {
    return std::unexpected(Error::Corruption("MANIFEST names file or log number " +
                                             std::to_string(largest_number) +
                                             ", which is not below its next file number"));
  }
  Result<Version> version = builder.Build();
  if (!version.has_value()) {
    return std::unexpected(
        Error::Corruption("MANIFEST: " + std::string(version.error().message())));
  }

  set->manifest_file_number_ = std::max(next, manifest_number.value() + 1);
  set->next_file_number_ = set->manifest_file_number_ + 1;
  set->last_sequence_ = last_sequence.value();
  set->log_number_ = log_number.value();
  set->prev_log_number_ = prev_log_number;
  set->Install(std::make_shared<const Version>(std::move(*version)));
  return set;
}

std::set<std::uint64_t> VersionSet::LiveFiles() const {
  std::set<std::uint64_t> live;
  for (const std::weak_ptr<const Version>& held : versions_) {
    const std::shared_ptr<const Version> version = held.lock();
    if (version == nullptr) {
      continue;
    }
    for (std::uint32_t level = 0; level < NumLevels; ++level) {
      for (const Version::File& file : version->files(level)) {
        live.insert(file->number);
      }
    }
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

void VersionSet::SetLastSequence(SequenceNumber sequence) noexcept {
  assert(sequence >= last_sequence_ && sequence <= MaxSequenceNumber);
  last_sequence_ = sequence;
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

  const std::uint64_t log_number = edit.log_number().value_or(log_number_);
  const std::uint64_t prev_log_number = edit.prev_log_number().value_or(prev_log_number_);
  edit.SetLogNumber(log_number);
  edit.SetPrevLogNumber(prev_log_number);
  Expect(edit.SetNextFileNumber(next_file_number_));
  Expect(edit.SetLastSequence(last_sequence_));
  std::array<std::optional<InternalKey>, NumLevels> compact_pointers = compact_pointers_;
  for (const CompactPointer& pointer : edit.compact_pointers()) {
    compact_pointers[pointer.level] = pointer.key;
  }

  const Status written = Write(edit);
  if (!written.has_value()) {
    failure_ = written.error();
    return written;
  }
  compact_pointers_ = std::move(compact_pointers);
  log_number_ = log_number;
  prev_log_number_ = prev_log_number;
  Install(std::make_shared<const Version>(std::move(*version)));
  return {};
}

Status VersionSet::Validate(const VersionEdit& edit) const {
  if (next_file_number_ > FileNumberLimit) {
    return std::unexpected(Error::InvalidArgument("file numbers are exhausted"));
  }
  const std::optional<std::string>& name = edit.comparator_name();
  if (name.has_value() && *name != comparator_->user_comparator().Name()) {
    return std::unexpected(Error::InvalidArgument("edit names comparator " + *name));
  }
  const std::uint64_t log_number = edit.log_number().value_or(log_number_);
  if (log_number < log_number_ || log_number >= next_file_number_) {
    return std::unexpected(
        Error::InvalidArgument("edit's log number " + std::to_string(log_number) +
                               " is below the current one or " + "not below the next file number"));
  }
  if (edit.prev_log_number().value_or(0) >= next_file_number_) {
    return std::unexpected(
        Error::InvalidArgument("edit's previous log number is not below the next file number"));
  }
  for (const NewFile& added : edit.new_files()) {
    if (added.file.number >= next_file_number_) {
      return std::unexpected(Error::InvalidArgument("edit adds file " +
                                                    std::to_string(added.file.number) +
                                                    ", which is not below the next file number"));
    }
  }
  return {};
}

Status VersionSet::Write(const VersionEdit& edit) {
  std::unique_ptr<WalWriter> created;
  if (manifest_ == nullptr) {
    Result<std::unique_ptr<WritableFile>> file =
        file_system_->OpenWritable(DescriptorFileName(directory_, manifest_file_number_));
    if (!file.has_value()) {
      return std::unexpected(std::move(file).error());
    }
    created = std::make_unique<WalWriter>(std::move(*file));
    const Status snapshot = created->AddRecord(
        Snapshot(comparator_->user_comparator(), compact_pointers_, *current_).Encode());
    if (!snapshot.has_value()) {
      return snapshot;
    }
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

// Points CURRENT at the new MANIFEST through a temporary file.
Status VersionSet::InstallCurrent() const {
  const std::filesystem::path temporary = TempFileName(directory_, manifest_file_number_);
  Result<std::unique_ptr<WritableFile>> file = file_system_->OpenWritable(temporary);
  if (!file.has_value()) {
    return std::unexpected(std::move(file).error());
  }
  const std::string contents = CurrentFileContents(manifest_file_number_);
  const Status appended = (*file)->Append(AsBytes(contents));
  if (!appended.has_value()) {
    return appended;
  }
  const Status synced = (*file)->Sync();
  if (!synced.has_value()) {
    return synced;
  }
  const Status closed = (*file)->Close();
  if (!closed.has_value()) {
    return closed;
  }
  const Status renamed = file_system_->RenameFile(temporary, CurrentFileName(directory_));
  if (!renamed.has_value()) {
    return renamed;
  }
  return file_system_->SyncDirectory(directory_);
}

void VersionSet::Install(std::shared_ptr<const Version> version) {
  std::erase_if(versions_, [](const std::weak_ptr<const Version>& held) { return held.expired(); });
  versions_.emplace_back(version);
  current_ = std::move(version);
}

}  // namespace modern_leveldb
