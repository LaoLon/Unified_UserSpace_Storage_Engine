#include <cerrno>
#include <cstdio>

#include "fio/fio_status_errno.hpp"
#include "test_support.hpp"

/* The fio adapter must both map statuses and store errno: fio reports err=0
 * and silently drops a job when open_file returns an error without errno. */
int main() {
    errno = 0;
    HFS_TEST_CHECK(hfs::fio::setErrnoFromStatus(hfs::Status::Busy) == EAGAIN);
    HFS_TEST_CHECK(errno == EAGAIN);

    errno = 0;
    HFS_TEST_CHECK(hfs::fio::setErrnoFromStatus(hfs::Status::NoMemory) == ENOMEM);
    HFS_TEST_CHECK(errno == ENOMEM);

    HFS_TEST_CHECK(hfs::fio::mapStatusToErrno(hfs::Status::TryAgain) == EAGAIN);
    HFS_TEST_CHECK(hfs::fio::mapStatusToErrno(hfs::Status::NoSpace) == ENOSPC);
    HFS_TEST_CHECK(hfs::fio::mapStatusToErrno(hfs::Status::NotFound) == ENOENT);
    HFS_TEST_CHECK(hfs::fio::mapStatusToErrno(hfs::Status::AlreadyExists) == EEXIST);
    HFS_TEST_CHECK(hfs::fio::mapStatusToErrno(hfs::Status::InvalidArgument) == EINVAL);
    HFS_TEST_CHECK(hfs::fio::mapStatusToErrno(hfs::Status::NotSupported) == ENOTSUP);
    HFS_TEST_CHECK(hfs::fio::mapStatusToErrno(hfs::Status::Cancelled) == ECANCELED);
    HFS_TEST_CHECK(hfs::fio::mapStatusToErrno(hfs::Status::TimedOut) == ETIMEDOUT);
    HFS_TEST_CHECK(hfs::fio::mapStatusToErrno(hfs::Status::IoError) == EIO);
    HFS_TEST_CHECK(hfs::fio::mapStatusToErrno(hfs::Status::Corrupt) == EIO);

    std::printf("fio_status_errno_test: ok\n");
    return 0;
}
