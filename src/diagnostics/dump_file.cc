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
            std::string line = "corruption offset=" + std::to_string(corruption->offset) +
                               " dropped_bytes=" + std::to_string(corruption->dropped_bytes) +
                               " error='" + EscapedError(corruption->error) + "'";
            const Status appended = AppendDiagnosticLine(output, std::move(line));
            if (!appended.has_value()) {
                return appended;
            }
            RememberFirst(first, corruption->error);
            continue;
        }
        const WalLogicalRecord& record = std::get<WalLogicalRecord>(**event);
        const Status formatted = format_record(record, output, first);
        if (!formatted.has_value()) {
            return formatted;
        }
    }
}

Status FormatWriteBatch(const WalLogicalRecord& record, WritableFile& output,
                        std::optional<Error>& first) {
    Result<WriteBatchReader> batch = WriteBatchReader::Open(record.data);
    if (!batch.has_value()) {
        const Error error = InvalidRecord("WAL", record.offset, batch.error());
        const Status appended = AppendRecordError(output, record.offset, error);
        if (!appended.has_value()) {
            return appended;
        }
        RememberFirst(first, error);
        return {};
    }

    std::string header = "record offset=" + std::to_string(record.offset) +
                         " sequence=" + std::to_string(batch->sequence()) +
                         " count=" + std::to_string(batch->count());
    const Status header_status = AppendDiagnosticLine(output, std::move(header));
    if (!header_status.has_value()) {
        return header_status;
    }
    while (const std::optional<WriteBatchEntry> entry = batch->Next()) {
        std::string line = "  ";
        line += entry->kind == ValueKind::Value ? "put" : "delete";
        line += " sequence=";
        line += std::to_string(entry->sequence);
        line += " key='";
        line += EscapeDiagnosticBytes(entry->key);
        line.push_back('\'');
        if (entry->kind == ValueKind::Value) {
            line += " value='";
            line += EscapeDiagnosticBytes(entry->value);
            line.push_back('\'');
        }
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

Status FormatVersionEdit(const WalLogicalRecord& record, WritableFile& output,
                         std::optional<Error>& first) {
    Result<VersionEdit> edit = VersionEdit::Decode(record.data);
    if (!edit.has_value()) {
        const Error error = InvalidRecord("MANIFEST", record.offset, edit.error());
        const Status appended = AppendRecordError(output, record.offset, error);
        if (!appended.has_value()) {
            return appended;
        }
        RememberFirst(first, error);
        return {};
    }
    Status appended =
        AppendDiagnosticLine(output, "record offset=" + std::to_string(record.offset));
    if (!appended.has_value()) {
        return appended;
    }
    if (edit->comparator_name().has_value()) {
        std::string line = "  comparator name='";
        line += EscapeDiagnosticBytes(AsBytes(*edit->comparator_name()));
        line.push_back('\'');
        appended = AppendDiagnosticLine(output, std::move(line));
        if (!appended.has_value()) {
            return appended;
        }
    }
    for (const auto& [name, value] :
         std::array<std::pair<std::string_view, std::optional<std::uint64_t>>, 4>{
             {{"log_number", edit->log_number()},
              {"previous_log_number", edit->prev_log_number()},
              {"next_file_number", edit->next_file_number()},
              {"last_sequence", edit->last_sequence()}}}) {
        appended = AppendOptionalNumber(output, name, value);
        if (!appended.has_value()) {
            return appended;
        }
    }
    for (const CompactPointer& pointer : edit->compact_pointers()) {
        std::string line = "  compact_pointer level=" + std::to_string(pointer.level) + " key=";
        AppendInternalKey(line, pointer.key);
        appended = AppendDiagnosticLine(output, std::move(line));
        if (!appended.has_value()) {
            return appended;
        }
    }
    for (const DeletedFile& file : edit->deleted_files()) {
        appended =
            AppendDiagnosticLine(output, "  delete_file level=" + std::to_string(file.level) +
                                             " number=" + std::to_string(file.number));
        if (!appended.has_value()) {
            return appended;
        }
    }
    for (const NewFile& added : edit->new_files()) {
        std::string line = "  add_file level=" + std::to_string(added.level) +
                           " number=" + std::to_string(added.file.number) +
                           " size=" + std::to_string(added.file.file_size) + " smallest=";
        AppendInternalKey(line, added.file.smallest);
        line += " largest=";
        AppendInternalKey(line, added.file.largest);
        appended = AppendDiagnosticLine(output, std::move(line));
        if (!appended.has_value()) {
            return appended;
        }
    }
    return {};
}

Status DumpTable(FileSystem& file_system, const std::filesystem::path& path,
                 std::string_view escaped_path, WritableFile& output) {
    Result<std::uint64_t> size = file_system.FileSize(path);
    if (!size.has_value()) {
        return std::unexpected(std::move(size).error());
    }
    Result<std::unique_ptr<RandomAccessFile>> file = file_system.OpenRandomAccess(path, *size);
    if (!file.has_value()) {
        return std::unexpected(std::move(file).error());
    }
    const InternalKeyComparator comparator(BytewiseComparator());
    Result<std::unique_ptr<Table>> table =
        Table::Open(std::move(*file), *size, comparator, TableOptions{});
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
        const Result<ParsedInternalKey> parsed = ParseInternalKey(iterator.key());
        std::string line;
        if (!parsed.has_value()) {
            line = "bad_key encoded='";
            line += EscapeDiagnosticBytes(iterator.key());
            line += "' value='";
            line += EscapeDiagnosticBytes(iterator.value());
            line.push_back('\'');
            RememberFirst(first, Error::Corruption("table entry has a malformed internal key"));
        } else {
            line = "entry user_key='";
            line += EscapeDiagnosticBytes(parsed->user_key);
            line += "' sequence=";
            line += std::to_string(parsed->sequence);
            line += " kind=";
            line += KindText(parsed->kind);
            line += " value='";
            line += EscapeDiagnosticBytes(iterator.value());
            line.push_back('\'');
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
                    escaped += "\\x";
                    escaped.push_back(HexDigits[byte >> 4U]);
                    escaped.push_back(HexDigits[byte & 0x0fU]);
                }
                break;
        }
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
