#include <gtest/gtest.h>

#include <set>

#include "hcs/result.hpp"

using namespace hcs;

TEST(Result, ValueAndError) {
    Result<int> ok = 42;
    ASSERT_TRUE(ok.ok());
    EXPECT_EQ(ok.value(), 42);
    EXPECT_THROW((void)ok.error(), BadResultAccess);

    Result<int> bad = make_error(ErrorCode::NotFound, "nope");
    ASSERT_FALSE(bad.ok());
    EXPECT_EQ(bad.error().code, ErrorCode::NotFound);
    EXPECT_THROW((void)bad.value(), BadResultAccess);
    EXPECT_EQ(bad.value_or(7), 7);
}

TEST(Result, VoidSpecialisation) {
    Result<void> ok;
    EXPECT_TRUE(ok.ok());
    EXPECT_THROW((void)ok.error(), BadResultAccess);
    Result<void> bad = make_error(ErrorCode::Busy, "b");
    EXPECT_FALSE(bad);
    EXPECT_EQ(bad.error().message, "b");
}

TEST(Result, MoveOnlyAndString) {
    Result<std::string> r = std::string("hello");
    EXPECT_EQ(std::move(r).value(), "hello");
}

// Exit codes are a stable public contract: this table must never change silently.
TEST(ErrorTaxonomy, ExitCodesAreStable) {
    EXPECT_EQ(exit_code(ErrorCode::Ok), 0);
    EXPECT_EQ(exit_code(ErrorCode::InvalidArgument), 2);
    EXPECT_EQ(exit_code(ErrorCode::ModelError), 3);
    EXPECT_EQ(exit_code(ErrorCode::BackendError), 4);
    EXPECT_EQ(exit_code(ErrorCode::OutOfMemory), 5);
    EXPECT_EQ(exit_code(ErrorCode::Cancelled), 6);
    EXPECT_EQ(exit_code(ErrorCode::NotFound), 7);
    EXPECT_EQ(exit_code(ErrorCode::IoError), 8);
    EXPECT_EQ(exit_code(ErrorCode::SecurityViolation), 9);
    EXPECT_EQ(exit_code(ErrorCode::Unsupported), 10);
    EXPECT_EQ(exit_code(ErrorCode::InvalidState), 11);
    EXPECT_EQ(exit_code(ErrorCode::AlreadyExists), 12);
    EXPECT_EQ(exit_code(ErrorCode::Busy), 13);
    EXPECT_EQ(exit_code(ErrorCode::Timeout), 14);
    EXPECT_EQ(exit_code(ErrorCode::Overloaded), 15);
    EXPECT_EQ(exit_code(ErrorCode::Internal), 70);
}

TEST(ErrorTaxonomy, NamesAreUniqueAndKnown) {
    std::set<std::string_view> names;
    for (ErrorCode c : {ErrorCode::Ok, ErrorCode::InvalidArgument, ErrorCode::ModelError,
                        ErrorCode::BackendError, ErrorCode::OutOfMemory, ErrorCode::Cancelled,
                        ErrorCode::NotFound, ErrorCode::IoError, ErrorCode::SecurityViolation,
                        ErrorCode::Unsupported, ErrorCode::InvalidState, ErrorCode::AlreadyExists,
                        ErrorCode::Busy, ErrorCode::Timeout, ErrorCode::Overloaded, ErrorCode::Internal}) {
        EXPECT_NE(to_string(c), "unknown");
        EXPECT_TRUE(names.insert(to_string(c)).second);
    }
    EXPECT_EQ(Error(ErrorCode::OutOfMemory, "x").exit_code(), 5);
}
