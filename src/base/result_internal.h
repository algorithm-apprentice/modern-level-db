#ifndef MODERN_LEVELDB_BASE_RESULT_INTERNAL_H_
#define MODERN_LEVELDB_BASE_RESULT_INTERNAL_H_

#include <memory>
#include <optional>
#include <source_location>
#include <string_view>
#include <utility>

#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

// Owner-synchronized failure state. Prepare fallback ownership before storage
// mutation; terminal transitions do not depend on allocating their diagnostics.
class TerminalError final {
public:
    TerminalError();
    TerminalError(const TerminalError&) = delete;
    TerminalError& operator=(const TerminalError&) = delete;

    [[nodiscard]] bool has_value() const noexcept { return error_ != nullptr; }
    [[nodiscard]] const Error& operator*() const noexcept { return *error_; }
    [[nodiscard]] std::optional<Error> Snapshot() const;

    void Record(const Error& error) noexcept;
    // Context is a short, representable internal diagnostic, not external input.
    void RecordException(std::string_view context,
                         std::source_location location = std::source_location::current()) noexcept;

private:
    const std::shared_ptr<const Error> fallback_;
    std::shared_ptr<const Error> error_;
};

// Checked boundaries report errors once. Internal paths use these helpers only
// after their inputs have been proven valid, preserving the invariant without
// reopening the error policy.
template <typename T>
[[nodiscard]] T TakeTrusted(Result<T> result) {
    return std::move(result).value();
}

// Retains the first failure of a multi-step resource operation. Later
// successes do not clear it, and later failures do not replace its context.
[[nodiscard]] Status RememberFirstError(std::optional<Error>& first_error, Status status);

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_BASE_RESULT_INTERNAL_H_
