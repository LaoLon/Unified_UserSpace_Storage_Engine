#ifndef HFS_LIB_H
#define HFS_LIB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/vfs.h>
#include <time.h>

#include "hfs.h"
#include "hfs_async.h"

/* Size of the fd table shared by all HFS file descriptors. */
#define HFS_FD_TABLE_SIZE	65536

struct hfs_dir;

/* One entry returned by hfs_lib_readdir. */
struct hfs_dirent {
	uint64_t inodeId;		/* inode id of the entry */
	unsigned char d_type;		/* DT_DIR / DT_REG */
	char name[FILENAME_LENGTH];	/* entry name */
};

/* Initialize the library (logging, SPDK env, mount, metadata). */
int hfs_lib_init(void);
/* Returns true when the fd belongs to HFS (interception helper). */
bool hfs_is_hfs_fd(int fd);
/* Return true when the caller inherited HFS state across fork. */
bool hfs_lib_is_fork_child(void);
/* Tear down the library (unmount, stop logging). */
void hfs_lib_fini(void);
/* Return the mounted filesystem instance, or NULL before init. */
Hfs *hfs_lib_get_fs(void);

/* Register DMA-capable memory for experimental asynchronous direct I/O. */
int hfsRegisterBuffer(void *buffer, size_t length);
/* Unregister DMA memory; returns EBUSY while requests still reference it. */
int hfsUnregisterBuffer(void *buffer);
/* Allocate DMA memory for explicit zero-copy asynchronous I/O. */
void *hfsDmaAlloc(size_t size, size_t alignment);
/* Release memory returned by hfsDmaAlloc. */
void hfsDmaFree(void *buffer);

/* Open a file/dir under the HFS mount prefix. */
int hfs_lib_open(const char *path, int flags, mode_t mode);
/* Open relative to an HFS directory fd (openat semantics). */
int hfs_lib_openat(int dirfd, const char *path, int flags, mode_t mode);
/* Copy the inner path of an open HFS directory. */
int hfs_lib_get_dir_path(int fd, char *pathOut, size_t pathSize);
/* Read from the current offset of an HFS fd. */
ssize_t hfs_lib_read(int fd, void *buf, size_t count);
/* Write at the current offset of an HFS fd. */
ssize_t hfs_lib_write(int fd, const void *buf, size_t count);
/* Read at an explicit offset without moving the fd offset. */
ssize_t hfs_lib_pread(int fd, void *buf, size_t count, off_t offset);
/* Write at an explicit offset without moving the fd offset. */
ssize_t hfs_lib_pwrite(int fd, const void *buf, size_t count, off_t offset);
/* Read from the current file offset using caller-provided iovec segments. */
ssize_t hfs_lib_readv(int fd, const struct iovec *iov, int iovcnt);
/* Write at the current file offset using caller-provided iovec segments. */
ssize_t hfs_lib_writev(int fd, const struct iovec *iov, int iovcnt);
/* Read at an explicit offset using caller-provided iovec segments. */
ssize_t hfs_lib_preadv(int fd, const struct iovec *iov, int iovcnt,
		       off_t offset);
/* Write at an explicit offset using caller-provided iovec segments. */
ssize_t hfs_lib_pwritev(int fd, const struct iovec *iov, int iovcnt,
			 off_t offset);
/* Change the current offset of an HFS fd. */
off_t hfs_lib_lseek(int fd, off_t offset, int whence);
/* Close an HFS fd. */
int hfs_lib_close(int fd);
/* Detach an HFS fd without closing its kernel placeholder. */
int hfs_lib_release_fd(int fd);
/* Copy an HFS fd table slot (dup/dup2 support, interceptor helper). */
int hfs_lib_fd_copy(int oldfd, int newfd);
/* Remove a file (deletes its inode and data blob). */
int hfs_lib_unlink(const char *path);
/* Truncate an HFS file to 'length' bytes. */
int hfs_lib_ftruncate(int fd, off_t length);
/* Flush data and metadata of an HFS fd. */
int hfs_lib_fsync(int fd);
/* Flush data and required size/extent metadata of an HFS fd. */
int hfs_lib_fdatasync(int fd);
/* Preallocate space for an HFS fd, preserving size when KEEP_SIZE is set. */
int hfs_lib_fallocate(int fd, int mode, off_t offset, off_t len);
/* Stat an open HFS fd. */
int hfs_lib_fstat(int fd, struct stat *st);

/* Create a directory. */
int hfs_lib_mkdir(const char *path, mode_t mode);
/* Remove an empty directory. */
int hfs_lib_rmdir(const char *path);
/* Rename a file or directory (single metadata transaction). */
int hfs_lib_rename(const char *oldPath, const char *newPath);
/* Open a directory for enumeration. */
struct hfs_dir *hfs_lib_opendir(const char *path);
/* Return the next entry of an open directory, or NULL at the end. */
struct hfs_dirent *hfs_lib_readdir(struct hfs_dir *dir);
/* Close an open directory. */
int hfs_lib_closedir(struct hfs_dir *dir);
/* Stat a path under the HFS mount prefix. */
int hfs_lib_stat(const char *path, struct stat *st);
/* Return HFS capacity information for a managed path. */
int hfs_lib_statvfs(const char *path, struct statvfs *st);
/* Return HFS capacity information for an HFS fd. */
int hfs_lib_fstatvfs(int fd, struct statvfs *st);
/* Return Linux statfs information for a managed path. */
int hfs_lib_statfs(const char *path, struct statfs *st);
/* Return Linux statfs information for an HFS fd. */
int hfs_lib_fstatfs(int fd, struct statfs *st);
/* Check access to a path under the HFS mount prefix (no permission model). */
int hfs_lib_access(const char *path, int mode);
/* Update permission bits on a managed path. */
int hfs_lib_chmod(const char *path, mode_t mode);
/* Update permission bits on an HFS fd. */
int hfs_lib_fchmod(int fd, mode_t mode);
/* Update ownership on a managed path. */
int hfs_lib_chown(const char *path, uid_t uid, gid_t gid);
/* Update ownership on an HFS fd. */
int hfs_lib_fchown(int fd, uid_t uid, gid_t gid);
/* Update selected timestamps on a managed path. */
int hfs_lib_utimens(const char *path, const struct timespec times[2]);
/* Update selected timestamps on an HFS fd. */
int hfs_lib_futimens(int fd, const struct timespec times[2]);
/* Validate an advisory I/O hint for an HFS fd. */
int hfs_lib_posix_fadvise(int fd, off_t offset, off_t len, int advice);
/* fcntl for HFS fds: flags and fd duplication. */
int hfs_lib_fcntl(int fd, int cmd, long arg);

/* Submit positioned I/O and fsync through the bounded async executor. */
int hfs_lib_async_pread(int fd, void *buf, size_t count, off_t offset,
			HfsAsyncCallback callback, void *context,
			HfsAsyncRequest **requestOut);
int hfs_lib_async_pwrite(int fd, const void *buf, size_t count, off_t offset,
			 HfsAsyncCallback callback, void *context,
			 HfsAsyncRequest **requestOut);
int hfs_lib_async_preadv(int fd, const struct iovec *iov, int iovcnt,
			 off_t offset, HfsAsyncCallback callback, void *context,
			 HfsAsyncRequest **requestOut);
int hfs_lib_async_pwritev(int fd, const struct iovec *iov, int iovcnt,
			  off_t offset, HfsAsyncCallback callback, void *context,
			  HfsAsyncRequest **requestOut);
int hfs_lib_async_fsync(int fd, HfsAsyncCallback callback, void *context,
			 HfsAsyncRequest **requestOut);

#endif /* HFS_LIB_H */
