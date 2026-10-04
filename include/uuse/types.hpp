#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace uuse {

using FileId = std::uint64_t;
using DeviceId = std::uint32_t;
using RequestId = std::uint64_t;

/* Status codes shared by the core API; frontends map them to their own errors. */
enum class Status : std::uint8_t {
    Ok = 0,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    NoSpace,
    NoMemory,
    Busy,
    TryAgain,
    IoError,
    Cancelled,
    TimedOut,
    ForkedProcess,
    Corrupt,
    NotSupported,
    InvalidState,
};

/* Stable status name for logs and diagnostics. */
inline const char *statusName(Status status) noexcept {
    switch (status) {
    case Status::Ok: return "Ok";
    case Status::InvalidArgument: return "InvalidArgument";
    case Status::NotFound: return "NotFound";
    case Status::AlreadyExists: return "AlreadyExists";
    case Status::NoSpace: return "NoSpace";
    case Status::NoMemory: return "NoMemory";
    case Status::Busy: return "Busy";
    case Status::TryAgain: return "TryAgain";
    case Status::IoError: return "IoError";
    case Status::Cancelled: return "Cancelled";
    case Status::TimedOut: return "TimedOut";
    case Status::ForkedProcess: return "ForkedProcess";
    case Status::Corrupt: return "Corrupt";
    case Status::NotSupported: return "NotSupported";
    case Status::InvalidState: return "InvalidState";
    }
    return "Unknown";
}

/* Backend result of one operation. */
struct ResultInfo {
    Status status = Status::Ok;
    int backendErrno = 0;
    std::size_t bytes = 0;
    std::size_t completedBytes = 0;
    RequestId requestId = 0;
};

/* Error-or-value container used by the no-exception core. */
template <typename T>
class Result {
public:
    Result(T value) : status_(Status::Ok), value_(std::move(value)) {}
    Result(Status status, std::string message)
        : status_(status), message_(std::move(message)) {}

    bool ok() const noexcept { return status_ == Status::Ok; }
    Status status() const noexcept { return status_; }
    const std::string &message() const noexcept { return message_; }

    /* Precondition: ok() is true. */
    T &value() & noexcept { return *value_; }
    const T &value() const & noexcept { return *value_; }
    T &&value() && noexcept { return std::move(*value_); }

private:
    Status status_ = Status::InvalidState;
    std::optional<T> value_;
    std::string message_;
};

/* Void specialization: the status itself is the payload. */
template <>
class Result<void> {
public:
    Result() = default;
    Result(Status status, std::string message = {})
        : status_(status), message_(std::move(message)) {}

    bool ok() const noexcept { return status_ == Status::Ok; }
    Status status() const noexcept { return status_; }
    const std::string &message() const noexcept { return message_; }

private:
    Status status_ = Status::InvalidState;
    std::string message_;
};

/* Helpers to keep Result construction sites short. */
template <typename T>
Result<T> makeError(Status status, std::string message) {
    return Result<T>(status, std::move(message));
}

inline Result<void> makeError(Status status, std::string message) {
    return Result<void>(status, std::move(message));
}

} // namespace uuse
