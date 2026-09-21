#ifndef MODERN_LEVELDB_BASE_RESULT_H_
#define MODERN_LEVELDB_BASE_RESULT_H_

#include <expected>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace modern_leveldb {

enum class ErrorCode {
  NotFound,
  Corruption,
  InvalidArgument,
  Io,
  NotSupported,
  Busy,
  Aborted,
};

[[nodiscard]] constexpr std::string_view ErrorCodeName(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::NotFound:
      return "not_found";
    case ErrorCode::Corruption:
      return "corruption";
    case ErrorCode::InvalidArgument:
      return "invalid_argument";
    case ErrorCode::Io:
      return "io";
    case ErrorCode::NotSupported:
      return "not_supported";
    case ErrorCode::Busy:
      return "busy";
    case ErrorCode::Aborted:
      return "aborted";
  }
  return "unknown";
}

class Error final {
 public:
  [[nodiscard]] static Error NotFound(
      std::string message, std::source_location location = std::source_location::current()) {
    return Error(ErrorCode::NotFound, std::move(message), location);
  }

  [[nodiscard]] static Error Corruption(
      std::string message, std::source_location location = std::source_location::current()) {
    return Error(ErrorCode::Corruption, std::move(message), location);
  }

  [[nodiscard]] static Error InvalidArgument(
      std::string message, std::source_location location = std::source_location::current()) {
    return Error(ErrorCode::InvalidArgument, std::move(message), location);
  }

  [[nodiscard]] static Error Io(std::string message,
                                std::source_location location = std::source_location::current()) {
    return Error(ErrorCode::Io, std::move(message), location);
  }

  [[nodiscard]] static Error NotSupported(
      std::string message, std::source_location location = std::source_location::current()) {
    return Error(ErrorCode::NotSupported, std::move(message), location);
  }

  [[nodiscard]] static Error Busy(std::string message,
                                  std::source_location location = std::source_location::current()) {
    return Error(ErrorCode::Busy, std::move(message), location);
  }

  [[nodiscard]] static Error Aborted(
      std::string message, std::source_location location = std::source_location::current()) {
    return Error(ErrorCode::Aborted, std::move(message), location);
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
