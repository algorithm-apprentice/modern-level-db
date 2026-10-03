#include "diagnostics/posix_output.h"

#include <errno.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <system_error>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

Error OutputError(std::string_view operation, int error) {
  return Error::Io(std::string(operation) + ": " +
                   std::error_code(error, std::generic_category()).message());
}

}  // namespace

Status IgnoreBrokenPipeSignal() {
  struct sigaction action{};
  action.sa_handler = SIG_IGN;
  (void)sigemptyset(&action.sa_mask);
  // GCOVR_EXCL_START: OS rejection of a valid SIGPIPE action is unavailable to unit tests
  if (::sigaction(SIGPIPE, &action, nullptr) == -1) {
    return std::unexpected(OutputError("ignore SIGPIPE", errno));
  }
  // GCOVR_EXCL_STOP
  return {};
}

Status PosixOutputFile::CheckOpen() const {
  if (closed_) {
    return std::unexpected(Error::InvalidArgument("output used after close"));
  }
  return {};
}

Status PosixOutputFile::Append(ByteView data) {
  const Status open = CheckOpen();
  if (!open.has_value()) {
    return open;
  }
  while (!data.empty()) {
    const std::size_t size =
        std::min(data.size(), static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    const ssize_t written = ::write(descriptor_, data.data(), size);
    if (written > 0) {
      data = data.subspan(static_cast<std::size_t>(written));
      continue;
    }
    if (written == -1) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 nonempty write returned zero
      const int error = errno;
      if (error == EINTR) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 requires timed signal interruption
        continue;            // GCOVR_EXCL_LINE: direct write has no injectable syscall seam
      }
      return std::unexpected(OutputError("write output", error));
    }
    // GCOVR_EXCL_START: nonempty POSIX write returned zero bytes
    return std::unexpected(Error::Io("write output returned zero"));
    // GCOVR_EXCL_STOP
  }
  return {};
}

Status PosixOutputFile::Flush() { return CheckOpen(); }

Status PosixOutputFile::Sync() { return CheckOpen(); }

Status PosixOutputFile::Close() {
  const Status open = CheckOpen();
  if (!open.has_value()) {
    return open;
  }
  closed_ = true;
  return {};
}

}  // namespace modern_leveldb
