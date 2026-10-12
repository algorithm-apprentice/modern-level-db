#include "modern_leveldb/base/result.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <source_location>
#include <string>
#include <string_view>

#include "base/result_internal.h"

namespace modern_leveldb {
namespace {

void ExpectErrorContext(const Error& error, ErrorCode code, std::string_view message,
                        const std::source_location& location) {
    EXPECT_EQ(error.code(), code);
    EXPECT_EQ(error.message(), message);
    EXPECT_EQ(error.location().line(), location.line());
    EXPECT_EQ(error.location().column(), location.column());
    EXPECT_EQ(std::string_view(error.location().file_name()), location.file_name());
    EXPECT_EQ(std::string_view(error.location().function_name()), location.function_name());
}

TEST(ResultTest, DefaultStatusIsSuccessful) {
    const Status status;

    EXPECT_TRUE(status.has_value());
}

TEST(ResultTest, StoresSuccessfulValue) {
    const Result<std::string> result = std::string("value");

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "value");
}

TEST(ResultTest, StoresTypedError) {
    const Result<std::uint64_t> result = std::unexpected(Error::NotFound("missing database key"));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    EXPECT_EQ(result.error().message(), "missing database key");
}

TEST(ResultTest, FormatsErrorForDiagnostics) {
    const Error error = Error::InvalidArgument("block size must be positive");

    EXPECT_EQ(error.ToString(), "invalid_argument: block size must be positive");
}

TEST(ResultTest, FormatsEmptyAndUnsupportedErrors) {
    const Error empty = Error::Io("");
    const Error unsupported = Error::NotSupported("direct I/O");

    EXPECT_EQ(empty.ToString(), "io");
    EXPECT_EQ(unsupported.code(), ErrorCode::NotSupported);
    EXPECT_EQ(unsupported.ToString(), "not_supported: direct I/O");
}

TEST(ResultTest, FactoryMessagesKeepLiteralNamesRawBytesAndOriginalLocations) {
    using Factory = Error (*)(std::string, std::source_location);
    struct FactoryCase {
        Factory create;
        ErrorCode code;
        std::string_view prefix;
    };
    const std::array factories{
        FactoryCase{Error::NotFound, ErrorCode::NotFound, "not_found"},
        FactoryCase{Error::Corruption, ErrorCode::Corruption, "corruption"},
        FactoryCase{Error::InvalidArgument, ErrorCode::InvalidArgument, "invalid_argument"},
        FactoryCase{Error::Io, ErrorCode::Io, "io"},
        FactoryCase{Error::NotSupported, ErrorCode::NotSupported, "not_supported"},
        FactoryCase{Error::Busy, ErrorCode::Busy, "busy"},
        FactoryCase{Error::Aborted, ErrorCode::Aborted, "aborted"},
    };
    const std::array messages{std::string{}, std::string("b:\n\0\xff", 5), std::string(256, 'x')};
    const std::source_location location = std::source_location::current();
    for (const auto& factory : factories) {
        for (const auto& message : messages) {
            SCOPED_TRACE(factory.prefix);
            SCOPED_TRACE(message.size());
            const std::string before = message;
            const Error error = factory.create(message, location);
            std::string expected(factory.prefix);
            if (!message.empty()) {
                expected += ": ";
                expected += message;
            }

            EXPECT_EQ(error.ToString(), expected);
            ExpectErrorContext(error, factory.code, before, location);
            EXPECT_EQ(message, before);
        }
    }
}

TEST(ResultTest, CapturesFactoryCallSite) {
    const std::uint_least32_t expected_line = __LINE__ + 1;
    const Error error = Error::Corruption("invalid record");

    EXPECT_EQ(error.location().line(), expected_line);
    EXPECT_NE(std::string_view(error.location().file_name()).find("result_test.cc"),
              std::string_view::npos);
}

TEST(ResultTest, ExposesStableErrorCodeNames) {
    EXPECT_EQ(ErrorCodeName(ErrorCode::NotFound), "not_found");
    EXPECT_EQ(ErrorCodeName(ErrorCode::Corruption), "corruption");
    EXPECT_EQ(ErrorCodeName(ErrorCode::InvalidArgument), "invalid_argument");
    EXPECT_EQ(ErrorCodeName(ErrorCode::Io), "io");
    EXPECT_EQ(ErrorCodeName(ErrorCode::NotSupported), "not_supported");
    EXPECT_EQ(ErrorCodeName(ErrorCode::Busy), "busy");
    EXPECT_EQ(ErrorCodeName(ErrorCode::Aborted), "aborted");
    EXPECT_EQ(ErrorCodeName(static_cast<ErrorCode>(0xff)), "unknown");
}

TEST(ResultInternalTest, RemembersTheFirstFailureAcrossLaterStatuses) {
    std::optional<Error> first_error;
    EXPECT_TRUE(RememberFirstError(first_error, {}).has_value());
    EXPECT_FALSE(first_error.has_value());

    const Status first =
        RememberFirstError(first_error, std::unexpected(Error::Io("first write failed")));
    ASSERT_FALSE(first.has_value());
    EXPECT_EQ(first.error().message(), "first write failed");

    const Status later_success = RememberFirstError(first_error, {});
    ASSERT_FALSE(later_success.has_value());
    EXPECT_EQ(later_success.error().message(), "first write failed");

    const Status later_failure =
        RememberFirstError(first_error, std::unexpected(Error::Io("later close failed")));
    ASSERT_FALSE(later_failure.has_value());
    EXPECT_EQ(later_failure.error().message(), "first write failed");
}

TEST(ResultInternalTest, StickyContextAndReturnedOwnersRemainIndependent) {
    const std::string message(256, 'x');
    const std::source_location location = std::source_location::current();
    const Error winning = Error::Busy(message, location);
    std::optional<Error> retained;
    const Status initial = RememberFirstError(retained, {});
    EXPECT_TRUE(initial.has_value());
    EXPECT_FALSE(retained.has_value());

    const Status first = RememberFirstError(retained, std::unexpected(winning));
    ASSERT_FALSE(first.has_value());
    ASSERT_TRUE(retained.has_value());
    const char* const retained_address = retained->message().data();
    ExpectErrorContext(*retained, ErrorCode::Busy, message, location);
    ExpectErrorContext(first.error(), ErrorCode::Busy, message, location);
    EXPECT_NE(first.error().message().data(), retained_address);
    Status later_success = RememberFirstError(retained, {});
    ASSERT_FALSE(later_success.has_value());
    const Status later_failure =
        RememberFirstError(retained, std::unexpected(Error::Io("later write failed")));
    ASSERT_FALSE(later_failure.has_value());
    ExpectErrorContext(later_success.error(), ErrorCode::Busy, message, location);
    ExpectErrorContext(later_failure.error(), ErrorCode::Busy, message, location);
    EXPECT_EQ(retained->message().data(), retained_address);
    EXPECT_NE(later_success.error().message().data(), retained_address);
    EXPECT_NE(later_failure.error().message().data(), retained_address);

    later_success.error() = Error::Aborted("replace one returned owner");

    ExpectErrorContext(*retained, ErrorCode::Busy, message, location);
    ExpectErrorContext(first.error(), ErrorCode::Busy, message, location);
    ExpectErrorContext(later_failure.error(), ErrorCode::Busy, message, location);
    EXPECT_EQ(later_success.error().code(), ErrorCode::Aborted);
    EXPECT_EQ(later_success.error().message(), "replace one returned owner");

    retained.reset();

    EXPECT_FALSE(retained.has_value());
    ExpectErrorContext(first.error(), ErrorCode::Busy, message, location);
    ExpectErrorContext(later_failure.error(), ErrorCode::Busy, message, location);
    ExpectErrorContext(winning, ErrorCode::Busy, message, location);
}

}  // namespace
}  // namespace modern_leveldb
