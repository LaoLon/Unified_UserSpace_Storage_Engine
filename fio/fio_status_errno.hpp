#pragma once

#include <cerrno>

#include "uuse/types.hpp"

namespace uuse::fio {

/* Maps one core status to the errno value used by the fio adapter. */
inline int mapStatusToErrno(::uuse::Status status) noexcept {
    switch (status) {
    case ::uuse::Status::Busy:
    case ::uuse::Status::TryAgain:
        return EAGAIN;
    case ::uuse::Status::NoMemory:
        return ENOMEM;
    case ::uuse::Status::NoSpace:
        return ENOSPC;
    case ::uuse::Status::NotFound:
        return ENOENT;
    case ::uuse::Status::AlreadyExists:
        return EEXIST;
    case ::uuse::Status::InvalidArgument:
        return EINVAL;
    case ::uuse::Status::NotSupported:
        return ENOTSUP;
    case ::uuse::Status::Cancelled:
        return ECANCELED;
    case ::uuse::Status::TimedOut:
        return ETIMEDOUT;
    case ::uuse::Status::IoError:
        return EIO;
    default:
        return EIO;
    }
}

/* Stores errno for fio's setup error path and returns the same value. The
 * engine must not return an error code without setting errno: fio reports
 * err=0 and silently drops the job otherwise. */
inline int setErrnoFromStatus(::uuse::Status status) noexcept {
    const int value = mapStatusToErrno(status);
    errno = value;
    return value;
}

} // namespace uuse::fio
