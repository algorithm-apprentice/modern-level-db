#ifndef MODERN_LEVELDB_BASE_RESULT_H_
#define MODERN_LEVELDB_BASE_RESULT_H_

#include <expected>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace modern_leveldb {

enum class ErrorCode {
  kNotFound,
  kCorruption,
  kInvalidArgument,
  kIo,
  kNotSupported,
  kBusy,
  kAborted,
};

[[nodiscard]] constexpr std::string_view ErrorCodeName(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kNotFound:
      return "not_found";
    case ErrorCode::kCorruption:
      return "corruption";
    case ErrorCode::kInvalidArgument:
      return "invalid_argument";
    case ErrorCode::kIo:
      return "io";
    case ErrorCode::kNotSupported:
      return "not_supported";
    case ErrorCode::kBusy:
      return "busy";
    case ErrorCode::kAborted:
      return "aborted";
  }
  return "unknown";
}

class Error final {
 public:
  [[nodiscard]] static Error NotFound(
      std::string message, std::source_location location = std::source_location::current()) {
    return Error(ErrorCode::kNotFound, std::move(message), location);
  }

  [[nodiscard]] static Error Corruption(
      std::string message, std::source_location location = std::source_location::current()) {
    return Error(ErrorCode::kCorruption, std::move(message), location);
  }

  [[nodiscard]] static Error InvalidArgument(
      std::string message, std::source_location location = std::source_location::current()) {
    return Error(ErrorCode::kInvalidArgument, std::move(message), location);
  }

  [[nodiscard]] static Error Io(std::string message,
                                std::source_location location = std::source_location::current()) {
    return Error(ErrorCode::kIo, std::move(message), location);
  }

  [[nodiscard]] static Error NotSupported(
      std::string message, std::source_location location = std::source_location::current()) {
    return Error(ErrorCode::kNotSupported, std::move(message), location);
  }

  [[nodiscard]] static Error Busy(std::string message,
                                  std::source_location location = std::source_location::current()) {
    return Error(ErrorCode::kBusy, std::move(message), location);
  }

  [[nodiscard]] static Error Aborted(
      std::string message, std::source_location location = std::source_location::current()) {
    return Error(ErrorCode::kAborted, std::move(message), location);
  }

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] std::string_view message() const noexcept { return message_; }
  [[nodiscard]] const std::source_location& location() const noexcept { return location_; }

  [[nodiscard]] std::string ToString() const {
    std::string text(ErrorCodeName(code_));
    if (!message_.empty()) {
      text.append(": ");
      text.append(message_);
    }
    return text;
  }

 private:
  Error(ErrorCode code, std::string message, std::source_location location)
      : code_(code), message_(std::move(message)), location_(location) {}

  ErrorCode code_;
  std::string message_;
  std::source_location location_;
};

template <typename T>
using Result = std::expected<T, Error>;

using Status = Result<void>;

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_BASE_RESULT_H_
