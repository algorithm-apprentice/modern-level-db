#ifndef MODERN_LEVELDB_BASE_RESULT_INTERNAL_H_
#define MODERN_LEVELDB_BASE_RESULT_INTERNAL_H_

#include <cassert>
#include <utility>

#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

// Checked boundaries report errors once. Internal paths use these helpers only
// after their inputs have been proven valid, preserving the invariant without
// reopening the error policy.
template <typename T>
[[nodiscard]] T TakeTrusted(Result<T> result) {
    assert(result.has_value());  // GCOVR_EXCL_BR_WITHOUT_HIT: trusted-result invariant
    return std::move(result).value();
}

inline void AssertSuccess(Status status) noexcept {
    assert(status.has_value());  // GCOVR_EXCL_BR_WITHOUT_HIT: trusted-status invariant
    static_cast<void>(status);
}

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_BASE_RESULT_INTERNAL_H_
