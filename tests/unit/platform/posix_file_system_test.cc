#include "platform/posix_file_system.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "modern-leveldb-fs-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* created = ::mkdtemp(writable.data());
    if (created == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    path_ = created;
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

void WriteFixture(const std::filesystem::path& path, ByteView data) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open());
  output.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
  ASSERT_TRUE(output.good());
}

std::vector<std::byte> ReadFixture(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  EXPECT_TRUE(input.is_open());
  const std::vector<char> chars{std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>()};
  std::vector<std::byte> bytes;
  bytes.reserve(chars.size());
  for (const char value : chars) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
  }
  return bytes;
}

std::vector<std::byte> Pattern(std::size_t size) {
  std::vector<std::byte> result(size);
  for (std::size_t index = 0; index < size; ++index) {
    result[index] = static_cast<std::byte>((index * 37U + 11U) & 0xffU);
  }
  return result;
}

bool ChildSeesKernelLock(const std::filesystem::path& path) {
  const pid_t child = ::fork();
  if (child == -1) {
    return false;
  }
  if (child == 0) {
    const int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) {
      ::_exit(2);
    }
    struct flock lock{};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    const int result = ::fcntl(fd, F_SETLK, &lock);
    const int lock_error = errno;
    ::close(fd);
    ::_exit(result == -1 && (lock_error == EACCES || lock_error == EAGAIN) ? 0 : 3);
  }

  int status = 0;
  if (::waitpid(child, &status, 0) != child) {
    return false;
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

TEST(PosixFileSystemTest, RejectsEmptyAndEmbeddedNullPaths) {
  PosixFileSystem file_system;
  const std::filesystem::path embedded_null(std::string("bad\0path", 8));

  const auto empty = file_system.OpenSequential({});
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(empty.error().code(), ErrorCode::InvalidArgument);

  const auto invalid = file_system.FileExists(embedded_null);
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(invalid.error().code(), ErrorCode::InvalidArgument);
}

TEST(PosixFileSystemTest, ReadsSequentiallyAndReportsEof) {
  TemporaryDirectory directory;
  PosixFileSystem file_system;
  const auto path = directory.path() / "sequential";
  WriteFixture(path, AsBytes("abcdef"));

  auto opened = file_system.OpenSequential(path);
  ASSERT_TRUE(opened.has_value());
  std::unique_ptr<SequentialFile> file = std::move(*opened);

  std::array<std::byte, 3> first{};
  const auto first_size = file->Read(first);
  ASSERT_TRUE(first_size.has_value());
  EXPECT_EQ(*first_size, 3U);
  EXPECT_EQ(AsStringView(first), "abc");

  std::array<std::byte, 4> last{};
  const auto last_size = file->Read(last);
  ASSERT_TRUE(last_size.has_value());
  EXPECT_EQ(*last_size, 3U);
  EXPECT_EQ(AsStringView(ByteView(last).first(*last_size)), "def");

  const auto eof = file->Read(last);
  ASSERT_TRUE(eof.has_value());
  EXPECT_EQ(*eof, 0U);
  const auto empty = file->Read({});
  ASSERT_TRUE(empty.has_value());
  EXPECT_EQ(*empty, 0U);
}

TEST(PosixFileSystemTest, PerformsConcurrentPositionedReads) {
  TemporaryDirectory directory;
  PosixFileSystem file_system;
  const auto path = directory.path() / "random";
  const std::vector<std::byte> expected = Pattern(8'192);
  WriteFixture(path, expected);

  auto opened = file_system.OpenRandomAccess(path);
  ASSERT_TRUE(opened.has_value());
  RandomAccessFile* file = opened->get();
  std::atomic<int> failures = 0;
  std::vector<std::jthread> readers;

  for (std::size_t reader = 0; reader < 4; ++reader) {
    readers.emplace_back([&, reader] {
      for (std::size_t iteration = 0; iteration < 100; ++iteration) {
        const std::size_t offset = (reader * 997U + iteration * 53U) % 8'000U;
        std::array<std::byte, 65> storage{};
        MutableByteView output = MutableByteView(storage).subspan(1, 64);
        const auto size = file->Read(offset, output);
        if (!size.has_value() || *size != output.size() ||
            !std::ranges::equal(ByteView(output), ByteView(expected).subspan(offset, 64))) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  readers.clear();

  EXPECT_EQ(failures.load(std::memory_order_relaxed), 0);
}

TEST(PosixFileSystemTest, RandomReadReturnsShortReadAndRejectsHugeOffset) {
  TemporaryDirectory directory;
  PosixFileSystem file_system;
  const auto path = directory.path() / "short-random";
  WriteFixture(path, AsBytes("abc"));
  auto opened = file_system.OpenRandomAccess(path);
  ASSERT_TRUE(opened.has_value());

  std::array<std::byte, 8> output{};
  const auto short_read = (*opened)->Read(2, output);
  ASSERT_TRUE(short_read.has_value());
  EXPECT_EQ(*short_read, 1U);
  EXPECT_EQ(output[0], std::byte{'c'});

  const auto eof = (*opened)->Read(100, output);
  ASSERT_TRUE(eof.has_value());
  EXPECT_EQ(*eof, 0U);

  const auto invalid = (*opened)->Read(std::numeric_limits<std::uint64_t>::max(), output);
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(invalid.error().code(), ErrorCode::InvalidArgument);
}

TEST(PosixFileSystemTest, BuffersFlushesSyncsAndAppendsWrites) {
  TemporaryDirectory directory;
  PosixFileSystem file_system;
  const auto path = directory.path() / "writable";
  WriteFixture(path, AsBytes("old contents"));
  auto writable = file_system.OpenWritable(path);
  ASSERT_TRUE(writable.has_value());
  EXPECT_TRUE(ReadFixture(path).empty());

  ASSERT_TRUE((*writable)->Append(AsBytes("abc")));
  EXPECT_TRUE(ReadFixture(path).empty());
  ASSERT_TRUE((*writable)->Flush());
  EXPECT_EQ(AsStringView(ReadFixture(path)), "abc");

  const std::vector<std::byte> large = Pattern(70'000);
  ASSERT_TRUE((*writable)->Append(large));
  ASSERT_TRUE((*writable)->Sync());
  ASSERT_TRUE((*writable)->Close());

  std::vector<std::byte> expected = {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  expected.insert(expected.end(), large.begin(), large.end());
  EXPECT_EQ(ReadFixture(path), expected);

  auto appendable = file_system.OpenAppendable(path);
  ASSERT_TRUE(appendable.has_value());
  ASSERT_TRUE((*appendable)->Append(AsBytes("tail")));
  ASSERT_TRUE((*appendable)->Sync());
  ASSERT_TRUE((*appendable)->Close());
  expected.insert(expected.end(), {std::byte{'t'}, std::byte{'a'}, std::byte{'i'}, std::byte{'l'}});
  EXPECT_EQ(ReadFixture(path), expected);
}

TEST(PosixFileSystemTest, WritableDestructorPerformsBestEffortClose) {
  TemporaryDirectory directory;
  PosixFileSystem file_system;
  const auto path = directory.path() / "destructor-close";

  {
    auto writable = file_system.OpenWritable(path);
    ASSERT_TRUE(writable.has_value());
    ASSERT_TRUE((*writable)->Append(AsBytes("persisted to kernel")));
  }

  EXPECT_EQ(AsStringView(ReadFixture(path)), "persisted to kernel");
}

#if defined(__linux__)
TEST(PosixFileSystemTest, WritableFileRetainsItsFirstIoError) {
  PosixFileSystem file_system;
  auto writable = file_system.OpenAppendable("/dev/full");
  ASSERT_TRUE(writable.has_value());
  ASSERT_TRUE((*writable)->Append(AsBytes("buffered")));

  const Status flush = (*writable)->Flush();
  ASSERT_FALSE(flush.has_value());
  EXPECT_EQ(flush.error().code(), ErrorCode::Io);

  const Status sync = (*writable)->Sync();
  ASSERT_FALSE(sync.has_value());
  EXPECT_EQ(sync.error().code(), flush.error().code());
  EXPECT_EQ(sync.error().message(), flush.error().message());

  const Status close = (*writable)->Close();
  ASSERT_FALSE(close.has_value());
  EXPECT_EQ(close.error().code(), flush.error().code());
  EXPECT_EQ(close.error().message(), flush.error().message());
}
#endif

TEST(PosixFileSystemTest, SupportsNamespaceOperationsAndExplicitDirectorySync) {
  TemporaryDirectory root;
  PosixFileSystem file_system;
  const auto directory = root.path() / "db";
  ASSERT_TRUE(file_system.CreateDirectory(directory));
  ASSERT_TRUE(file_system.CreateDirectory(directory));

  const auto source = directory / "source";
  const auto destination = directory / "destination";
  WriteFixture(source, AsBytes("source-data"));
  WriteFixture(destination, AsBytes("old-data"));

  const auto exists = file_system.FileExists(source);
  ASSERT_TRUE(exists.has_value());
  EXPECT_TRUE(*exists);
  const auto missing = file_system.FileExists(directory / "missing");
  ASSERT_TRUE(missing.has_value());
  EXPECT_FALSE(*missing);

  const auto size = file_system.FileSize(source);
  ASSERT_TRUE(size.has_value());
  EXPECT_EQ(*size, 11U);

  const auto children = file_system.ListDirectory(directory);
  ASSERT_TRUE(children.has_value());
  const std::set<std::filesystem::path> names(children->begin(), children->end());
  EXPECT_EQ(names, (std::set<std::filesystem::path>{"destination", "source"}));

  ASSERT_TRUE(file_system.RenameFile(source, destination));
  EXPECT_EQ(AsStringView(ReadFixture(destination)), "source-data");
  ASSERT_TRUE(file_system.SyncDirectory(directory));

  const Status non_empty = file_system.RemoveDirectory(directory);
  EXPECT_FALSE(non_empty.has_value());
  ASSERT_TRUE(file_system.RemoveFile(destination));
  ASSERT_TRUE(file_system.SyncDirectory(directory));
  ASSERT_TRUE(file_system.RemoveDirectory(directory));
}

TEST(PosixFileSystemTest, RejectsCreateDirectoryOverExistingFile) {
  TemporaryDirectory directory;
  PosixFileSystem file_system;
  const auto path = directory.path() / "file";
  WriteFixture(path, {});

  const Status status = file_system.CreateDirectory(path);

  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::InvalidArgument);
}

TEST(PosixFileSystemTest, ReportsMissingPaths) {
  TemporaryDirectory directory;
  PosixFileSystem file_system;
  const auto missing = directory.path() / "missing";

  const auto sequential = file_system.OpenSequential(missing);
  ASSERT_FALSE(sequential.has_value());
  EXPECT_EQ(sequential.error().code(), ErrorCode::NotFound);

  const auto size = file_system.FileSize(missing);
  ASSERT_FALSE(size.has_value());
  EXPECT_EQ(size.error().code(), ErrorCode::NotFound);

  const auto children = file_system.ListDirectory(missing);
  ASSERT_FALSE(children.has_value());
  EXPECT_EQ(children.error().code(), ErrorCode::NotFound);
}

TEST(PosixFileSystemTest, PreventsDuplicateProcessLocksWithoutDroppingKernelLock) {
  TemporaryDirectory directory;
  PosixFileSystem first_file_system;
  PosixFileSystem second_file_system;
  const auto lock_path = directory.path() / "LOCK";

  auto first = first_file_system.LockFile(lock_path);
  ASSERT_TRUE(first.has_value());

  const auto duplicate = second_file_system.LockFile(directory.path() / "." / "LOCK");
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(duplicate.error().code(), ErrorCode::Busy);
  EXPECT_TRUE(ChildSeesKernelLock(lock_path));

  first->reset();
  auto acquired_after_release = second_file_system.LockFile(lock_path);
  EXPECT_TRUE(acquired_after_release.has_value());
}

TEST(PosixFileSystemTest, TreatsSymlinkedDatabasePathsAsOneProcessLock) {
  TemporaryDirectory root;
  PosixFileSystem first_file_system;
  PosixFileSystem second_file_system;
  const auto real_directory = root.path() / "real-db";
  const auto alias_directory = root.path() / "alias-db";
  ASSERT_TRUE(first_file_system.CreateDirectory(real_directory));
  ASSERT_EQ(::symlink(real_directory.c_str(), alias_directory.c_str()), 0);

  auto first = first_file_system.LockFile(real_directory / "LOCK");
  ASSERT_TRUE(first.has_value());
  const auto duplicate = second_file_system.LockFile(alias_directory / "LOCK");

  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(duplicate.error().code(), ErrorCode::Busy);
}

TEST(PosixFileSystemTest, ReleasesDescriptorsThroughRaii) {
  TemporaryDirectory directory;
  PosixFileSystem file_system;
  const auto path = directory.path() / "many-opens";
  WriteFixture(path, AsBytes("x"));

  for (int iteration = 0; iteration < 1'024; ++iteration) {
    auto file = file_system.OpenSequential(path);
    ASSERT_TRUE(file.has_value()) << iteration;
  }
}

}  // namespace
}  // namespace modern_leveldb
