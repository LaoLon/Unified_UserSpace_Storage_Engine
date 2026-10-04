#include <cerrno>
#include <cstdio>

#include "fio/fio_status_errno.hpp"
#include "test_support.hpp"

/* The fio adapter must both map statuses and store errno: fio reports err=0
 * and silently drops a job when open_file returns an error without errno. */
int main() {
    errno = 0;
    UUSE_TEST_CHECK(uuse::fio::setErrnoFromStatus(uuse::Status::Busy) == EAGAIN);
    UUSE_TEST_CHECK(errno == EAGAIN);

    errno = 0;
    UUSE_TEST_CHECK(uuse::fio::setErrnoFromStatus(uuse::Status::NoMemory) == ENOMEM);
    UUSE_TEST_CHECK(errno == ENOMEM);

    UUSE_TEST_CHECK(uuse::fio::mapStatusToErrno(uuse::Status::TryAgain) == EAGAIN);
    UUSE_TEST_CHECK(uuse::fio::mapStatusToErrno(uuse::Status::NoSpace) == ENOSPC);
    UUSE_TEST_CHECK(uuse::fio::mapStatusToErrno(uuse::Status::NotFound) == ENOENT);
    UUSE_TEST_CHECK(uuse::fio::mapStatusToErrno(uuse::Status::AlreadyExists) == EEXIST);
    UUSE_TEST_CHECK(uuse::fio::mapStatusToErrno(uuse::Status::InvalidArgument) == EINVAL);
    UUSE_TEST_CHECK(uuse::fio::mapStatusToErrno(uuse::Status::NotSupported) == ENOTSUP);
    UUSE_TEST_CHECK(uuse::fio::mapStatusToErrno(uuse::Status::Cancelled) == ECANCELED);
    UUSE_TEST_CHECK(uuse::fio::mapStatusToErrno(uuse::Status::TimedOut) == ETIMEDOUT);
    UUSE_TEST_CHECK(uuse::fio::mapStatusToErrno(uuse::Status::IoError) == EIO);
    UUSE_TEST_CHECK(uuse::fio::mapStatusToErrno(uuse::Status::Corrupt) == EIO);

    std::printf("fio_status_errno_test: ok\n");
    return 0;
}
