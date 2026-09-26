#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <iostream>
#include <string>

#include "modern_leveldb/db.h"
#include "support/temporary_directory.h"

namespace modern_leveldb {
namespace {

[[noreturn]] void WriteAndExit(const std::filesystem::path& path) {
  try {
    Options options;
    options.create_if_missing = true;
    options.write_buffer_size = 64 * 1024;
    auto database = Database::Open(options, path);
    if (!database.has_value()) {
      std::cerr << database.error().ToString() << '\n';
      ::_exit(1);
    }
    const std::string value(80 * 1024, 'x');
    for (unsigned index = 0; index < 8; ++index) {
      const std::string key = "key-" + std::to_string(index);
      const Status written = database->Put(AsBytes(key), AsBytes(value), {.sync = true});
      if (!written.has_value()) {
        std::cerr << written.error().ToString() << '\n';
        ::_exit(1);
      }
    }
    ::_exit(0);
  } catch (const std::exception& error) {
    std::cerr << "crash child failed: " << error.what() << '\n';
    ::_exit(2);
  }
}

TEST(ProcessCrashTest, RecoversAcknowledgedWritesWithoutRunningDestructors) {
  test_support::TemporaryDirectory directory;
  const auto path = directory.path() / "db";
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    WriteAndExit(path);
  }
  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  ASSERT_EQ(waited, child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  auto database = Database::Open({}, path);
  ASSERT_TRUE(database.has_value()) << database.error().ToString();
  const std::string expected(80 * 1024, 'x');
  for (unsigned index = 0; index < 8; ++index) {
    const auto value = database->Get(AsBytes("key-" + std::to_string(index)));
    ASSERT_TRUE(value.has_value()) << value.error().ToString();
    ASSERT_TRUE(value->has_value());
    EXPECT_EQ(AsStringView(**value), expected);
  }
}

}  // namespace
}  // namespace modern_leveldb
