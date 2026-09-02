#ifndef HFS_LIB_H
#define HFS_LIB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "hfs.h"

/* Size of the fd table shared by all HFS file descriptors. */
#define HFS_FD_TABLE_SIZE	65536

struct hfs_dir;

/* One entry returned by hfs_lib_readdir. */
struct hfs_dirent {
	uint64_t inodeId;		/* inode id of the entry */
	char name[FILENAME_LENGTH];	/* entry name */
};

/* Initialize the library (logging, SPDK env, mount, metadata). */
int hfs_lib_init(void);
/* Returns true when the fd belongs to HFS (interception helper). */
bool hfs_is_hfs_fd(int fd);
/* Tear down the library (unmount, stop logging). */
void hfs_lib_fini(void);
/* Return the mounted filesystem instance, or NULL before init. */
Hfs *hfs_lib_get_fs(void);

/* Open a file/dir under the HFS mount prefix. */
int hfs_lib_open(const char *path, int flags, mode_t mode);
/* Read from the current offset of an HFS fd. */
ssize_t hfs_lib_read(int fd, void *buf, size_t count);
/* Write at the current offset of an HFS fd. */
ssize_t hfs_lib_write(int fd, const void *buf, size_t count);
/* Read at an explicit offset without moving the fd offset. */
ssize_t hfs_lib_pread(int fd, void *buf, size_t count, off_t offset);
/* Write at an explicit offset without moving the fd offset. */
ssize_t hfs_lib_pwrite(int fd, const void *buf, size_t count, off_t offset);
/* Change the current offset of an HFS fd. */
off_t hfs_lib_lseek(int fd, off_t offset, int whence);
/* Close an HFS fd. */
int hfs_lib_close(int fd);
/* Copy an HFS fd table slot (dup/dup2 support, interceptor helper). */
int hfs_lib_fd_copy(int oldfd, int newfd);
/* Remove a file (deletes its inode and data blob). */
int hfs_lib_unlink(const char *path);
/* Truncate an HFS file to 'length' bytes. */
int hfs_lib_ftruncate(int fd, off_t length);
/* Flush data and metadata of an HFS fd. */
int hfs_lib_fsync(int fd);
/* Preallocate space for an HFS fd (KEEP_SIZE is a no-op). */
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

#endif /* HFS_LIB_H */