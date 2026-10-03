#include "diagnostics/posix_output.h"

#include <gtest/gtest.h>
#include <signal.h>
#include <unistd.h>

#include <array>
#include <cstddef>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

TEST(PosixOutputTest, ConvertsAClosedPipeToIoInsteadOfSigpipeTermination) {
  struct sigaction previous{};
  ASSERT_EQ(::sigaction(SIGPIPE, nullptr, &previous), 0);
  ASSERT_TRUE(IgnoreBrokenPipeSignal().has_value());
  std::array<int, 2> descriptors{};
  ASSERT_EQ(::pipe(descriptors.data()), 0);
  ASSERT_EQ(::close(descriptors[0]), 0);
  PosixOutputFile output(descriptors[1]);

  const Status written = output.Append(AsBytes("diagnostic"));

  ASSERT_FALSE(written.has_value());
  EXPECT_EQ(written.error().code(), ErrorCode::Io);
  EXPECT_EQ(::close(descriptors[1]), 0);
  ASSERT_EQ(::sigaction(SIGPIPE, &previous, nullptr), 0);
}

TEST(PosixOutputTest, FlushesSyncsAndClosesWithoutOwningTheDescriptor) {
  std::array<int, 2> descriptors{};
  ASSERT_EQ(::pipe(descriptors.data()), 0);
  PosixOutputFile output(descriptors[1]);

  EXPECT_TRUE(output.Flush().has_value());
  EXPECT_TRUE(output.Sync().has_value());
  EXPECT_TRUE(output.Append(AsBytes("ok")).has_value());
  EXPECT_TRUE(output.Close().has_value());
  const Status append_after_close = output.Append(AsBytes("later"));
  ASSERT_FALSE(append_after_close.has_value());
  EXPECT_EQ(append_after_close.error().code(), ErrorCode::InvalidArgument);
  const Status repeated = output.Close();
  ASSERT_FALSE(repeated.has_value());
  EXPECT_EQ(repeated.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(::close(descriptors[0]), 0);
  EXPECT_EQ(::close(descriptors[1]), 0);
}

}  // namespace
}  // namespace modern_leveldb
