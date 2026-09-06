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

#include <spdk/thread.h>

#include "hfs.h"
#include "hfs_durability.h"
#include "hfs_fd.h"
#include "hfs_async.h"
#include "hfs_internal.h"
#include "hfs_inode_runtime.h"
#include "hfs_io.h"
#include "hfs_lib.h"
#include "hfs_log.h"
#include "hfs_meta.h"
#include "hfs_runtime.h"
#include "hfs_stats.h"

/* Protects the mounted inode table and metadata snapshots. */
static pthread_rwlock_t g_metaLock = PTHREAD_RWLOCK_INITIALIZER;
/* Protects final descriptions whose SPDK close or orphan delete must retry. */
static pthread_mutex_t g_deferredCloseLock = PTHREAD_MUTEX_INITIALIZER;

/* Count metadata read-lock acquisitions without extending lock ownership. */
static void hfsMetaReadLock(void) {
	HFS_STATS_META_READ_LOCK();
	hfsStatsLockBeforeFast(HFS_STATS_LOCK_META);
	pthread_rwlock_rdlock(&g_metaLock);
	hfsStatsLockAfterFast(HFS_STATS_LOCK_META);
}

/* Count metadata write-lock acquisitions without extending lock ownership. */
static void hfsMetaWriteLock(void) {
	HFS_STATS_META_WRITE_LOCK();
	hfsStatsLockBeforeFast(HFS_STATS_LOCK_META);
	pthread_rwlock_wrlock(&g_metaLock);
	hfsStatsLockAfterFast(HFS_STATS_LOCK_META);
}

/* Classify a rwlock release so metadata and data-path hold times stay separate. */
static HfsStatsLockClass hfsLibRwlockClass(pthread_rwlock_t *lock) {
	return lock == &g_metaLock ? HFS_STATS_LOCK_META : HFS_STATS_LOCK_IO;
}

/* Instrument write rwlock acquisition without changing lock ownership. */
static int hfsLibRwlockWrlock(pthread_rwlock_t *lock) {
	HfsStatsLockClass lockClass = hfsLibRwlockClass(lock);
	int rc;

	hfsStatsLockBeforeFast(lockClass);
	rc = pthread_rwlock_wrlock(lock);
	hfsStatsLockAfterFast(lockClass);
	return rc;
}

/* Release a rwlock and account for the protected section duration. */
static int hfsLibRwlockUnlock(pthread_rwlock_t *lock) {
	HfsStatsLockClass lockClass = hfsLibRwlockClass(lock);

	hfsStatsLockReleaseFast(lockClass);
	return pthread_rwlock_unlock(lock);
}

/* Instrument process mutexes that remain outside per-inode data locks. */
static HfsStatsLockClass hfsLibMutexClass(pthread_mutex_t *lock) {
	return HFS_STATS_LOCK_OTHER;
}

/* Instrument mutex acquisition without serializing independent statistics. */
static int hfsLibMutexLock(pthread_mutex_t *lock) {
	HfsStatsLockClass lockClass = hfsLibMutexClass(lock);
	int rc;

	hfsStatsLockBeforeFast(lockClass);
	rc = pthread_mutex_lock(lock);
	hfsStatsLockAfterFast(lockClass);
	return rc;
}

/* Release a mutex and account for hold time. */
static int hfsLibMutexUnlock(pthread_mutex_t *lock) {
	HfsStatsLockClass lockClass = hfsLibMutexClass(lock);

	hfsStatsLockReleaseFast(lockClass);
	return pthread_mutex_unlock(lock);
}

/* Keep every library lock operation paired with its statistics frame. */
#define pthread_rwlock_wrlock(lock) hfsLibRwlockWrlock(lock)
#define pthread_rwlock_unlock(lock) hfsLibRwlockUnlock(lock)
#define pthread_mutex_lock(lock) hfsLibMutexLock(lock)
#define pthread_mutex_unlock(lock) hfsLibMutexUnlock(lock)

/* The single mounted filesystem instance (one per process). */
static Hfs *g_fs;
/* Exclusive host-side lock for the configured blobstore backend. */
static int g_backendLockFd = -1;
/* Last-reference descriptions retained until close and orphan deletion succeed. */
static HfsOpenFile *g_deferredCloseFiles;

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

/* Stop at a requested fsync boundary to test recovery without library teardown. */
static void hfsFsyncCrashPoint(int point) {
	const char *step = getenv("HFS_FSYNC_CRASH_STEP");

	if (step != NULL && atoi(step) == point) {
		_exit(210 + point);
	}
}

/* Request state used to sample blobstore capacity on the SPDK owner thread. */
typedef struct HfsCapacityRequest {
	Hfs *fs;
	uint64_t freeClusters;
	HfsRequestResult result;
} HfsCapacityRequest;

/* Refresh the free-cluster cache without touching blobstore state from callers. */
static void hfsDoRefreshCapacity(void *arg) {
	HfsCapacityRequest *request = arg;

	if (hfsRequestCompletionInjected(request->result.operation)) {
		hfsRequestComplete(&request->result, -EIO, 0);
		return;
	}
	if (request->fs == NULL || request->fs->blobstore == NULL) {
		hfsRequestComplete(&request->result, -ENODEV, 0);
	} else {
		request->freeClusters =
			spdk_bs_free_cluster_count(request->fs->blobstore);
		atomic_store_explicit(&request->fs->freeClusterCount,
				      request->freeClusters, memory_order_relaxed);
		hfsRequestComplete(&request->result, 0, 0);
	}
}

/* Sample current blobstore capacity before reporting POSIX filesystem stats. */
static int hfsRefreshCapacity(Hfs *fs) {
	HfsCapacityRequest request;

	if (fs == NULL || fs->blobstore == NULL) {
		errno = ENODEV;
		return -1;
	}
	memset(&request, 0, sizeof(request));
	request.fs = fs;
	hfsRequestInit(&request.result, HFS_REQUEST_CAPACITY);
	if (hfsRequestWait(&request.result, hfsDoRefreshCapacity, &request) != 0) {
		return -1;
	}
	return 0;
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
			hfsMetaMkdir(fs, parent, leaf, 0755U);
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
static int hfs_delete_orphan(HfsOrphan *orphan) {
	HfsFile file;

	if (orphan == NULL) {
		return 0;
	}
	if (g_fs == NULL) {
		errno = ENODEV;
		return -1;
	}
	memset(&file, 0, sizeof(file));
	file.fs = g_fs;
	file.blobId = orphan->blobId;
	if (!hfsDeleteBlobById(&file)) {
		HFS_LOG_ERROR("deferred blob delete failed inode=%"PRIu64,
			      orphan->inodeId);
		return -1;
	}
	hfsFdOrphanDestroy(orphan);
	return 0;
}

/* Queue a last-reference description until its SPDK resources can be retried. */
static void hfs_defer_open_file(HfsOpenFile *openFile, HfsOrphan *orphan) {
	if (openFile == NULL) {
		return;
	}
	openFile->orphan = orphan;
	pthread_mutex_lock(&g_deferredCloseLock);
	openFile->deferredNext = g_deferredCloseFiles;
	g_deferredCloseFiles = openFile;
	pthread_mutex_unlock(&g_deferredCloseLock);
}

/* Retry retained final descriptions before the blobstore unmounts. */
static void hfs_retry_deferred_closes(void) {
	HfsOpenFile *openFile;
	HfsOrphan *orphan;

	for (;;) {
		pthread_mutex_lock(&g_deferredCloseLock);
		openFile = g_deferredCloseFiles;
		if (openFile != NULL) {
			g_deferredCloseFiles = openFile->deferredNext;
			openFile->deferredNext = NULL;
		}
		pthread_mutex_unlock(&g_deferredCloseLock);
		if (openFile == NULL) {
			return;
		}
		if (openFile->file != NULL && !hfsClose(openFile->file)) {
			HFS_LOG_ERROR("deferred blob close failed inode=%"PRIu64,
				      openFile->inodeId);
			hfs_defer_open_file(openFile, openFile->orphan);
			return;
		}
		free(openFile->file);
		openFile->file = NULL;
		orphan = openFile->orphan;
		if (hfs_delete_orphan(orphan) != 0) {
			HFS_LOG_ERROR("deferred orphan retry failed inode=%"PRIu64,
				      openFile->inodeId);
			hfs_defer_open_file(openFile, orphan);
			return;
		}
		openFile->orphan = NULL;
		hfsFdDestroy(openFile);
	}
}

/* Release an acquired description and retain failed final cleanup for retry. */
static int hfs_release_open_file(HfsOpenFile *openFile) {
	HfsOrphan *readyOrphan = NULL;
	int savedErrno;

	if (openFile == NULL || !hfsFdRelease(openFile, &readyOrphan)) {
		return 0;
	}
	if (openFile->file != NULL && !hfsClose(openFile->file)) {
		savedErrno = errno;
		hfs_defer_open_file(openFile, readyOrphan);
		errno = savedErrno;
		return -1;
	}
	free(openFile->file);
	openFile->file = NULL;
	if (hfs_delete_orphan(readyOrphan) != 0) {
		savedErrno = errno;
		hfs_defer_open_file(openFile, readyOrphan);
		errno = savedErrno;
		return -1;
	}
	openFile->orphan = NULL;
	hfsFdDestroy(openFile);
	return 0;
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

	hfsMetaReadLock();
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

/* Update the in-memory inode while the metadata write lock is held. */
static void hfsPublishWriteEndLocked(HfsOpenFile *openFile, uint64_t end) {
	struct hfs_inode *in;
	uint64_t nowNs = hfs_now_ns();

	in = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
	if (in != NULL) {
		if (end > in->size) {
			in->size = end;
		}
		/* Every successful content write changes mtime and ctime. */
		in->mtimeNs = nowNs;
		in->ctimeNs = nowNs;
		hfsMetaMarkInodeDirty(g_fs, in->id);
		atomic_store_explicit(&openFile->size, in->size,
				      memory_order_relaxed);
		atomic_store_explicit(&openFile->mtimeNs, in->mtimeNs,
				      memory_order_relaxed);
		atomic_store_explicit(&openFile->ctimeNs, nowNs, memory_order_relaxed);
	} else if (in == NULL && hfsFdGetOrphanInfo(openFile, NULL, NULL, NULL)) {
		uint64_t size = atomic_load_explicit(&openFile->size,
					     memory_order_relaxed);

		if (end > size) {
			atomic_store_explicit(&openFile->size, end,
					      memory_order_relaxed);
		}
		atomic_store_explicit(&openFile->mtimeNs, nowNs,
				      memory_order_relaxed);
		atomic_store_explicit(&openFile->ctimeNs, nowNs, memory_order_relaxed);
	}
}

/* Publish a successful write end for both linked and unlinked open files. */
static void hfsPublishWriteEnd(HfsOpenFile *openFile, uint64_t end) {
	hfsMetaWriteLock();
	hfsPublishWriteEndLocked(openFile, end);
	pthread_rwlock_unlock(&g_metaLock);
}

/* Try to publish a write without blocking the SPDK owner thread. */
static bool hfsTryPublishWriteEnd(HfsOpenFile *openFile, uint64_t end) {
	if (pthread_rwlock_trywrlock(&g_metaLock) != 0) {
		return false;
	}
	hfsPublishWriteEndLocked(openFile, end);
	pthread_rwlock_unlock(&g_metaLock);
	return true;
}

/* Prepare a truncate range before publishing its new logical file size. */
static int hfsPrepareTruncateInternal(HfsFile *file, uint64_t currentSize,
				       uint64_t targetSize, bool syncLocked) {
	if (targetSize > currentSize) {
		if (hfsResizeTo(file, targetSize) != 0) {
			return -1;
		}
		return syncLocked
			? hfsIoZeroRangeLocked(file, currentSize, targetSize - currentSize)
			: hfsIoZeroRange(file, currentSize, targetSize - currentSize);
	}
	if (targetSize < currentSize) {
		int rc = syncLocked
			? hfsIoZeroRangeLocked(file, targetSize, currentSize - targetSize)
			: hfsIoZeroRange(file, targetSize, currentSize - targetSize);

		if (rc != 0) {
			return -1;
		}
		return hfsResizeExact(file, targetSize);
	}
	return 0;
}

/* Prepare a truncate range using the ordinary inode read side. */
static int hfsPrepareTruncate(HfsFile *file, uint64_t currentSize,
			       uint64_t targetSize) {
	return hfsPrepareTruncateInternal(file, currentSize, targetSize, false);
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
	hfsMetaWriteLock();
	hfsStatsInit();
	hfsDurabilityTraceInit();
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
	/* Keep filesystem capacity separate from individual data blob allocation. */
	g_fs->clusterSize = spdk_bs_get_cluster_size(g_fs->blobstore);
	g_fs->totalDataClusters =
		spdk_bs_total_data_cluster_count(g_fs->blobstore);
	atomic_store_explicit(&g_fs->freeClusterCount,
			      spdk_bs_free_cluster_count(g_fs->blobstore),
			      memory_order_relaxed);

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
	if (hfsAsyncInit() != 0) {
		HFS_LOG_ERROR("hfsAsyncInit failed");
		hfsMetaFini(g_fs);
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
	/* Stop new async work before taking the metadata write lock for teardown. */
	hfsAsyncFini();
	hfsMetaWriteLock();
	if (g_fs == NULL) {
		hfsDurabilityTraceFini();
		hfsStatsFini();
		pthread_rwlock_unlock(&g_metaLock);
		return;
	}
	/* Persist dirty inode sizes before closing the last open descriptions. */
	if (g_fs->metaDirty && hfsMetaSaveAll(g_fs) != 0) {
		HFS_LOG_ERROR("final metadata save failed");
	}
	/* Close every placeholder before unmounting the blobstore. */
	while ((openFile = hfsFdDetachAny(&fd)) != NULL) {
		(void)hfs_release_open_file(openFile);
		(void)close(fd);
	}
	hfsFdReset();
	/* Retry final-close failures while SPDK and the blobstore are still live. */
	hfs_retry_deferred_closes();

	hfsIoFini(g_fs);
	hfsMetaFini(g_fs);
	if (!hfsUmount(g_fs)) {
		HFS_LOG_ERROR("hfsUmount failed");
	}
	free(g_fs);
	g_fs = NULL;
	hfsRuntimeStop();
	hfs_release_backend_lock();
	HFS_LOG_DEBUG("libhfs finalized");
	hfsDurabilityTraceFini();
	hfsStatsFini();
	hfs_log_fini();
	pthread_rwlock_unlock(&g_metaLock);
}

/* Context retained by a worker-free positioned data request. */
typedef struct HfsLibAsyncDirectContext {
	HfsAsyncRequest *request;
	HfsOpenFile *openFile;
	uint64_t writeOffset;
	bool write;
} HfsLibAsyncDirectContext;

/* Carry a multi-reactor write completion through metadata ordering. */
typedef struct HfsLibAsyncDirectCommit {
	HfsLibAsyncDirectContext *direct;
	ssize_t result;
	int error;
	uint32_t dataReactor;
	struct spdk_poller *poller;
} HfsLibAsyncDirectCommit;

/* Finish a direct request on its data reactor after metadata publication. */
static void hfs_lib_async_direct_finish(void *arg) {
	HfsLibAsyncDirectCommit *commit = arg;
	HfsLibAsyncDirectContext *direct = commit->direct;

	hfsAsyncCompleteDirect(direct->request, commit->result, commit->error);
	(void)hfs_release_open_file(direct->openFile);
	free(direct);
	free(commit);
}

/* Retry metadata publication from the owner poller without blocking it. */
static int hfs_lib_async_direct_publish_poller(void *arg);

/* Publish a direct write's inode size on the metadata reactor. */
static void hfs_lib_async_direct_publish(void *arg) {
	HfsLibAsyncDirectCommit *commit = arg;
	HfsLibAsyncDirectContext *direct = commit->direct;

	if (direct->write && commit->error == 0 && commit->result > 0) {
		if (!hfsTryPublishWriteEnd(direct->openFile,
				direct->writeOffset + (uint64_t)commit->result)) {
			if (commit->poller == NULL) {
				commit->poller = spdk_poller_register_named(
					hfs_lib_async_direct_publish_poller, commit, 1000,
					"hfs_direct_publish");
			}
			if (commit->poller != NULL) {
				return;
			}
			commit->error = EIO;
		}
	}
	if (commit->poller != NULL) {
		spdk_poller_unregister(&commit->poller);
	}
	if (hfsRuntimeSubmitData(commit->dataReactor,
			 hfs_lib_async_direct_finish, commit) != 0) {
		commit->error = EIO;
		hfs_lib_async_direct_finish(commit);
	}
}

/* Run one deferred metadata publication attempt on the owner reactor. */
static int hfs_lib_async_direct_publish_poller(void *arg) {
	hfs_lib_async_direct_publish(arg);
	return SPDK_POLLER_BUSY;
}

/* Complete a direct data request without taking metadata locks on a data reactor. */
static void hfs_lib_async_direct_done(void *context, ssize_t result, int error) {
	HfsLibAsyncDirectContext *direct = context;
	uint32_t dataReactor;
	HfsLibAsyncDirectCommit *commit;

	dataReactor = hfsRuntimeDataReactorFor(direct->openFile->file->fs,
					      direct->openFile->file->blobId);
	if (!direct->write || dataReactor == 0) {
		if (direct->write && error == 0 && result > 0) {
			hfsPublishWriteEnd(direct->openFile,
				direct->writeOffset + (uint64_t)result);
		}
		hfsAsyncCompleteDirect(direct->request, result, error);
		(void)hfs_release_open_file(direct->openFile);
		free(direct);
		return;
	}
	commit = calloc(1, sizeof(*commit));
	if (commit == NULL) {
		hfsAsyncCompleteDirect(direct->request, -1, ENOMEM);
		(void)hfs_release_open_file(direct->openFile);
		free(direct);
		return;
	}
	commit->direct = direct;
	commit->result = result;
	commit->error = error;
	commit->dataReactor = dataReactor;
	if (hfsRuntimeSubmit(hfs_lib_async_direct_publish, commit) != 0) {
		commit->error = EIO;
		hfs_lib_async_direct_finish(commit);
	}
}

/* Return true when a request is large and aligned enough for direct child fan-out. */
static bool hfs_lib_async_direct_shape(HfsOpenFile *openFile, size_t count,
				       uint64_t offset) {
	uint64_t unitSize;

	if (openFile == NULL || openFile->file == NULL || openFile->file->fs == NULL) {
		return false;
	}
	unitSize = openFile->file->fs->unitSize;
	return unitSize != 0 && count / (size_t)unitSize >= 2U &&
		count % (size_t)unitSize == 0 && offset % unitSize == 0;
}

/* Try the worker-free positioned read path and report whether fallback is needed. */
static int hfs_lib_async_try_pread(int fd, void *buffer, size_t count,
				   off_t offset, HfsAsyncCallback callback,
				   void *context, HfsAsyncRequest **requestOut) {
	HfsOpenFile *openFile;
	HfsLibAsyncDirectContext *direct;
	struct hfs_inode *in;
	uint64_t fileSize;
	uint64_t effectiveCount;
	int savedErrno;
	int rc;

	if (offset < 0 || count == 0 || !hfsFdIsHfs(fd)) {
		return 0;
	}
	openFile = hfs_acquire_file(fd, true);
	if (openFile == NULL || !hfs_lib_async_direct_shape(openFile, count,
							       (uint64_t)offset)) {
		if (openFile != NULL) {
			hfs_release_open_file(openFile);
		}
		return 0;
	}
	hfsMetaReadLock();
	in = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		hfs_release_open_file(openFile);
		return 0;
	}
	if (in != NULL) {
		fileSize = in->size;
	} else if (!hfsFdGetOrphanInfo(openFile, &fileSize, NULL, NULL)) {
		pthread_rwlock_unlock(&g_metaLock);
		hfs_release_open_file(openFile);
		return 0;
	}
	if ((uint64_t)offset >= fileSize) {
		pthread_rwlock_unlock(&g_metaLock);
		hfs_release_open_file(openFile);
		return 0;
	}
	effectiveCount = count;
	if ((uint64_t)offset + effectiveCount > fileSize) {
		effectiveCount = fileSize - (uint64_t)offset;
	}
	pthread_rwlock_unlock(&g_metaLock);
	if (effectiveCount == 0 || effectiveCount % openFile->file->fs->unitSize != 0 ||
	    effectiveCount > SIZE_MAX) {
		hfs_release_open_file(openFile);
		return 0;
	}
	direct = calloc(1, sizeof(*direct));
	if (direct == NULL) {
		hfs_release_open_file(openFile);
		errno = ENOMEM;
		return -1;
	}
	direct->openFile = openFile;
	if (hfsAsyncCreateDirect(callback, context, &direct->request) != 0) {
		hfs_release_open_file(openFile);
		free(direct);
		return -1;
	}
	*requestOut = direct->request;
	rc = hfsIoReadAtAsync(openFile->file, buffer, (size_t)effectiveCount,
				      (uint64_t)offset, hfs_lib_async_direct_done, direct);
	if (rc == 0) {
		hfsAsyncRecordDirectSubmit(direct->request);
		return 1;
	}
	*requestOut = NULL;
	savedErrno = errno;
	hfsAsyncDiscardDirect(direct->request);
	hfs_release_open_file(openFile);
	free(direct);
	errno = savedErrno;
	return savedErrno == EAGAIN ? 0 : -1;
}

/* Try the worker-free positioned write path and report whether fallback is needed. */
static int hfs_lib_async_try_pwrite(int fd, const void *buffer, size_t count,
				    off_t offset, HfsAsyncCallback callback,
				    void *context, HfsAsyncRequest **requestOut) {
	HfsOpenFile *openFile;
	HfsLibAsyncDirectContext *direct;
	int savedErrno;
	int rc;

	if (offset < 0 || count == 0 || !hfsFdIsHfs(fd)) {
		return 0;
	}
	openFile = hfs_acquire_file(fd, true);
	if (openFile == NULL || !openFile->writable ||
	    (openFile->flags & O_APPEND) != 0 ||
	    !hfs_lib_async_direct_shape(openFile, count, (uint64_t)offset)) {
		if (openFile != NULL) {
			hfs_release_open_file(openFile);
		}
		return 0;
	}
	direct = calloc(1, sizeof(*direct));
	if (direct == NULL) {
		hfs_release_open_file(openFile);
		errno = ENOMEM;
		return -1;
	}
	direct->openFile = openFile;
	direct->write = true;
	direct->writeOffset = (uint64_t)offset;
	if (hfsAsyncCreateDirect(callback, context, &direct->request) != 0) {
		hfs_release_open_file(openFile);
		free(direct);
		return -1;
	}
	*requestOut = direct->request;
	rc = hfsIoWriteAtAsync(openFile->file, buffer, count, (uint64_t)offset,
				       hfs_lib_async_direct_done, direct);
	if (rc == 0) {
		hfsAsyncRecordDirectSubmit(direct->request);
		return 1;
	}
	*requestOut = NULL;
	savedErrno = errno;
	hfsAsyncDiscardDirect(direct->request);
	hfs_release_open_file(openFile);
	free(direct);
	errno = savedErrno;
	return savedErrno == EAGAIN ? 0 : -1;
}

/* Submit a positioned read through the async core. */
int hfs_lib_async_pread(int fd, void *buf, size_t count, off_t offset,
			HfsAsyncCallback callback, void *context,
			HfsAsyncRequest **requestOut) {
	int direct = hfs_lib_async_try_pread(fd, buf, count, offset, callback,
						 context, requestOut);
	if (direct != 0) {
		return direct > 0 ? 0 : -1;
	}
	return hfsAsyncPread(fd, buf, count, offset, callback, context, requestOut);
}

/* Submit a positioned write through the async core. */
int hfs_lib_async_pwrite(int fd, const void *buf, size_t count, off_t offset,
			 HfsAsyncCallback callback, void *context,
			 HfsAsyncRequest **requestOut) {
	int direct = hfs_lib_async_try_pwrite(fd, buf, count, offset, callback,
						  context, requestOut);
	if (direct != 0) {
		return direct > 0 ? 0 : -1;
	}
	return hfsAsyncPwrite(fd, buf, count, offset, callback, context, requestOut);
}

/* Submit a positioned vector read through the async core. */
int hfs_lib_async_preadv(int fd, const struct iovec *iov, int iovcnt,
			 off_t offset, HfsAsyncCallback callback, void *context,
			 HfsAsyncRequest **requestOut) {
	return hfsAsyncPreadv(fd, iov, iovcnt, offset, callback, context, requestOut);
}

/* Submit a positioned vector write through the async core. */
int hfs_lib_async_pwritev(int fd, const struct iovec *iov, int iovcnt,
			  off_t offset, HfsAsyncCallback callback, void *context,
			  HfsAsyncRequest **requestOut) {
	return hfsAsyncPwritev(fd, iov, iovcnt, offset, callback, context, requestOut);
}

/* Submit an fsync through the async core. */
int hfs_lib_async_fsync(int fd, HfsAsyncCallback callback, void *context,
			 HfsAsyncRequest **requestOut) {
	return hfsAsyncFsync(fd, callback, context, requestOut);
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
	uint64_t inodeSize;
	uint64_t inodeCtimeNs;
	uint64_t inodeMtimeNs;
	uint64_t inodeAtimeNs;
	uint32_t inodeMode;
	uint32_t inodeUid;
	uint32_t inodeGid;
	struct hfs_inode *current;
	int fd;
	int rc;
	bool created = false;
	bool truncateRequested = false;

	hfsMetaWriteLock();
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
		rc = hfsMetaCreateFile(g_fs, parent, leaf, (uint32_t)mode);
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
		atomic_store_explicit(&openFile->ctimeNs, in->ctimeNs,
				      memory_order_relaxed);
		openFile->mode = in->mode;
		openFile->uid = in->uid;
		openFile->gid = in->gid;
		openFile->atimeNs = in->atimeNs;
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
		truncateRequested = true;
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
	file->inodeId = in->id;
	file->runtime = hfsInodeRuntimeGet(g_fs, in->id);
	if (file->runtime == NULL) {
		close(fd);
		free(file);
		pthread_rwlock_unlock(&g_metaLock);
		return -1;
	}
	inodeSize = in->size;
	inodeCtimeNs = in->ctimeNs;
	inodeMtimeNs = in->mtimeNs;
	inodeAtimeNs = in->atimeNs;
	inodeMode = in->mode;
	inodeUid = in->uid;
	inodeGid = in->gid;

	/* Do not hold the process metadata lock while waiting for SPDK metadata I/O. */
	pthread_rwlock_unlock(&g_metaLock);
	if (!hfsOpenById(file)) {
		close(fd);
		free(file);
		return -1;
	}
	hfsMetaWriteLock();
	current = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, file->inodeId);
	atomic_store_explicit(&file->runtime->allocatedClusters,
			      spdk_blob_get_num_clusters(file->blob),
			      memory_order_release);
	if (truncateRequested && inodeSize != 0) {
		int savedErrno;

		if (hfsPrepareTruncate(file, inodeSize, 0) != 0 ||
		    (current != NULL && hfsMetaSetSize(g_fs, current, 0) != 0)) {
			savedErrno = errno;
			hfsClose(file);
			free(file);
			close(fd);
			pthread_rwlock_unlock(&g_metaLock);
			errno = savedErrno;
			return -1;
		}
	}
	if (hfsFdCreate(&openFile, file, file->inodeId, flags,
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
	atomic_store_explicit(&openFile->size,
			      current != NULL ? current->size : inodeSize,
			      memory_order_relaxed);
	if ((flags & O_APPEND) != 0) {
		openFile->offset = current != NULL ? current->size : inodeSize;
	}
	atomic_store_explicit(&openFile->ctimeNs, inodeCtimeNs,
			      memory_order_relaxed);
	openFile->mode = inodeMode;
	openFile->uid = inodeUid;
	openFile->gid = inodeGid;
	openFile->atimeNs = inodeAtimeNs;
	atomic_store_explicit(&openFile->mtimeNs, inodeMtimeNs,
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
	hfsMetaReadLock();
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
		pthread_mutex_lock(&openFile->runtime->appendLock);
	}
	writeOffset = openFile->offset;
	if (append) {
		uint64_t appendSize;

		hfsMetaReadLock();
		in = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
		if (g_fs == NULL) {
			pthread_rwlock_unlock(&g_metaLock);
			pthread_mutex_unlock(&openFile->ioLock);
			pthread_mutex_unlock(&openFile->runtime->appendLock);
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
			pthread_mutex_unlock(&openFile->runtime->appendLock);
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
			pthread_mutex_unlock(&openFile->runtime->appendLock);
		}
		hfs_release_open_file(openFile);
		errno = n < 0 ? savedErrno : EIO;
		return -1;
	}
	openFile->offset = writeOffset + (uint64_t)n;
	hfsPublishWriteEnd(openFile, openFile->offset);
	pthread_mutex_unlock(&openFile->ioLock);
	if (append) {
		pthread_mutex_unlock(&openFile->runtime->appendLock);
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
	hfsMetaReadLock();
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
		pthread_mutex_lock(&openFile->runtime->appendLock);
	}
	writeOffset = (uint64_t)offset;
	if (append) {
		uint64_t appendSize;

		hfsMetaReadLock();
		in = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
		if (g_fs == NULL) {
			pthread_rwlock_unlock(&g_metaLock);
			pthread_mutex_unlock(&openFile->runtime->appendLock);
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
			pthread_mutex_unlock(&openFile->runtime->appendLock);
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
			pthread_mutex_unlock(&openFile->runtime->appendLock);
		}
		hfs_release_open_file(openFile);
		errno = n < 0 ? savedErrno : EIO;
		return -1;
	}
	hfsPublishWriteEnd(openFile, writeOffset + (uint64_t)n);
	if (append) {
		pthread_mutex_unlock(&openFile->runtime->appendLock);
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
		pthread_mutex_lock(&openFile->runtime->appendLock);
	}
	writeOffset = openFile->offset;
	if (append && hfsSnapshotFileSize(openFile, &writeOffset) != 0) {
		if (append) {
			pthread_mutex_unlock(&openFile->runtime->appendLock);
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
		pthread_mutex_unlock(&openFile->runtime->appendLock);
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
		pthread_mutex_lock(&openFile->runtime->appendLock);
	}
	writeOffset = (uint64_t)offset;
	if (append && hfsSnapshotFileSize(openFile, &writeOffset) != 0) {
		if (append) {
			pthread_mutex_unlock(&openFile->runtime->appendLock);
		}
		hfs_release_open_file(openFile);
		return -1;
	}
	n = hfsWritevAt(openFile, iov, iovcnt, total, writeOffset);
	if (n > 0) {
		hfsPublishWriteEnd(openFile, writeOffset + (uint64_t)n);
	}
	if (append) {
		pthread_mutex_unlock(&openFile->runtime->appendLock);
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
	hfsMetaReadLock();
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
/* Detach one HFS fd slot and release its open-file description. */
int hfs_lib_release_fd(int fd) {
	HfsOpenFile *openFile = hfsFdDetach(fd);

	if (openFile == NULL) {
		errno = EBADF;
		return -1;
	}
	return hfs_release_open_file(openFile);
}

/* Close an HFS fd and then release its kernel placeholder. */
int hfs_lib_close(int fd) {
	int releaseRc;
	int savedErrno;

	releaseRc = hfs_lib_release_fd(fd);
	savedErrno = errno;
	/* The HFS slot is detached even when its final SPDK close failed. */
	if (close(fd) != 0 && releaseRc == 0) {
		return -1;
	}
	if (releaseRc != 0) {
		errno = savedErrno;
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

	hfsMetaWriteLock();
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
		if (!g_fs->metaRecoveryRequired) {
			hfsFdClearOrphan(orphan);
		}
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
	HfsInodeRuntime *runtime;
	int savedErrno;
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
	runtime = openFile->runtime;
	if (runtime == NULL) {
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = EIO;
		return -1;
	}
	pthread_rwlock_wrlock(&runtime->syncLock);
	hfsMetaReadLock();
	in = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		pthread_rwlock_unlock(&runtime->syncLock);
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = ENODEV;
		return -1;
	}
	if (in != NULL) {
		currentSize = in->size;
	} else if (!hfsFdGetOrphanInfo(openFile, &currentSize, NULL, NULL)) {
		pthread_rwlock_unlock(&g_metaLock);
		pthread_rwlock_unlock(&runtime->syncLock);
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = ENOENT;
		return -1;
	}

	pthread_rwlock_unlock(&g_metaLock);

	/* Clear both growth and shrink ranges before publishing the new EOF. */
	if (hfsPrepareTruncateInternal(openFile->file, currentSize,
				       (uint64_t)length, true) != 0) {
		savedErrno = errno;
		pthread_rwlock_unlock(&runtime->syncLock);
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = savedErrno;
		return -1;
	}

	/* Upgrade to a writer only for the stable inode update. */
	hfsMetaWriteLock();
	in = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		pthread_rwlock_unlock(&runtime->syncLock);
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
		pthread_rwlock_unlock(&runtime->syncLock);
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = ENOENT;
		return -1;
	}
	pthread_rwlock_unlock(&g_metaLock);
	pthread_rwlock_unlock(&runtime->syncLock);
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

	if (st == NULL) {
		errno = EFAULT;
		return -1;
	}

	openFile = hfsFdAcquire(fd);
	if (openFile == NULL) {
		errno = EBADF;
		return -1;
	}
	hfsMetaReadLock();
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
		snapshot.mode = openFile->mode;
		snapshot.uid = openFile->uid;
		snapshot.gid = openFile->gid;
		snapshot.size = orphanSize;
	snapshot.atimeNs = openFile->atimeNs;
		snapshot.ctimeNs = orphanCtimeNs;
		snapshot.mtimeNs = orphanMtimeNs;
	}
	pthread_rwlock_unlock(&g_metaLock);
	hfs_release_open_file(openFile);
	in = &snapshot;
	memset(st, 0, sizeof(*st));
	st->st_ino = (ino_t)in->id;
	st->st_mode = (in->type == HFS_INODE_DIR ? S_IFDIR : S_IFREG) |
			     (in->mode & 07777U);
	st->st_size = (off_t)in->size;
	st->st_atim.tv_sec = (time_t)(in->atimeNs / 1000000000ULL);
	st->st_atim.tv_nsec = (long)(in->atimeNs % 1000000000ULL);
	st->st_mtim.tv_sec = (time_t)(in->mtimeNs / 1000000000ULL);
	st->st_mtim.tv_nsec = (long)(in->mtimeNs % 1000000000ULL);
	st->st_ctim.tv_sec = (time_t)(in->ctimeNs / 1000000000ULL);
	st->st_ctim.tv_nsec = (long)(in->ctimeNs % 1000000000ULL);
	st->st_uid = (uid_t)in->uid;
	st->st_gid = (gid_t)in->gid;
	st->st_nlink = 1;
	st->st_blksize = (blksize_t)(g_fs != NULL && g_fs->unitSize != 0 ?
					g_fs->unitSize : 4096);
	st->st_blocks = (blkcnt_t)((in->size + 511U) / 512U);
	return 0;
}

/*
 * Check access to a path under the HFS mount prefix. HFS has no
 * permission model, so any existing path passes R/W/X; missing paths
 * fail with ENOENT.
 */
int hfs_lib_access(const char *path, int mode) {
	struct hfs_inode *in;

	hfsMetaReadLock();
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

/* Decode POSIX timestamp arguments into nanosecond values. */
static int hfs_decode_times(const struct timespec times[2],
				    uint64_t *atimeNs, uint64_t *mtimeNs,
				    bool *setAtime, bool *setMtime) {
	uint64_t nowNs = hfs_now_ns();
	int i;

	*atimeNs = 0;
	*mtimeNs = 0;
	*setAtime = false;
	*setMtime = false;
	if (times == NULL) {
		*atimeNs = nowNs;
		*mtimeNs = nowNs;
		*setAtime = true;
		*setMtime = true;
		return 0;
	}
	for (i = 0; i < 2; i++) {
		const struct timespec *value = &times[i];
		uint64_t *result = i == 0 ? atimeNs : mtimeNs;
		bool *setValue = i == 0 ? setAtime : setMtime;

		if (value->tv_nsec == UTIME_OMIT) {
			continue;
		}
		if (value->tv_nsec == UTIME_NOW) {
			*result = nowNs;
			*setValue = true;
			continue;
		}
		if (value->tv_sec < 0 || value->tv_nsec < 0 ||
		    value->tv_nsec >= 1000000000L) {
			return EINVAL;
		}
		*result = (uint64_t)value->tv_sec * 1000000000ULL +
			  (uint64_t)value->tv_nsec;
		*setValue = true;
	}
	return 0;
}

/* Update one path attribute while holding the metadata write lock. */
static int hfs_update_path_attribute(const char *path, int attribute,
					     mode_t mode, uid_t uid, gid_t gid,
					     const struct timespec times[2]) {
	struct hfs_inode *in;
	uint64_t atimeNs = 0;
	uint64_t mtimeNs = 0;
	bool setAtime = false;
	bool setMtime = false;
	int rc = 0;

	hfsMetaWriteLock();
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
	switch (attribute) {
	case 1:
		rc = hfsMetaSetMode(g_fs, in, (uint32_t)mode);
		break;
	case 2:
		rc = hfsMetaSetOwner(g_fs, in, (uint32_t)uid, (uint32_t)gid,
					      uid != (uid_t)-1, gid != (gid_t)-1);
		break;
	case 3:
		rc = hfs_decode_times(times, &atimeNs, &mtimeNs,
				       &setAtime, &setMtime);
		if (rc == 0) {
			rc = hfsMetaSetTimes(g_fs, in, atimeNs, mtimeNs,
					       setAtime, setMtime);
		}
		break;
	default:
		rc = EINVAL;
		break;
	}
	pthread_rwlock_unlock(&g_metaLock);
	if (rc != 0) {
		if (rc > 0) {
			errno = rc;
		}
		return -1;
	}
	return 0;
}

/* Change permission bits on a managed path. */
int hfs_lib_chmod(const char *path, mode_t mode) {
	return hfs_update_path_attribute(path, 1, mode, 0, 0, NULL);
}

/* Change owner ids on a managed path. */
int hfs_lib_chown(const char *path, uid_t uid, gid_t gid) {
	if (uid == (uid_t)-1 && gid == (gid_t)-1) {
		return 0;
	}
	return hfs_update_path_attribute(path, 2, 0, uid, gid, NULL);
}

/* Update timestamps on a managed path. */
int hfs_lib_utimens(const char *path, const struct timespec times[2]) {
	return hfs_update_path_attribute(path, 3, 0, 0, 0, times);
}

/* Change permission bits on an HFS fd, including an already unlinked file. */
int hfs_lib_fchmod(int fd, mode_t mode) {
	HfsOpenFile *openFile = hfsFdAcquire(fd);
	struct hfs_inode *in;
	int rc;

	if (openFile == NULL) {
		errno = EBADF;
		return -1;
	}
	pthread_mutex_lock(&openFile->ioLock);
	hfsMetaWriteLock();
	in = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
	if (g_fs == NULL) {
		rc = ENODEV;
	} else if (in != NULL) {
		rc = hfsMetaSetMode(g_fs, in, (uint32_t)mode) == 0 ? 0 : errno;
		if (rc == 0) {
			openFile->mode = in->mode;
		}
	} else if (hfsFdGetOrphanInfo(openFile, NULL, NULL, NULL)) {
		openFile->mode = (uint32_t)mode & 07777U;
		rc = 0;
	} else {
		rc = ENOENT;
	}
	pthread_rwlock_unlock(&g_metaLock);
	pthread_mutex_unlock(&openFile->ioLock);
	hfs_release_open_file(openFile);
	if (rc != 0) {
		errno = rc;
		return -1;
	}
	return 0;
}

/* Change owner ids on an HFS fd, including an already unlinked file. */
int hfs_lib_fchown(int fd, uid_t uid, gid_t gid) {
	HfsOpenFile *openFile = hfsFdAcquire(fd);
	struct hfs_inode *in;
	int rc;

	if (openFile == NULL) {
		errno = EBADF;
		return -1;
	}
	pthread_mutex_lock(&openFile->ioLock);
	hfsMetaWriteLock();
	in = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
	if (g_fs == NULL) {
		rc = ENODEV;
	} else if (in != NULL) {
		rc = hfsMetaSetOwner(g_fs, in, (uint32_t)uid, (uint32_t)gid,
					      uid != (uid_t)-1, gid != (gid_t)-1) == 0 ?
			  0 : errno;
		if (rc == 0) {
			openFile->uid = in->uid;
			openFile->gid = in->gid;
		}
	} else if (hfsFdGetOrphanInfo(openFile, NULL, NULL, NULL)) {
		if (uid != (uid_t)-1) {
			openFile->uid = (uint32_t)uid;
		}
		if (gid != (gid_t)-1) {
			openFile->gid = (uint32_t)gid;
		}
		rc = 0;
	} else {
		rc = ENOENT;
	}
	pthread_rwlock_unlock(&g_metaLock);
	pthread_mutex_unlock(&openFile->ioLock);
	hfs_release_open_file(openFile);
	if (rc != 0) {
		errno = rc;
		return -1;
	}
	return 0;
}

/* Update timestamps on an HFS fd, including an already unlinked file. */
int hfs_lib_futimens(int fd, const struct timespec times[2]) {
	HfsOpenFile *openFile = hfsFdAcquire(fd);
	struct hfs_inode *in;
	uint64_t atimeNs;
	uint64_t mtimeNs;
	bool setAtime;
	bool setMtime;
	int rc;

	if (openFile == NULL) {
		errno = EBADF;
		return -1;
	}
	rc = hfs_decode_times(times, &atimeNs, &mtimeNs, &setAtime, &setMtime);
	if (rc != 0) {
		hfs_release_open_file(openFile);
		errno = rc;
		return -1;
	}
	pthread_mutex_lock(&openFile->ioLock);
	hfsMetaWriteLock();
	in = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
	if (g_fs == NULL) {
		rc = ENODEV;
	} else if (in != NULL) {
		rc = hfsMetaSetTimes(g_fs, in, atimeNs, mtimeNs,
					  setAtime, setMtime) == 0 ? 0 : errno;
		if (rc == 0) {
			if (setAtime) {
				openFile->atimeNs = in->atimeNs;
			}
			if (setMtime) {
				atomic_store_explicit(&openFile->mtimeNs, in->mtimeNs,
						      memory_order_relaxed);
			}
			atomic_store_explicit(&openFile->ctimeNs, in->ctimeNs,
				      memory_order_relaxed);
		}
	} else if (hfsFdGetOrphanInfo(openFile, NULL, NULL, NULL)) {
		if (setAtime) {
			openFile->atimeNs = atimeNs;
		}
		if (setMtime) {
			atomic_store_explicit(&openFile->mtimeNs, mtimeNs,
					      memory_order_relaxed);
		}
		if (setAtime || setMtime) {
			atomic_store_explicit(&openFile->ctimeNs, hfs_now_ns(),
				      memory_order_relaxed);
		}
		rc = 0;
	} else {
		rc = ENOENT;
	}
	pthread_rwlock_unlock(&g_metaLock);
	pthread_mutex_unlock(&openFile->ioLock);
	hfs_release_open_file(openFile);
	if (rc != 0) {
		errno = rc;
		return -1;
	}
	return 0;
}

/* Validate a POSIX advisory hint without disturbing the HFS data path. */
int hfs_lib_posix_fadvise(int fd, off_t offset, off_t len, int advice) {
	HfsOpenFile *openFile;

	if (offset < 0 || len < 0) {
		return EINVAL;
	}
	switch (advice) {
	case POSIX_FADV_NORMAL:
	case POSIX_FADV_RANDOM:
	case POSIX_FADV_SEQUENTIAL:
	case POSIX_FADV_WILLNEED:
	case POSIX_FADV_DONTNEED:
#ifdef POSIX_FADV_NOREUSE
	case POSIX_FADV_NOREUSE:
#endif
		break;
	default:
		return EINVAL;
	}
	openFile = hfsFdAcquire(fd);
	if (openFile == NULL) {
		return EBADF;
	}
	hfs_release_open_file(openFile);
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
 * Flush an HFS fd with ordered data and metadata durability barriers.
 * The second sync flushes the metadata transaction committed after data.
 */
int hfs_lib_fsync(int fd) {
	HfsOpenFile *openFile = hfsFdAcquire(fd);
	struct hfs_inode *inode;
	HfsInodeRuntime *runtime = NULL;
	bool ioLocked = false;
	int savedErrno;

	if (openFile == NULL) {
		errno = EBADF;
		return -1;
	}
	pthread_mutex_lock(&openFile->ioLock);
	if (openFile->file != NULL) {
		runtime = openFile->runtime;
		if (runtime == NULL) {
			pthread_mutex_unlock(&openFile->ioLock);
			hfs_release_open_file(openFile);
			errno = EIO;
			return -1;
		}
		pthread_rwlock_wrlock(&runtime->syncLock);
		ioLocked = true;
	}
	if (openFile->file != NULL && hfsSync(openFile->file) != 0) {
		savedErrno = errno;
		hfsDurabilityTraceRecord(openFile->inodeId, "data-barrier",
					savedErrno != 0 ? -savedErrno : -EIO);
		if (ioLocked) {
			pthread_rwlock_unlock(&runtime->syncLock);
		}
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = savedErrno;
		return -1;
	}
	if (openFile->file != NULL) {
		hfsDurabilityTraceRecord(openFile->inodeId, "data-barrier", 0);
	}
	hfsFsyncCrashPoint(1);
	hfsMetaWriteLock();
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		if (ioLocked) {
			pthread_rwlock_unlock(&runtime->syncLock);
		}
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = ENODEV;
		return -1;
	}
	if (!g_fs->metaDirty) {
		hfsDurabilityTraceRecord(openFile->inodeId, "metadata-skip", 0);
		pthread_rwlock_unlock(&g_metaLock);
		if (ioLocked) {
			pthread_rwlock_unlock(&runtime->syncLock);
		}
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		return 0;
	}
	inode = hfsMetaFindById(g_fs, openFile->inodeId);
	if ((inode != NULL
		? hfsMetaCommitInode(g_fs, openFile->inodeId)
		: hfsMetaCommitDirty(g_fs)) != 0) {
		savedErrno = errno;
		hfsDurabilityTraceRecord(openFile->inodeId, "metadata-commit",
					savedErrno != 0 ? -savedErrno : -EIO);
		pthread_rwlock_unlock(&g_metaLock);
		if (ioLocked) {
			pthread_rwlock_unlock(&runtime->syncLock);
		}
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = savedErrno;
		return -1;
	}
	if (inode != NULL) {
		hfsDurabilityTraceRecord(openFile->inodeId, "metadata-commit", 0);
	} else {
		hfsDurabilityTraceRecord(openFile->inodeId, "metadata-skip", 0);
	}
	pthread_rwlock_unlock(&g_metaLock);
	hfsFsyncCrashPoint(2);
	/* Commit is complete in blob metadata; persist it behind a second bdev flush. */
	if (openFile->file != NULL && hfsSyncPostCommit(openFile->file) != 0) {
		savedErrno = errno;
		hfsDurabilityTraceRecord(openFile->inodeId, "post-metadata-barrier",
					savedErrno != 0 ? -savedErrno : -EIO);
		if (ioLocked) {
			pthread_rwlock_unlock(&runtime->syncLock);
		}
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = savedErrno;
		return -1;
	}
	if (openFile->file != NULL) {
		hfsDurabilityTraceRecord(openFile->inodeId, "post-metadata-barrier", 0);
	}
	hfsFsyncCrashPoint(3);
	if (ioLocked) {
		pthread_rwlock_unlock(&runtime->syncLock);
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
	HfsInodeRuntime *runtime;
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
		runtime = openFile->runtime;
		if (runtime == NULL) {
			pthread_mutex_unlock(&openFile->ioLock);
			hfs_release_open_file(openFile);
			errno = EIO;
			return -1;
		}
		pthread_rwlock_wrlock(&runtime->syncLock);
		if (hfsSnapshotFileSize(openFile, &fileSize) != 0) {
			pthread_rwlock_unlock(&runtime->syncLock);
			pthread_mutex_unlock(&openFile->ioLock);
			hfs_release_open_file(openFile);
			return -1;
		}
		if ((uint64_t)offset >= fileSize) {
			pthread_rwlock_unlock(&runtime->syncLock);
			pthread_mutex_unlock(&openFile->ioLock);
			hfs_release_open_file(openFile);
			return 0;
		}
		punchLength = end - (uint64_t)offset;
		if (punchLength > fileSize - (uint64_t)offset) {
			punchLength = fileSize - (uint64_t)offset;
		}
		if (hfsIoPunchHoleLocked(openFile->file, (uint64_t)offset,
				   punchLength) != 0) {
			pthread_rwlock_unlock(&runtime->syncLock);
			pthread_mutex_unlock(&openFile->ioLock);
			hfs_release_open_file(openFile);
			return -1;
		}
		pthread_rwlock_unlock(&runtime->syncLock);
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		return 0;
	}

	/* KEEP_SIZE reserves backing clusters but does not change inode size. */
	pthread_mutex_lock(&openFile->ioLock);
	runtime = openFile->runtime;
	if (runtime == NULL) {
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		errno = EIO;
		return -1;
	}
	pthread_rwlock_wrlock(&runtime->syncLock);
	if (hfsResizeTo(openFile->file, end) != 0) {
		pthread_rwlock_unlock(&runtime->syncLock);
		pthread_mutex_unlock(&openFile->ioLock);
		hfs_release_open_file(openFile);
		return -1;
	}
	pthread_rwlock_unlock(&runtime->syncLock);
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

	hfsMetaWriteLock();
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

	rc = hfsMetaMkdir(g_fs, parent, leaf, (uint32_t)mode);
	pthread_rwlock_unlock(&g_metaLock);
	return rc;
}

/* Remove an empty directory under the HFS mount prefix. */
int hfs_lib_rmdir(const char *path) {
	int rc;

	hfsMetaWriteLock();
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

	hfsMetaWriteLock();
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
	if (replacedId != 0 &&
	    hfsFdMarkOrphan(replacedId, replacedBlobId, replacedSize,
			    replacedCtimeNs, replacedMtimeNs, &orphan) != 0) {
		pthread_rwlock_unlock(&g_metaLock);
		return -1;
	}
	rc = hfsMetaRename(g_fs, oldPath, newPath);
	if (rc != 0) {
		if (!g_fs->metaRecoveryRequired) {
			hfsFdClearOrphan(orphan);
		}
		pthread_rwlock_unlock(&g_metaLock);
		return -1;
	}
	if (replacedId != 0 && orphan == NULL) {
		HfsFile file;

		memset(&file, 0, sizeof(file));
		file.fs = g_fs;
		file.blobId = replacedBlobId;
		if (!hfsDeleteBlobById(&file)) {
			HFS_LOG_ERROR("rename target blob delete failed inode=%"PRIu64,
				      replacedId);
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
	uint32_t i;
	uint64_t n = 0;

	hfsMetaReadLock();
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

	n = hfsMetaChildCount(g_fs, in->id);
	dir->entries = calloc(n > 0 ? n : 1, sizeof(struct hfs_dirent));
	if (dir->entries == NULL) {
		pthread_mutex_destroy(&dir->lock);
		free(dir);
		pthread_rwlock_unlock(&g_metaLock);
		errno = ENOMEM;
		return NULL;
	}

	for (i = 0; i < (uint32_t)n; i++) {
		struct hfs_inode *child = hfsMetaChildAt(g_fs, in->id, i);
		struct hfs_dirent *e;

		if (child == NULL) {
			continue;
		}
		e = &dir->entries[dir->count];
		strncpy(e->name, child->name, sizeof(e->name) - 1);
		e->name[sizeof(e->name) - 1] = '\0';
		e->inodeId = child->id;
		e->d_type = child->type == HFS_INODE_DIR ? DT_DIR : DT_REG;
		dir->count++;
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

	if (st == NULL) {
		errno = EFAULT;
		return -1;
	}

	hfsMetaReadLock();
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
	st->st_mode = (in->type == HFS_INODE_DIR ? S_IFDIR : S_IFREG) |
			     (in->mode & 07777U);
	st->st_size = (off_t)in->size;
	st->st_atim.tv_sec = (time_t)(in->atimeNs / 1000000000ULL);
	st->st_atim.tv_nsec = (long)(in->atimeNs % 1000000000ULL);
	st->st_mtim.tv_sec = (time_t)(in->mtimeNs / 1000000000ULL);
	st->st_mtim.tv_nsec = (long)(in->mtimeNs % 1000000000ULL);
	st->st_ctim.tv_sec = (time_t)(in->ctimeNs / 1000000000ULL);
	st->st_ctim.tv_nsec = (long)(in->ctimeNs % 1000000000ULL);
	st->st_uid = (uid_t)in->uid;
	st->st_gid = (gid_t)in->gid;
	st->st_nlink = 1;
	st->st_blksize = (blksize_t)(g_fs != NULL && g_fs->unitSize != 0 ?
					g_fs->unitSize : 4096);
	st->st_blocks = (blkcnt_t)((in->size + 511U) / 512U);
	return 0;
}

/* Fill statvfs capacity fields from the current blobstore capacity snapshot. */
static void hfs_fill_statvfs(const Hfs *fs, struct statvfs *st) {
	uint64_t totalClusters = fs->totalDataClusters;
	uint64_t freeClusters = atomic_load_explicit(&fs->freeClusterCount,
					       memory_order_relaxed);
	uint64_t blockSize = fs->clusterSize != 0 ? fs->clusterSize :
		fs->unitSize != 0 ? fs->unitSize : 4096;

	if (freeClusters > totalClusters) {
		freeClusters = totalClusters;
	}
	memset(st, 0, sizeof(*st));
	st->f_bsize = blockSize;
	st->f_frsize = blockSize;
	st->f_blocks = totalClusters;
	st->f_bfree = freeClusters;
	st->f_bavail = freeClusters;
	st->f_files = HFS_MAX_INODES;
	st->f_ffree = HFS_MAX_INODES - fs->inodeCount;
	st->f_favail = st->f_ffree;
	st->f_fsid = HFS_SUPER_MAGIC;
	st->f_namemax = FILENAME_LENGTH - 1;
}

/* Fill Linux statfs capacity fields from the mounted blobstore snapshot. */
static void hfs_fill_statfs(const Hfs *fs, struct statfs *st) {
	struct statvfs vfs;

	hfs_fill_statvfs(fs, &vfs);
	memset(st, 0, sizeof(*st));
	st->f_type = HFS_SUPER_MAGIC;
	st->f_bsize = (long)vfs.f_bsize;
	st->f_blocks = vfs.f_blocks;
	st->f_bfree = vfs.f_bfree;
	st->f_bavail = vfs.f_bavail;
	st->f_files = vfs.f_files;
	st->f_ffree = vfs.f_ffree;
	st->f_fsid.__val[0] = (int)HFS_SUPER_MAGIC;
	st->f_namelen = (long)vfs.f_namemax;
	st->f_frsize = (long)vfs.f_frsize;
}

/* Return statvfs information for an existing managed path. */
int hfs_lib_statvfs(const char *path, struct statvfs *st) {
	if (st == NULL) {
		errno = EFAULT;
		return -1;
	}
	hfsMetaReadLock();
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		errno = ENODEV;
		return -1;
	}
	if (hfsMetaResolvePath(g_fs, path) == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		return -1;
	}
	if (hfsRefreshCapacity(g_fs) != 0) {
		pthread_rwlock_unlock(&g_metaLock);
		return -1;
	}
	hfs_fill_statvfs(g_fs, st);
	pthread_rwlock_unlock(&g_metaLock);
	return 0;
}

/* Return statvfs information for an HFS fd. */
int hfs_lib_fstatvfs(int fd, struct statvfs *st) {
	HfsOpenFile *openFile;

	if (st == NULL) {
		errno = EFAULT;
		return -1;
	}
	openFile = hfsFdAcquire(fd);
	if (openFile == NULL) {
		errno = EBADF;
		return -1;
	}
	hfsMetaReadLock();
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		hfs_release_open_file(openFile);
		errno = ENODEV;
		return -1;
	}
	if (hfsRefreshCapacity(g_fs) != 0) {
		pthread_rwlock_unlock(&g_metaLock);
		hfs_release_open_file(openFile);
		return -1;
	}
	hfs_fill_statvfs(g_fs, st);
	pthread_rwlock_unlock(&g_metaLock);
	hfs_release_open_file(openFile);
	return 0;
}

/* Return Linux statfs information for an existing managed path. */
int hfs_lib_statfs(const char *path, struct statfs *st) {
	if (st == NULL) {
		errno = EFAULT;
		return -1;
	}
	hfsMetaReadLock();
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		errno = ENODEV;
		return -1;
	}
	if (hfsMetaResolvePath(g_fs, path) == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		return -1;
	}
	if (hfsRefreshCapacity(g_fs) != 0) {
		pthread_rwlock_unlock(&g_metaLock);
		return -1;
	}
	hfs_fill_statfs(g_fs, st);
	pthread_rwlock_unlock(&g_metaLock);
	return 0;
}

/* Return Linux statfs information for an HFS fd. */
int hfs_lib_fstatfs(int fd, struct statfs *st) {
	HfsOpenFile *openFile;

	if (st == NULL) {
		errno = EFAULT;
		return -1;
	}
	openFile = hfsFdAcquire(fd);
	if (openFile == NULL) {
		errno = EBADF;
		return -1;
	}
	hfsMetaReadLock();
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		hfs_release_open_file(openFile);
		errno = ENODEV;
		return -1;
	}
	if (hfsRefreshCapacity(g_fs) != 0) {
		pthread_rwlock_unlock(&g_metaLock);
		hfs_release_open_file(openFile);
		return -1;
	}
	hfs_fill_statfs(g_fs, st);
	pthread_rwlock_unlock(&g_metaLock);
	hfs_release_open_file(openFile);
	return 0;
}
