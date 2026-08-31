#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "hfs_lib.h"

static pthread_mutex_t g_libLock = PTHREAD_MUTEX_INITIALIZER;

struct hfs_handle {
	int fd;
	HfsFile *file;
	uint64_t offset;
	int flags;
	int isHfs;
	bool writable;
};

static struct hfs_handle g_fdTable[HFS_FD_TABLE_SIZE];

struct hfs_dir {
	uint64_t inodeId;
	uint64_t count;
	uint64_t cursor;
	struct hfs_dirent *entries;
};

int hfs_lib_init(void) {
	memset(g_fdTable, 0, sizeof(g_fdTable));
	(void)g_libLock;
	errno = ENOSYS;
	return -1;
}

void hfs_lib_fini(void) {
}

int hfs_lib_open(const char *path, int flags, mode_t mode) {
	errno = ENOSYS;
	return -1;
}

ssize_t hfs_lib_read(int fd, void *buf, size_t count) {
	errno = ENOSYS;
	return -1;
}

ssize_t hfs_lib_write(int fd, const void *buf, size_t count) {
	errno = ENOSYS;
	return -1;
}

ssize_t hfs_lib_pread(int fd, void *buf, size_t count, off_t offset) {
	errno = ENOSYS;
	return -1;
}

ssize_t hfs_lib_pwrite(int fd, const void *buf, size_t count, off_t offset) {
	errno = ENOSYS;
	return -1;
}

off_t hfs_lib_lseek(int fd, off_t offset, int whence) {
	errno = ENOSYS;
	return -1;
}

int hfs_lib_close(int fd) {
	errno = ENOSYS;
	return -1;
}

int hfs_lib_unlink(const char *path) {
	errno = ENOSYS;
	return -1;
}

int hfs_lib_ftruncate(int fd, off_t length) {
	errno = ENOSYS;
	return -1;
}

int hfs_lib_fsync(int fd) {
	errno = ENOSYS;
	return -1;
}

int hfs_lib_fstat(int fd, struct stat *st) {
	errno = ENOSYS;
	return -1;
}

int hfs_lib_mkdir(const char *path, mode_t mode) {
	errno = ENOSYS;
	return -1;
}

int hfs_lib_rmdir(const char *path) {
	errno = ENOSYS;
	return -1;
}

int hfs_lib_rename(const char *oldPath, const char *newPath) {
	errno = ENOSYS;
	return -1;
}

struct hfs_dir *hfs_lib_opendir(const char *path) {
	errno = ENOSYS;
	return NULL;
}

struct hfs_dirent *hfs_lib_readdir(struct hfs_dir *dir) {
	errno = ENOSYS;
	return NULL;
}

int hfs_lib_closedir(struct hfs_dir *dir) {
	errno = ENOSYS;
	return -1;
}

int hfs_lib_stat(const char *path, struct stat *st) {
	errno = ENOSYS;
	return -1;
}