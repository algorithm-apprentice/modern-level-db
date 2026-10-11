#ifndef MODERN_LEVELDB_BASE_RESULT_INTERNAL_H_
#define MODERN_LEVELDB_BASE_RESULT_INTERNAL_H_

#include <optional>
#include <utility>

#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

// Checked boundaries report errors once. Internal paths use these helpers only
// after their inputs have been proven valid, preserving the invariant without
// reopening the error policy.
template <typename T>
[[nodiscard]] T TakeTrusted(Result<T> result) {
    return std::move(result).value();
}

inline void AssertSuccess(Status status) noexcept { static_cast<void>(status); }

// Retains the first failure of a multi-step resource operation. Later
// successes do not clear it, and later failures do not replace its context.
[[nodiscard]] Status RememberFirstError(std::optional<Error>& first_error, Status status);

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_BASE_RESULT_INTERNAL_H_
