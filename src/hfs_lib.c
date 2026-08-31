#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "hfs.h"
#include "hfs_internal.h"
#include "hfs_lib.h"
#include "hfs_log.h"
#include "hfs_meta.h"

/* Serializes all public library entry points (init/fini today). */
static pthread_mutex_t g_libLock = PTHREAD_MUTEX_INITIALIZER;

/* The single mounted filesystem instance (one per process). */
static Hfs *g_fs;

/*
 * Per-fd bookkeeping. isHfs=1 means the fd belongs to HFS; otherwise it
 * is a transparent (pass-through) fd handled by glibc.
 */
struct hfs_handle {
	int fd;			/* application-visible fd */
	HfsFile *file;		/* open HFS file handle */
	uint64_t offset;	/* current file offset */
	int flags;		/* open flags (O_APPEND etc.) */
	int isHfs;		/* 1 = HFS handle, 0 = pass-through fd */
	bool writable;		/* opened with write access */
};

/* Fixed-size fd table shared by all library calls. */
static struct hfs_handle g_fdTable[HFS_FD_TABLE_SIZE];

/*
 * Directory enumeration state: a snapshot of child inodes taken at
 * opendir time, plus a read cursor.
 */
struct hfs_dir {
	uint64_t inodeId;	/* directory inode being listed */
	uint64_t count;		/* number of entries in the snapshot */
	uint64_t cursor;	/* current position in the snapshot */
	struct hfs_dirent *entries;	/* snapshot array */
};

/*
 * Initialize the library: logging, SPDK environment, mount and metadata.
 * Each step rolls back on failure. Idempotent: a second call returns 0
 * without re-initializing.
 */
int hfs_lib_init(void) {
	pthread_mutex_lock(&g_libLock);
	if (g_fs != NULL) {
		pthread_mutex_unlock(&g_libLock);
		return 0;
	}

	if (hfs_log_init() != 0) {
		pthread_mutex_unlock(&g_libLock);
		return -1;
	}

	if (hfsEnvSetup() != 0) {
		HFS_LOG_ERROR("hfsEnvSetup failed");
		hfs_log_fini();
		pthread_mutex_unlock(&g_libLock);
		return -1;
	}

	g_fs = calloc(1, sizeof(Hfs));
	if (g_fs == NULL) {
		hfs_log_fini();
		pthread_mutex_unlock(&g_libLock);
		return -1;
	}

	if (!hfsMount(g_fs)) {
		HFS_LOG_ERROR("hfsMount failed");
		free(g_fs);
		g_fs = NULL;
		hfs_log_fini();
		pthread_mutex_unlock(&g_libLock);
		return -1;
	}

	if (hfsMetaInit(g_fs) != 0) {
		HFS_LOG_ERROR("hfsMetaInit failed");
		free(g_fs);
		g_fs = NULL;
		hfs_log_fini();
		pthread_mutex_unlock(&g_libLock);
		return -1;
	}

	memset(g_fdTable, 0, sizeof(g_fdTable));
	HFS_LOG_INFO("libhfs initialized");
	pthread_mutex_unlock(&g_libLock);
	return 0;
}

/* Return the mounted filesystem instance (or NULL before init). */
Hfs *hfs_lib_get_fs(void) {
	return g_fs;
}

/* Runs before main(): proves the library was loaded by LD_PRELOAD. */
__attribute__((constructor))
static void hfs_preload_ctor(void) {
	fprintf(stderr, "libhfs loaded\n");
	HFS_LOG_INFO("libhfs loaded");
}

/* Runs at process exit. */
__attribute__((destructor))
static void hfs_preload_dtor(void) {
	HFS_LOG_INFO("libhfs unloaded");
}

/* Tear down: unmount, free the instance and stop logging. */
void hfs_lib_fini(void) {
	pthread_mutex_lock(&g_libLock);
	if (g_fs == NULL) {
		pthread_mutex_unlock(&g_libLock);
		return;
	}

	hfsUmount(g_fs);
	free(g_fs);
	g_fs = NULL;
	hfs_log_fini();
	HFS_LOG_INFO("libhfs finalized");
	pthread_mutex_unlock(&g_libLock);
}

/* Stubs: filled in by shard-11 (open) and later shards. */
int hfs_lib_open(const char *path, int flags, mode_t mode) {
	errno = ENOSYS;
	return -1;
}

/* Stub: filled in by shard-16 (read/write path). */
ssize_t hfs_lib_read(int fd, void *buf, size_t count) {
	errno = ENOSYS;
	return -1;
}

/* Stub: filled in by shard-16 (read/write path). */
ssize_t hfs_lib_write(int fd, const void *buf, size_t count) {
	errno = ENOSYS;
	return -1;
}

/* Stub: filled in by shard-16 (read/write path). */
ssize_t hfs_lib_pread(int fd, void *buf, size_t count, off_t offset) {
	errno = ENOSYS;
	return -1;
}

/* Stub: filled in by shard-16 (read/write path). */
ssize_t hfs_lib_pwrite(int fd, const void *buf, size_t count, off_t offset) {
	errno = ENOSYS;
	return -1;
}

/* Stub: filled in by shard-16 (read/write path). */
off_t hfs_lib_lseek(int fd, off_t offset, int whence) {
	errno = ENOSYS;
	return -1;
}

/* Stub: filled in by shard-15 (close handling). */
int hfs_lib_close(int fd) {
	errno = ENOSYS;
	return -1;
}

/* Stub: filled in by shard-12 (unlink). */
int hfs_lib_unlink(const char *path) {
	errno = ENOSYS;
	return -1;
}

/* Stub: filled in by shard-14 (ftruncate). */
int hfs_lib_ftruncate(int fd, off_t length) {
	errno = ENOSYS;
	return -1;
}

/* Stub: filled in by shard-17 (fsync handling). */
int hfs_lib_fsync(int fd) {
	errno = ENOSYS;
	return -1;
}

/* Stub: filled in by shard-14 (stat family). */
int hfs_lib_fstat(int fd, struct stat *st) {
	errno = ENOSYS;
	return -1;
}

/* Stub: filled in by shard-10 (mkdir). */
int hfs_lib_mkdir(const char *path, mode_t mode) {
	errno = ENOSYS;
	return -1;
}

/* Stub: filled in by shard-10 (rmdir). */
int hfs_lib_rmdir(const char *path) {
	errno = ENOSYS;
	return -1;
}

/* Stub: filled in by shard-13 (rename). */
int hfs_lib_rename(const char *oldPath, const char *newPath) {
	errno = ENOSYS;
	return -1;
}

/* Stub: filled in by shard-19 (directory listing). */
struct hfs_dir *hfs_lib_opendir(const char *path) {
	errno = ENOSYS;
	return NULL;
}

/* Stub: filled in by shard-19 (directory listing). */
struct hfs_dirent *hfs_lib_readdir(struct hfs_dir *dir) {
	errno = ENOSYS;
	return NULL;
}

/* Stub: filled in by shard-19 (directory listing). */
int hfs_lib_closedir(struct hfs_dir *dir) {
	errno = ENOSYS;
	return -1;
}

/* Stub: filled in by shard-14 (stat family). */
int hfs_lib_stat(const char *path, struct stat *st) {
	errno = ENOSYS;
	return -1;
}