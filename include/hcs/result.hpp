#pragma once
// Result<T> / Error taxonomy. Exit codes are a STABLE public contract (docs/api.md, CLI).
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace hcs {

enum class ErrorCode : int {
    Ok = 0,
    InvalidArgument = 2,  // bad CLI/API/config input
    ModelError = 3,       // model missing/corrupt/unsupported format
    BackendError = 4,     // engine/Vulkan/device failure
    OutOfMemory = 5,      // budget or allocation exhausted
    Cancelled = 6,        // cooperative cancellation
    NotFound = 7,
    IoError = 8,
    SecurityViolation = 9,  // path escape, auth failure, limit abuse
    Unsupported = 10,       // feature/model/backend not available
    InvalidState = 11,      // illegal lifecycle transition / shut down
    AlreadyExists = 12,
    Busy = 13,  // queue full / resource in use
    Timeout = 14,
    Internal = 70,  // bug / unexpected exception
};

constexpr int exit_code(ErrorCode c) noexcept { return static_cast<int>(c); }

constexpr std::string_view to_string(ErrorCode c) noexcept {
    switch (c) {
        case ErrorCode::Ok: return "ok";
        case ErrorCode::InvalidArgument: return "invalid_argument";
        case ErrorCode::ModelError: return "model_error";
        case ErrorCode::BackendError: return "backend_error";
        case ErrorCode::OutOfMemory: return "out_of_memory";
        case ErrorCode::Cancelled: return "cancelled";
        case ErrorCode::NotFound: return "not_found";
        case ErrorCode::IoError: return "io_error";
        case ErrorCode::SecurityViolation: return "security_violation";
        case ErrorCode::Unsupported: return "unsupported";
        case ErrorCode::InvalidState: return "invalid_state";
        case ErrorCode::AlreadyExists: return "already_exists";
        case ErrorCode::Busy: return "busy";
        case ErrorCode::Timeout: return "timeout";
        case ErrorCode::Internal: return "internal";
    }
    return "unknown";
}

struct Error {
    ErrorCode code = ErrorCode::Internal;
    std::string message;

    Error() = default;
    Error(ErrorCode c, std::string m) : code(c), message(std::move(m)) {}
    int exit_code() const noexcept { return hcs::exit_code(code); }
};

class BadResultAccess : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : v_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : v_(std::in_place_index<1>, std::move(error)) {}

    bool ok() const noexcept { return v_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    T& value() & { check_ok(); return std::get<0>(v_); }
    const T& value() const& { check_ok(); return std::get<0>(v_); }
    T&& value() && { check_ok(); return std::get<0>(std::move(v_)); }
    T value_or(T fallback) const& { return ok() ? std::get<0>(v_) : std::move(fallback); }

    const Error& error() const {
        if (ok()) throw BadResultAccess("Result::error() called on a success value");
        return std::get<1>(v_);
    }

private:
    void check_ok() const {
        if (!ok()) throw BadResultAccess("Result::value() called on error: " + std::get<1>(v_).message);
    }
    std::variant<T, Error> v_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;  // success
    Result(Error error) : has_error_(true), error_(std::move(error)) {}

    bool ok() const noexcept { return !has_error_; }
    explicit operator bool() const noexcept { return ok(); }
    const Error& error() const {
        if (!has_error_) throw BadResultAccess("Result<void>::error() called on success");
        return error_;
    }

private:
    bool has_error_ = false;
    Error error_;
};

inline Error make_error(ErrorCode c, std::string message) { return Error(c, std::move(message)); }

}  // namespace hcs
