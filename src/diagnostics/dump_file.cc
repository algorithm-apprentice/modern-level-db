#include "diagnostics/dump_file.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "format/internal_key.h"
#include "format/write_batch.h"
#include "metadata/filenames.h"
#include "metadata/version_edit.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "platform/path.h"
#include "table/table.h"
#include "wal/wal_io.h"

namespace modern_leveldb {
namespace {

constexpr std::array<char, 16> HexDigits{
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
};

void AppendDiagnosticHexByte(std::string& escaped, unsigned int byte) {
    escaped += "\\x";
    escaped.push_back(HexDigits[byte >> 4U]);
    escaped.push_back(HexDigits[byte & 0x0fU]);
}

void AppendEscapedDiagnosticByte(std::string& escaped, std::byte value) {
    const unsigned int byte = std::to_integer<unsigned int>(value);
    switch (byte) {
        case '\'':
            escaped += "\\'";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (byte >= 0x20U && byte <= 0x7eU) {
                escaped.push_back(static_cast<char>(byte));
            } else {
                AppendDiagnosticHexByte(escaped, byte);
            }
            break;
    }
}

std::string EscapedError(const Error& error) {
    const std::string text = error.ToString();
    return EscapeDiagnosticBytes(AsBytes(text));
}

void RememberFirst(std::optional<Error>& first, const Error& error) {
    if (!first.has_value()) {
        first = error;
    }
}

Status Finish(std::optional<Error> first) {
    if (first.has_value()) {
        return std::unexpected(std::move(*first));
    }
    return {};
}

std::string KindText(ValueKind kind) { return kind == ValueKind::Deletion ? "deletion" : "value"; }

void AppendInternalKey(std::string& output, const InternalKey& key) {
    output += "{user_key='";
    output += EscapeDiagnosticBytes(key.user_key());
    output += "' sequence=";
    output += std::to_string(key.sequence());
    output += " kind=";
    output += KindText(key.kind());
    output.push_back('}');
}

Status AppendHeader(WritableFile& output, std::string_view type, std::string_view escaped_path) {
    std::string line = "dump version=1 type=";
    line += type;
    line += " file='";
    line += escaped_path;
    line.push_back('\'');
    return AppendDiagnosticLine(output, std::move(line));
}

Error InvalidRecord(std::string_view type, std::uint64_t offset, const Error& error) {
    return Error::Corruption(std::string(type) + " record at offset " + std::to_string(offset) +
                             " is invalid: " + std::string(error.message()));
}

Status AppendRecordError(WritableFile& output, std::uint64_t offset, const Error& error) {
    std::string line = "record_error offset=" + std::to_string(offset) + " error='";
    line += EscapedError(error);
    line.push_back('\'');
    return AppendDiagnosticLine(output, std::move(line));
}

Status ReportInvalidRecord(std::string_view type, std::uint64_t offset, const Error& cause,
                           WritableFile& output, std::optional<Error>& first) {
    const Error error = InvalidRecord(type, offset, cause);
    const Status appended = AppendRecordError(output, offset, error);
    if (!appended.has_value()) {
        return appended;
    }
    RememberFirst(first, error);
    return {};
}

std::string FormatPhysicalCorruption(const WalCorruption& corruption) {
    return "corruption offset=" + std::to_string(corruption.offset) +
           " dropped_bytes=" + std::to_string(corruption.dropped_bytes) + " error='" +
           EscapedError(corruption.error) + "'";
}

Status ReportPhysicalCorruption(const WalCorruption& corruption, WritableFile& output,
                                std::optional<Error>& first) {
    std::string line = FormatPhysicalCorruption(corruption);
    const Status appended = AppendDiagnosticLine(output, std::move(line));
    if (!appended.has_value()) {
        return appended;
    }
    RememberFirst(first, corruption.error);
    return {};
}

template <typename RecordFormatter>
Status DumpWalRecords(WalReader& reader, WritableFile& output, RecordFormatter& format_record) {
    std::optional<Error> first;
    while (true) {
        WalReadResult event = reader.ReadNext();
        if (!event.has_value()) {
            return std::unexpected(std::move(event).error());
        }
        if (!event->has_value()) {
            return Finish(std::move(first));
        }
        if (const auto* corruption = std::get_if<WalCorruption>(&**event)) {
            Status reported = ReportPhysicalCorruption(*corruption, output, first);
            if (!reported.has_value()) {
                return std::unexpected(std::move(reported).error());
            }
            continue;
        }
        const WalLogicalRecord& record = std::get<WalLogicalRecord>(**event);
        const Status formatted = format_record(record, output, first);
        if (!formatted.has_value()) {
            return formatted;
        }
    }
}

template <typename RecordFormatter>
Status DumpWalFramed(FileSystem& file_system, const std::filesystem::path& path,
                     std::string_view type, std::string_view escaped_path, WritableFile& output,
                     RecordFormatter&& format_record) {
    Result<std::unique_ptr<SequentialFile>> file = file_system.OpenSequential(path);
    if (!file.has_value()) {
        return std::unexpected(std::move(file).error());
    }
    const Status header = AppendHeader(output, type, escaped_path);
    if (!header.has_value()) {
        return header;
    }
    WalReader reader(std::move(*file));
    return DumpWalRecords(reader, output, format_record);
}

void AppendDiagnosticNumberField(std::string& output, std::string_view name, std::uint64_t value) {
    output.push_back(' ');
    output.append(name);
    output.push_back('=');
    output += std::to_string(value);
}

std::string WriteBatchEntryPrefix(const WriteBatchEntry& entry) {
    std::string prefix = "  ";
    prefix += entry.kind == ValueKind::Value ? "put" : "delete";
    AppendDiagnosticNumberField(prefix, "sequence", entry.sequence);
    return prefix;
}

void AppendDiagnosticBytesField(std::string& output, std::string_view name, ByteView value) {
    output.push_back(' ');
    output.append(name);
    output += "='";
    output += EscapeDiagnosticBytes(value);
    output.push_back('\'');
}

void AppendDiagnosticKindField(std::string& output, ValueKind kind) {
    output += " kind=";
    output += KindText(kind);
}

std::string FormatWriteBatchEntry(const WriteBatchEntry& entry) {
    std::string line = WriteBatchEntryPrefix(entry);
    AppendDiagnosticBytesField(line, "key", entry.key);
    if (entry.kind == ValueKind::Value) {
        AppendDiagnosticBytesField(line, "value", entry.value);
    }
    return line;
}

Status FormatWriteBatch(const WalLogicalRecord& record, WritableFile& output,
                        std::optional<Error>& first) {
    Result<WriteBatchReader> batch = WriteBatchReader::Open(record.data);
    if (!batch.has_value()) {
        return ReportInvalidRecord("WAL", record.offset, batch.error(), output, first);
    }

    std::string header = "record offset=" + std::to_string(record.offset) +
                         " sequence=" + std::to_string(batch->sequence()) +
                         " count=" + std::to_string(batch->count());
    const Status header_status = AppendDiagnosticLine(output, std::move(header));
    if (!header_status.has_value()) {
        return header_status;
    }
    while (const std::optional<WriteBatchEntry> entry = batch->Next()) {
        std::string line = FormatWriteBatchEntry(*entry);
        const Status appended = AppendDiagnosticLine(output, std::move(line));
        if (!appended.has_value()) {
            return appended;
        }
    }
    return {};
}

Status AppendOptionalNumber(WritableFile& output, std::string_view name,
                            std::optional<std::uint64_t> value) {
    if (!value.has_value()) {
        return {};
    }
    return AppendDiagnosticLine(output,
                                "  " + std::string(name) + " value=" + std::to_string(*value));
}

std::string FormatManifestComparator(std::string_view name) {
    std::string line = "  comparator";
    AppendDiagnosticBytesField(line, "name", AsBytes(name));
    return line;
}

std::string FormatCompactPointer(const CompactPointer& pointer) {
    std::string line = "  compact_pointer";
    AppendDiagnosticNumberField(line, "level", pointer.level);
    line += " key=";
    AppendInternalKey(line, pointer.key);
    return line;
}

std::string FormatDeletedFile(const DeletedFile& file) {
    std::string line = "  delete_file";
    AppendDiagnosticNumberField(line, "level", file.level);
    AppendDiagnosticNumberField(line, "number", file.number);
    return line;
}

std::string FormatNewFile(const NewFile& added) {
    std::string line = "  add_file";
    AppendDiagnosticNumberField(line, "level", added.level);
    AppendDiagnosticNumberField(line, "number", added.file.number);
    AppendDiagnosticNumberField(line, "size", added.file.file_size);
    line += " smallest=";
    AppendInternalKey(line, added.file.smallest);
    line += " largest=";
    AppendInternalKey(line, added.file.largest);
    return line;
}

Status AppendComparatorField(WritableFile& output, const VersionEdit& edit) {
    if (!edit.comparator_name().has_value()) {
        return {};
    }
    return AppendDiagnosticLine(output, FormatManifestComparator(*edit.comparator_name()));
}

Status AppendScalarFields(WritableFile& output, const VersionEdit& edit) {
    for (const auto& [name, value] :
         std::array<std::pair<std::string_view, std::optional<std::uint64_t>>, 4>{
             {{"log_number", edit.log_number()},
              {"previous_log_number", edit.prev_log_number()},
              {"next_file_number", edit.next_file_number()},
              {"last_sequence", edit.last_sequence()}}}) {
        Status appended = AppendOptionalNumber(output, name, value);
        if (!appended.has_value()) {
            return appended;
        }
    }
    return {};
}

Status AppendCompactPointers(WritableFile& output, const VersionEdit& edit) {
    for (const CompactPointer& pointer : edit.compact_pointers()) {
        Status appended = AppendDiagnosticLine(output, FormatCompactPointer(pointer));
        if (!appended.has_value()) {
            return appended;
        }
    }
    return {};
}

Status AppendDeletedFiles(WritableFile& output, const VersionEdit& edit) {
    for (const DeletedFile& file : edit.deleted_files()) {
        Status appended = AppendDiagnosticLine(output, FormatDeletedFile(file));
        if (!appended.has_value()) {
            return appended;
        }
    }
    return {};
}

Status AppendNewFiles(WritableFile& output, const VersionEdit& edit) {
    for (const NewFile& added : edit.new_files()) {
        Status appended = AppendDiagnosticLine(output, FormatNewFile(added));
        if (!appended.has_value()) {
            return appended;
        }
    }
    return {};
}

Status AppendVersionEditFields(WritableFile& output, const VersionEdit& edit) {
    Status appended = AppendComparatorField(output, edit);
    if (!appended.has_value()) {
        return appended;
    }
    appended = AppendScalarFields(output, edit);
    if (!appended.has_value()) {
        return appended;
    }
    appended = AppendCompactPointers(output, edit);
    if (!appended.has_value()) {
        return appended;
    }
    appended = AppendDeletedFiles(output, edit);
    if (!appended.has_value()) {
        return appended;
    }
    return AppendNewFiles(output, edit);
}

Status FormatVersionEdit(const WalLogicalRecord& record, WritableFile& output,
                         std::optional<Error>& first) {
    Result<VersionEdit> edit = VersionEdit::Decode(record.data);
    if (!edit.has_value()) {
        return ReportInvalidRecord("MANIFEST", record.offset, edit.error(), output, first);
    }
    Status header = AppendDiagnosticLine(output, "record offset=" + std::to_string(record.offset));
    if (!header.has_value()) {
        return header;
    }
    return AppendVersionEditFields(output, *edit);
}

std::string FormatTableEntry(const InternalKeyView& key, ByteView value) {
    std::string line = "entry";
    AppendDiagnosticBytesField(line, "user_key", key.user_key);
    AppendDiagnosticNumberField(line, "sequence", key.sequence);
    AppendDiagnosticKindField(line, key.kind);
    AppendDiagnosticBytesField(line, "value", value);
    return line;
}

std::string FormatBadTableEntry(ByteView encoded_key, ByteView value) {
    std::string line = "bad_key";
    AppendDiagnosticBytesField(line, "encoded", encoded_key);
    AppendDiagnosticBytesField(line, "value", value);
    return line;
}

Status DumpTable(FileSystem& file_system, const std::filesystem::path& path,
                 std::string_view escaped_path, WritableFile& output) {
    // Diagnostic traversal uses arbitrary bytewise keys and no cache population.
    // It exposes visited physical history, not a latest-value database view.
    Result<std::uint64_t> size = file_system.FileSize(path);
    if (!size.has_value()) {
        return std::unexpected(std::move(size).error());
    }
    Result<std::unique_ptr<RandomAccessFile>> file = file_system.OpenRandomAccess(path, *size);
    if (!file.has_value()) {
        return std::unexpected(std::move(file).error());
    }
    const InternalKeyComparator comparator(BytewiseComparator());
    TableOptions table_options;
    table_options.block_key_format = BlockKeyFormat::Arbitrary;
    Result<std::unique_ptr<Table>> table =
        Table::Open(std::move(*file), *size, comparator, table_options);
    if (!table.has_value()) {
        return std::unexpected(std::move(table).error());
    }
    const Status header = AppendHeader(output, "table", escaped_path);
    if (!header.has_value()) {
        return header;
    }

    TableReadOptions options;
    options.fill_cache = false;
    Table::Iterator iterator(**table, options);
    std::optional<Error> first;
    Status moved = iterator.SeekToFirst();
    while (moved.has_value() && iterator.valid()) {
        const Result<InternalKeyView> parsed = ParseInternalKey(iterator.key());
        std::string line;
        if (!parsed.has_value()) {
            line = FormatBadTableEntry(iterator.key(), iterator.value());
            RememberFirst(first, Error::Corruption("table entry has a malformed internal key"));
        } else {
            line = FormatTableEntry(*parsed, iterator.value());
        }
        const Status appended = AppendDiagnosticLine(output, std::move(line));
        if (!appended.has_value()) {
            return appended;
        }
        moved = iterator.Next();
    }
    if (!moved.has_value()) {
        if (moved.error().code() != ErrorCode::Corruption) {
            return moved;
        }
        RememberFirst(first, moved.error());
    }
    return Finish(std::move(first));
}

}  // namespace

std::string EscapeDiagnosticBytes(ByteView bytes) {
    std::string escaped;
    escaped.reserve(bytes.size());
    for (const std::byte value : bytes) {
        AppendEscapedDiagnosticByte(escaped, value);
    }
    return escaped;
}

Result<std::string> EscapeDiagnosticPath(const std::filesystem::path& path) {
    return PathUtf8(path).transform(
        [](const std::string& text) { return EscapeDiagnosticBytes(AsBytes(text)); });
}

Status AppendDiagnosticLine(WritableFile& output, std::string line) {
    line.push_back('\n');
    return output.Append(AsBytes(line));
}

Status DumpFile(FileSystem& file_system, const std::filesystem::path& path, WritableFile& output) {
    Result<std::string> path_text = PathUtf8(path);
    if (!path_text.has_value()) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 path conversion failure
        return std::unexpected(std::move(path_text).error());  // GCOVR_EXCL_LINE: see PathUtf8
    }
    Result<std::string> file_name = PathUtf8(path.filename());
    if (!file_name.has_value()) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 path conversion failure
        return std::unexpected(std::move(file_name).error());  // GCOVR_EXCL_LINE: see PathUtf8
    }
    const std::optional<ParsedFileName> parsed = ParseFileName(*file_name);
    if (!parsed.has_value()) {
        return std::unexpected(Error::InvalidArgument("file name is not canonical"));
    }
    const std::string escaped_path = EscapeDiagnosticBytes(AsBytes(*path_text));
    switch (parsed->type) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/5 validated FileType default
        case FileType::Log:
            return DumpWalFramed(file_system, path, "log", escaped_path, output, FormatWriteBatch);
        case FileType::Descriptor:
            return DumpWalFramed(file_system, path, "manifest", escaped_path, output,
                                 FormatVersionEdit);
        case FileType::Table:
            return DumpTable(file_system, path, escaped_path, output);
        case FileType::Lock:
        case FileType::Current:
        case FileType::Temp:
            // GCOVR_EXCL_START: ParseFileName returns an exhaustive FileType
            return std::unexpected(Error::InvalidArgument("file type is not dumpable"));
            // GCOVR_EXCL_STOP
    }
    // GCOVR_EXCL_START: exhaustive switch over validated FileType
    return std::unexpected(Error::InvalidArgument("file type is not dumpable"));
    // GCOVR_EXCL_STOP
}

}  // namespace modern_leveldb
