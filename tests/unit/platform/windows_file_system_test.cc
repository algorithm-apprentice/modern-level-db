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
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "platform/windows_file_system_internal.h"
#include "support/temporary_directory.h"
#include "table/table.h"
#include "table/table_builder.h"

namespace modern_leveldb {
namespace {

using test_support::TemporaryDirectory;

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
    unsigned fail_close_call = 0;
    UINT forced_drive_type = std::numeric_limits<UINT>::max();
    DWORD maximum_write = MAXDWORD;
    mutable unsigned live_handles = 0;
    mutable unsigned live_finds = 0;
    mutable unsigned completed_reads = 0;
    mutable unsigned write_calls = 0;
    mutable unsigned sync_calls = 0;
    mutable unsigned close_calls = 0;
    mutable unsigned live_views = 0;

    HANDLE Open(const wchar_t* path, DWORD access, DWORD share, DWORD disposition,
                DWORD flags) const noexcept override {
        if (fail_open) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        const HANDLE handle = WindowsFileOperations::Open(path, access, share, disposition, flags);
        if (handle != INVALID_HANDLE_VALUE) {
            ++live_handles;
        }
        return handle;
    }
    BOOL Inspect(HANDLE file, BY_HANDLE_FILE_INFORMATION* information) const noexcept override {
        if (fail_inspect) {
            ::SetLastError(ERROR_READ_FAULT);
            return FALSE;
        }
        return WindowsFileOperations::Inspect(file, information);
    }
    HANDLE Event() const noexcept override {
        if (fail_event) {
            ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
        const HANDLE event = WindowsFileOperations::Event();
        if (event != nullptr) {
            ++live_handles;
        }
        return event;
    }
    BOOL Read(HANDLE file, void* output, DWORD size, DWORD* read,
              OVERLAPPED* overlapped) const noexcept override {
        if (fail_read) {
            ::SetLastError(ERROR_READ_FAULT);
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
        ++completed_reads;
        const BOOL completed = WindowsFileOperations::Complete(file, overlapped, read);
        if (fail_complete) {
            ::SetLastError(ERROR_CRC);
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
        if (fail_size) {
            ::SetLastError(ERROR_READ_FAULT);
            return FALSE;
        }
        return WindowsFileOperations::Size(file, size);
    }
    HANDLE Mapping(HANDLE file) const noexcept override {
        if (fail_mapping) {
            ::SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
        const HANDLE handle = WindowsFileOperations::Mapping(file);
        if (handle != nullptr) {
            ++live_handles;
        }
        return handle;
    }
    void* Map(HANDLE mapping) const noexcept override {
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
        const HANDLE handle = WindowsFileOperations::Find(pattern, information);
        if (handle != INVALID_HANDLE_VALUE) {
            ++live_finds;
        }
        return handle;
    }
    BOOL Next(HANDLE handle, WIN32_FIND_DATAW* information) const noexcept override {
        if (fail_next) {
            ::SetLastError(ERROR_READ_FAULT);
            return FALSE;
        }
        return WindowsFileOperations::Next(handle, information);
    }
    BOOL CloseFind(HANDLE handle) const noexcept override {
        const BOOL closed = WindowsFileOperations::CloseFind(handle);
        if (closed) {
            --live_finds;
        }
        if (fail_find_close) {
            ::SetLastError(ERROR_READ_FAULT);
            return FALSE;
        }
        return closed;
    }
    UINT DriveType(const wchar_t* volume) const noexcept override {
        return forced_drive_type == std::numeric_limits<UINT>::max()
                   ? WindowsFileOperations::DriveType(volume)
                   : forced_drive_type;
    }
    BOOL VolumeInformation(const wchar_t* volume, wchar_t* file_system,
                           DWORD size) const noexcept override {
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
    auto limiter = WindowsFileSystem::NewMmapBudgetForTesting(1);
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
    WindowsFileSystem filesystem(false, operations, WindowsFileSystem::NewMmapBudgetForTesting(1));
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
    auto limiter = WindowsFileSystem::NewMmapBudgetForTesting(1);
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
    auto limiter = WindowsFileSystem::NewMmapBudgetForTesting(1);
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
    WindowsFileSystem disabled(false, operations, WindowsFileSystem::NewMmapBudgetForTesting(0));
    auto zero = disabled.OpenRandomAccess(path, 3);
    ASSERT_TRUE(zero.has_value());
    EXPECT_FALSE((*zero)->TryReadView(0, 3).has_value());
}

TEST(WindowsFileSystemTest, ConcurrentOpensReserveOnlyTheAvailableMappingSlots) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "mapped";
    WriteFixture(path, "abc");
    auto limiter = WindowsFileSystem::NewMmapBudgetForTesting(1);
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
        auto limiter = WindowsFileSystem::NewMmapBudgetForTesting(1);
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
            WindowsFileSystem filesystem(false, operations,
                                         WindowsFileSystem::NewMmapBudgetForTesting(1));
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
    EXPECT_EQ(file_system.OpenSequential(missing).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(file_system.ListDirectory(missing).error().code(), ErrorCode::NotFound);
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

}  // namespace
}  // namespace modern_leveldb
