#pragma once

#include <cerrno>

#include "hfs/types.hpp"

namespace hfs::fio {

/* Maps one core status to the errno value used by the fio adapter. */
inline int mapStatusToErrno(::hfs::Status status) noexcept {
    switch (status) {
    case ::hfs::Status::Busy:
    case ::hfs::Status::TryAgain:
        return EAGAIN;
    case ::hfs::Status::NoMemory:
        return ENOMEM;
    case ::hfs::Status::NoSpace:
        return ENOSPC;
    case ::hfs::Status::NotFound:
        return ENOENT;
    case ::hfs::Status::AlreadyExists:
        return EEXIST;
    case ::hfs::Status::InvalidArgument:
        return EINVAL;
    case ::hfs::Status::NotSupported:
        return ENOTSUP;
    case ::hfs::Status::Cancelled:
        return ECANCELED;
    case ::hfs::Status::TimedOut:
        return ETIMEDOUT;
    case ::hfs::Status::IoError:
        return EIO;
    default:
        return EIO;
    }
}

/* Stores errno for fio's setup error path and returns the same value. The
 * engine must not return an error code without setting errno: fio reports
 * err=0 and silently drops the job otherwise. */
inline int setErrnoFromStatus(::hfs::Status status) noexcept {
    const int value = mapStatusToErrno(status);
    errno = value;
    return value;
}

} // namespace hfs::fio
