#include "platform/windows_file_system.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <latch>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "platform/mapped_read_limiter.h"
#include "platform/path.h"
#include "platform/windows_file_system_internal.h"
#include "support/temporary_directory.h"
#include "table/table.h"
#include "table/table_builder.h"

namespace modern_leveldb {
namespace {

using test_support::TemporaryDirectory;

enum class NativeCall {
    Open,
    Inspect,
    Size,
    Mapping,
    Map,
    Unmap,
    Close,
    Find,
    Next,
    CloseFind,
    Event,
    Read,
    Complete
};

class FaultOperations final : public WindowsFileOperations {
public:
    bool fail_open = false;
    bool fail_inspect = false;
    bool fail_event = false;
    bool fail_read = false;
    bool force_pending = false;
    bool fail_complete = false;
    bool fail_write = false;
    bool zero_write = false;
    bool fail_sync = false;
    bool fail_close = false;
    bool fail_next = false;
    bool fail_find_close = false;
    bool fail_volume = false;
    bool report_refs = false;
    bool fail_size = false;
    bool fail_mapping = false;
    bool fail_map = false;
    bool fail_unmap = false;
    bool scripted_enumeration = false;
    std::optional<std::int64_t> reported_size;
    DWORD find_error = ERROR_SUCCESS;
    DWORD find_close_error = ERROR_READ_FAULT;
    DWORD read_error = ERROR_READ_FAULT;
    DWORD complete_error = ERROR_CRC;
    unsigned fail_close_call = 0;
    unsigned fail_next_call = 0;
    UINT forced_drive_type = std::numeric_limits<UINT>::max();
    DWORD maximum_write = MAXDWORD;
    mutable unsigned live_handles = 0;
    mutable unsigned live_finds = 0;
    mutable unsigned completed_reads = 0;
    mutable unsigned write_calls = 0;
    mutable unsigned sync_calls = 0;
    mutable unsigned close_calls = 0;
    mutable unsigned next_calls = 0;
    mutable unsigned live_views = 0;
    mutable unsigned open_calls = 0;
    mutable unsigned inspect_calls = 0;
    mutable unsigned drive_calls = 0;
    mutable unsigned volume_calls = 0;
    mutable DWORD opened_access = 0;
    mutable DWORD opened_share = 0;
    mutable DWORD opened_disposition = 0;
    mutable DWORD opened_flags = 0;
    mutable std::wstring opened_path;
    mutable std::array<NativeCall, 256> calls{};
    mutable std::size_t call_count = 0;
    mutable std::size_t child_index = 0;
    mutable std::array<wchar_t, 1'024> find_pattern{};
    mutable HANDLE acquired_file = INVALID_HANDLE_VALUE;
    mutable HANDLE acquired_section = nullptr;
    mutable std::array<HANDLE, 256> closed_handles{};
    mutable HANDLE read_event = nullptr;
    mutable OVERLAPPED read_operation{};
    mutable DWORD read_request = 0;
    mutable bool null_positioned_count = false;
    mutable bool completion_was_terminal = false;
    mutable bool closed_completed_event = false;
    static constexpr std::array<std::wstring_view, 5> ScriptedChildren{L"\u03b2", L".", L"..",
                                                                       L"two words", L"child"};

    void Record(NativeCall call) const noexcept {
        if (call_count == calls.size()) {
            std::abort();
        }
        calls[call_count++] = call;
    }

    std::vector<NativeCall> Trace() const {
        return {calls.begin(), calls.begin() + static_cast<std::ptrdiff_t>(call_count)};
    }

    void SetChildName(WIN32_FIND_DATAW* information) const noexcept {
        const auto name = ScriptedChildren[child_index];
        std::copy(name.begin(), name.end(), information->cFileName);
        information->cFileName[name.size()] = L'\0';
    }

    HANDLE Open(const wchar_t* path, DWORD access, DWORD share, DWORD disposition,
                DWORD flags) const noexcept override {
        Record(NativeCall::Open);
        ++open_calls;
        opened_path = path;
        opened_access = access;
        opened_share = share;
        opened_disposition = disposition;
        opened_flags = flags;
        if (fail_open) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        const HANDLE handle = WindowsFileOperations::Open(path, access, share, disposition, flags);
        acquired_file = handle;
        if (handle != INVALID_HANDLE_VALUE) {
            ++live_handles;
        }
        return handle;
    }
    BOOL Inspect(HANDLE file, BY_HANDLE_FILE_INFORMATION* information) const noexcept override {
        Record(NativeCall::Inspect);
        ++inspect_calls;
        if (fail_inspect) {
            ::SetLastError(ERROR_READ_FAULT);
            return FALSE;
        }
        return WindowsFileOperations::Inspect(file, information);
    }
    HANDLE Event() const noexcept override {
        Record(NativeCall::Event);
        completion_was_terminal = false;
        closed_completed_event = false;
        if (fail_event) {
            ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
        const HANDLE event = WindowsFileOperations::Event();
        read_event = event;
        if (event != nullptr) {
            ++live_handles;
        }
        return event;
    }
    BOOL Read(HANDLE file, void* output, DWORD size, DWORD* read,
              OVERLAPPED* overlapped) const noexcept override {
        Record(NativeCall::Read);
        if (overlapped != nullptr) {
            read_operation = *overlapped;
            read_request = size;
            null_positioned_count = read == nullptr;
        }
        if (fail_read) {
            ::SetLastError(read_error);
            return FALSE;
        }
        const BOOL result = WindowsFileOperations::Read(file, output, size, read, overlapped);
        if (result && force_pending && overlapped != nullptr) {
            ::SetLastError(ERROR_IO_PENDING);
            return FALSE;
        }
        return result;
    }
    BOOL Complete(HANDLE file, OVERLAPPED* overlapped, DWORD* read) const noexcept override {
        Record(NativeCall::Complete);
        ++completed_reads;
        const BOOL completed = WindowsFileOperations::Complete(file, overlapped, read);
        completion_was_terminal = HasOverlappedIoCompleted(overlapped);
        if (fail_complete) {
            ::SetLastError(complete_error);
            return FALSE;
        }
        return completed;
    }
    BOOL Write(HANDLE file, const void* data, DWORD size, DWORD* written) const noexcept override {
        ++write_calls;
        if (fail_write) {
            ::SetLastError(ERROR_DISK_FULL);
            return FALSE;
        }
        if (zero_write) {
            *written = 0;
            return TRUE;
        }
        return WindowsFileOperations::Write(file, data, std::min(size, maximum_write), written);
    }
    BOOL Sync(HANDLE file) const noexcept override {
        ++sync_calls;
        if (fail_sync) {
            ::SetLastError(ERROR_WRITE_FAULT);
            return FALSE;
        }
        return WindowsFileOperations::Sync(file);
    }
    BOOL Size(HANDLE file, LARGE_INTEGER* size) const noexcept override {
        Record(NativeCall::Size);
        if (fail_size) {
            ::SetLastError(ERROR_READ_FAULT);
            return FALSE;
        }
        if (reported_size.has_value()) {
            size->QuadPart = *reported_size;
            return TRUE;
        }
        return WindowsFileOperations::Size(file, size);
    }
    HANDLE Mapping(HANDLE file) const noexcept override {
        Record(NativeCall::Mapping);
        if (fail_mapping) {
            ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
        const HANDLE handle = WindowsFileOperations::Mapping(file);
        acquired_section = handle;
        if (handle != nullptr) {
            ++live_handles;
        }
        return handle;
    }
    void* Map(HANDLE mapping) const noexcept override {
        Record(NativeCall::Map);
        if (fail_map) {
            ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
        void* view = WindowsFileOperations::Map(mapping);
        if (view != nullptr) {
            ++live_views;
        }
        return view;
    }
    BOOL Unmap(const void* mapping) const noexcept override {
        Record(NativeCall::Unmap);
        const BOOL unmapped = WindowsFileOperations::Unmap(mapping);
        if (unmapped) {
            --live_views;
        }
        if (fail_unmap) {
            ::SetLastError(ERROR_INVALID_ADDRESS);
            return FALSE;
        }
        return unmapped;
    }
    BOOL Close(HANDLE file) const noexcept override {
        Record(NativeCall::Close);
        if (file == read_event) {
            closed_completed_event = completion_was_terminal;
        }
        closed_handles[close_calls] = file;
        ++close_calls;
        const BOOL closed = WindowsFileOperations::Close(file);
        if (closed) {
            --live_handles;
        }
        if (fail_close || close_calls == fail_close_call) {
            ::SetLastError(ERROR_WRITE_FAULT);
            return FALSE;
        }
        return closed;
    }
    HANDLE Find(const wchar_t* pattern, WIN32_FIND_DATAW* information) const noexcept override {
        Record(NativeCall::Find);
        const std::wstring_view name(pattern);
        if (name.size() >= find_pattern.size()) {
            std::abort();
        }
        std::copy(name.begin(), name.end(), find_pattern.begin());
        find_pattern[name.size()] = L'\0';
        if (find_error != ERROR_SUCCESS) {
            ::SetLastError(find_error);
            return INVALID_HANDLE_VALUE;
        }
        if (scripted_enumeration) {
            ++live_finds;
            child_index = 0;
            SetChildName(information);
            return const_cast<FaultOperations*>(this);
        }
        const HANDLE handle = WindowsFileOperations::Find(pattern, information);
        if (handle != INVALID_HANDLE_VALUE) {
            ++live_finds;
        }
        return handle;
    }
    BOOL Next(HANDLE handle, WIN32_FIND_DATAW* information) const noexcept override {
        Record(NativeCall::Next);
        if (fail_next || ++next_calls == fail_next_call) {
            ::SetLastError(ERROR_READ_FAULT);
            return FALSE;
        }
        if (scripted_enumeration) {
            if (++child_index == ScriptedChildren.size()) {
                ::SetLastError(ERROR_NO_MORE_FILES);
                return FALSE;
            }
            SetChildName(information);
            return TRUE;
        }
        return WindowsFileOperations::Next(handle, information);
    }
    BOOL CloseFind(HANDLE handle) const noexcept override {
        Record(NativeCall::CloseFind);
        if (scripted_enumeration && handle != this) {
            std::abort();
        }
        const BOOL closed = scripted_enumeration ? TRUE : WindowsFileOperations::CloseFind(handle);
        if (closed) {
            --live_finds;
        }
        if (fail_find_close) {
            ::SetLastError(find_close_error);
            return FALSE;
        }
        return closed;
    }
    UINT DriveType(const wchar_t* volume) const noexcept override {
        ++drive_calls;
        return forced_drive_type == std::numeric_limits<UINT>::max()
                   ? WindowsFileOperations::DriveType(volume)
                   : forced_drive_type;
    }
    BOOL VolumeInformation(const wchar_t* volume, wchar_t* file_system,
                           DWORD size) const noexcept override {
        ++volume_calls;
        if (fail_volume) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        if (report_refs && size >= 5) {
            std::copy_n(L"ReFS", 5, file_system);
            return TRUE;
        }
        return WindowsFileOperations::VolumeInformation(volume, file_system, size);
    }
};

Status OpenRegularMode(WindowsFileSystem& file_system, const std::filesystem::path& path,
                       int mode) {
    const auto outcome = [](auto result) -> Status {
        if (!result.has_value()) {
            return std::unexpected(std::move(result).error());
        }
        return {};
    };
    switch (mode) {
        case 0:
            return outcome(file_system.OpenWritable(path));
        case 1:
            return outcome(file_system.OpenAppendable(path));
        case 2:
            return outcome(file_system.OpenSequential(path));
        case 3:
            return outcome(file_system.OpenRandomAccess(path));
        default:
            return outcome(file_system.LockFile(path));
    }
}

struct NativeHandleCloser {
    void operator()(void* handle) const noexcept {
        if (handle != INVALID_HANDLE_VALUE) {
            static_cast<void>(::CloseHandle(handle));
        }
    }
};

using NativeHandle = std::unique_ptr<void, NativeHandleCloser>;

bool RunLockChild(const std::filesystem::path& path, bool expect_busy) {
    const std::filesystem::path executable{MODERN_LEVELDB_WINDOWS_LOCK_CHILD};
    std::wstring command = L"\"" + executable.native() + L"\" " +
                           (expect_busy ? L"busy" : L"free") + L" \"" + path.native() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    if (!::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information)) {
        return false;
    }
    NativeHandle process(information.hProcess);
    NativeHandle thread(information.hThread);
    if (::WaitForSingleObject(process.get(), 4'000) != WAIT_OBJECT_0) {
        static_cast<void>(::TerminateProcess(process.get(), 1));
        static_cast<void>(::WaitForSingleObject(process.get(), 1'000));
        return false;
    }
    DWORD code = 0;
    return ::GetExitCodeProcess(process.get(), &code) && code == 0;
}

Status CreateJunction(const std::filesystem::path& link, const std::filesystem::path& target) {
    WindowsFileSystem file_system;
    const Status created = file_system.CreateDirectory(link);
    if (!created.has_value()) {
        return created;
    }
    NativeHandle directory(::CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                         FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                         nullptr));
    if (directory.get() == INVALID_HANDLE_VALUE) {
        return std::unexpected(Error::Io("could not open the junction directory"));
    }
    struct MountPoint {
        DWORD tag;
        WORD data_length;
        WORD reserved;
        WORD substitute_offset;
        WORD substitute_length;
        WORD print_offset;
        WORD print_length;
        std::array<wchar_t, 1'024> names;
    };
    static_assert(offsetof(MountPoint, names) == 16);
    MountPoint point{};
    const std::wstring substitute = L"\\??\\" + target.native();
    const std::wstring& printable = target.native();
    const std::size_t characters = substitute.size() + printable.size() + 2;
    if (characters > point.names.size()) {
        return std::unexpected(Error::InvalidArgument("junction fixture path is too long"));
    }
    point.tag = IO_REPARSE_TAG_MOUNT_POINT;
    point.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    point.print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    point.print_length = static_cast<WORD>(printable.size() * sizeof(wchar_t));
    point.data_length = static_cast<WORD>(8 + characters * sizeof(wchar_t));
    std::copy(substitute.begin(), substitute.end(), point.names.begin());
    std::copy(printable.begin(), printable.end(), point.names.begin() + substitute.size() + 1);
    DWORD received = 0;
    if (!::DeviceIoControl(directory.get(), FSCTL_SET_REPARSE_POINT, &point,
                           static_cast<DWORD>(point.data_length + 8), nullptr, 0, &received,
                           nullptr)) {
        return std::unexpected(Error::Io("could not create the junction fixture (Win32 " +
                                         std::to_string(::GetLastError()) + ")"));
    }
    return {};
}

void WriteFixture(const std::filesystem::path& path, std::string_view data) {
    std::ofstream output(path, std::ios::binary);
    ASSERT_TRUE(output.is_open());
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    ASSERT_TRUE(output.good());
}

TEST(WindowsFileSystemTest, ReadsSequentiallyAndReportsEof) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "sequential";
    WriteFixture(path, "abcdef");
    WindowsFileSystem file_system;
    auto file = file_system.OpenSequential(path);
    ASSERT_TRUE(file.has_value()) << file.error().ToString();
    std::array<std::byte, 4> output{};
    auto read = (*file)->Read(output);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, 4U);
    EXPECT_EQ(AsStringView(output), "abcd");
    read = (*file)->Read(output);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, 2U);
    EXPECT_EQ(AsStringView(ByteView(output).first(*read)), "ef");
    read = (*file)->Read(output);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, 0U);
    EXPECT_EQ((*file)->Read({}).value(), 0U);
}

TEST(WindowsFileSystemTest, PerformsConcurrentPositionedCopiedReads) {
    TemporaryDirectory directory;
    std::string data(8'192, '\0');
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index] = static_cast<char>(index % 127U);
    }
    const auto path = directory.path() / "random";
    WriteFixture(path, data);
    WindowsFileSystem file_system(false, false);
    auto opened = file_system.OpenRandomAccess(path, data.size());
    ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
    EXPECT_FALSE((*opened)->TryReadView(0, 1).has_value());
    std::atomic<unsigned> failures{0};
    std::vector<std::jthread> threads;
    for (std::size_t reader = 0; reader < 4; ++reader) {
        threads.emplace_back([&, reader] {
            for (std::size_t iteration = 0; iteration < 100; ++iteration) {
                const std::size_t offset = (reader * 997U + iteration * 53U) % 8'000U;
                std::array<std::byte, 65> storage{};
                const auto output = MutableByteView(storage).subspan(1);
                const auto read = (*opened)->Read(offset, output);
                if (!read.has_value() || *read != output.size() ||
                    AsStringView(output) != std::string_view(data).substr(offset, output.size())) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    threads.clear();
    EXPECT_EQ(failures.load(), 0U);
}

TEST(WindowsFileSystemTest, MapsExactNativeFilesByDefaultAndKeepsCopiedControl) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "mapped";
    WriteFixture(path, "abcdef");
    WindowsFileSystem mapped;
    auto file = mapped.OpenRandomAccess(path, 6);
    ASSERT_TRUE(file.has_value()) << file.error().ToString();
    const auto view = (*file)->TryReadView(1, 4);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(AsStringView(*view), "bcde");
    EXPECT_TRUE((*file)->TryReadView(6, 0).has_value());
    EXPECT_FALSE((*file)->TryReadView(6, 1).has_value());
    EXPECT_FALSE((*file)->TryReadView(7, 0).has_value());
    EXPECT_FALSE((*file)->TryReadView(std::numeric_limits<std::uint64_t>::max(), 1).has_value());
    EXPECT_FALSE((*file)->TryReadView(1, std::numeric_limits<std::size_t>::max()).has_value());
    std::array<std::byte, 8> bytes{};
    EXPECT_EQ((*file)->Read(4, bytes).value(), 2U);
    EXPECT_EQ(AsStringView(ByteView(bytes).first(2)), "ef");
    EXPECT_EQ((*file)->Read(6, bytes).value(), 0U);
    EXPECT_EQ((*file)->Read(0, {}).value(), 0U);
    EXPECT_FALSE((*file)->Read(std::numeric_limits<std::uint64_t>::max(), bytes).has_value());
    EXPECT_FALSE(
        (*file)
            ->Read(static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()), bytes)
            .has_value());
    EXPECT_EQ((*file)->Read(std::numeric_limits<std::uint64_t>::max(), {}).value(), 0U);
    WindowsFileSystem copied(false, false);
    auto control = copied.OpenRandomAccess(path, 6);
    ASSERT_TRUE(control.has_value());
    EXPECT_FALSE((*control)->TryReadView(0, 6).has_value());
    EXPECT_EQ((*control)->Read(0, bytes).value(), 6U);
}

TEST(WindowsFileSystemTest, MappedFilesReleaseAcquisitionHandlesAndOutliveFilesystem) {
    TemporaryDirectory directory;
    const auto path = directory.path() / std::filesystem::path{u8"mapped-\u4e2d-\U0001f680"};
    WriteFixture(path, "owned view");
    auto operations = std::make_shared<FaultOperations>();
    auto limiter = std::make_shared<MappedReadLimiter>(1);
    std::unique_ptr<RandomAccessFile> file;
    {
        WindowsFileSystem filesystem(false, operations, limiter);
        auto opened = filesystem.OpenRandomAccess(path, 10);
        ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
        file = std::move(*opened);
        EXPECT_EQ(operations->live_handles, 0U);
        EXPECT_EQ(operations->live_views, 1U);
    }
    ASSERT_TRUE(file->TryReadView(0, 10).has_value());
    EXPECT_EQ(AsStringView(*file->TryReadView(0, 10)), "owned view");
    file.reset();
    EXPECT_EQ(operations->live_views, 0U);
    WindowsFileSystem another(false, operations, limiter);
    auto reacquired = another.OpenRandomAccess(path, 10);
    ASSERT_TRUE(reacquired.has_value());
    EXPECT_TRUE((*reacquired)->TryReadView(0, 10).has_value());
}

TEST(WindowsFileSystemTest, ConcurrentMappedReadsDoNotUseCopiedIoOrInvalidateViews) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "mapped";
    std::string data(8'192, 'x');
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index] = static_cast<char>(index % 127U);
    }
    WriteFixture(path, data);
    auto operations = std::make_shared<FaultOperations>();
    WindowsFileSystem filesystem(false, operations, std::make_shared<MappedReadLimiter>(1));
    auto mapped = filesystem.OpenRandomAccess(path, data.size());
    ASSERT_TRUE(mapped.has_value());
    ASSERT_TRUE((*mapped)->TryReadView(0, data.size()).has_value());
    operations->fail_read = true;
    std::atomic<unsigned> failures{0};
    std::vector<std::jthread> threads;
    for (unsigned reader = 0; reader != 4; ++reader) {
        threads.emplace_back([&, reader] {
            for (unsigned index = 0; index != 100; ++index) {
                const std::size_t offset = (reader * 997U + index * 53U) % 8'000U;
                const auto view = (*mapped)->TryReadView(offset, 64);
                std::array<std::byte, 64> output{};
                auto read = (*mapped)->Read(offset, output);
                if (!view.has_value() ||
                    AsStringView(*view) != std::string_view(data).substr(offset, 64) ||
                    !read.has_value() || *read != 64 ||
                    AsStringView(output) != std::string_view(data).substr(offset, 64)) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    threads.clear();
    EXPECT_EQ(failures.load(), 0U);
    EXPECT_EQ(operations->completed_reads, 0U);
    EXPECT_EQ(operations->live_handles, 0U);
}

TEST(WindowsFileSystemTest, NativeMappedTablesPreserveBorrowedAndCompressedCacheability) {
    TemporaryDirectory directory;
    InternalKeyComparator comparator(BytewiseComparator());
    const auto key = InternalKey::Create(AsBytes("key"), 1, ValueKind::Value);
    const auto lookup = LookupKey::Create(AsBytes("key"), MaxSequenceNumber);
    ASSERT_TRUE(key.has_value());
    ASSERT_TRUE(lookup.has_value());
    const std::string expected(1'024, 'x');
    for (const auto compression :
         {BlockCompression::None, BlockCompression::Snappy, BlockCompression::Zstd}) {
        const auto path = directory.path() / std::to_string(static_cast<int>(compression));
        WindowsFileSystem filesystem;
        auto writable = filesystem.OpenWritable(path);
        ASSERT_TRUE(writable.has_value());
        TableBuilderOptions options;
        options.compression = compression;
        TableBuilder builder(std::move(*writable), comparator, options);
        ASSERT_TRUE(builder.Add(key->encoded(), AsBytes(expected)).has_value());
        ASSERT_TRUE(builder.Finish().has_value());
        BlockCache cache(1U << 20U);
        auto file = filesystem.OpenRandomAccess(path, builder.file_size());
        ASSERT_TRUE(file.has_value());
        ASSERT_TRUE(
            (*file)->TryReadView(0, static_cast<std::size_t>(builder.file_size())).has_value());
        auto table = Table::Open(std::move(*file), builder.file_size(), comparator,
                                 TableOptions{.block_cache = &cache});
        ASSERT_TRUE(table.has_value()) << table.error().ToString();
        std::vector<std::byte> value;
        ASSERT_EQ((*table)->Get(*lookup, value).value(), TableLookupKind::Value);
        EXPECT_EQ(AsStringView(value), expected);
        if (compression == BlockCompression::None) {
            EXPECT_EQ(cache.total_charge(), 0U);
        } else {
            EXPECT_GT(cache.total_charge(), 0U);
        }
        Table::Iterator iterator(**table);
        ASSERT_TRUE(iterator.SeekToFirst().has_value());
        EXPECT_EQ(AsStringView(iterator.value()), expected);
    }
}

TEST(WindowsFileSystemTest, SharesCountLimitAndRestoresExhaustedSlots) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "mapped";
    WriteFixture(path, "abc");
    auto limiter = std::make_shared<MappedReadLimiter>(1);
    auto operations = std::make_shared<WindowsFileOperations>();
    WindowsFileSystem first(false, operations, limiter);
    WindowsFileSystem second(false, operations, limiter);
    auto held = first.OpenRandomAccess(path, 3);
    ASSERT_TRUE(held.has_value());
    EXPECT_TRUE((*held)->TryReadView(0, 3).has_value());
    auto exhausted = second.OpenRandomAccess(path, 3);
    ASSERT_TRUE(exhausted.has_value());
    EXPECT_FALSE((*exhausted)->TryReadView(0, 3).has_value());
    held->reset();
    auto restored = second.OpenRandomAccess(path, 3);
    ASSERT_TRUE(restored.has_value());
    EXPECT_TRUE((*restored)->TryReadView(0, 3).has_value());
}

TEST(WindowsFileSystemTest, MappingEligibilityGuardsDoNotConsumeSlots) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "mapped";
    WriteFixture(path, "abc");
    auto operations = std::make_shared<WindowsFileOperations>();
    auto limiter = std::make_shared<MappedReadLimiter>(1);
    WindowsFileSystem filesystem(false, operations, limiter);
    for (const auto expected :
         {std::optional<std::uint64_t>{}, std::optional<std::uint64_t>{0},
          std::optional<std::uint64_t>{2}, std::optional<std::uint64_t>{4},
          std::optional<std::uint64_t>{std::numeric_limits<std::uint64_t>::max()}}) {
        auto copied = filesystem.OpenRandomAccess(path, expected);
        ASSERT_TRUE(copied.has_value());
        EXPECT_FALSE((*copied)->TryReadView(0, 3).has_value());
    }
    auto mapped = filesystem.OpenRandomAccess(path, 3);
    ASSERT_TRUE(mapped.has_value());
    EXPECT_TRUE((*mapped)->TryReadView(0, 3).has_value());
    WriteFixture(directory.path() / "empty", "");
    auto empty = filesystem.OpenRandomAccess(directory.path() / "empty", 0);
    ASSERT_TRUE(empty.has_value());
    EXPECT_FALSE((*empty)->TryReadView(0, 0).has_value());
    WindowsFileSystem disabled(false, operations, std::make_shared<MappedReadLimiter>(0));
    auto zero = disabled.OpenRandomAccess(path, 3);
    ASSERT_TRUE(zero.has_value());
    EXPECT_FALSE((*zero)->TryReadView(0, 3).has_value());
}

TEST(WindowsFileSystemTest, ConcurrentOpensReserveOnlyTheAvailableMappingSlots) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "mapped";
    WriteFixture(path, "abc");
    auto limiter = std::make_shared<MappedReadLimiter>(1);
    auto operations = std::make_shared<WindowsFileOperations>();
    std::latch ready(8);
    std::latch release(1);
    std::atomic<unsigned> mapped{0};
    std::atomic<unsigned> failures{0};
    std::vector<std::jthread> threads;
    for (unsigned index = 0; index != 8; ++index) {
        threads.emplace_back([&] {
            WindowsFileSystem filesystem(false, operations, limiter);
            auto opened = filesystem.OpenRandomAccess(path, 3);
            if (!opened.has_value()) {
                failures.fetch_add(1, std::memory_order_relaxed);
            } else if ((*opened)->TryReadView(0, 3).has_value()) {
                mapped.fetch_add(1, std::memory_order_relaxed);
            }
            ready.count_down();
            release.wait();
        });
    }
    ready.wait();
    EXPECT_EQ(mapped.load(), 1U);
    EXPECT_EQ(failures.load(), 0U);
    release.count_down();
    threads.clear();
    WindowsFileSystem filesystem(false, operations, limiter);
    auto opened = filesystem.OpenRandomAccess(path, 3);
    ASSERT_TRUE(opened.has_value());
    EXPECT_TRUE((*opened)->TryReadView(0, 3).has_value());
}

TEST(WindowsFileSystemTest, NativeMappingFailuresReportErrorsAndRollbackAllResources) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "mapped";
    WriteFixture(path, "abc");
    for (unsigned failure = 0; failure != 5; ++failure) {
        auto operations = std::make_shared<FaultOperations>();
        auto limiter = std::make_shared<MappedReadLimiter>(1);
        WindowsFileSystem filesystem(false, operations, limiter);
        operations->fail_size = failure == 0;
        operations->fail_mapping = failure == 1;
        operations->fail_map = failure == 2;
        operations->fail_close = failure == 3;
        operations->fail_close_call = failure == 4 ? 2 : 0;
        auto failed = filesystem.OpenRandomAccess(path, 3);
        ASSERT_FALSE(failed.has_value()) << failure;
        EXPECT_EQ(failed.error().code(), ErrorCode::Io);
        EXPECT_EQ(operations->live_handles, 0U);
        EXPECT_EQ(operations->live_views, 0U);
        operations->fail_size = operations->fail_mapping = operations->fail_map =
            operations->fail_close = false;
        operations->fail_close_call = 0;
        auto recovered = filesystem.OpenRandomAccess(path, 3);
        ASSERT_TRUE(recovered.has_value()) << recovered.error().ToString();
        EXPECT_TRUE((*recovered)->TryReadView(0, 3).has_value());
    }
}

TEST(WindowsFileSystemTest, MappingAdmissionPreservesNativeQueryAndSlotOrder) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "admission";
    WriteFixture(path, "abc");
    for (unsigned scenario = 0; scenario != 9; ++scenario) {
        SCOPED_TRACE(scenario);
        auto operations = std::make_shared<FaultOperations>();
        auto limiter = std::make_shared<MappedReadLimiter>(scenario == 4 ? 0 : 1);
        WindowsFileSystem file_system(false, operations, scenario == 0 ? nullptr : limiter);
        std::optional<std::uint64_t> expected = 3;
        if (scenario == 1) {
            expected.reset();
        } else if (scenario == 2) {
            expected = 0;
        } else if (scenario == 3) {
            expected = std::numeric_limits<std::uint64_t>::max();
        }
        std::unique_ptr<RandomAccessFile> held;
        if (scenario == 5) {
            auto mapped = file_system.OpenRandomAccess(path, 3);
            ASSERT_TRUE(mapped.has_value());
            held = std::move(*mapped);
        }
        if (scenario >= 6) {
            operations->reported_size = scenario == 6 ? -1 : (scenario == 7 ? 2 : 4);
        }
        operations->call_count = 0;
        auto copied = file_system.OpenRandomAccess(path, expected);
        ASSERT_TRUE(copied.has_value());
        EXPECT_FALSE((*copied)->TryReadView(0, 3).has_value());
        std::vector<NativeCall> calls{NativeCall::Open, NativeCall::Inspect};
        if (scenario >= 6) {
            calls.push_back(NativeCall::Size);
        }
        EXPECT_EQ(operations->Trace(), calls);
        std::array<std::byte, 3> bytes{};
        ASSERT_EQ((*copied)->Read(0, bytes).value(), bytes.size());
        EXPECT_EQ(AsStringView(bytes), "abc");
        copied->reset();
        held.reset();
        EXPECT_EQ(operations->live_handles, 0U);
        EXPECT_EQ(operations->live_views, 0U);
        operations->reported_size.reset();
        if (scenario != 4) {
            WindowsFileSystem enabled(false, operations, limiter);
            auto restored = enabled.OpenRandomAccess(path, 3);
            ASSERT_TRUE(restored.has_value());
            EXPECT_TRUE((*restored)->TryReadView(0, 3).has_value());
        }
    }
}

TEST(WindowsFileSystemTest, MappingPublicationAndFailureKeepExactNativePrefixes) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "publication";
    WriteFixture(path, "abc");
    for (unsigned scenario = 0; scenario != 6; ++scenario) {
        SCOPED_TRACE(scenario);
        auto operations = std::make_shared<FaultOperations>();
        auto limiter = std::make_shared<MappedReadLimiter>(1);
        WindowsFileSystem file_system(false, operations, limiter);
        operations->fail_size = scenario == 1;
        operations->fail_mapping = scenario == 2;
        operations->fail_map = scenario == 3;
        operations->fail_close = scenario >= 1 && scenario <= 3;
        operations->fail_close_call = scenario == 4 ? 1 : (scenario == 5 ? 2 : 0);
        auto result = file_system.OpenRandomAccess(path, 3);
        std::vector<NativeCall> expected{NativeCall::Open, NativeCall::Inspect, NativeCall::Size};
        if (scenario != 1) {
            expected.push_back(NativeCall::Mapping);
            if (scenario != 2) {
                expected.push_back(NativeCall::Map);
                expected.push_back(NativeCall::Close);
                if (scenario == 4) {
                    expected.push_back(NativeCall::Unmap);
                }
            }
        }
        expected.push_back(NativeCall::Close);
        if (scenario == 5) {
            expected.push_back(NativeCall::Unmap);
        }
        EXPECT_EQ(operations->Trace(), expected);
        EXPECT_EQ(operations->live_handles, 0U);
        if (scenario == 0) {
            ASSERT_TRUE(result.has_value());
            EXPECT_EQ(operations->live_views, 1U);
            EXPECT_EQ(operations->closed_handles[0], operations->acquired_section);
            EXPECT_EQ(operations->closed_handles[1], operations->acquired_file);
            EXPECT_EQ(AsStringView(*(*result)->TryReadView(0, 3)), "abc");
            result->reset();
        } else {
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error().code(), ErrorCode::Io);
            const std::array<std::string_view, 5> names{"mapped file size", "create file mapping",
                                                        "map file view", "close", "close"};
            const std::array<DWORD, 5> errors{ERROR_READ_FAULT, ERROR_NOT_ENOUGH_MEMORY,
                                              ERROR_NOT_ENOUGH_MEMORY, ERROR_WRITE_FAULT,
                                              ERROR_WRITE_FAULT};
            EXPECT_TRUE(result.error().message().starts_with(names[scenario - 1]));
            EXPECT_NE(
                result.error().message().find("Win32 " + std::to_string(errors[scenario - 1])),
                std::string_view::npos);
        }
        EXPECT_EQ(operations->live_views, 0U);
        operations->fail_size = operations->fail_mapping = operations->fail_map =
            operations->fail_close = false;
        operations->fail_close_call = 0;
        auto restored = file_system.OpenRandomAccess(path, 3);
        ASSERT_TRUE(restored.has_value());
        EXPECT_TRUE((*restored)->TryReadView(0, 3).has_value());
    }
}

TEST(WindowsFileSystemTest, MappedViewsSurviveNativeRenameRemovalAndAllowReuseAfterRelease) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "mapped";
    const auto renamed = directory.path() / "renamed";
    WriteFixture(path, "abc");
    WindowsFileSystem filesystem;
    auto mapped = filesystem.OpenRandomAccess(path, 3);
    ASSERT_TRUE(mapped.has_value());
    ASSERT_TRUE((*mapped)->TryReadView(0, 3).has_value());
    ASSERT_TRUE(filesystem.RenameFile(path, renamed).has_value());
    EXPECT_EQ(AsStringView(*(*mapped)->TryReadView(0, 3)), "abc");
    ASSERT_TRUE(filesystem.RemoveFile(renamed).has_value());
    EXPECT_EQ(AsStringView(*(*mapped)->TryReadView(0, 3)), "abc");
    mapped->reset();
    auto reused = filesystem.OpenWritable(renamed);
    ASSERT_TRUE(reused.has_value()) << reused.error().ToString();
    ASSERT_TRUE((*reused)->Append(AsBytes("new")).has_value());
    ASSERT_TRUE((*reused)->Close().has_value());
}

TEST(WindowsFileSystemDeathTest, FailedOwnedViewUnmapDoesNotRecycleAnActiveSlot) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "mapped";
    WriteFixture(path, "abc");
    EXPECT_DEATH(
        {
            auto operations = std::make_shared<FaultOperations>();
            WindowsFileSystem filesystem(false, operations, std::make_shared<MappedReadLimiter>(1));
            auto mapped = filesystem.OpenRandomAccess(path, 3);
            if (!mapped.has_value() || !(*mapped)->TryReadView(0, 3).has_value()) {
                std::exit(0);
            }
            operations->fail_unmap = true;
            mapped->reset();
        },
        "");
}

TEST(WindowsFileSystemTest, RandomReadsHandleShortEofAndInvalidOffsets) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "random";
    WriteFixture(path, "abc");
    WindowsFileSystem file_system;
    auto file = file_system.OpenRandomAccess(path);
    ASSERT_TRUE(file.has_value());
    std::array<std::byte, 8> output{};
    const auto read = (*file)->Read(2, output);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, 1U);
    EXPECT_EQ(output[0], std::byte{'c'});
    EXPECT_EQ((*file)->Read(100, output).value(), 0U);
    EXPECT_EQ((*file)->Read(0, {}).value(), 0U);
    const auto invalid = (*file)->Read(std::numeric_limits<std::uint64_t>::max(), output);
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().code(), ErrorCode::InvalidArgument);
}

TEST(WindowsFileSystemTest, BuffersSyncsAppendsAndReopensWrites) {
    TemporaryDirectory directory;
    WindowsFileSystem file_system;
    const auto path = directory.path() / "writer";
    const std::string large(200'000, 'x');
    auto file = file_system.OpenWritable(path);
    ASSERT_TRUE(file.has_value()) << file.error().ToString();
    ASSERT_TRUE((*file)->Append(AsBytes("prefix")).has_value());
    EXPECT_EQ(file_system.FileSize(path).value(), 0U);
    ASSERT_TRUE((*file)->Flush().has_value());
    EXPECT_EQ(file_system.FileSize(path).value(), 6U);
    ASSERT_TRUE((*file)->Append(AsBytes(large)).has_value());
    ASSERT_TRUE((*file)->Sync().has_value());
    ASSERT_TRUE((*file)->Close().has_value());
    auto append = file_system.OpenAppendable(path);
    ASSERT_TRUE(append.has_value());
    ASSERT_TRUE((*append)->Append(AsBytes("suffix")).has_value());
    ASSERT_TRUE((*append)->Close().has_value());
    auto reader = file_system.OpenSequential(path);
    ASSERT_TRUE(reader.has_value());
    std::vector<std::byte> bytes(large.size() + 12);
    ASSERT_EQ((*reader)->Read(bytes).value(), bytes.size());
    EXPECT_EQ(AsStringView(bytes), "prefix" + large + "suffix");
}

TEST(WindowsFileSystemTest, CreatesListsReplacesAndRemovesNames) {
    TemporaryDirectory directory;
    WindowsFileSystem file_system(true);
    const auto child = directory.path() / "child";
    ASSERT_TRUE(file_system.CreateDirectory(child).has_value());
    ASSERT_TRUE(file_system.CreateDirectory(child).has_value());
    auto names = file_system.ListDirectory(child);
    ASSERT_TRUE(names.has_value());
    EXPECT_TRUE(names->empty());
    WriteFixture(child / "source", "new");
    WriteFixture(child / "target", "old");
    ASSERT_TRUE(file_system.RenameFile(child / "source", child / "target").has_value());
    EXPECT_FALSE(file_system.FileExists(child / "source").value());
    EXPECT_EQ(file_system.FileSize(child / "target").value(), 3U);
    names = file_system.ListDirectory(child);
    ASSERT_TRUE(names.has_value());
    EXPECT_EQ(*names, std::vector<std::filesystem::path>{"target"});
    ASSERT_TRUE(file_system.SyncDirectory(child).has_value());
    ASSERT_TRUE(file_system.RemoveFile(child / "target").has_value());
    ASSERT_TRUE(file_system.RemoveDirectory(child).has_value());
    EXPECT_FALSE(file_system.FileExists(child).value());
}

TEST(WindowsFileSystemTest, RejectsCreateDirectoryOverExistingFile) {
    TemporaryDirectory directory;
    WindowsFileSystem file_system(true);
    const auto path = directory.path() / "file";
    WriteFixture(path, "");

    const Status status = file_system.CreateDirectory(path);

    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(status.error().message(), "an existing file is not a directory");
    EXPECT_TRUE(std::string_view(status.error().location().file_name())
                    .ends_with("windows_file_system.cc"));
    EXPECT_NE(std::string_view(status.error().location().function_name())
                  .find("WindowsFileSystem::CreateDirectory"),
              std::string_view::npos);
}

TEST(WindowsFileSystemTest, LocksContendAcrossInstancesAndReleaseThroughRaii) {
    TemporaryDirectory directory;
    WindowsFileSystem first;
    WindowsFileSystem second;
    const auto path = directory.path() / "LOCK";
    {
        auto lock = first.LockFile(path);
        ASSERT_TRUE(lock.has_value()) << lock.error().ToString();
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            const auto busy = second.LockFile(path);
            ASSERT_FALSE(busy.has_value());
            EXPECT_EQ(busy.error().code(), ErrorCode::Busy);
        }
    }
    EXPECT_TRUE(second.LockFile(path).has_value());
}

TEST(WindowsFileSystemTest, PreservesNativeUnicodeNames) {
    TemporaryDirectory directory;
    const std::filesystem::path name{u8"\u6570\u636e-\U0001f4be"};
    const auto child = directory.path() / name;
    WindowsFileSystem file_system(true);
    ASSERT_TRUE(file_system.CreateDirectory(child).has_value());
    auto writer = file_system.OpenWritable(child / name);
    ASSERT_TRUE(writer.has_value()) << writer.error().ToString();
    ASSERT_TRUE((*writer)->Append(AsBytes("unicode")).has_value());
    ASSERT_TRUE((*writer)->Close().has_value());
    const auto names = file_system.ListDirectory(child);
    ASSERT_TRUE(names.has_value());
    EXPECT_EQ(*names, std::vector<std::filesystem::path>{name});
}

TEST(WindowsFileSystemTest, RejectsInvalidPathsAndClassifiesMissingFiles) {
    WindowsFileSystem file_system(true);
    const std::filesystem::path null_path{std::wstring(L"bad\0path", 8)};
    for (const auto& path : {std::filesystem::path{}, null_path, std::filesystem::path{"bad. "}}) {
        const auto file = file_system.OpenSequential(path);
        ASSERT_FALSE(file.has_value());
        EXPECT_EQ(file.error().code(), ErrorCode::InvalidArgument);
    }
    TemporaryDirectory directory;
    const auto missing = directory.path() / "missing";
    EXPECT_FALSE(file_system.FileExists(missing).value());
    const auto missing_file = file_system.OpenSequential(missing);
    ASSERT_FALSE(missing_file.has_value());
    EXPECT_EQ(missing_file.error().code(), ErrorCode::NotFound);
    EXPECT_TRUE(std::string_view(missing_file.error().location().file_name())
                    .ends_with("windows_file_system.cc"));
    EXPECT_NE(std::string_view(missing_file.error().location().function_name()).find("FileError"),
              std::string_view::npos);
    EXPECT_EQ(file_system.ListDirectory(missing).error().code(), ErrorCode::NotFound);
}

TEST(WindowsFileSystemTest, DefaultFactoryRejectsMissingConsentWithoutChangingDirectory) {
    TemporaryDirectory directory;
    auto path = directory.path() / "new-db";
    const auto original = path;

    const auto rejected = CreateDefaultFileSystem(path, false, true);

    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), ErrorCode::NotSupported);
    EXPECT_EQ(path, original);
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(WindowsFileSystemTest, DefaultFactoryPreparesDirectoryAndPreservesMmapPolicy) {
    TemporaryDirectory directory;
    for (const bool allow_mmap_reads : {false, true}) {
        auto path = directory.path() / (allow_mmap_reads ? "mapped" : "copied");
        auto created = CreateDefaultFileSystem(path, true, allow_mmap_reads);
        ASSERT_TRUE(created.has_value()) << created.error().ToString();
        ASSERT_NE(created->get(), nullptr);
        EXPECT_TRUE(path.is_absolute());
        EXPECT_FALSE(std::filesystem::exists(path));
        auto& file_system = **created;
        ASSERT_TRUE(file_system.CreateDirectory(path).has_value());
        auto file = file_system.OpenWritable(path / "data");
        ASSERT_TRUE(file.has_value()) << file.error().ToString();
        ASSERT_TRUE((*file)->Append(AsBytes("value")).has_value());
        ASSERT_TRUE((*file)->Close().has_value());
        auto reader = file_system.OpenRandomAccess(path / "data", 5);
        ASSERT_TRUE(reader.has_value()) << reader.error().ToString();
        const auto view = (*reader)->TryReadView(0, 5);
        EXPECT_EQ(view.has_value(), allow_mmap_reads);
        if (view.has_value()) {
            EXPECT_EQ(AsStringView(*view), "value");
        }
    }
}

TEST(WindowsFileSystemTest, StrictNamespaceConsentFailsBeforeCreatingADatabase) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "new-db";
    WindowsFileSystem strict;
    const auto rejected = strict.PrepareDatabaseDirectory(path);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), ErrorCode::NotSupported);
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_EQ(strict.SyncDirectory(directory.path()).error().code(), ErrorCode::NotSupported);
    WindowsFileSystem weak(true);
    const auto prepared = weak.PrepareDatabaseDirectory(path);
    ASSERT_TRUE(prepared.has_value()) << prepared.error().ToString();
    EXPECT_TRUE(prepared->is_absolute());
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(WindowsFileSystemTest, SharingContentionOnLockOpenIsBusy) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "LOCK";
    const HANDLE exclusive = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(exclusive, INVALID_HANDLE_VALUE);
    WindowsFileSystem file_system;
    const auto lock = file_system.LockFile(path);
    EXPECT_FALSE(lock.has_value());
    if (!lock.has_value()) {
        EXPECT_EQ(lock.error().code(), ErrorCode::Busy);
    }
    EXPECT_TRUE(::CloseHandle(exclusive));
}

TEST(WindowsFileSystemTest, ClampsNativeRequestsWithoutNarrowingOverflow) {
    EXPECT_EQ(WindowsIoRequestSize(0), 0U);
    EXPECT_EQ(WindowsIoRequestSize(100), 100U);
    EXPECT_EQ(WindowsIoRequestSize(static_cast<std::size_t>(MAXDWORD) + 1), MAXDWORD);
    EXPECT_EQ(WindowsIoRequestSize(std::numeric_limits<std::size_t>::max()), MAXDWORD);
}

TEST(WindowsFileSystemTest, ReadsSparseOffsetsBeyondFourGiB) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "sparse";
    constexpr std::uint64_t Offset = (std::uint64_t{1} << 32U) + 7U;
    {
        NativeHandle handle(::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                          FILE_ATTRIBUTE_NORMAL, nullptr));
        ASSERT_NE(handle.get(), INVALID_HANDLE_VALUE);
        DWORD received = 0;
        ASSERT_TRUE(::DeviceIoControl(handle.get(), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                                      &received, nullptr));
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(Offset);
        ASSERT_TRUE(::SetFilePointerEx(handle.get(), position, nullptr, FILE_BEGIN));
        DWORD written = 0;
        ASSERT_TRUE(::WriteFile(handle.get(), "end", 3, &written, nullptr));
        ASSERT_EQ(written, 3U);
    }
    WindowsFileSystem file_system;
    EXPECT_EQ(file_system.FileSize(path).value(), Offset + 3U);
    auto file = file_system.OpenRandomAccess(path);
    ASSERT_TRUE(file.has_value());
    std::array<std::byte, 4> output{};
    const auto read = (*file)->Read(Offset, output);
    ASSERT_TRUE(read.has_value());
    ASSERT_EQ(*read, 3U);
    EXPECT_EQ(AsStringView(ByteView(output).first(*read)), "end");
}

TEST(WindowsFileSystemTest, ResolvesLongOrdinaryAndExtendedPaths) {
    TemporaryDirectory directory;
    WindowsFileSystem file_system;
    auto path = directory.path();
    for (unsigned index = 0; index < 6; ++index) {
        path /= std::string(45, 'a') + std::to_string(index);
        ASSERT_TRUE(file_system.CreateDirectory(path).has_value());
    }
    ASSERT_GT(path.native().size(), static_cast<std::size_t>(MAX_PATH));
    const auto ordinary = path / "file";
    auto writer = file_system.OpenWritable(ordinary);
    ASSERT_TRUE(writer.has_value()) << writer.error().ToString();
    ASSERT_TRUE((*writer)->Append(AsBytes("long")).has_value());
    ASSERT_TRUE((*writer)->Close().has_value());
    const std::filesystem::path extended(L"\\\\?\\" + ordinary.native());
    auto reader = file_system.OpenSequential(extended);
    ASSERT_TRUE(reader.has_value());
    std::array<std::byte, 4> bytes{};
    ASSERT_EQ((*reader)->Read(bytes).value(), bytes.size());
    EXPECT_EQ(AsStringView(bytes), "long");
}

TEST(WindowsFileSystemTest, DistinguishesOrdinaryAndExtendedLiteralSuffixes) {
    TemporaryDirectory directory;
    WindowsFileSystem file_system;
    for (const std::wstring suffix : {L"literal.", L"literal "}) {
        const auto ordinary = directory.path() / suffix;
        const auto rejected = file_system.OpenWritable(ordinary);
        ASSERT_FALSE(rejected.has_value());
        EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
        const std::filesystem::path extended(L"\\\\?\\" + ordinary.native());
        auto writer = file_system.OpenWritable(extended);
        ASSERT_TRUE(writer.has_value()) << writer.error().ToString();
        ASSERT_TRUE((*writer)->Close().has_value());
        EXPECT_TRUE(file_system.FileExists(extended).value());
        ASSERT_TRUE(file_system.RemoveFile(extended).has_value());
    }
    const auto device = file_system.OpenWritable(directory.path() / "NUL.txt");
    ASSERT_FALSE(device.has_value());
    EXPECT_EQ(device.error().code(), ErrorCode::NotSupported);
    EXPECT_EQ(file_system.OpenSequential(L"\\\\.\\NUL").error().code(), ErrorCode::NotSupported);
}

TEST(WindowsFileSystemTest, ShortWritesPreserveEveryByteAndOrder) {
    TemporaryDirectory directory;
    auto operations = std::make_shared<FaultOperations>();
    operations->maximum_write = 7;
    WindowsFileSystem file_system(false, operations);
    const auto path = directory.path() / "short";
    auto file = file_system.OpenWritable(path);
    ASSERT_TRUE(file.has_value());
    const std::string bytes(150'000, 'x');
    ASSERT_TRUE((*file)->Append(AsBytes("prefix")).has_value());
    ASSERT_TRUE((*file)->Append(AsBytes(bytes)).has_value());
    ASSERT_TRUE((*file)->Append(AsBytes("suffix")).has_value());
    ASSERT_TRUE((*file)->Close().has_value());
    EXPECT_GT(operations->write_calls, 1U);
    EXPECT_EQ(operations->live_handles, 0U);
    auto reader = file_system.OpenSequential(path);
    ASSERT_TRUE(reader.has_value());
    std::vector<std::byte> output(bytes.size() + 12);
    ASSERT_EQ((*reader)->Read(output).value(), output.size());
    EXPECT_EQ(AsStringView(output), "prefix" + bytes + "suffix");
}

TEST(WindowsFileSystemTest, ZeroWriteAndDiskFullErrorsStayStickyThroughClose) {
    TemporaryDirectory directory;
    for (const bool zero : {false, true}) {
        auto operations = std::make_shared<FaultOperations>();
        operations->zero_write = zero;
        operations->fail_write = !zero;
        WindowsFileSystem file_system(false, operations);
        auto file = file_system.OpenWritable(directory.path() / (zero ? "zero" : "full"));
        ASSERT_TRUE(file.has_value());
        ASSERT_TRUE((*file)->Append(AsBytes("buffered")).has_value());
        const auto failed = (*file)->Flush();
        ASSERT_FALSE(failed.has_value());
        EXPECT_EQ(failed.error().code(), ErrorCode::Io);
        const auto message = failed.error().ToString();
        EXPECT_EQ((*file)->Append(AsBytes("later")).error().ToString(), message);
        EXPECT_EQ((*file)->Sync().error().ToString(), message);
        operations->fail_close = true;
        EXPECT_EQ((*file)->Close().error().ToString(), message);
        EXPECT_EQ(operations->live_handles, 0U);
        EXPECT_EQ(operations->write_calls, 1U);
        EXPECT_EQ(operations->sync_calls, 0U);
        EXPECT_EQ(operations->close_calls, 1U);
    }
}

TEST(WindowsFileSystemTest, SyncFailureStaysStickyAndCloseStillReleasesHandle) {
    TemporaryDirectory directory;
    auto operations = std::make_shared<FaultOperations>();
    operations->fail_sync = true;
    WindowsFileSystem file_system(false, operations);
    auto file = file_system.OpenWritable(directory.path() / "sync");
    ASSERT_TRUE(file.has_value());
    ASSERT_TRUE((*file)->Append(AsBytes("content")).has_value());
    const auto failed = (*file)->Sync();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code(), ErrorCode::Io);
    EXPECT_EQ((*file)->Append({}).error().ToString(), failed.error().ToString());
    EXPECT_EQ((*file)->Close().error().ToString(), failed.error().ToString());
    EXPECT_EQ(operations->live_handles, 0U);
}

TEST(WindowsFileSystemTest, CloseFailureIsReportedOnceWithoutRetryOrDestructorReclose) {
    TemporaryDirectory directory;
    auto operations = std::make_shared<FaultOperations>();
    WindowsFileSystem file_system(false, operations);
    {
        auto file = file_system.OpenWritable(directory.path() / "close");
        ASSERT_TRUE(file.has_value());
        ASSERT_TRUE((*file)->Append(AsBytes("content")).has_value());
        operations->fail_close = true;
        const auto closed = (*file)->Close();
        ASSERT_FALSE(closed.has_value());
        EXPECT_EQ(closed.error().code(), ErrorCode::Io);
        EXPECT_EQ(operations->live_handles, 0U);
    }
    EXPECT_EQ(operations->close_calls, 1U);
}

TEST(WindowsFileSystemTest, DestructorFlushesUnsyncedBufferAndReleasesItsHandle) {
    TemporaryDirectory directory;
    auto operations = std::make_shared<FaultOperations>();
    WindowsFileSystem file_system(false, operations);
    const auto path = directory.path() / "destructor";
    {
        auto file = file_system.OpenWritable(path);
        ASSERT_TRUE(file.has_value());
        ASSERT_TRUE((*file)->Append(AsBytes("buffered")).has_value());
    }
    EXPECT_EQ(file_system.FileSize(path).value(), 8U);
    EXPECT_EQ(operations->live_handles, 0U);
    EXPECT_EQ(operations->sync_calls, 0U);
}

TEST(WindowsFileSystemTest, PartialAcquisitionFailureReleasesHandlesAndRetainsNativeError) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "file";
    WriteFixture(path, "content");
    auto operations = std::make_shared<FaultOperations>();
    WindowsFileSystem file_system(false, operations);
    operations->fail_open = true;
    const auto denied = file_system.OpenSequential(path);
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().code(), ErrorCode::Io);
    EXPECT_NE(denied.error().message().find("Win32 5"), std::string_view::npos);
    operations->fail_open = false;
    operations->fail_inspect = true;
    const auto rejected = file_system.OpenRandomAccess(path);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), ErrorCode::Io);
    EXPECT_EQ(operations->live_handles, 0U);
}

TEST(WindowsFileSystemTest, RegularOpeningModesKeepTheirExactNativePolicies) {
    struct Policy {
        DWORD access;
        DWORD share;
        DWORD disposition;
        DWORD flags;
    };
    const std::array<Policy, 5> policies{{
        {GENERIC_WRITE, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL},
        {GENERIC_WRITE, 0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL},
        {GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL},
        {GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, OPEN_EXISTING, FILE_FLAG_OVERLAPPED},
        {GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS,
         FILE_ATTRIBUTE_NORMAL},
    }};
    TemporaryDirectory directory;
    const auto path = directory.path() / "regular";
    for (int mode = 0; mode < 5; ++mode) {
        SCOPED_TRACE(mode);
        WriteFixture(path, "content");
        const auto operations = std::make_shared<FaultOperations>();
        WindowsFileSystem file_system(true, operations, nullptr);
        ASSERT_TRUE(OpenRegularMode(file_system, path, mode).has_value());
        const auto& policy = policies[static_cast<std::size_t>(mode)];
        EXPECT_EQ(operations->opened_access, policy.access);
        EXPECT_EQ(operations->opened_share, policy.share);
        EXPECT_EQ(operations->opened_disposition, policy.disposition);
        EXPECT_EQ(operations->opened_flags, policy.flags);
        EXPECT_TRUE(operations->opened_path.starts_with(L"\\\\?\\"));
        EXPECT_TRUE(operations->opened_path.ends_with(path.filename().native()));
        EXPECT_EQ(operations->open_calls, 1U);
        EXPECT_EQ(operations->inspect_calls, 1U);
        EXPECT_EQ(operations->close_calls, 1U);
        EXPECT_EQ(operations->live_handles, 0U);
    }
}

TEST(WindowsFileSystemTest, RegularOpeningModesRejectPathSyntaxBeforeNativeOpen) {
    struct InvalidPath {
        std::filesystem::path path;
        ErrorCode code;
        std::string_view message;
    };
    const std::vector<InvalidPath> invalid{
        {{}, ErrorCode::InvalidArgument, "filesystem path is empty or contains a null"},
        {std::filesystem::path{std::wstring(L"bad\0path", 8)}, ErrorCode::InvalidArgument,
         "filesystem path is empty or contains a null"},
        {L"\\\\.\\device", ErrorCode::NotSupported, "device namespace paths are not supported"},
        {L"\\\\?\\GLOBALROOT\\device", ErrorCode::NotSupported,
         "this extended path namespace is not supported"},
        {L"CON", ErrorCode::NotSupported, "DOS device names require an explicit file path"},
        {L"CON. ", ErrorCode::InvalidArgument,
         "ordinary DOS path components must not end in a dot or space"},
    };
    for (int mode = 0; mode < 5; ++mode) {
        SCOPED_TRACE(mode);
        const auto operations = std::make_shared<FaultOperations>();
        operations->fail_open = true;
        WindowsFileSystem file_system(true, operations, nullptr);
        for (const auto& test_case : invalid) {
            const Status result = OpenRegularMode(file_system, test_case.path, mode);
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error().code(), test_case.code);
            EXPECT_EQ(result.error().message(), test_case.message);
        }
        EXPECT_EQ(operations->open_calls, 0U);
        EXPECT_EQ(operations->inspect_calls, 0U);
        EXPECT_EQ(operations->close_calls, 0U);
    }
}

TEST(WindowsFileSystemTest, RegularAcquisitionFailurePreservesPrimaryErrorAndClosesOnce) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "regular";
    for (int mode = 0; mode < 5; ++mode) {
        for (const bool open_failure : {true, false}) {
            SCOPED_TRACE(testing::Message() << mode << "/" << open_failure);
            WriteFixture(path, "content");
            const auto operations = std::make_shared<FaultOperations>();
            operations->fail_open = open_failure;
            operations->fail_inspect = true;
            operations->fail_close = true;
            WindowsFileSystem file_system(true, operations, nullptr);
            const Status result = OpenRegularMode(file_system, path, mode);
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error().code(), ErrorCode::Io);
            const std::string expected =
                open_failure ? "open '"
                             : "file information '" + PathUtf8(path).value() + "' (Win32 30)";
            EXPECT_TRUE(result.error().message().starts_with(expected));
            if (open_failure) {
                EXPECT_NE(result.error().message().find("Win32 5"), std::string_view::npos);
            }
            EXPECT_EQ(operations->open_calls, 1U);
            EXPECT_EQ(operations->inspect_calls, open_failure ? 0U : 1U);
            EXPECT_EQ(operations->close_calls, open_failure ? 0U : 1U);
            EXPECT_EQ(operations->live_handles, 0U);
        }
    }
}

TEST(WindowsFileSystemTest, PendingReadsCompleteBeforeTheirEventIsReleased) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "pending";
    WriteFixture(path, "content");
    auto operations = std::make_shared<FaultOperations>();
    operations->force_pending = true;
    WindowsFileSystem file_system(false, operations);
    auto file = file_system.OpenRandomAccess(path);
    ASSERT_TRUE(file.has_value());
    std::array<std::byte, 7> bytes{};
    ASSERT_EQ((*file)->Read(0, bytes).value(), bytes.size());
    EXPECT_EQ(AsStringView(bytes), "content");
    EXPECT_EQ(operations->completed_reads, 1U);
    EXPECT_EQ(operations->live_handles, 1U);
    operations->fail_complete = true;
    const auto failed = (*file)->Read(0, bytes);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code(), ErrorCode::Io);
    EXPECT_NE(failed.error().message().find("Win32 23"), std::string_view::npos);
    EXPECT_EQ(operations->live_handles, 1U);
}

TEST(WindowsFileSystemTest, EventAndReadFailureDoNotLeakPerReadResources) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "read-failures";
    WriteFixture(path, "content");
    auto operations = std::make_shared<FaultOperations>();
    WindowsFileSystem file_system(false, operations);
    auto file = file_system.OpenRandomAccess(path);
    ASSERT_TRUE(file.has_value());
    std::array<std::byte, 7> output{};
    operations->fail_event = true;
    EXPECT_EQ((*file)->Read(0, output).error().code(), ErrorCode::Io);
    operations->fail_event = false;
    operations->fail_read = true;
    EXPECT_EQ((*file)->Read(0, output).error().code(), ErrorCode::Io);
    EXPECT_EQ(operations->live_handles, 1U);
}

TEST(WindowsFileSystemTest, PositionedReadRangeGuardsMatchCopiedAndMappedOwners) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "range";
    WriteFixture(path, "abc");
    constexpr auto MaximumOffset =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    for (bool mapped : {false, true}) {
        SCOPED_TRACE(mapped);
        auto operations = std::make_shared<FaultOperations>();
        WindowsFileSystem file_system(false, operations,
                                      mapped ? std::make_shared<MappedReadLimiter>(1) : nullptr);
        auto file = file_system.OpenRandomAccess(path, 3);
        ASSERT_TRUE(file.has_value());
        ASSERT_EQ((*file)->TryReadView(0, 3).has_value(), mapped);
        operations->call_count = 0;
        EXPECT_EQ((*file)->Read(std::numeric_limits<std::uint64_t>::max(), {}).value(), 0U);
        EXPECT_TRUE(operations->Trace().empty());
        std::array<std::byte, 2> bytes{};
        for (auto offset : {MaximumOffset - 1, std::numeric_limits<std::uint64_t>::max()}) {
            const auto invalid = (*file)->Read(offset, bytes);
            ASSERT_FALSE(invalid.has_value());
            EXPECT_EQ(invalid.error().code(), ErrorCode::InvalidArgument);
            EXPECT_EQ(invalid.error().message(), "read range exceeds signed 64-bit offsets");
            EXPECT_TRUE(operations->Trace().empty());
        }
        operations->fail_read = true;
        operations->read_error = ERROR_HANDLE_EOF;
        EXPECT_EQ((*file)->Read(MaximumOffset - 1, MutableByteView(bytes).first(1)).value(), 0U);
        if (mapped) {
            EXPECT_TRUE(operations->Trace().empty());
        } else {
            const std::vector<NativeCall> expected{NativeCall::Event, NativeCall::Read,
                                                   NativeCall::Close};
            EXPECT_EQ(operations->Trace(), expected);
            EXPECT_EQ(operations->read_operation.Offset, static_cast<DWORD>(MaximumOffset - 1));
            EXPECT_EQ(operations->read_operation.OffsetHigh,
                      static_cast<DWORD>((MaximumOffset - 1) >> 32U));
            EXPECT_EQ(operations->read_request, 1U);
        }
    }
}

TEST(WindowsFileSystemTest, PositionedReadStagesPreservePrefixesAndPrimaryErrors) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "stages";
    WriteFixture(path, "content");
    for (unsigned scenario = 0; scenario != 7; ++scenario) {
        SCOPED_TRACE(scenario);
        auto operations = std::make_shared<FaultOperations>();
        WindowsFileSystem file_system(false, operations, nullptr);
        auto file = file_system.OpenRandomAccess(path);
        ASSERT_TRUE(file.has_value());
        operations->call_count = 0;
        operations->force_pending = scenario == 1;
        operations->fail_read = scenario == 2 || scenario == 3;
        operations->read_error = scenario == 2 ? ERROR_HANDLE_EOF : ERROR_READ_FAULT;
        operations->fail_event = scenario == 4;
        operations->fail_complete = scenario == 5 || scenario == 6;
        operations->complete_error = scenario == 5 ? ERROR_HANDLE_EOF : ERROR_CRC;
        operations->fail_close = scenario == 3 || scenario == 6;
        std::array<std::byte, 3> bytes{};
        const auto read = (*file)->Read(1, bytes);
        std::vector<NativeCall> expected{NativeCall::Event};
        if (scenario != 4) {
            expected.push_back(NativeCall::Read);
            if (scenario != 2 && scenario != 3) {
                expected.push_back(NativeCall::Complete);
            }
            expected.push_back(NativeCall::Close);
            EXPECT_EQ(operations->read_request, bytes.size());
            EXPECT_TRUE(operations->null_positioned_count);
            EXPECT_EQ(operations->read_operation.Offset, 1U);
            EXPECT_EQ(operations->read_operation.OffsetHigh, 0U);
            EXPECT_EQ(operations->read_operation.Internal, 0U);
            EXPECT_EQ(operations->read_operation.InternalHigh, 0U);
            EXPECT_NE(operations->read_event, nullptr);
            EXPECT_EQ(operations->read_operation.hEvent, operations->read_event);
            EXPECT_EQ(operations->closed_handles[0], operations->read_event);
        }
        EXPECT_EQ(operations->Trace(), expected);
        EXPECT_EQ(operations->live_handles, 1U);
        if (scenario == 3 || scenario == 4 || scenario == 6) {
            ASSERT_FALSE(read.has_value());
            EXPECT_EQ(read.error().code(), ErrorCode::Io);
            const std::string_view name =
                scenario == 3 ? "positioned read"
                              : (scenario == 4 ? "create read event" : "complete positioned read");
            const DWORD error = scenario == 3
                                    ? ERROR_READ_FAULT
                                    : (scenario == 4 ? ERROR_NOT_ENOUGH_MEMORY : ERROR_CRC);
            EXPECT_TRUE(read.error().message().starts_with(name));
            EXPECT_NE(read.error().message().find("Win32 " + std::to_string(error)),
                      std::string_view::npos);
        } else {
            ASSERT_TRUE(read.has_value());
            EXPECT_EQ(*read, scenario == 2 || scenario == 5 ? 0U : bytes.size());
            if (scenario <= 1) {
                EXPECT_EQ(AsStringView(bytes), "ont");
            }
        }
        if (scenario == 0 || scenario == 1 || scenario >= 5) {
            EXPECT_TRUE(operations->completion_was_terminal);
            EXPECT_TRUE(operations->closed_completed_event);
        } else {
            EXPECT_EQ(operations->completed_reads, 0U);
        }
        operations->fail_close = false;
        file->reset();
        EXPECT_EQ(operations->live_handles, 0U);
    }
}

TEST(WindowsFileSystemTest, RepeatedPositionedReadsSnapshotPerCallOperationFields) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "repeat";
    const std::string data = "abcdefghi";
    WriteFixture(path, data);
    auto operations = std::make_shared<FaultOperations>();
    operations->force_pending = true;
    WindowsFileSystem file_system(false, operations, nullptr);
    auto file = file_system.OpenRandomAccess(path);
    ASSERT_TRUE(file.has_value());
    constexpr auto HighOffset = (std::uint64_t{1} << 32U) + 7U;
    for (auto offset : {std::uint64_t{2}, std::uint64_t{0}, HighOffset, std::uint64_t{4}}) {
        SCOPED_TRACE(offset);
        operations->call_count = 0;
        operations->fail_read = offset == HighOffset;
        operations->read_error = ERROR_HANDLE_EOF;
        std::array<std::byte, 3> bytes{};
        const auto read = (*file)->Read(offset, bytes);
        ASSERT_TRUE(read.has_value());
        EXPECT_EQ(operations->read_operation.Offset, static_cast<DWORD>(offset));
        EXPECT_EQ(operations->read_operation.OffsetHigh, static_cast<DWORD>(offset >> 32U));
        EXPECT_EQ(operations->read_operation.Internal, 0U);
        EXPECT_EQ(operations->read_operation.InternalHigh, 0U);
        EXPECT_EQ(operations->read_request, bytes.size());
        EXPECT_TRUE(operations->null_positioned_count);
        EXPECT_EQ(operations->read_operation.hEvent, operations->read_event);
        std::vector<NativeCall> expected{NativeCall::Event, NativeCall::Read};
        if (offset == HighOffset) {
            EXPECT_EQ(*read, 0U);
        } else {
            expected.push_back(NativeCall::Complete);
            EXPECT_EQ(*read, bytes.size());
            EXPECT_EQ(AsStringView(bytes), std::string_view(data).substr(
                                               static_cast<std::size_t>(offset), bytes.size()));
            EXPECT_TRUE(operations->closed_completed_event);
        }
        expected.push_back(NativeCall::Close);
        EXPECT_EQ(operations->Trace(), expected);
        EXPECT_EQ(operations->live_handles, 1U);
    }
}

TEST(WindowsFileSystemTest, FailedEnumerationDoesNotReturnPartialSuccess) {
    TemporaryDirectory directory;
    WriteFixture(directory.path() / "child", "content");
    auto operations = std::make_shared<FaultOperations>();
    operations->fail_next = true;
    WindowsFileSystem file_system(true, operations);
    const auto listed = file_system.ListDirectory(directory.path());
    EXPECT_FALSE(listed.has_value());
    if (!listed.has_value()) {
        EXPECT_EQ(listed.error().code(), ErrorCode::Io);
    }
    EXPECT_EQ(operations->live_finds, 0U);
    EXPECT_EQ(operations->live_handles, 0U);
}

TEST(WindowsFileSystemTest, NamespaceChecksDoNotHideWrongKindOrCloseFailure) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "file";
    WriteFixture(path, "content");
    WindowsFileSystem file_system(true);
    EXPECT_EQ(file_system.CreateDirectory(path).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(file_system.SyncDirectory(path).error().code(), ErrorCode::InvalidArgument);
    auto operations = std::make_shared<FaultOperations>();
    operations->fail_close = true;
    WindowsFileSystem close_failure(true, operations);
    EXPECT_EQ(close_failure.SyncDirectory(directory.path()).error().code(), ErrorCode::Io);
    EXPECT_EQ(operations->live_handles, 0U);
    EXPECT_EQ(file_system.PrepareDatabaseDirectory(L"\\\\server\\share\\db").error().code(),
              ErrorCode::NotSupported);
}

TEST(WindowsFileSystemTest, EnumerationCloseFailureIsNotSuccessful) {
    TemporaryDirectory directory;
    auto operations = std::make_shared<FaultOperations>();
    operations->fail_find_close = true;
    WindowsFileSystem file_system(false, operations);
    const auto names = file_system.ListDirectory(directory.path());
    ASSERT_FALSE(names.has_value());
    EXPECT_EQ(names.error().code(), ErrorCode::Io);
    EXPECT_EQ(operations->live_finds, 0U);
}

TEST(WindowsFileSystemTest, EnumerationOwnsNativeNamesBeforeBufferReuse) {
    TemporaryDirectory directory;
    auto operations = std::make_shared<FaultOperations>();
    operations->scripted_enumeration = true;
    WindowsFileSystem file_system(false, operations);
    const auto listed = file_system.ListDirectory(directory.path());
    ASSERT_TRUE(listed.has_value());
    const std::vector<std::filesystem::path> expected{L"\u03b2", L"two words", L"child"};
    EXPECT_EQ(*listed, expected);
    const std::vector<NativeCall> calls{
        NativeCall::Open, NativeCall::Inspect,  NativeCall::Close, NativeCall::Find,
        NativeCall::Next, NativeCall::Next,     NativeCall::Next,  NativeCall::Next,
        NativeCall::Next, NativeCall::CloseFind};
    EXPECT_EQ(operations->Trace(), calls);
    EXPECT_EQ(std::wstring_view(operations->find_pattern.data()),
              (std::filesystem::path(operations->opened_path) / L"*").native());
    EXPECT_EQ(operations->live_handles, 0U);
    EXPECT_EQ(operations->live_finds, 0U);
}

TEST(WindowsFileSystemTest, EnumerationAdmissionAndFindErrorsSuppressLaterStages) {
    TemporaryDirectory directory;
    for (unsigned scenario = 0; scenario != 4; ++scenario) {
        SCOPED_TRACE(scenario);
        auto operations = std::make_shared<FaultOperations>();
        operations->scripted_enumeration = true;
        operations->fail_open = scenario == 0;
        operations->fail_close = scenario == 1;
        operations->find_error = scenario == 2 ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED;
        WindowsFileSystem file_system(false, operations);
        const auto listed = file_system.ListDirectory(directory.path());
        std::vector<NativeCall> calls{NativeCall::Open};
        if (scenario != 0) {
            calls.push_back(NativeCall::Inspect);
            calls.push_back(NativeCall::Close);
            if (scenario != 1) {
                calls.push_back(NativeCall::Find);
            }
        }
        EXPECT_EQ(operations->Trace(), calls);
        if (scenario == 2) {
            ASSERT_TRUE(listed.has_value());
            EXPECT_TRUE(listed->empty());
        } else {
            ASSERT_FALSE(listed.has_value());
            const std::array<std::string_view, 4> names{"open", "close", "", "list directory"};
            EXPECT_TRUE(listed.error().message().starts_with(names[scenario]));
            const DWORD error = scenario == 1 ? ERROR_WRITE_FAULT : ERROR_ACCESS_DENIED;
            EXPECT_NE(listed.error().message().find("Win32 " + std::to_string(error)),
                      std::string_view::npos);
        }
        EXPECT_EQ(operations->live_handles, 0U);
        EXPECT_EQ(operations->live_finds, 0U);
    }
}

TEST(WindowsFileSystemTest, EnumerationReadErrorPrecedesCleanupCloseError) {
    TemporaryDirectory directory;
    for (bool fail_next : {true, false}) {
        SCOPED_TRACE(fail_next);
        auto operations = std::make_shared<FaultOperations>();
        operations->scripted_enumeration = true;
        operations->fail_next_call = fail_next ? 4 : 0;
        operations->fail_find_close = true;
        operations->find_close_error = ERROR_WRITE_FAULT;
        WindowsFileSystem file_system(false, operations);
        const auto listed = file_system.ListDirectory(directory.path());
        ASSERT_FALSE(listed.has_value());
        EXPECT_EQ(listed.error().code(), ErrorCode::Io);
        const std::string_view name = fail_next ? "list directory" : "close directory enumeration";
        EXPECT_TRUE(listed.error().message().starts_with(name));
        const DWORD error = fail_next ? ERROR_READ_FAULT : ERROR_WRITE_FAULT;
        EXPECT_NE(listed.error().message().find("Win32 " + std::to_string(error)),
                  std::string_view::npos);
        std::vector<NativeCall> calls{NativeCall::Open, NativeCall::Inspect, NativeCall::Close,
                                      NativeCall::Find};
        calls.insert(calls.end(), fail_next ? 4U : FaultOperations::ScriptedChildren.size(),
                     NativeCall::Next);
        calls.push_back(NativeCall::CloseFind);
        EXPECT_EQ(operations->Trace(), calls);
        EXPECT_EQ(operations->live_handles, 0U);
        EXPECT_EQ(operations->live_finds, 0U);
    }
}

TEST(WindowsFileSystemTest, CopiedReadHandleSurvivesRemovalAndLocksRecognizeCaseAliases) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "data";
    WriteFixture(path, "content");
    WindowsFileSystem file_system;
    auto file = file_system.OpenRandomAccess(path);
    ASSERT_TRUE(file.has_value());
    ASSERT_TRUE(file_system.RemoveFile(path).has_value());
    std::array<std::byte, 7> bytes{};
    ASSERT_EQ((*file)->Read(0, bytes).value(), bytes.size());
    EXPECT_EQ(AsStringView(bytes), "content");
    auto lock = file_system.LockFile(directory.path() / "LOCK");
    ASSERT_TRUE(lock.has_value());
    EXPECT_EQ(file_system.LockFile(directory.path() / "lock").error().code(), ErrorCode::Busy);
}

TEST(WindowsFileSystemTest, KernelLockContendsAcrossAChildProcess) {
    TemporaryDirectory directory;
    WindowsFileSystem file_system;
    const auto path = directory.path() / "LOCK";
    {
        auto lock = file_system.LockFile(path);
        ASSERT_TRUE(lock.has_value());
        EXPECT_TRUE(RunLockChild(path, true));
        EXPECT_EQ(file_system.LockFile(path).error().code(), ErrorCode::Busy);
        EXPECT_TRUE(RunLockChild(path, true));
    }
    EXPECT_TRUE(RunLockChild(path, false));
}

TEST(WindowsFileSystemTest, JunctionAliasesShareLocksAndResolveTheActualDatabaseLocation) {
    TemporaryDirectory directory;
    WindowsFileSystem file_system(true);
    const auto target = directory.path() / "target";
    const auto alias = directory.path() / "alias";
    ASSERT_TRUE(file_system.CreateDirectory(target).has_value());
    const auto junction = CreateJunction(alias, target);
    ASSERT_TRUE(junction.has_value()) << junction.error().ToString();
    {
        auto lock = file_system.LockFile(target / "LOCK");
        ASSERT_TRUE(lock.has_value());
        EXPECT_EQ(file_system.LockFile(alias / "LOCK").error().code(), ErrorCode::Busy);
        const auto direct = file_system.PrepareDatabaseDirectory(target / "new");
        const auto resolved = file_system.PrepareDatabaseDirectory(alias / "new");
        ASSERT_TRUE(direct.has_value());
        ASSERT_TRUE(resolved.has_value());
        EXPECT_EQ(*direct, *resolved);
        EXPECT_FALSE(std::filesystem::exists(target / "new"));
    }
    ASSERT_TRUE(file_system.RemoveDirectory(alias).has_value());
    EXPECT_TRUE(std::filesystem::exists(target));
}

TEST(WindowsFileSystemTest, FailedReplacementKeepsTheCurrentFileAndReportsNativeError) {
    TemporaryDirectory directory;
    const auto source = directory.path() / "next";
    const auto current = directory.path() / "CURRENT";
    WriteFixture(source, "new");
    WriteFixture(current, "old");
    NativeHandle held(::CreateFileW(current.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    ASSERT_NE(held.get(), INVALID_HANDLE_VALUE);
    ASSERT_FALSE(::MoveFileExW(source.c_str(), current.c_str(), MOVEFILE_REPLACE_EXISTING));
    const DWORD expected_error = ::GetLastError();
    WindowsFileSystem file_system;
    const auto renamed = file_system.RenameFile(source, current);
    ASSERT_FALSE(renamed.has_value());
    EXPECT_EQ(renamed.error().code(), ErrorCode::Io);
    EXPECT_NE(renamed.error().message().find("Win32 " + std::to_string(expected_error)),
              std::string_view::npos);
    EXPECT_TRUE(file_system.FileExists(source).value());
    auto reader = file_system.OpenSequential(current);
    ASSERT_TRUE(reader.has_value());
    std::array<std::byte, 3> bytes{};
    ASSERT_EQ((*reader)->Read(bytes).value(), bytes.size());
    EXPECT_EQ(AsStringView(bytes), "old");
}

TEST(WindowsFileSystemTest, FileObjectsOutliveTheirFilesystemAndUnicodeErrorsKeepTheirPaths) {
    TemporaryDirectory directory;
    const auto path = directory.path() / std::filesystem::path{u8"\u6570\u636e"};
    WriteFixture(path, "content");
    std::unique_ptr<SequentialFile> reader;
    {
        WindowsFileSystem file_system;
        auto file = file_system.OpenSequential(path);
        ASSERT_TRUE(file.has_value());
        reader = std::move(*file);
    }
    std::array<std::byte, 7> bytes{};
    ASSERT_EQ(reader->Read(bytes).value(), bytes.size());
    EXPECT_EQ(AsStringView(bytes), "content");
    WindowsFileSystem file_system;
    const auto missing = file_system.OpenSequential(path / "missing");
    ASSERT_FALSE(missing.has_value());
    EXPECT_NE(missing.error().message().find("\xe6\x95\xb0\xe6\x8d\xae"), std::string_view::npos);
    EXPECT_NE(missing.error().message().find("Win32 "), std::string_view::npos);
}

TEST(WindowsFileSystemTest, PreflightDistinguishesUnsupportedStorageFromQueryErrors) {
    TemporaryDirectory directory;
    auto operations = std::make_shared<FaultOperations>();
    WindowsFileSystem file_system(true, operations);
    const auto path = directory.path() / "new-db";
    operations->forced_drive_type = DRIVE_REMOTE;
    EXPECT_EQ(file_system.PrepareDatabaseDirectory(path).error().code(), ErrorCode::NotSupported);
    operations->forced_drive_type = DRIVE_UNKNOWN;
    EXPECT_EQ(file_system.PrepareDatabaseDirectory(path).error().code(), ErrorCode::Io);
    operations->forced_drive_type = DRIVE_FIXED;
    operations->fail_volume = true;
    const auto denied = file_system.PrepareDatabaseDirectory(path);
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error().code(), ErrorCode::Io);
    EXPECT_NE(denied.error().message().find("Win32 5"), std::string_view::npos);
    operations->fail_volume = false;
    operations->report_refs = true;
    EXPECT_EQ(file_system.PrepareDatabaseDirectory(path).error().code(), ErrorCode::NotSupported);
    EXPECT_EQ(operations->live_handles, 0U);
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(WindowsFileSystemTest, DirectoryAdmissionRejectsEarlierGuardsBeforeNativeAcquisition) {
    TemporaryDirectory directory;
    const auto file = directory.path() / "file";
    WriteFixture(file, "content");
    struct GuardCase {
        std::filesystem::path path;
        bool consent;
        ErrorCode code;
        std::string_view message;
    };
    const std::vector<GuardCase> cases{
        {{}, false, ErrorCode::InvalidArgument, "filesystem path is empty or contains a null"},
        {L"\\\\?\\UNC\\server\\share\\db", false, ErrorCode::NotSupported,
         "Windows requires explicit consent to weak namespace durability"},
        {L"\\\\?\\UNC\\server\\share\\db", true, ErrorCode::NotSupported,
         "Windows databases require local fixed NTFS"},
        {file, true, ErrorCode::InvalidArgument, "database path is not a directory"},
    };
    for (const auto& test_case : cases) {
        const auto operations = std::make_shared<FaultOperations>();
        operations->fail_open = true;
        operations->fail_volume = true;
        WindowsFileSystem file_system(test_case.consent, operations, nullptr);
        const auto result = file_system.PrepareDatabaseDirectory(test_case.path);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), test_case.code);
        EXPECT_EQ(result.error().message(), test_case.message);
        EXPECT_EQ(operations->open_calls, 0U);
        EXPECT_EQ(operations->inspect_calls, 0U);
        EXPECT_EQ(operations->drive_calls, 0U);
        EXPECT_EQ(operations->volume_calls, 0U);
        EXPECT_EQ(operations->close_calls, 0U);
    }
}

TEST(WindowsFileSystemTest, DirectoryAdmissionResolvesExistingAndSingleMissingAliasPaths) {
    TemporaryDirectory directory;
    const auto target = directory.path() / "target";
    const auto alias = directory.path() / "alias";
    ASSERT_TRUE(std::filesystem::create_directory(target));
    ASSERT_TRUE(CreateJunction(alias, target).has_value());
    const std::filesystem::path resolved_root{L"\\\\?\\" +
                                              std::filesystem::canonical(target).native()};
    for (const auto& source : {target, alias}) {
        for (const bool missing : {false, true}) {
            const auto path = missing ? source / "new" : source;
            const auto operations = std::make_shared<FaultOperations>();
            operations->forced_drive_type = DRIVE_FIXED;
            WindowsFileSystem file_system(true, operations, nullptr);
            const auto result =
                file_system.PrepareDatabaseDirectory(std::filesystem::path(path.native() + L"\\"));
            ASSERT_TRUE(result.has_value()) << result.error().ToString();
            EXPECT_EQ(*result, missing ? resolved_root / "new" : resolved_root);
            EXPECT_EQ(operations->opened_access, FILE_READ_ATTRIBUTES);
            EXPECT_EQ(operations->opened_share,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
            EXPECT_EQ(operations->opened_disposition, OPEN_EXISTING);
            EXPECT_EQ(operations->opened_flags, FILE_FLAG_BACKUP_SEMANTICS);
            EXPECT_EQ(operations->open_calls, 1U);
            EXPECT_EQ(operations->inspect_calls, 1U);
            EXPECT_EQ(operations->drive_calls, 1U);
            EXPECT_EQ(operations->volume_calls, 1U);
            EXPECT_EQ(operations->close_calls, 1U);
            EXPECT_EQ(operations->live_handles, 0U);
            EXPECT_FALSE(std::filesystem::exists(target / "new"));
        }
    }
    const auto operations = std::make_shared<FaultOperations>();
    WindowsFileSystem file_system(true, operations, nullptr);
    const auto absent_parent = target / "absent";
    const auto result = file_system.PrepareDatabaseDirectory(absent_parent / "new");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    EXPECT_EQ(operations->open_calls, 1U);
    EXPECT_EQ(operations->inspect_calls, 0U);
    EXPECT_EQ(operations->drive_calls, 0U);
    EXPECT_EQ(operations->volume_calls, 0U);
    EXPECT_EQ(operations->close_calls, 0U);
    EXPECT_FALSE(std::filesystem::exists(absent_parent));
    ASSERT_TRUE(file_system.RemoveDirectory(alias).has_value());
}

TEST(WindowsFileSystemTest, DirectoryAdmissionRetainsFirstFailureAndReleasesItsHandleOnce) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "new";
    const std::array<std::pair<ErrorCode, std::string>, 8> failures{{
        {ErrorCode::Io, "open '"},
        {ErrorCode::Io, "file information '"},
        {ErrorCode::Io, "could not determine the database volume type"},
        {ErrorCode::Io, "could not determine the database volume type"},
        {ErrorCode::NotSupported, "Windows databases require a fixed volume"},
        {ErrorCode::Io, "database filesystem '" + PathUtf8(path).value() + "' (Win32 5)"},
        {ErrorCode::NotSupported, "Windows databases require NTFS"},
        {ErrorCode::Io, "close '" + PathUtf8(path).value() + "' (Win32 29)"},
    }};
    for (std::size_t stage = 0; stage < failures.size(); ++stage) {
        SCOPED_TRACE(stage);
        const auto operations = std::make_shared<FaultOperations>();
        operations->fail_open = stage == 0;
        operations->fail_inspect = stage <= 1;
        operations->forced_drive_type = stage == 2   ? DRIVE_UNKNOWN
                                        : stage == 3 ? DRIVE_NO_ROOT_DIR
                                        : stage == 4 ? DRIVE_REMOTE
                                                     : DRIVE_FIXED;
        operations->fail_volume = stage <= 5;
        operations->report_refs = true;
        if (stage == 7) {
            operations->report_refs = false;
        }
        operations->fail_close = true;
        WindowsFileSystem file_system(true, operations, nullptr);
        const auto result = file_system.PrepareDatabaseDirectory(path);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), failures[stage].first);
        EXPECT_TRUE(result.error().message().starts_with(failures[stage].second))
            << result.error().ToString();
        EXPECT_EQ(operations->open_calls, 1U);
        EXPECT_EQ(operations->inspect_calls, stage == 0 ? 0U : 1U);
        EXPECT_EQ(operations->drive_calls, stage < 2 ? 0U : 1U);
        EXPECT_EQ(operations->volume_calls, stage < 5 ? 0U : 1U);
        EXPECT_EQ(operations->close_calls, stage == 0 ? 0U : 1U);
        EXPECT_EQ(operations->live_handles, 0U);
        EXPECT_FALSE(std::filesystem::exists(path));
    }
}

}  // namespace
}  // namespace modern_leveldb
