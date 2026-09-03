#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/falloc.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "hfs.h"
#include "hfs_fd.h"
#include "hfs_internal.h"
#include "hfs_io.h"
#include "hfs_lib.h"
#include "hfs_log.h"
#include "hfs_meta.h"
#include "hfs_runtime.h"

/* Protects the mounted inode table and metadata snapshots. */
static pthread_rwlock_t g_metaLock = PTHREAD_RWLOCK_INITIALIZER;
/* Serializes append writes across independent open-file descriptions. */
static pthread_mutex_t g_appendLock = PTHREAD_MUTEX_INITIALIZER;

/* The single mounted filesystem instance (one per process). */
static Hfs *g_fs;
/* Exclusive host-side lock for the configured blobstore backend. */
static int g_backendLockFd = -1;

/* Return the configured backend name for lock naming and diagnostics. */
static const char *hfs_backend_name(void) {
	const char *name = getenv("HFS_BDEV_NAME");

	return (name != NULL && name[0] != '\0') ? name : "Malloc0";
}

/* Return the current realtime timestamp in nanoseconds for inode metadata. */
static uint64_t hfs_now_ns(void) {
	struct timespec ts;

	if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
		return 0;
	}
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Acquire an external lock so two processes cannot mount one backend. */
static int hfs_acquire_backend_lock(void) {
	const char *configured = getenv("HFS_LOCK_PATH");
	const char *backendName = hfs_backend_name();
	char defaultPath[PATH_MAX];
	char safeName[128];
	size_t i;
	size_t safeLen = 0;
	int fd;
	int savedErrno;

	if (configured != NULL && configured[0] != '\0') {
		if (snprintf(defaultPath, sizeof(defaultPath), "%s", configured)
		    >= (int)sizeof(defaultPath)) {
			errno = ENAMETOOLONG;
			return -1;
		}
	} else {
		for (i = 0; backendName[i] != '\0' && safeLen + 1 < sizeof(safeName); i++) {
			unsigned char ch = (unsigned char)backendName[i];
			safeName[safeLen++] = isalnum(ch) ? (char)ch : '_';
		}
		safeName[safeLen] = '\0';
		if (snprintf(defaultPath, sizeof(defaultPath), "/tmp/hfs-%s.lock",
			     safeName) >= (int)sizeof(defaultPath)) {
			errno = ENAMETOOLONG;
			return -1;
		}
	}

	fd = open(defaultPath, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
	if (fd < 0) {
		return -1;
	}
	if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
		savedErrno = errno;
		close(fd);
		errno = savedErrno == EWOULDBLOCK ? EBUSY : savedErrno;
		return -1;
	}

	g_backendLockFd = fd;
	return 0;
}

/* Release the backend lock after the blobstore is fully unmounted. */
static void hfs_release_backend_lock(void) {
	if (g_backendLockFd >= 0) {
		flock(g_backendLockFd, LOCK_UN);
		close(g_backendLockFd);
		g_backendLockFd = -1;
	}
}

/*
 * Create each directory in a relative path (mkdir -p semantics) inside
 * HFS. Used by HFS_AUTO_MKDIRS so single-process servers (mariadbd etc.)
 * get their datadir before they check for it.
 */
static void hfs_auto_mkdir_path(Hfs *fs, const char *rel) {
	char path[PATH_MAX];
	char *p;
	struct hfs_inode *parent;
	const char *leaf;

	if (rel == NULL || rel[0] == '\0') {
		return;
	}
	if (snprintf(path, sizeof(path), "/%s", rel) >= (int)sizeof(path)) {
		return;
	}

	p = path + 1;
	while (p != NULL && *p != '\0') {
		char *slash = strchr(p, '/');
		if (slash != NULL) {
			*slash = '\0';
		}
		if (hfsMetaResolvePath(fs, path) == NULL &&
		    hfsMetaResolveParent(fs, path, &parent, &leaf) == 0) {
			hfsMetaMkdir(fs, parent, leaf);
		}
		if (slash != NULL) {
			*slash = '/';
			p = slash + 1;
		} else {
			break;
		}
	}
}

/* Create all comma-separated paths from the HFS_AUTO_MKDIRS env var. */
static void hfs_auto_mkdirs(Hfs *fs) {
	const char *s = getenv("HFS_AUTO_MKDIRS");
	char *copy;
	char *tok;

	if (s == NULL || s[0] == '\0') {
		return;
	}
	copy = strdup(s);
	if (copy == NULL) {
		return;
	}
	for (tok = strtok(copy, ","); tok != NULL;
	     tok = strtok(NULL, ",")) {
		hfs_auto_mkdir_path(fs, tok);
	}
	free(copy);
}

/*
 * Directory enumeration state: a snapshot of child inodes taken at
 * opendir time, plus a read cursor.
 */
struct hfs_dir {
	uint64_t inodeId;	/* directory inode being listed */
	uint64_t count;		/* number of entries in the snapshot */
	uint64_t cursor;	/* current position in the snapshot */
	struct hfs_dirent *entries;	/* snapshot array */
	pthread_mutex_t lock;	/* protects the enumeration cursor */
};

/* Delete a deferred blob after every open description for its inode is gone. */
static void hfs_delete_orphan(HfsOrphan *orphan) {
	HfsFile file;

	if (orphan == NULL) {
		return;
	}
	memset(&file, 0, sizeof(file));
	file.fs = g_fs;
	file.blobId = orphan->blobId;
	if (g_fs != NULL && !hfsDeleteBlobById(&file)) {
		HFS_LOG_ERROR("deferred blob delete failed inode=%"PRIu64,
			      orphan->inodeId);
	}
	hfsFdOrphanDestroy(orphan);
}

/* Release an acquired description and close its blob at the final reference. */
static void hfs_release_open_file(HfsOpenFile *openFile) {
	HfsOrphan *readyOrphan = NULL;

	if (openFile == NULL || !hfsFdRelease(openFile, &readyOrphan)) {
		return;
	}
	if (openFile->file != NULL) {
		(void)hfsClose(openFile->file);
		free(openFile->file);
	}
	hfsFdDestroy(openFile);
	hfs_delete_orphan(readyOrphan);
}

/* Acquire an HFS description and enforce file-vs-directory operation rules. */
static HfsOpenFile *hfs_acquire_file(int fd, bool requireData) {
	HfsOpenFile *openFile = hfsFdAcquire(fd);

	if (openFile == NULL) {
		errno = EBADF;
		return NULL;
	}
	if (requireData && openFile->file == NULL) {
		hfs_release_open_file(openFile);
		errno = EISDIR;
		return NULL;
	}
	return openFile;
}

/* Validate an iovec array and return its total length without allocating. */
static int hfsIovTotal(const struct iovec *iov, int iovcnt, size_t *totalOut) {
	size_t total = 0;
	int i;

	if (iovcnt < 0 || (iovcnt != 0 && iov == NULL)) {
		errno = EINVAL;
		return -1;
	}
#ifdef IOV_MAX
	if (iovcnt > IOV_MAX) {
		errno = EINVAL;
		return -1;
	}
#endif
	for (i = 0; i < iovcnt; i++) {
		if (iov[i].iov_len != 0 && iov[i].iov_base == NULL) {
			errno = EFAULT;
			return -1;
		}
		if (iov[i].iov_len > SIZE_MAX - total) {
			errno = EOVERFLOW;
			return -1;
		}
		total += iov[i].iov_len;
	}
	if (total > (size_t)SSIZE_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	*totalOut = total;
	return 0;
}

/* Snapshot one inode size while keeping metadata lock scope short. */
static int hfsSnapshotFileSize(HfsOpenFile *openFile, uint64_t *sizeOut) {
	struct hfs_inode *in;
	uint64_t size;

	pthread_rwlock_rdlock(&g_metaLock);
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		errno = ENODEV;
		return -1;
	}
	in = hfsMetaFindById(g_fs, openFile->inodeId);
	if (in != NULL) {
		size = in->size;
	} else if (!hfsFdGetOrphanInfo(openFile, &size, NULL, NULL)) {
		pthread_rwlock_unlock(&g_metaLock);
		errno = ENOENT;
		return -1;
	}
	pthread_rwlock_unlock(&g_metaLock);
	*sizeOut = size;
	return 0;
}

/* Read iovec segments directly into caller buffers through request-local DMA. */
static ssize_t hfsReadvAt(HfsOpenFile *openFile, const struct iovec *iov,
			  int iovcnt, size_t total, uint64_t offset,
			  uint64_t fileSize) {
	size_t done = 0;
	int i;

	for (i = 0; i < iovcnt && done < total; i++) {
		size_t want = iov[i].iov_len;
		ssize_t n;

		if (want == 0) {
			continue;
		}
		if (offset >= fileSize) {
			break;
		}
		if ((uint64_t)want > fileSize - offset) {
			want = (size_t)(fileSize - offset);
		}
		n = hfsReadAt(openFile->file, iov[i].iov_base, want, offset);
		if (n < 0) {
			return done != 0 ? (ssize_t)done : -1;
		}
		done += (size_t)n;
		offset += (uint64_t)n;
		if ((size_t)n != want) {
			break;
		}
	}
	return (ssize_t)done;
}

/* Write iovec segments directly from caller buffers through request-local DMA. */
static ssize_t hfsWritevAt(HfsOpenFile *openFile, const struct iovec *iov,
			   int iovcnt, size_t total, uint64_t offset) {
	size_t done = 0;
	int i;

	for (i = 0; i < iovcnt && done < total; i++) {
		size_t want = iov[i].iov_len;
		ssize_t n;

		if (want == 0) {
			continue;
		}
		n = hfsWriteAt(openFile->file, iov[i].iov_base, want, offset);
		if (n < 0) {
			return done != 0 ? (ssize_t)done : -1;
		}
		done += (size_t)n;
		offset += (uint64_t)n;
		if ((size_t)n != want) {
			break;
		}
	}
	return (ssize_t)done;
}

/* Publish a successful write end for both linked and unlinked open files. */
static void hfsPublishWriteEnd(HfsOpenFile *openFile, uint64_t end) {
	struct hfs_inode *in;

	pthread_rwlock_wrlock(&g_metaLock);
	in = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
	if (in != NULL && end > in->size) {
		in->size = end;
		in->mtimeNs = hfs_now_ns();
		g_fs->metaDirty = true;
		atomic_store_explicit(&openFile->size, in->size,
				      memory_order_relaxed);
		atomic_store_explicit(&openFile->mtimeNs, in->mtimeNs,
				      memory_order_relaxed);
	} else if (in == NULL && hfsFdGetOrphanInfo(openFile, NULL, NULL, NULL)) {
		uint64_t size = atomic_load_explicit(&openFile->size,
					     memory_order_relaxed);

		if (end > size) {
			atomic_store_explicit(&openFile->size, end,
					      memory_order_relaxed);
		}
		atomic_store_explicit(&openFile->mtimeNs, hfs_now_ns(),
				      memory_order_relaxed);
	}
	pthread_rwlock_unlock(&g_metaLock);
}

/*
 * Initialize the library: logging, SPDK environment, mount and metadata.
 * Each step rolls back on failure. Idempotent: a second call returns 0
 * without re-initializing.
 */
int hfs_lib_init(void) {
	if (hfs_lib_is_fork_child()) {
		errno = ECHILD;
		return -1;
	}
	pthread_rwlock_wrlock(&g_metaLock);
	if (g_fs != NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		return 0;
	}

	if (hfs_log_init() != 0) {
		pthread_rwlock_unlock(&g_metaLock);
		return -1;
	}
	HFS_LOG_DEBUG("hfs_log_init done");

	if (hfs_acquire_backend_lock() != 0) {
		HFS_LOG_ERROR("backend lock failed for %s: %s", hfs_backend_name(),
			       strerror(errno));
		hfs_log_fini();
		pthread_rwlock_unlock(&g_metaLock);
		return -1;
	}

	if (hfsEnvSetup() != 0) {
		HFS_LOG_ERROR("hfsEnvSetup failed");
		hfsRuntimeStop();
		hfs_release_backend_lock();
		hfs_log_fini();
		pthread_rwlock_unlock(&g_metaLock);
		return -1;
	}

	g_fs = calloc(1, sizeof(Hfs));
	if (g_fs == NULL) {
		hfsRuntimeStop();
		hfs_release_backend_lock();
		hfs_log_fini();
		pthread_rwlock_unlock(&g_metaLock);
		return -1;
	}

	if (!hfsMount(g_fs)) {
		HFS_LOG_ERROR("hfsMount failed");
		free(g_fs);
		g_fs = NULL;
		hfsRuntimeStop();
		hfs_release_backend_lock();
		hfs_log_fini();
		pthread_rwlock_unlock(&g_metaLock);
		return -1;
	}

	if (hfsIoInit(g_fs) != 0) {
		HFS_LOG_ERROR("hfsIoInit failed");
		hfsUmount(g_fs);
		free(g_fs);
		g_fs = NULL;
		hfsRuntimeStop();
		hfs_release_backend_lock();
		hfs_log_fini();
		pthread_rwlock_unlock(&g_metaLock);
		return -1;
	}

	if (hfsMetaInit(g_fs) != 0) {
		HFS_LOG_ERROR("hfsMetaInit failed");
		hfsIoFini(g_fs);
		hfsUmount(g_fs);
		free(g_fs);
		g_fs = NULL;
		hfsRuntimeStop();
		hfs_release_backend_lock();
		hfs_log_fini();
		pthread_rwlock_unlock(&g_metaLock);
		return -1;
	}

	hfsFdReset();
	HFS_LOG_INFO("libhfs initialized");

	/* Auto-create directories requested via HFS_AUTO_MKDIRS. */
	hfs_auto_mkdirs(g_fs);

	/*
	 * Mark the environment so forked+exec'd children of this process
	 * skip SPDK re-initialization (a second primary conflicts with the
	 * parent's DPDK state and hangs). They run in pass-through mode.
	 */
	setenv("_HFS_INIT_DONE", "1", 0);
	pthread_rwlock_unlock(&g_metaLock);
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
	HfsOpenFile *openFile;
	int fd;

	/* Inherited pthread/SPDK state cannot be safely finalized after fork. */
	if (hfs_lib_is_fork_child()) {
		return;
	}
	pthread_rwlock_wrlock(&g_metaLock);
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		return;
	}
	/* Persist dirty inode sizes before closing the last open descriptions. */
	if (g_fs->metaDirty && hfsMetaSaveAll(g_fs) != 0) {
		HFS_LOG_ERROR("final metadata save failed");
	}
	/* Close every placeholder before unmounting the blobstore. */
	while ((openFile = hfsFdDetachAny(&fd)) != NULL) {
		hfs_release_open_file(openFile);
		(void)close(fd);
	}
	hfsFdReset();

	hfsIoFini(g_fs);
	if (!hfsUmount(g_fs)) {
		HFS_LOG_ERROR("hfsUmount failed");
	}
	free(g_fs);
	g_fs = NULL;
	hfsRuntimeStop();
	hfs_release_backend_lock();
	HFS_LOG_DEBUG("libhfs finalized");
	hfs_log_fini();
	pthread_rwlock_unlock(&g_metaLock);
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
	HfsOpenFile *openFile = NULL;
	int fd;
	int rc;
	bool created = false;

	(void)mode;

	pthread_rwlock_wrlock(&g_metaLock);
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		errno = ENODEV;
		return -1;
	}

	in = hfsMetaResolvePath(g_fs, path);
	if (in == NULL) {
		if (errno != ENOENT || !(flags & O_CREAT)) {
			pthread_rwlock_unlock(&g_metaLock);
			return -1;
		}
		/* Create the file and re-resolve its inode. */
		if (hfsMetaResolveParent(g_fs, path, &parent, &leaf) != 0) {
			pthread_rwlock_unlock(&g_metaLock);
			return -1;
		}
		rc = hfsMetaCreateFile(g_fs, parent, leaf);
		if (rc != 0) {
			pthread_rwlock_unlock(&g_metaLock);
			return -1;
		}
		created = true;
		in = hfsMetaFindByName(g_fs, parent->id, leaf);
		if (in == NULL) {
			pthread_rwlock_unlock(&g_metaLock);
			errno = ENOENT;
			return -1;
		}
	}
	if (!created && (flags & O_CREAT) != 0 && (flags & O_EXCL) != 0) {
		pthread_rwlock_unlock(&g_metaLock);
		errno = EEXIST;
		return -1;
	}
	if ((flags & O_DIRECTORY) != 0 && in->type != HFS_INODE_DIR) {
		pthread_rwlock_unlock(&g_metaLock);
		errno = ENOTDIR;
		return -1;
	}

	if (in->type == HFS_INODE_DIR) {
		/*
		 * Directory fd: only read-only opens are allowed (used for
		 * O_DIRECTORY probes and fsync on the datadir). There is no
		 * data blob; read/write on it fails with EISDIR later.
		 */
		if ((flags & O_ACCMODE) != O_RDONLY) {
			pthread_rwlock_unlock(&g_metaLock);
			errno = EISDIR;
			return -1;
		}
		fd = open("/dev/null", O_RDONLY);
		if (fd < 0 || fd >= HFS_FD_TABLE_SIZE) {
			if (fd >= 0) {
				close(fd);
			}
			pthread_rwlock_unlock(&g_metaLock);
			errno = EMFILE;
			return -1;
		}
		if (hfsFdCreate(&openFile, NULL, in->id, flags, false, true,
				path) != 0 || hfsFdRegister(fd, openFile) != 0) {
			if (openFile != NULL) {
				hfsFdDestroy(openFile);
			}
			close(fd);
			pthread_rwlock_unlock(&g_metaLock);
			return -1;
		}
		openFile->ctimeNs = in->ctimeNs;
		atomic_store_explicit(&openFile->mtimeNs, in->mtimeNs,
				      memory_order_relaxed);
		pthread_rwlock_unlock(&g_metaLock);
		return fd;
	}
	if (in->type != HFS_INODE_FILE) {
		pthread_rwlock_unlock(&g_metaLock);
		errno = EISDIR;
		return -1;
	}

	if ((flags & O_TRUNC) != 0 &&
	    (flags & O_ACCMODE) != O_RDONLY) {
		if (hfsMetaSetSize(g_fs, in, 0) != 0) {
			pthread_rwlock_unlock(&g_metaLock);
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
		pthread_rwlock_unlock(&g_metaLock);
		errno = EMFILE;
		return -1;
	}
	if (fd >= HFS_FD_TABLE_SIZE) {
		close(fd);
		pthread_rwlock_unlock(&g_metaLock);
		errno = EMFILE;
		return -1;
	}

	file = calloc(1, sizeof(HfsFile));
	if (file == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		errno = ENOMEM;
		return -1;
	}
	file->fs = g_fs;
	file->blobId = in->blobId;

	if (!hfsOpenById(file)) {
		close(fd);
		free(file);
		pthread_rwlock_unlock(&g_metaLock);
		errno = EIO;
		return -1;
	}
	if (hfsFdCreate(&openFile, file, in->id, flags,
			((flags & O_ACCMODE) != O_RDONLY), false, NULL) != 0 ||
	    hfsFdRegister(fd, openFile) != 0) {
		if (openFile != NULL) {
			hfsFdDestroy(openFile);
		}
		hfsClose(file);
		free(file);
		close(fd);
		pthread_rwlock_unlock(&g_metaLock);
		return -1;
	}
	atomic_store_explicit(&openFile->size, in->size, memory_order_relaxed);
	if ((flags & O_APPEND) != 0) {
		openFile->offset = in->size;
	}
	openFile->ctimeNs = in->ctimeNs;
	atomic_store_explicit(&openFile->mtimeNs, in->mtimeNs,
			      memory_order_relaxed);

	pthread_rwlock_unlock(&g_metaLock);
	return fd;
}

/*
 * Open a file relative to an HFS directory fd (openat semantics).
 * The relative name is resolved against the directory fd's path.
 */
int hfs_lib_openat(int dirfd, const char *path, int flags, mode_t mode) {
	HfsOpenFile *openFile;
	char buf[PATH_MAX];
	const char *vp;

	openFile = hfsFdAcquire(dirfd);
	if (openFile == NULL || !openFile->isDirectory) {
		if (openFile != NULL) {
			hfsFdRelease(openFile, NULL);
		}
		errno = openFile == NULL ? EBADF : ENOTDIR;
		return -1;
	}
	if (path[0] == '/') {
		vp = path;
	} else {
		if (snprintf(buf, sizeof(buf), "%s/%s", openFile->dirPath, path)
		    >= (int)sizeof(buf)) {
			hfsFdRelease(openFile, NULL);
			errno = ENAMETOOLONG;
			return -1;
		}
		vp = buf;
	}
	hfsFdRelease(openFile, NULL);
	return hfs_lib_open(vp, flags, mode);
}

/* Copy the inner path of an open HFS directory for fchdir/fdopendir. */
int hfs_lib_get_dir_path(int fd, char *pathOut, size_t pathSize) {
	HfsOpenFile *openFile;

	if (pathOut == NULL || pathSize == 0) {
		errno = EINVAL;
		return -1;
	}
	openFile = hfsFdAcquire(fd);
	if (openFile == NULL) {
		errno = EBADF;
		return -1;
	}
	if (!openFile->isDirectory) {
		hfs_release_open_file(openFile);
		errno = ENOTDIR;
		return -1;
	}
	if (snprintf(pathOut, pathSize, "%s", openFile->dirPath)
	    >= (int)pathSize) {
		hfs_release_open_file(openFile);
		errno = ENAMETOOLONG;
		return -1;
	}
	hfs_release_open_file(openFile);
	return 0;
}

/*
 * Read from the current offset of an HFS fd.
 * Returns 0 at end of file; updates the fd offset.
 */
ssize_t hfs_lib_read(int fd, void *buf, size_t count) {
	HfsOpenFile *openFile;
	struct hfs_inode *in;
	uint64_t fileSize;
	ssize_t n;

	if (count == 0) {
		return 0;
	}

	openFile = hfs_acquire_file(fd, true);
	if (openFile == NULL) {
		return -1;
	}
	pthread_mutex_lock(&openFile->ioLock);
	pthread_rwlock_rdlock(&g_metaLock);
	in = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = ENODEV;
		return -1;
	}
	if (in == NULL && !hfsFdGetOrphanInfo(openFile, &fileSize, NULL, NULL)) {
		pthread_rwlock_unlock(&g_metaLock);
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = ENOENT;
		return -1;
	}
	if (in != NULL) {
		fileSize = in->size;
	}
	if (openFile->offset >= fileSize) {
		pthread_rwlock_unlock(&g_metaLock);
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		return 0;	/* end of file */
	}
	if (openFile->offset + count > fileSize) {
		count = (size_t)(fileSize - openFile->offset);
	}
	pthread_rwlock_unlock(&g_metaLock);
	n = hfsReadAt(openFile->file, buf, count, openFile->offset);
	if (n > 0) {
		openFile->offset += (uint64_t)n;
	}
	pthread_mutex_unlock(&openFile->ioLock);
	hfs_release_open_file(openFile);
	return n;
}

/*
 * Write at the current offset of an HFS fd.
 * Grows the file size when writing past the end.
 */
ssize_t hfs_lib_write(int fd, const void *buf, size_t count) {
	HfsOpenFile *openFile;
	struct hfs_inode *in;
	uint64_t writeOffset;
	bool append;
	ssize_t n;
	int savedErrno;

	if (count == 0) {
		return 0;
	}

	openFile = hfs_acquire_file(fd, true);
	if (openFile == NULL) {
		return -1;
	}
	if (!openFile->writable) {
		hfs_release_open_file(openFile);
		errno = EBADF;
		return -1;
	}
	pthread_mutex_lock(&openFile->ioLock);
	append = (openFile->flags & O_APPEND) != 0;
	if (append) {
		/* Serialize EOF selection so independent append fds do not overlap. */
		pthread_mutex_lock(&g_appendLock);
	}
	writeOffset = openFile->offset;
	if (append) {
		uint64_t appendSize;

		pthread_rwlock_rdlock(&g_metaLock);
		in = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
		if (g_fs == NULL) {
			pthread_rwlock_unlock(&g_metaLock);
			pthread_mutex_unlock(&openFile->ioLock);
			pthread_mutex_unlock(&g_appendLock);
			hfs_release_open_file(openFile);
			errno = ENODEV;
			return -1;
		}
		if (in != NULL) {
			writeOffset = in->size;
		} else if (hfsFdGetOrphanInfo(openFile, &appendSize, NULL, NULL)) {
			writeOffset = appendSize;
		} else {
			pthread_rwlock_unlock(&g_metaLock);
			pthread_mutex_unlock(&openFile->ioLock);
			pthread_mutex_unlock(&g_appendLock);
			hfs_release_open_file(openFile);
			errno = ENOENT;
			return -1;
		}
		pthread_rwlock_unlock(&g_metaLock);
	}
	n = hfsWriteAt(openFile->file, buf, count, writeOffset);
	if (n <= 0) {
		savedErrno = errno;
		HFS_LOG_ERROR("sequential write failed n=%zd errno=%d", n, savedErrno);
		pthread_mutex_unlock(&openFile->ioLock);
		if (append) {
			pthread_mutex_unlock(&g_appendLock);
		}
		hfs_release_open_file(openFile);
		errno = n < 0 ? savedErrno : EIO;
		return -1;
	}
	openFile->offset = writeOffset + (uint64_t)n;
	pthread_rwlock_wrlock(&g_metaLock);
	in = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
	if (in != NULL && openFile->offset > in->size) {
		/* Update size/mtime in memory; persisted at fsync/close. */
		in->size = openFile->offset;
		in->mtimeNs = hfs_now_ns();
		g_fs->metaDirty = true;
		atomic_store_explicit(&openFile->size, in->size,
				      memory_order_relaxed);
		atomic_store_explicit(&openFile->mtimeNs, in->mtimeNs,
				      memory_order_relaxed);
	} else if (in == NULL && hfsFdGetOrphanInfo(openFile, NULL, NULL, NULL)) {
		uint64_t size = atomic_load_explicit(&openFile->size,
					     memory_order_relaxed);

		if (openFile->offset > size) {
			atomic_store_explicit(&openFile->size, openFile->offset,
					      memory_order_relaxed);
		}
		atomic_store_explicit(&openFile->mtimeNs, hfs_now_ns(),
				      memory_order_relaxed);
	}
	pthread_rwlock_unlock(&g_metaLock);
	pthread_mutex_unlock(&openFile->ioLock);
	if (append) {
		pthread_mutex_unlock(&g_appendLock);
	}
	hfs_release_open_file(openFile);
	return n;
}

/* Read at an explicit offset without moving the fd offset. */
ssize_t hfs_lib_pread(int fd, void *buf, size_t count, off_t offset) {
	HfsOpenFile *openFile;
	struct hfs_inode *in;
	uint64_t fileSize;
	ssize_t n;

	if (count == 0) {
		return 0;
	}
	if (offset < 0) {
		errno = EINVAL;
		return -1;
	}

	openFile = hfs_acquire_file(fd, true);
	if (openFile == NULL) {
		return -1;
	}
	pthread_rwlock_rdlock(&g_metaLock);
	in = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		hfs_release_open_file(openFile);
		errno = ENODEV;
		return -1;
	}
	if (in == NULL && !hfsFdGetOrphanInfo(openFile, &fileSize, NULL, NULL)) {
		pthread_rwlock_unlock(&g_metaLock);
		hfs_release_open_file(openFile);
		errno = ENOENT;
		return -1;
	}
	if (in != NULL) {
		fileSize = in->size;
	}
	if ((uint64_t)offset >= fileSize) {
		pthread_rwlock_unlock(&g_metaLock);
		hfs_release_open_file(openFile);
		return 0;
	}
	if ((uint64_t)offset + count > fileSize) {
		count = (size_t)(fileSize - (uint64_t)offset);
	}

	pthread_rwlock_unlock(&g_metaLock);
	n = hfsReadAt(openFile->file, buf, count, (uint64_t)offset);
	hfs_release_open_file(openFile);
	return n;
}

/* Write at an explicit offset without moving the fd offset. */
ssize_t hfs_lib_pwrite(int fd, const void *buf, size_t count, off_t offset) {
	HfsOpenFile *openFile;
	struct hfs_inode *in;
	uint64_t writeOffset;
	bool append;
	ssize_t n;
	int savedErrno;

	if (count == 0) {
		return 0;
	}
	if (offset < 0) {
		errno = EINVAL;
		return -1;
	}

	openFile = hfs_acquire_file(fd, true);
	if (openFile == NULL) {
		return -1;
	}
	if (!openFile->writable) {
		hfs_release_open_file(openFile);
		errno = EBADF;
		return -1;
	}
	append = (openFile->flags & O_APPEND) != 0;
	if (append) {
		/* Linux honors O_APPEND for pwrite; serialize the EOF decision. */
		pthread_mutex_lock(&g_appendLock);
	}
	writeOffset = (uint64_t)offset;
	if (append) {
		uint64_t appendSize;

		pthread_rwlock_rdlock(&g_metaLock);
		in = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
		if (g_fs == NULL) {
			pthread_rwlock_unlock(&g_metaLock);
			pthread_mutex_unlock(&g_appendLock);
			hfs_release_open_file(openFile);
			errno = ENODEV;
			return -1;
		}
		if (in != NULL) {
			writeOffset = in->size;
		} else if (hfsFdGetOrphanInfo(openFile, &appendSize, NULL, NULL)) {
			writeOffset = appendSize;
		} else {
			pthread_rwlock_unlock(&g_metaLock);
			pthread_mutex_unlock(&g_appendLock);
			hfs_release_open_file(openFile);
			errno = ENOENT;
			return -1;
		}
		pthread_rwlock_unlock(&g_metaLock);
	}
	n = hfsWriteAt(openFile->file, buf, count, writeOffset);
	if (n <= 0) {
		savedErrno = errno;
		HFS_LOG_ERROR("positioned write failed n=%zd errno=%d", n, savedErrno);
		if (append) {
			pthread_mutex_unlock(&g_appendLock);
		}
		hfs_release_open_file(openFile);
		errno = n < 0 ? savedErrno : EIO;
		return -1;
	}
	pthread_rwlock_wrlock(&g_metaLock);
	in = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
	if (in != NULL && writeOffset + (uint64_t)n > in->size) {
		in->size = writeOffset + (uint64_t)n;
		in->mtimeNs = hfs_now_ns();
		g_fs->metaDirty = true;
		atomic_store_explicit(&openFile->size, in->size,
				      memory_order_relaxed);
		atomic_store_explicit(&openFile->mtimeNs, in->mtimeNs,
				      memory_order_relaxed);
	} else if (in == NULL && hfsFdGetOrphanInfo(openFile, NULL, NULL, NULL)) {
		uint64_t size = atomic_load_explicit(&openFile->size,
					     memory_order_relaxed);
		uint64_t end = writeOffset + (uint64_t)n;

		if (end > size) {
			atomic_store_explicit(&openFile->size, end,
					      memory_order_relaxed);
		}
		atomic_store_explicit(&openFile->mtimeNs, hfs_now_ns(),
				      memory_order_relaxed);
	}
	pthread_rwlock_unlock(&g_metaLock);
	if (append) {
		pthread_mutex_unlock(&g_appendLock);
	}
	hfs_release_open_file(openFile);
	return n;
}

/* Read an iovec array from the current file offset without a large temporary buffer. */
ssize_t hfs_lib_readv(int fd, const struct iovec *iov, int iovcnt) {
	HfsOpenFile *openFile;
	uint64_t fileSize;
	ssize_t n;
	size_t total;

	if (hfsIovTotal(iov, iovcnt, &total) != 0) {
		return -1;
	}
	if (total == 0) {
		return 0;
	}
	openFile = hfs_acquire_file(fd, true);
	if (openFile == NULL) {
		return -1;
	}
	pthread_mutex_lock(&openFile->ioLock);
	if (hfsSnapshotFileSize(openFile, &fileSize) != 0) {
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		return -1;
	}
	if (openFile->offset >= fileSize) {
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		return 0;
	}
	n = hfsReadvAt(openFile, iov, iovcnt, total, openFile->offset,
		       fileSize);
	if (n > 0) {
		openFile->offset += (uint64_t)n;
	}
	pthread_mutex_unlock(&openFile->ioLock);
	hfs_release_open_file(openFile);
	return n;
}

/* Write an iovec array at the current file offset without a large temporary buffer. */
ssize_t hfs_lib_writev(int fd, const struct iovec *iov, int iovcnt) {
	HfsOpenFile *openFile;
	uint64_t writeOffset;
	ssize_t n;
	size_t total;
	bool append;

	if (hfsIovTotal(iov, iovcnt, &total) != 0) {
		return -1;
	}
	if (total == 0) {
		return 0;
	}
	openFile = hfs_acquire_file(fd, true);
	if (openFile == NULL) {
		return -1;
	}
	if (!openFile->writable) {
		hfs_release_open_file(openFile);
		errno = EBADF;
		return -1;
	}
	pthread_mutex_lock(&openFile->ioLock);
	append = (openFile->flags & O_APPEND) != 0;
	if (append) {
		pthread_mutex_lock(&g_appendLock);
	}
	writeOffset = openFile->offset;
	if (append && hfsSnapshotFileSize(openFile, &writeOffset) != 0) {
		if (append) {
			pthread_mutex_unlock(&g_appendLock);
		}
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		return -1;
	}
	n = hfsWritevAt(openFile, iov, iovcnt, total, writeOffset);
	if (n > 0) {
		openFile->offset = writeOffset + (uint64_t)n;
		hfsPublishWriteEnd(openFile, openFile->offset);
	}
	if (append) {
		pthread_mutex_unlock(&g_appendLock);
	}
	pthread_mutex_unlock(&openFile->ioLock);
	hfs_release_open_file(openFile);
	return n;
}

/* Read an iovec array at an explicit offset without moving the file offset. */
ssize_t hfs_lib_preadv(int fd, const struct iovec *iov, int iovcnt,
		       off_t offset) {
	HfsOpenFile *openFile;
	uint64_t fileSize;
	ssize_t n;
	size_t total;

	if (offset < 0 || hfsIovTotal(iov, iovcnt, &total) != 0) {
		if (offset < 0) {
			errno = EINVAL;
		}
		return -1;
	}
	if (total == 0) {
		return 0;
	}
	openFile = hfs_acquire_file(fd, true);
	if (openFile == NULL) {
		return -1;
	}
	if (hfsSnapshotFileSize(openFile, &fileSize) != 0) {
		hfs_release_open_file(openFile);
		return -1;
	}
	if ((uint64_t)offset >= fileSize) {
		hfs_release_open_file(openFile);
		return 0;
	}
	n = hfsReadvAt(openFile, iov, iovcnt, total, (uint64_t)offset,
		       fileSize);
	hfs_release_open_file(openFile);
	return n;
}

/* Write an iovec array at an explicit offset without moving the file offset. */
ssize_t hfs_lib_pwritev(int fd, const struct iovec *iov, int iovcnt,
			 off_t offset) {
	HfsOpenFile *openFile;
	uint64_t writeOffset;
	ssize_t n;
	size_t total;
	bool append;

	if (offset < 0 || hfsIovTotal(iov, iovcnt, &total) != 0) {
		if (offset < 0) {
			errno = EINVAL;
		}
		return -1;
	}
	if (total == 0) {
		return 0;
	}
	openFile = hfs_acquire_file(fd, true);
	if (openFile == NULL) {
		return -1;
	}
	if (!openFile->writable) {
		hfs_release_open_file(openFile);
		errno = EBADF;
		return -1;
	}
	append = (openFile->flags & O_APPEND) != 0;
	if (append) {
		pthread_mutex_lock(&g_appendLock);
	}
	writeOffset = (uint64_t)offset;
	if (append && hfsSnapshotFileSize(openFile, &writeOffset) != 0) {
		if (append) {
			pthread_mutex_unlock(&g_appendLock);
		}
		hfs_release_open_file(openFile);
		return -1;
	}
	n = hfsWritevAt(openFile, iov, iovcnt, total, writeOffset);
	if (n > 0) {
		hfsPublishWriteEnd(openFile, writeOffset + (uint64_t)n);
	}
	if (append) {
		pthread_mutex_unlock(&g_appendLock);
	}
	hfs_release_open_file(openFile);
	return n;
}

/* Change the current offset of an HFS fd (SEEK_SET/CUR/END). */
off_t hfs_lib_lseek(int fd, off_t offset, int whence) {
	HfsOpenFile *openFile;
	struct hfs_inode *in;
	uint64_t base;

	openFile = hfs_acquire_file(fd, true);
	if (openFile == NULL) {
		return -1;
	}
	pthread_mutex_lock(&openFile->ioLock);
	pthread_rwlock_rdlock(&g_metaLock);
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = ENODEV;
		return -1;
	}
	switch (whence) {
	case SEEK_SET:
		base = 0;
		break;
	case SEEK_CUR:
		base = openFile->offset;
		break;
	case SEEK_END:
		in = hfsMetaFindById(g_fs, openFile->inodeId);
		if (in != NULL) {
			base = in->size;
		} else if (!hfsFdGetOrphanInfo(openFile, &base, NULL, NULL)) {
			pthread_rwlock_unlock(&g_metaLock);
			pthread_mutex_unlock(&openFile->ioLock);
			hfs_release_open_file(openFile);
			errno = ENOENT;
			return -1;
		}
		break;
	default:
		pthread_rwlock_unlock(&g_metaLock);
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = EINVAL;
		return -1;
	}

	if (offset < 0 && (uint64_t)(-(offset + 1)) + 1 > base) {
		pthread_rwlock_unlock(&g_metaLock);
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = EINVAL;
		return -1;
	}

	openFile->offset = offset < 0 ? base - (uint64_t)(-offset) :
				       base + (uint64_t)offset;
	offset = (off_t)openFile->offset;
	pthread_rwlock_unlock(&g_metaLock);
	pthread_mutex_unlock(&openFile->ioLock);
	hfs_release_open_file(openFile);
	return offset;
}

/* Copy the HFS fd table slot to another fd (dup/dup2 interceptor helper). */
int hfs_lib_fd_copy(int oldfd, int newfd) {
	HfsOpenFile *replaced = NULL;
	int rc = hfsFdCopy(oldfd, newfd, &replaced);

	if (replaced != NULL) {
		hfs_release_open_file(replaced);
	}
	return rc;
}

/*
 * Returns true when the fd belongs to HFS (interception helper).
 * The fd registry uses a short read lock and never takes the metadata lock, so
 * close() can classify internal descriptors while initialization is active.
 */
bool hfs_is_hfs_fd(int fd) {
	if (hfs_lib_is_fork_child()) {
		return false;
	}
	return hfsFdIsHfs(fd);
}

/* Return true when the process inherited the active runtime across fork. */
bool hfs_lib_is_fork_child(void) {
	return hfsRuntimeIsForkChild();
}

/*
 * Close an HFS fd: drop one reference; close the blob handle and free
 * the file only when the last reference (dup shares) goes away.
 */
int hfs_lib_close(int fd) {
	HfsOpenFile *openFile = hfsFdDetach(fd);

	if (openFile == NULL) {
		errno = EBADF;
		return -1;
	}
	hfs_release_open_file(openFile);

	/* Release the kernel placeholder fd. The table slot is already
	 * cleared, so the close interceptor passes this through. */
	if (close(fd) != 0) {
		return -1;
	}
	return 0;
}

/*
 * Remove a file under the HFS mount prefix: drop the inode, then
 * delete its data blob and persist the table.
 */
int hfs_lib_unlink(const char *path) {
	struct hfs_inode *parent;
	struct hfs_inode *in;
	const char *leaf;
	uint64_t blobId;
	HfsFile file;
	HfsOrphan *orphan = NULL;
	int rc;

	pthread_rwlock_wrlock(&g_metaLock);
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		errno = ENODEV;
		return -1;
	}

	rc = hfsMetaResolveParent(g_fs, path, &parent, &leaf);
	if (rc != 0) {
		pthread_rwlock_unlock(&g_metaLock);
		return -1;
	}
	in = hfsMetaFindByName(g_fs, parent->id, leaf);
	if (in == NULL || in->type != HFS_INODE_FILE) {
		pthread_rwlock_unlock(&g_metaLock);
		errno = in == NULL ? ENOENT : EISDIR;
		return -1;
	}
	blobId = in->blobId;
	if (hfsFdMarkOrphan(in->id, blobId, in->size, in->ctimeNs,
			   in->mtimeNs, &orphan) != 0) {
		pthread_rwlock_unlock(&g_metaLock);
		return -1;
	}

	rc = hfsMetaRemoveFile(g_fs, parent, leaf, &blobId);
	if (rc != 0) {
		pthread_rwlock_unlock(&g_metaLock);
		return -1;
	}

	/* Delete the data blob via the waiter chain. */
	if (orphan == NULL) {
		memset(&file, 0, sizeof(file));
		file.fs = g_fs;
		file.blobId = blobId;
		if (!hfsDeleteBlobById(&file)) {
			pthread_rwlock_unlock(&g_metaLock);
			errno = EIO;
			return -1;
		}
	}

	pthread_rwlock_unlock(&g_metaLock);
	return 0;
}

/*
 * Truncate the file behind 'fd' to 'length'.
 * Updates the inode size and grows the data blob as needed.
 */
int hfs_lib_ftruncate(int fd, off_t length) {
	HfsOpenFile *openFile;
	struct hfs_inode *in;
	uint64_t currentSize;
	int rc;

	if (length < 0) {
		errno = EINVAL;
		return -1;
	}

	openFile = hfs_acquire_file(fd, true);
	if (openFile == NULL) {
		return -1;
	}
	if (!openFile->writable) {
		hfs_release_open_file(openFile);
		errno = EBADF;
		return -1;
	}
	pthread_mutex_lock(&openFile->ioLock);
	pthread_rwlock_rdlock(&g_metaLock);
	in = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = ENODEV;
		return -1;
	}
	if (in != NULL) {
		currentSize = in->size;
	} else if (!hfsFdGetOrphanInfo(openFile, &currentSize, NULL, NULL)) {
		pthread_rwlock_unlock(&g_metaLock);
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = ENOENT;
		return -1;
	}

	pthread_rwlock_unlock(&g_metaLock);

	/* Grow the blob first so the new size is backed by storage. */
	if (length > (off_t)currentSize) {
		if (hfsResizeTo(openFile->file, (uint64_t)length) != 0) {
			pthread_mutex_unlock(&openFile->ioLock);
			hfs_release_open_file(openFile);
			errno = EIO;
			return -1;
		}
	}

	/* Upgrade to a writer only for the stable inode update. */
	pthread_rwlock_wrlock(&g_metaLock);
	in = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = ENODEV;
		return -1;
	}
	if (in != NULL) {
		rc = hfsMetaSetSize(g_fs, in, (uint64_t)length);
		if (rc == 0) {
			atomic_store_explicit(&openFile->size, (uint64_t)length,
					      memory_order_relaxed);
			atomic_store_explicit(&openFile->mtimeNs, in->mtimeNs,
					      memory_order_relaxed);
		}
	} else if (hfsFdGetOrphanInfo(openFile, NULL, NULL, NULL)) {
		atomic_store_explicit(&openFile->size, (uint64_t)length,
				      memory_order_relaxed);
		atomic_store_explicit(&openFile->mtimeNs, hfs_now_ns(),
				      memory_order_relaxed);
		rc = 0;
	} else {
		pthread_rwlock_unlock(&g_metaLock);
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = ENOENT;
		return -1;
	}
	pthread_rwlock_unlock(&g_metaLock);
	pthread_mutex_unlock(&openFile->ioLock);
	hfs_release_open_file(openFile);
	return rc;
}

/* Stat the file behind 'fd' (filled from the inode table). */
int hfs_lib_fstat(int fd, struct stat *st) {
	struct hfs_inode *in;
	HfsOpenFile *openFile;
	struct hfs_inode snapshot;
	uint64_t orphanSize;
	uint64_t orphanCtimeNs;
	uint64_t orphanMtimeNs;

	openFile = hfsFdAcquire(fd);
	if (openFile == NULL) {
		errno = EBADF;
		return -1;
	}
	pthread_rwlock_rdlock(&g_metaLock);
	in = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
	if (in == NULL && !hfsFdGetOrphanInfo(openFile, &orphanSize,
					     &orphanCtimeNs, &orphanMtimeNs)) {
		pthread_rwlock_unlock(&g_metaLock);
		hfs_release_open_file(openFile);
		errno = g_fs == NULL ? ENODEV : ENOENT;
		return -1;
	}
	if (in != NULL) {
		snapshot = *in;
	} else {
		memset(&snapshot, 0, sizeof(snapshot));
		snapshot.id = openFile->inodeId;
		snapshot.type = HFS_INODE_FILE;
		snapshot.size = orphanSize;
		snapshot.ctimeNs = orphanCtimeNs;
		snapshot.mtimeNs = orphanMtimeNs;
	}
	pthread_rwlock_unlock(&g_metaLock);
	hfs_release_open_file(openFile);
	in = &snapshot;
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

/*
 * Check access to a path under the HFS mount prefix. HFS has no
 * permission model, so any existing path passes R/W/X; missing paths
 * fail with ENOENT.
 */
int hfs_lib_access(const char *path, int mode) {
	struct hfs_inode *in;

	pthread_rwlock_rdlock(&g_metaLock);
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		errno = ENODEV;
		return -1;
	}

	in = hfsMetaResolvePath(g_fs, path);
	if (in == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		return -1;	/* errno set by the resolver */
	}
	pthread_rwlock_unlock(&g_metaLock);
	return 0;
}

/*
 * fcntl for HFS fds: fd flags and file-status flags. Duplication
 * (F_DUPFD/F_DUPFD_CLOEXEC) is handled by the interceptor, which needs
 * the real fcntl to reserve a kernel fd.
 */
int hfs_lib_fcntl(int fd, int cmd, long arg) {
	HfsOpenFile *openFile = hfsFdAcquire(fd);

	if (openFile == NULL) {
		errno = EBADF;
		return -1;
	}
	pthread_mutex_lock(&openFile->ioLock);
	switch (cmd) {
	case F_GETFD:
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		return 0;
	case F_SETFD:
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		return 0;
	case F_GETFL:
		arg = openFile->flags;
		break;
	case F_SETFL:
		openFile->flags = (int)arg;
		break;
	case F_SETLK:
	case F_SETLKW:
		/* No cross-process locking in HFS: accept the lock. */
		arg = 0;
		break;
	case F_GETLK:
		/* Report the lock as free. */
		if (arg != 0) {
			memset((struct flock *)(uintptr_t)arg, 0,
			       sizeof(struct flock));
		}
		arg = 0;
		break;
	default:
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_unlock(&openFile->ioLock);
	hfs_release_open_file(openFile);
	return (cmd == F_GETFL) ? (int)arg : 0;
}

/*
 * Flush an HFS fd: persist written data and the dirty inode size/mtime
 * so both survive an abrupt process exit.
 */
int hfs_lib_fsync(int fd) {
	HfsOpenFile *openFile = hfsFdAcquire(fd);
	bool ioLocked = false;

	if (openFile == NULL) {
		errno = EBADF;
		return -1;
	}
	pthread_mutex_lock(&openFile->ioLock);
	if (openFile->file != NULL) {
		pthread_rwlock_wrlock(&openFile->file->fs->ioLock);
		ioLocked = true;
	}
	if (openFile->file != NULL && hfsSync(openFile->file) != 0) {
		if (ioLocked) {
			pthread_rwlock_unlock(&openFile->file->fs->ioLock);
		}
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		return -1;
	}
	pthread_rwlock_wrlock(&g_metaLock);
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		if (ioLocked) {
			pthread_rwlock_unlock(&openFile->file->fs->ioLock);
		}
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = ENODEV;
		return -1;
	}
	if (!g_fs->metaDirty) {
		pthread_rwlock_unlock(&g_metaLock);
		if (ioLocked) {
			pthread_rwlock_unlock(&openFile->file->fs->ioLock);
		}
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		return 0;
	}
	if (hfsMetaFindById(g_fs, openFile->inodeId) != NULL &&
	    hfsMetaCommitInode(g_fs, openFile->inodeId) != 0) {
		pthread_rwlock_unlock(&g_metaLock);
		if (ioLocked) {
			pthread_rwlock_unlock(&openFile->file->fs->ioLock);
		}
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = EIO;
		return -1;
	}
	pthread_rwlock_unlock(&g_metaLock);
	if (ioLocked) {
		pthread_rwlock_unlock(&openFile->file->fs->ioLock);
	}
	pthread_mutex_unlock(&openFile->ioLock);
	hfs_release_open_file(openFile);
	return 0;
}

/* Preallocate or punch a range while preserving Linux fallocate semantics. */
int hfs_lib_fallocate(int fd, int mode, off_t offset, off_t len) {
	HfsOpenFile *openFile;
	uint64_t fileSize;
	uint64_t end;
	uint64_t punchLength;
	int punchMode = FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE;

	if (offset < 0 || len < 0) {
		errno = EINVAL;
		return -1;
	}
	if (mode != 0 && mode != FALLOC_FL_KEEP_SIZE && mode != punchMode) {
		errno = EOPNOTSUPP;
		return -1;
	}
	if ((uint64_t)offset > UINT64_MAX - (uint64_t)len) {
		errno = EOVERFLOW;
		return -1;
	}
	end = (uint64_t)offset + (uint64_t)len;
	if (mode == 0) {
		if (end > (uint64_t)INT64_MAX) {
			errno = EOVERFLOW;
			return -1;
		}
		openFile = hfs_acquire_file(fd, true);
		if (openFile == NULL) {
			return -1;
		}
		if (hfsSnapshotFileSize(openFile, &fileSize) != 0) {
			hfs_release_open_file(openFile);
			return -1;
		}
		hfs_release_open_file(openFile);
		/* Fallocate must not shrink a file when the range is already covered. */
		if (end <= fileSize) {
			return 0;
		}
		return hfs_lib_ftruncate(fd, (off_t)end);
	}

	openFile = hfs_acquire_file(fd, true);
	if (openFile == NULL) {
		return -1;
	}
	if (!openFile->writable) {
		hfs_release_open_file(openFile);
		errno = EBADF;
		return -1;
	}
	if (mode == punchMode) {
		pthread_mutex_lock(&openFile->ioLock);
		if (hfsSnapshotFileSize(openFile, &fileSize) != 0) {
			pthread_mutex_unlock(&openFile->ioLock);
			hfs_release_open_file(openFile);
			return -1;
		}
		if ((uint64_t)offset >= fileSize) {
			pthread_mutex_unlock(&openFile->ioLock);
			hfs_release_open_file(openFile);
			return 0;
		}
		punchLength = end - (uint64_t)offset;
		if (punchLength > fileSize - (uint64_t)offset) {
			punchLength = fileSize - (uint64_t)offset;
		}
		if (hfsIoPunchHole(openFile->file, (uint64_t)offset,
				   punchLength) != 0) {
			pthread_mutex_unlock(&openFile->ioLock);
			hfs_release_open_file(openFile);
			return -1;
		}
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		return 0;
	}

	/* KEEP_SIZE reserves backing clusters but does not change inode size. */
	pthread_mutex_lock(&openFile->ioLock);
	pthread_rwlock_wrlock(&openFile->file->fs->ioLock);
	if (hfsResizeTo(openFile->file, end) != 0) {
		pthread_rwlock_unlock(&openFile->file->fs->ioLock);
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		return -1;
	}
	pthread_rwlock_unlock(&openFile->file->fs->ioLock);
	pthread_mutex_unlock(&openFile->ioLock);
	hfs_release_open_file(openFile);
	return 0;
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

	pthread_rwlock_wrlock(&g_metaLock);
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		errno = ENODEV;
		return -1;
	}
	if (path != NULL && strcmp(path, "/") == 0) {
		/* The virtual HFS root already exists as the mount point. */
		pthread_rwlock_unlock(&g_metaLock);
		errno = EEXIST;
		return -1;
	}

	rc = hfsMetaResolveParent(g_fs, path, &parent, &leaf);
	if (rc != 0) {
		pthread_rwlock_unlock(&g_metaLock);
		return -1;
	}

	rc = hfsMetaMkdir(g_fs, parent, leaf);
	pthread_rwlock_unlock(&g_metaLock);
	return rc;
}

/* Remove an empty directory under the HFS mount prefix. */
int hfs_lib_rmdir(const char *path) {
	int rc;

	pthread_rwlock_wrlock(&g_metaLock);
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		errno = ENODEV;
		return -1;
	}

	rc = hfsMetaRmdir(g_fs, path);
	pthread_rwlock_unlock(&g_metaLock);
	return rc;
}

/* Rename a path under the HFS mount prefix (same or cross-directory). */
int hfs_lib_rename(const char *oldPath, const char *newPath) {
	struct hfs_inode *src;
	struct hfs_inode *dst;
	uint64_t replacedId = 0;
	uint64_t replacedBlobId = 0;
	uint64_t replacedSize = 0;
	uint64_t replacedCtimeNs = 0;
	uint64_t replacedMtimeNs = 0;
	HfsOrphan *orphan = NULL;
	int rc;

	pthread_rwlock_wrlock(&g_metaLock);
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		errno = ENODEV;
		return -1;
	}

	/* Save destination identity before the metadata table may compact. */
	src = hfsMetaResolvePath(g_fs, oldPath);
	dst = hfsMetaResolvePath(g_fs, newPath);
	if (dst != NULL && dst->type == HFS_INODE_FILE &&
	    (src == NULL || src->id != dst->id)) {
		replacedId = dst->id;
		replacedBlobId = dst->blobId;
		replacedSize = dst->size;
		replacedCtimeNs = dst->ctimeNs;
		replacedMtimeNs = dst->mtimeNs;
	}
	rc = hfsMetaRename(g_fs, oldPath, newPath);
	if (rc == 0 && replacedId != 0) {
		if (hfsFdMarkOrphan(replacedId, replacedBlobId, replacedSize,
				    replacedCtimeNs, replacedMtimeNs,
				    &orphan) == 0 && orphan == NULL) {
			HfsFile file;

			memset(&file, 0, sizeof(file));
			file.fs = g_fs;
			file.blobId = replacedBlobId;
			if (!hfsDeleteBlobById(&file)) {
				HFS_LOG_ERROR("rename target blob delete failed inode=%"PRIu64,
					      replacedId);
			}
		}
	}
	pthread_rwlock_unlock(&g_metaLock);
	return rc;
}

/*
 * Open a directory under the HFS mount prefix: resolve the inode,
 * snapshot its children (name + type) into the hfs_dir state, and
 * hand it back for readdir iteration.
 */
struct hfs_dir *hfs_lib_opendir(const char *path) {
	struct hfs_inode *in;
	struct hfs_dir *dir;
	int i;
	uint64_t n = 0;

	pthread_rwlock_rdlock(&g_metaLock);
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		errno = ENODEV;
		return NULL;
	}

	in = hfsMetaResolvePath(g_fs, path);
	if (in == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		return NULL;	/* errno set by the resolver */
	}
	if (in->type != HFS_INODE_DIR) {
		pthread_rwlock_unlock(&g_metaLock);
		errno = ENOTDIR;
		return NULL;
	}

	dir = calloc(1, sizeof(*dir));
	if (dir == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		errno = ENOMEM;
		return NULL;
	}
	if (pthread_mutex_init(&dir->lock, NULL) != 0) {
		free(dir);
		pthread_rwlock_unlock(&g_metaLock);
		errno = ENOMEM;
		return NULL;
	}
	dir->inodeId = in->id;

	for (i = 0; i < g_fs->inodeCount; i++) {
		if (g_fs->inodes[i].parentId == in->id) {
			n++;
		}
	}
	dir->entries = calloc(n > 0 ? n : 1, sizeof(struct hfs_dirent));
	if (dir->entries == NULL) {
		pthread_mutex_destroy(&dir->lock);
		free(dir);
		pthread_rwlock_unlock(&g_metaLock);
		errno = ENOMEM;
		return NULL;
	}

	for (i = 0; i < g_fs->inodeCount; i++) {
		if (g_fs->inodes[i].parentId == in->id) {
			struct hfs_dirent *e = &dir->entries[dir->count];
			strncpy(e->name, g_fs->inodes[i].name,
				sizeof(e->name) - 1);
			e->name[sizeof(e->name) - 1] = '\0';
			e->inodeId = g_fs->inodes[i].id;
			e->d_type = (g_fs->inodes[i].type == HFS_INODE_DIR)
				     ? DT_DIR : DT_REG;
			dir->count++;
		}
	}
	pthread_rwlock_unlock(&g_metaLock);
	return dir;
}

/* Return the next snapshot entry, or NULL when the listing is done. */
struct hfs_dirent *hfs_lib_readdir(struct hfs_dir *dir) {
	struct hfs_dirent *entry = NULL;

	if (dir == NULL) {
		errno = EBADF;
		return NULL;
	}
	pthread_mutex_lock(&dir->lock);
	if (dir->cursor < dir->count) {
		entry = &dir->entries[dir->cursor++];
	}
	pthread_mutex_unlock(&dir->lock);
	return entry;
}

/* Release the directory snapshot state. */
int hfs_lib_closedir(struct hfs_dir *dir) {
	if (dir == NULL) {
		errno = EBADF;
		return -1;
	}
	pthread_mutex_destroy(&dir->lock);
	free(dir->entries);
	free(dir);
	return 0;
}

/*
 * Stat a path under the HFS mount prefix; fields come from the inode.
 */
int hfs_lib_stat(const char *path, struct stat *st) {
	struct hfs_inode *in;
	struct hfs_inode snapshot;

	pthread_rwlock_rdlock(&g_metaLock);
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		errno = ENODEV;
		return -1;
	}

	in = hfsMetaResolvePath(g_fs, path);
	if (in == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		return -1;
	}
	snapshot = *in;
	pthread_rwlock_unlock(&g_metaLock);
	in = &snapshot;

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
