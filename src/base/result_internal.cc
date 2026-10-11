#include "base/result_internal.h"

#include <expected>
#include <utility>

namespace modern_leveldb {

Status RememberFirstError(std::optional<Error>& first_error, Status status) {
    if (!status.has_value() && !first_error.has_value()) {
        first_error = std::move(status).error();
    }
    if (first_error.has_value()) {
        return std::unexpected(*first_error);
    }
    return {};
}

}  // namespace modern_leveldb
