#ifndef MODERN_LEVELDB_TESTS_SUPPORT_CRASH_OBSERVATION_H_
#define MODERN_LEVELDB_TESTS_SUPPORT_CRASH_OBSERVATION_H_

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "modern_leveldb/base/result.h"

namespace modern_leveldb::test_support {

class CrashObservation {
 public:
  [[nodiscard]] Status Append(std::string_view bytes) {
    if (error_.has_value()) {
      return std::unexpected(*error_);
    }
    if (bytes.size() > MaximumBytes - received_) {
      return Fail("crash observation exceeds its byte budget");
    }
    received_ += bytes.size();
    pending_.append(bytes);
    std::size_t consumed = 0;
    while (true) {
      const std::size_t end = pending_.find('\n', consumed);
      if (end == std::string::npos) {
        break;
      }
      const std::string_view line(pending_.data() + consumed, end - consumed);
      if (next_ack_ < Acknowledgements.size() && line == Acknowledgements[next_ack_]) {
        ++next_ack_;
      } else if (!ready_ && next_ack_ == Acknowledgements.size() && line == "ready") {
        ready_ = true;
      } else {
        return Fail("crash observation contains an out-of-order or unknown record");
      }
      consumed = end + 1;
    }
    pending_.erase(0, consumed);
    return {};
  }
  [[nodiscard]] Status Finish() const {
    if (error_.has_value()) {
      return std::unexpected(*error_);
    }
    if (!ready_ || !pending_.empty()) {
      return std::unexpected(Error::Corruption("crash observation is incomplete at EOF"));
    }
    return {};
  }
  [[nodiscard]] bool ready() const noexcept { return ready_ && !error_.has_value(); }

 private:
  Status Fail(std::string message) {
    error_ = Error::Corruption(std::move(message));
    return std::unexpected(*error_);
  }
  static constexpr std::size_t MaximumBytes = 4'096;
  static constexpr std::array<std::string_view, 8> Acknowledgements{
      "ack 0", "ack 1", "ack 2", "ack 3", "ack 4", "ack 5", "ack 6", "ack 7"};
  std::string pending_;
  std::optional<Error> error_;
  std::size_t received_ = 0;
  std::size_t next_ack_ = 0;
  bool ready_ = false;
};

}  // namespace modern_leveldb::test_support

#endif  // MODERN_LEVELDB_TESTS_SUPPORT_CRASH_OBSERVATION_H_
