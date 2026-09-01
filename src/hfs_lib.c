#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

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
	uint64_t inodeId;	/* inode id backing this fd */
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
	HFS_LOG_DEBUG("hfs_log_init done");

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

	/*
	 * Mark the environment so forked+exec'd children of this process
	 * skip SPDK re-initialization (a second primary conflicts with the
	 * parent's DPDK state and hangs). They run in pass-through mode.
	 */
	setenv("_HFS_INIT_DONE", "1", 0);
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
	HFS_LOG_DEBUG("libhfs loaded");
}

/* Runs at process exit. */
__attribute__((destructor))
static void hfs_preload_dtor(void) {
	HFS_LOG_DEBUG("libhfs unloaded");
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
	HFS_LOG_DEBUG("libhfs finalized");
	pthread_mutex_unlock(&g_libLock);
}

/*
 * Open a file under the HFS mount prefix.
 * O_CREAT creates a fresh file (data blob + inode); O_TRUNC resets its
 * size. Returns a file descriptor registered in the fd table.
 */
int hfs_lib_open(const char *path, int flags, mode_t mode) {
	struct hfs_inode *in;
	struct hfs_inode *parent;
	const char *leaf;
	HfsFile *file;
	int fd;
	int rc;

	pthread_mutex_lock(&g_libLock);
	if (g_fs == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = ENODEV;
		return -1;
	}

	in = hfsMetaResolvePath(g_fs, path);
	if (in == NULL) {
		if (errno != ENOENT || !(flags & O_CREAT)) {
			pthread_mutex_unlock(&g_libLock);
			return -1;
		}
		/* Create the file and re-resolve its inode. */
		if (hfsMetaResolveParent(g_fs, path, &parent, &leaf) != 0) {
			pthread_mutex_unlock(&g_libLock);
			return -1;
		}
		rc = hfsMetaCreateFile(g_fs, parent, leaf);
		if (rc != 0) {
			pthread_mutex_unlock(&g_libLock);
			return -1;
		}
		in = hfsMetaFindByName(g_fs, parent->id, leaf);
		if (in == NULL) {
			pthread_mutex_unlock(&g_libLock);
			errno = ENOENT;
			return -1;
		}
	} else if (in->type != HFS_INODE_FILE) {
		pthread_mutex_unlock(&g_libLock);
		errno = EISDIR;
		return -1;
	}

	if ((flags & O_TRUNC) && in->size != 0) {
		in->size = 0;
		if (hfsMetaSaveAll(g_fs) != 0) {
			pthread_mutex_unlock(&g_libLock);
			return -1;
		}
	}

	/*
	 * Reserve a matching kernel fd (via /dev/null) so that later real
	 * opens in the same process cannot reuse this number. Otherwise a
	 * genuine kernel fd would collide with the HFS fd and the close
	 * interceptor would close the wrong handle.
	 */
	fd = open("/dev/null", O_RDONLY);
	if (fd < 0) {
		pthread_mutex_unlock(&g_libLock);
		errno = EMFILE;
		return -1;
	}
	if (fd >= HFS_FD_TABLE_SIZE) {
		close(fd);
		pthread_mutex_unlock(&g_libLock);
		errno = EMFILE;
		return -1;
	}

	file = calloc(1, sizeof(HfsFile));
	if (file == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = ENOMEM;
		return -1;
	}
	file->fs = g_fs;
	file->blobId = in->blobId;
	file->refCount = 1;

	if (!hfsOpenById(file)) {
		free(file);
		pthread_mutex_unlock(&g_libLock);
		errno = EIO;
		return -1;
	}

	g_fdTable[fd].fd = fd;
	g_fdTable[fd].file = file;
	g_fdTable[fd].inodeId = in->id;
	g_fdTable[fd].offset = 0;
	g_fdTable[fd].flags = flags;
	g_fdTable[fd].isHfs = 1;
	g_fdTable[fd].writable = ((flags & O_ACCMODE) != O_RDONLY);

	pthread_mutex_unlock(&g_libLock);
	return fd;
}

/*
 * Read from the current offset of an HFS fd.
 * Returns 0 at end of file; updates the fd offset.
 */
ssize_t hfs_lib_read(int fd, void *buf, size_t count) {
	struct hfs_handle *h;
	struct hfs_inode *in;
	ssize_t n;

	if (count == 0) {
		return 0;
	}

	pthread_mutex_lock(&g_libLock);
	if (g_fs == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = ENODEV;
		return -1;
	}
	if (fd < 0 || fd >= HFS_FD_TABLE_SIZE || g_fdTable[fd].file == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = EBADF;
		return -1;
	}

	h = &g_fdTable[fd];
	in = hfsMetaFindById(g_fs, h->inodeId);
	if (in == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = ENOENT;
		return -1;
	}
	if (h->offset >= in->size) {
		pthread_mutex_unlock(&g_libLock);
		return 0;	/* end of file */
	}
	if (h->offset + count > in->size) {
		count = (size_t)(in->size - h->offset);
	}

	n = hfsReadAt(h->file, buf, count, h->offset);
	if (n > 0) {
		h->offset += (uint64_t)n;
	}
	pthread_mutex_unlock(&g_libLock);
	return n;
}

/*
 * Write at the current offset of an HFS fd.
 * Grows the file size when writing past the end.
 */
ssize_t hfs_lib_write(int fd, const void *buf, size_t count) {
	struct hfs_handle *h;
	struct hfs_inode *in;
	struct timespec ts;
	ssize_t n;

	if (count == 0) {
		return 0;
	}

	pthread_mutex_lock(&g_libLock);
	if (g_fs == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = ENODEV;
		return -1;
	}
	if (fd < 0 || fd >= HFS_FD_TABLE_SIZE || g_fdTable[fd].file == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = EBADF;
		return -1;
	}

	h = &g_fdTable[fd];
	if (!h->writable) {
		pthread_mutex_unlock(&g_libLock);
		errno = EBADF;
		return -1;
	}

	n = hfsWriteAt(h->file, buf, count, h->offset);
	if (n <= 0) {
		pthread_mutex_unlock(&g_libLock);
		errno = EIO;
		return -1;
	}

	h->offset += (uint64_t)n;
	in = hfsMetaFindById(g_fs, h->inodeId);
	if (in != NULL && h->offset > in->size) {
		/* Update size/mtime in memory; persisted at fsync/close. */
		clock_gettime(CLOCK_REALTIME, &ts);
		in->size = h->offset;
		in->mtimeNs = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
	}
	pthread_mutex_unlock(&g_libLock);
	return n;
}

/* Read at an explicit offset without moving the fd offset. */
ssize_t hfs_lib_pread(int fd, void *buf, size_t count, off_t offset) {
	struct hfs_handle *h;
	struct hfs_inode *in;
	ssize_t n;

	if (count == 0) {
		return 0;
	}
	if (offset < 0) {
		errno = EINVAL;
		return -1;
	}

	pthread_mutex_lock(&g_libLock);
	if (g_fs == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = ENODEV;
		return -1;
	}
	if (fd < 0 || fd >= HFS_FD_TABLE_SIZE || g_fdTable[fd].file == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = EBADF;
		return -1;
	}

	h = &g_fdTable[fd];
	in = hfsMetaFindById(g_fs, h->inodeId);
	if (in == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = ENOENT;
		return -1;
	}
	if ((uint64_t)offset >= in->size) {
		pthread_mutex_unlock(&g_libLock);
		return 0;
	}
	if ((uint64_t)offset + count > in->size) {
		count = (size_t)(in->size - (uint64_t)offset);
	}

	n = hfsReadAt(h->file, buf, count, (uint64_t)offset);
	pthread_mutex_unlock(&g_libLock);
	return n;
}

/* Write at an explicit offset without moving the fd offset. */
ssize_t hfs_lib_pwrite(int fd, const void *buf, size_t count, off_t offset) {
	struct hfs_handle *h;
	struct hfs_inode *in;
	struct timespec ts;
	ssize_t n;

	if (count == 0) {
		return 0;
	}
	if (offset < 0) {
		errno = EINVAL;
		return -1;
	}

	pthread_mutex_lock(&g_libLock);
	if (g_fs == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = ENODEV;
		return -1;
	}
	if (fd < 0 || fd >= HFS_FD_TABLE_SIZE || g_fdTable[fd].file == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = EBADF;
		return -1;
	}

	h = &g_fdTable[fd];
	if (!h->writable) {
		pthread_mutex_unlock(&g_libLock);
		errno = EBADF;
		return -1;
	}

	n = hfsWriteAt(h->file, buf, count, (uint64_t)offset);
	if (n <= 0) {
		pthread_mutex_unlock(&g_libLock);
		errno = EIO;
		return -1;
	}

	in = hfsMetaFindById(g_fs, h->inodeId);
	if (in != NULL && (uint64_t)offset + (uint64_t)n > in->size) {
		clock_gettime(CLOCK_REALTIME, &ts);
		in->size = (uint64_t)offset + (uint64_t)n;
		in->mtimeNs = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
	}
	pthread_mutex_unlock(&g_libLock);
	return n;
}

/* Change the current offset of an HFS fd (SEEK_SET/CUR/END). */
off_t hfs_lib_lseek(int fd, off_t offset, int whence) {
	struct hfs_handle *h;
	struct hfs_inode *in;
	uint64_t base;

	pthread_mutex_lock(&g_libLock);
	if (g_fs == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = ENODEV;
		return -1;
	}
	if (fd < 0 || fd >= HFS_FD_TABLE_SIZE || g_fdTable[fd].file == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = EBADF;
		return -1;
	}

	h = &g_fdTable[fd];
	switch (whence) {
	case SEEK_SET:
		base = 0;
		break;
	case SEEK_CUR:
		base = h->offset;
		break;
	case SEEK_END:
		in = hfsMetaFindById(g_fs, h->inodeId);
		if (in == NULL) {
			pthread_mutex_unlock(&g_libLock);
			errno = ENOENT;
			return -1;
		}
		base = in->size;
		break;
	default:
		pthread_mutex_unlock(&g_libLock);
		errno = EINVAL;
		return -1;
	}

	if (offset < 0 && (uint64_t)(-offset) > base) {
		pthread_mutex_unlock(&g_libLock);
		errno = EINVAL;
		return -1;
	}

	h->offset = base + (uint64_t)offset;
	offset = (off_t)h->offset;
	pthread_mutex_unlock(&g_libLock);
	return offset;
}

/* Copy the HFS fd table slot to another fd (dup/dup2 interceptor helper). */
int hfs_lib_fd_copy(int oldfd, int newfd) {
	pthread_mutex_lock(&g_libLock);
	if (oldfd < 0 || oldfd >= HFS_FD_TABLE_SIZE ||
	    g_fdTable[oldfd].file == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = EBADF;
		return -1;
	}
	if (newfd < 0 || newfd >= HFS_FD_TABLE_SIZE) {
		pthread_mutex_unlock(&g_libLock);
		errno = EBADF;
		return -1;
	}
	/* dup2 overwrites the target: drop any existing HFS slot first. */
	if (g_fdTable[newfd].file != NULL) {
		HfsFile *f = g_fdTable[newfd].file;
		if (--f->refCount == 0) {
			hfsClose(f);
			free(f);
		}
	}
	g_fdTable[newfd] = g_fdTable[oldfd];
	g_fdTable[newfd].file->refCount++;
	pthread_mutex_unlock(&g_libLock);
	return 0;
}

/*
 * Returns true when the fd belongs to HFS (interception helper).
 * Lock-free on purpose: this is a fast path called by the close()
 * interceptor, which may run while hfs_lib_init already holds
 * g_libLock (e.g. hfsLogTrimTail closes its fd during init). Taking
 * the lock there would self-deadlock. The fd table is a fixed-size
 * array whose slots are only written by hfs_lib_open/hfs_lib_close.
 */
bool hfs_is_hfs_fd(int fd) {
	if (fd < 0 || fd >= HFS_FD_TABLE_SIZE) {
		return false;
	}
	return (g_fdTable[fd].file != NULL);
}

/*
 * Close an HFS fd: drop one reference; close the blob handle and free
 * the file only when the last reference (dup shares) goes away.
 */
int hfs_lib_close(int fd) {
	struct hfs_handle *h;
	HfsFile *file;
	bool lastRef;

	pthread_mutex_lock(&g_libLock);
	if (g_fs == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = ENODEV;
		return -1;
	}
	if (fd < 0 || fd >= HFS_FD_TABLE_SIZE || g_fdTable[fd].file == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = EBADF;
		return -1;
	}

	h = &g_fdTable[fd];
	file = h->file;
	lastRef = (--file->refCount == 0);

	if (lastRef && !hfsClose(file)) {
		pthread_mutex_unlock(&g_libLock);
		errno = EIO;
		return -1;
	}

	if (lastRef) {
		free(file);
	}
	memset(h, 0, sizeof(*h));
	pthread_mutex_unlock(&g_libLock);

	/* Release the kernel placeholder fd. The table slot is already
	 * cleared, so the close interceptor passes this through. */
	close(fd);
	return 0;
}

/*
 * Remove a file under the HFS mount prefix: drop the inode, then
 * delete its data blob and persist the table.
 */
int hfs_lib_unlink(const char *path) {
	struct hfs_inode *parent;
	const char *leaf;
	uint64_t blobId;
	HfsFile file;
	int rc;

	pthread_mutex_lock(&g_libLock);
	if (g_fs == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = ENODEV;
		return -1;
	}

	rc = hfsMetaResolveParent(g_fs, path, &parent, &leaf);
	if (rc != 0) {
		pthread_mutex_unlock(&g_libLock);
		return -1;
	}

	rc = hfsMetaRemoveFile(g_fs, parent, leaf, &blobId);
	if (rc != 0) {
		pthread_mutex_unlock(&g_libLock);
		return -1;
	}

	/* Delete the data blob via the waiter chain. */
	memset(&file, 0, sizeof(file));
	file.fs = g_fs;
	file.blobId = blobId;
	if (!hfsDeleteBlobById(&file)) {
		pthread_mutex_unlock(&g_libLock);
		errno = EIO;
		return -1;
	}

	rc = hfsMetaSaveAll(g_fs);
	pthread_mutex_unlock(&g_libLock);
	return rc;
}

/*
 * Truncate the file behind 'fd' to 'length'.
 * Updates the inode size and grows the data blob as needed.
 */
int hfs_lib_ftruncate(int fd, off_t length) {
	struct hfs_handle *h;
	struct hfs_inode *in;
	int rc;

	if (length < 0) {
		errno = EINVAL;
		return -1;
	}

	pthread_mutex_lock(&g_libLock);
	if (g_fs == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = ENODEV;
		return -1;
	}
	if (fd < 0 || fd >= HFS_FD_TABLE_SIZE || g_fdTable[fd].file == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = EBADF;
		return -1;
	}

	h = &g_fdTable[fd];
	in = hfsMetaFindById(g_fs, h->inodeId);
	if (in == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = ENOENT;
		return -1;
	}

	/* Grow the blob first so the new size is backed by storage. */
	if (length > (off_t)in->size) {
		if (hfsResizeTo(h->file, (uint64_t)length) != 0) {
			pthread_mutex_unlock(&g_libLock);
			errno = EIO;
			return -1;
		}
	}

	rc = hfsMetaSetSize(g_fs, in, (uint64_t)length);
	pthread_mutex_unlock(&g_libLock);
	return rc;
}

/* Stat the file behind 'fd' (filled from the inode table). */
int hfs_lib_fstat(int fd, struct stat *st) {
	struct hfs_inode *in;

	pthread_mutex_lock(&g_libLock);
	if (g_fs == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = ENODEV;
		return -1;
	}
	if (fd < 0 || fd >= HFS_FD_TABLE_SIZE || g_fdTable[fd].file == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = EBADF;
		return -1;
	}

	in = hfsMetaFindById(g_fs, g_fdTable[fd].inodeId);
	pthread_mutex_unlock(&g_libLock);
	if (in == NULL) {
		errno = ENOENT;
		return -1;
	}

	memset(st, 0, sizeof(*st));
	st->st_ino = (ino_t)in->id;
	st->st_mode = (in->type == HFS_INODE_DIR) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
	st->st_size = (off_t)in->size;
	st->st_mtim.tv_sec = (time_t)(in->mtimeNs / 1000000000ULL);
	st->st_mtim.tv_nsec = (long)(in->mtimeNs % 1000000000ULL);
	st->st_ctim.tv_sec = (time_t)(in->ctimeNs / 1000000000ULL);
	st->st_ctim.tv_nsec = (long)(in->ctimeNs % 1000000000ULL);
	st->st_uid = getuid();
	st->st_gid = getgid();
	st->st_nlink = 1;
	st->st_blksize = 4096;
	return 0;
}

/* Stub: filled in by shard-17 (fsync handling). */
int hfs_lib_fsync(int fd) {
	errno = ENOSYS;
	return -1;
}

/*
 * Create a directory under the HFS mount prefix.
 * Uses resolveParent to locate the target's parent, then creates
 * the directory inode and persists the table.
 */
int hfs_lib_mkdir(const char *path, mode_t mode) {
	struct hfs_inode *parent;
	const char *leaf;
	int rc;

	pthread_mutex_lock(&g_libLock);
	if (g_fs == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = ENODEV;
		return -1;
	}

	rc = hfsMetaResolveParent(g_fs, path, &parent, &leaf);
	if (rc != 0) {
		pthread_mutex_unlock(&g_libLock);
		return -1;
	}

	rc = hfsMetaMkdir(g_fs, parent, leaf);
	pthread_mutex_unlock(&g_libLock);
	return rc;
}

/* Remove an empty directory under the HFS mount prefix. */
int hfs_lib_rmdir(const char *path) {
	int rc;

	pthread_mutex_lock(&g_libLock);
	if (g_fs == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = ENODEV;
		return -1;
	}

	rc = hfsMetaRmdir(g_fs, path);
	pthread_mutex_unlock(&g_libLock);
	return rc;
}

/* Rename a path under the HFS mount prefix (same or cross-directory). */
int hfs_lib_rename(const char *oldPath, const char *newPath) {
	int rc;

	pthread_mutex_lock(&g_libLock);
	if (g_fs == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = ENODEV;
		return -1;
	}

	rc = hfsMetaRename(g_fs, oldPath, newPath);
	pthread_mutex_unlock(&g_libLock);
	return rc;
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

/*
 * Stat a path under the HFS mount prefix; fields come from the inode.
 */
int hfs_lib_stat(const char *path, struct stat *st) {
	struct hfs_inode *in;

	pthread_mutex_lock(&g_libLock);
	if (g_fs == NULL) {
		pthread_mutex_unlock(&g_libLock);
		errno = ENODEV;
		return -1;
	}

	in = hfsMetaResolvePath(g_fs, path);
	pthread_mutex_unlock(&g_libLock);
	if (in == NULL) {
		return -1;
	}

	memset(st, 0, sizeof(*st));
	st->st_ino = (ino_t)in->id;
	st->st_mode = (in->type == HFS_INODE_DIR) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
	st->st_size = (off_t)in->size;
	st->st_mtim.tv_sec = (time_t)(in->mtimeNs / 1000000000ULL);
	st->st_mtim.tv_nsec = (long)(in->mtimeNs % 1000000000ULL);
	st->st_ctim.tv_sec = (time_t)(in->ctimeNs / 1000000000ULL);
	st->st_ctim.tv_nsec = (long)(in->ctimeNs % 1000000000ULL);
	st->st_uid = getuid();
	st->st_gid = getgid();
	st->st_nlink = 1;
	st->st_blksize = 4096;
	return 0;
}