#include "base/result_internal.h"

#include <expected>
#include <new>
#include <string>
#include <utility>

namespace modern_leveldb {
namespace {

void CaptureFirstFailure(std::optional<Error>& first_error, Status& status) {
    if (!status.has_value() && !first_error.has_value()) {
        first_error = std::move(status).error();
    }
}

}  // namespace

TerminalError::TerminalError()
    : fallback_(std::make_shared<const Error>(Error::Aborted(
          "operation aborted; detailed error unavailable during allocation failure"))) {}

std::optional<Error> TerminalError::Snapshot() const {
    if (!has_value()) {
        return std::nullopt;
    }
    return *error_;
}

void TerminalError::Record(const Error& error) noexcept {
    if (has_value()) {
        return;
    }
    error_ = fallback_;
    try {
        error_ = std::make_shared<const Error>(error);
    } catch (const std::bad_alloc&) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 std-only error copy
        // The preallocated terminal diagnostic is already installed.
    }
}

void TerminalError::RecordException(std::string_view context,
                                    std::source_location location) noexcept {
    if (has_value()) {
        return;
    }
    error_ = fallback_;
    try {
        const Error error = Error::Aborted(std::string(context), location);
        error_ = std::make_shared<const Error>(error);
    } catch (const std::bad_alloc&) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 bounded std context
        // Keep the terminal state even when constructing its context fails.
    }
}

Status RememberFirstError(std::optional<Error>& first_error, Status status) {
    CaptureFirstFailure(first_error, status);
    if (first_error.has_value()) {
        return std::unexpected(*first_error);
    }
    return {};
}

}  // namespace modern_leveldb
