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
#include "hfs_internal.h"
#include "hfs_inode_runtime.h"
#include "hfs_io.h"
#include "hfs_lib.h"
#include "hfs_log.h"
#include "hfs_meta.h"
#include "hfs_profile.h"
#include "hfs_runtime.h"
#include "hfs_stats.h"
#include "hfs_trace.h"

/* Protects the mounted inode table and metadata snapshots. */
static pthread_rwlock_t g_metaLock = PTHREAD_RWLOCK_INITIALIZER;
#define HFS_TRACE_META_LOCK_DEPTH 16U
#define HFS_TRACE_SYNC_LOCK_DEPTH 16U
/* Keep metadata and sync-lock hold tokens on the owning business thread. */
static _Thread_local HfsTraceLockToken g_metaTraceTokens[HFS_TRACE_META_LOCK_DEPTH];
static _Thread_local uint32_t g_metaTraceDepth;
static _Thread_local HfsTraceLockToken g_syncTraceTokens[HFS_TRACE_SYNC_LOCK_DEPTH];
static _Thread_local uint32_t g_syncTraceDepth;
/* Protects final descriptions whose SPDK close or orphan delete must retry. */
static pthread_mutex_t g_deferredCloseLock = PTHREAD_MUTEX_INITIALIZER;

/* Reserve one metadata lock token and publish its wait-begin event. */
static HfsTraceLockToken *hfsMetaTracePush(void) {
	HfsTraceLockToken *token;

	if (!hfsTraceEnabledFast() ||
	    g_metaTraceDepth >= HFS_TRACE_META_LOCK_DEPTH) {
		return NULL;
	}
	token = &g_metaTraceTokens[g_metaTraceDepth++];
	hfsTraceLockWaitBegin(token, HFS_TRACE_LOCK_METADATA, 0,
			HFS_TRACE_OPERATION_METADATA, 0, 0, 0, 0, 0);
	token->lockKey = (uintptr_t)&g_metaLock;
	return token;
}

/* Release the most recent metadata lock token after its protected section. */
static void hfsMetaTracePop(void) {
	HfsTraceLockToken *token;

	if (g_metaTraceDepth == 0) {
		return;
	}
	token = &g_metaTraceTokens[g_metaTraceDepth - 1U];
	hfsTraceLockReleased(token, 0);
	g_metaTraceDepth--;
}

/* Reserve one inode sync-lock token and publish its wait-begin event. */
static HfsTraceLockToken *hfsSyncTracePush(HfsInodeRuntime *runtime) {
	HfsTraceLockToken *token;

	if (runtime == NULL || !hfsTraceEnabledFast() ||
	    g_syncTraceDepth >= HFS_TRACE_SYNC_LOCK_DEPTH) {
		return NULL;
	}
	token = &g_syncTraceTokens[g_syncTraceDepth++];
	hfsTraceLockWaitBegin(token, HFS_TRACE_LOCK_SYNC, 0,
			HFS_TRACE_OPERATION_LOCK, 0, runtime->inodeId, 0, 0, 0);
	token->lockKey = (uintptr_t)&runtime->syncLock;
	return token;
}

/* Release the most recent inode sync-lock token after its protected section. */
static void hfsSyncTracePop(pthread_rwlock_t *lock) {
	HfsTraceLockToken *token;

	if (g_syncTraceDepth == 0) {
		return;
	}
	token = &g_syncTraceTokens[g_syncTraceDepth - 1U];
	if (token->lockKey != (uintptr_t)lock) {
		return;
	}
	hfsTraceLockReleased(token, 0);
	g_syncTraceDepth--;
}

/* Acquire an inode sync write lock with explicit wait timing. */
static int hfsLibSyncWriteLock(HfsInodeRuntime *runtime) {
	HfsTraceLockToken *token = hfsSyncTracePush(runtime);
	int rc = pthread_rwlock_wrlock(&runtime->syncLock);

	hfsTraceLockAcquired(token, rc);
	return rc;
}

/* Count metadata read-lock acquisitions without extending lock ownership. */
static void hfsMetaReadLock(void) {
	HfsTraceLockToken *token = hfsMetaTracePush();
	int rc;

	HFS_STATS_META_READ_LOCK();
	hfsStatsLockBeforeFast(HFS_STATS_LOCK_META);
	rc = pthread_rwlock_rdlock(&g_metaLock);
	hfsStatsLockAfterFast(HFS_STATS_LOCK_META);
	hfsTraceLockAcquired(token, rc);
}

/* Count metadata write-lock acquisitions without extending lock ownership. */
static void hfsMetaWriteLock(void) {
	HfsTraceLockToken *token = hfsMetaTracePush();
	int rc;

	HFS_STATS_META_WRITE_LOCK();
	hfsStatsLockBeforeFast(HFS_STATS_LOCK_META);
	rc = pthread_rwlock_wrlock(&g_metaLock);
	hfsStatsLockAfterFast(HFS_STATS_LOCK_META);
	hfsTraceLockAcquired(token, rc);
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
	if (lock == &g_metaLock) {
		hfsMetaTracePop();
	} else {
		hfsSyncTracePop(lock);
	}
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

/* Coalesce overlapping durability calls for one inode into a single epoch. */
static pthread_mutex_t g_durabilityLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_durabilityDone = PTHREAD_COND_INITIALIZER;
static bool g_durabilityRunning;
static bool g_durabilityStrong;
static uint64_t g_durabilityInodeId;
static uint64_t g_durabilityGeneration;
static int g_durabilityResult;
static int g_durabilityErrno;

static int hfs_lib_process_durability_batch(
	Hfs *fs, HfsDurabilityRequest **requests, size_t requestCount);

/* Group commit is enabled by default and can be disabled for A/B comparison. */
static bool hfs_durability_group_commit_enabled(void) {
	const char *value = getenv("HFS_DURABILITY_GROUP_COMMIT");

	return value == NULL || strcmp(value, "0") != 0;
}

/* Trace one durability condition-variable wait without changing its semantics. */
static int hfsDurabilityCondWait(void) {
	HfsTraceWaitToken traceWait = {0};
	int rc;

	hfsTraceThreadWaitBegin(&traceWait, HFS_TRACE_OPERATION_METADATA,
		HFS_TRACE_WAKE_DURABILITY, HFS_TRACE_META_OP_FLUSH, 0,
		g_durabilityInodeId, 0, 0, 0, 0, 0, 0);
	rc = pthread_cond_wait(&g_durabilityDone, &g_durabilityLock);
	hfsTraceThreadWaitEnd(&traceWait, rc);
	return rc;
}

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

/* Close one optional profile while preserving the caller's errno value. */
static void hfsProfileEndCall(HfsIoProfile *profile, ssize_t result)
{
	int savedErrno;

	if (profile == NULL || !profile->enabled) {
		return;
	}
	savedErrno = errno;
	hfsProfileFinish(profile, result < 0 ? -savedErrno : 0);
	hfsProfileSetCurrent(NULL);
	errno = savedErrno;
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

/* Copy one logical iovec range into a bounded DMA staging buffer. */
static void hfsIovCopyIn(const struct iovec *iov, int iovcnt,
			 size_t logicalOffset, uint8_t *destination, size_t bytes) {
	int i;
	size_t skipped = logicalOffset;
	size_t copied = 0;

	for (i = 0; i < iovcnt && copied < bytes; i++) {
		size_t available = iov[i].iov_len;
		size_t take;

		if (skipped >= available) {
			skipped -= available;
			continue;
		}
		available -= skipped;
		take = bytes - copied < available ? bytes - copied : available;
		memcpy(destination + copied,
		       (const uint8_t *)iov[i].iov_base + skipped, take);
		copied += take;
		skipped = 0;
	}
}

/* Copy one logical range from a bounded DMA staging buffer to iovec segments. */
static void hfsIovCopyOut(const uint8_t *source, const struct iovec *iov,
			  int iovcnt, size_t logicalOffset, size_t bytes) {
	int i;
	size_t skipped = logicalOffset;
	size_t copied = 0;

	for (i = 0; i < iovcnt && copied < bytes; i++) {
		size_t available = iov[i].iov_len;
		size_t take;

		if (skipped >= available) {
			skipped -= available;
			continue;
		}
		available -= skipped;
		take = bytes - copied < available ? bytes - copied : available;
		memcpy((uint8_t *)iov[i].iov_base + skipped,
		       source + copied, take);
		copied += take;
		skipped = 0;
	}
}

/* Coalesce mixed vector segments into bounded requests to avoid per-segment RMW. */
static ssize_t hfsReadvBounceAt(HfsOpenFile *openFile,
				const struct iovec *iov, int iovcnt,
				size_t total, uint64_t offset, uint64_t fileSize) {
	const size_t chunkBytes = 1U << 20;
	uint8_t *staging;
	size_t available;
	size_t done = 0;

	if (offset >= fileSize) {
		return 0;
	}
	available = total;
	if ((uint64_t)available > fileSize - offset) {
		available = (size_t)(fileSize - offset);
	}
	staging = hfsIoDmaAlloc(chunkBytes, 0);
	if (staging == NULL) {
		return -1;
	}
	while (done < available) {
		size_t requestBytes = available - done;
		ssize_t n;

		if (requestBytes > chunkBytes) {
			requestBytes = chunkBytes;
		}
		n = hfsReadAt(openFile->file, staging, requestBytes,
			       offset + done);
		if (n < 0) {
			hfsIoDmaFree(staging);
			return done != 0 ? (ssize_t)done : -1;
		}
		hfsIovCopyOut(staging, iov, iovcnt, done, (size_t)n);
		done += (size_t)n;
		if ((size_t)n != requestBytes) {
			break;
		}
	}
	hfsIoDmaFree(staging);
	return (ssize_t)done;
}

/* Coalesce mixed vector segments into bounded writes with one edge decision. */
static ssize_t hfsWritevBounceAt(HfsOpenFile *openFile,
				 const struct iovec *iov, int iovcnt,
				 size_t total, uint64_t offset) {
	const size_t chunkBytes = 1U << 20;
	uint8_t *staging;
	size_t done = 0;

	staging = hfsIoDmaAlloc(chunkBytes, 0);
	if (staging == NULL) {
		return -1;
	}
	while (done < total) {
		size_t requestBytes = total - done;
		ssize_t n;

		if (requestBytes > chunkBytes) {
			requestBytes = chunkBytes;
		}
		hfsIovCopyIn(iov, iovcnt, done, staging, requestBytes);
		n = hfsWriteAt(openFile->file, staging, requestBytes,
				 offset + done);
		if (n < 0) {
			hfsIoDmaFree(staging);
			return done != 0 ? (ssize_t)done : -1;
		}
		done += (size_t)n;
		if ((size_t)n != requestBytes) {
			break;
		}
	}
	hfsIoDmaFree(staging);
	return (ssize_t)done;
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
	ssize_t direct;

	/* Use SPDK native vector I/O when every segment is DMA-addressable. */
	if (offset % openFile->file->fs->unitSize == 0 && offset <= fileSize &&
	    (uint64_t)total <= fileSize - offset) {
		direct = hfsIoReadvAt(openFile->file, iov, iovcnt, total, offset);
		if (direct >= 0 || errno != EAGAIN) {
			return direct;
		}
	}

	return hfsReadvBounceAt(openFile, iov, iovcnt, total, offset,
					fileSize);
}

/* Write iovec segments directly from caller buffers through request-local DMA. */
static ssize_t hfsWritevAt(HfsOpenFile *openFile, const struct iovec *iov,
			   int iovcnt, size_t total, uint64_t offset) {
	ssize_t direct;

	/* Use SPDK native vector I/O before falling back to per-segment copies. */
	if (offset % openFile->file->fs->unitSize == 0) {
		direct = hfsIoWritevAt(openFile->file, iov, iovcnt, total, offset);
		if (direct >= 0 || errno != EAGAIN) {
			return direct;
		}
	}

	return hfsWritevBounceAt(openFile, iov, iovcnt, total, offset);
}

/* Serialize EOF-extension writes while their zero-filled gaps are materialized. */
static int hfsPrepareWriteGap(HfsOpenFile *openFile, uint64_t offset,
			      uint64_t end, bool *lockHeld) {
	HfsInodeRuntime *runtime;
	uint64_t logicalSize;

	if (lockHeld == NULL || openFile == NULL || openFile->file == NULL ||
		end < offset) {
		errno = end < offset ? EOVERFLOW : EINVAL;
		return -1;
	}
	*lockHeld = false;
	runtime = openFile->runtime;
	if (runtime == NULL) {
		return hfsResizeTo(openFile->file, end);
	}
	logicalSize = hfsInodeRuntimeGetLogicalSize(runtime);
	if (end <= logicalSize) {
		return hfsResizeTo(openFile->file, end);
	}
	pthread_mutex_lock(&runtime->writeGapLock);
	logicalSize = hfsInodeRuntimeGetLogicalSize(runtime);
	if (hfsResizeTo(openFile->file, end) != 0) {
		pthread_mutex_unlock(&runtime->writeGapLock);
		return -1;
	}
	if (offset > logicalSize &&
		hfsIoZeroRange(openFile->file, logicalSize, offset - logicalSize) != 0) {
		pthread_mutex_unlock(&runtime->writeGapLock);
		return -1;
	}
	*lockHeld = true;
	return 0;
}

/* Release the EOF-extension gate after write data and inode size publication. */
static void hfsReleaseWriteGap(HfsOpenFile *openFile, bool lockHeld) {
	if (lockHeld && openFile != NULL && openFile->runtime != NULL) {
		pthread_mutex_unlock(&openFile->runtime->writeGapLock);
	}
}

/* Publish a non-extending write through runtime state without global locking. */
static bool hfsPublishWriteEndFast(HfsOpenFile *openFile, uint64_t end) {
	HfsInodeRuntime *runtime;
	uint64_t logicalSize;
	uint64_t nowNs;
	uint64_t traceStartNs;

	if (openFile == NULL) {
		return false;
	}
	runtime = openFile->runtime;
	if (runtime == NULL) {
		return false;
	}
	logicalSize = hfsInodeRuntimeGetLogicalSize(runtime);
	if (end > logicalSize) {
		return false;
	}
	traceStartNs = hfsTraceNowNs();
	nowNs = hfs_now_ns();
	/* The data path is complete; only timestamp publication is deferred. */
	hfsInodeRuntimeRecordMetadata(runtime, nowNs, nowNs);
	atomic_store_explicit(&openFile->mtimeNs, nowNs, memory_order_release);
	atomic_store_explicit(&openFile->ctimeNs, nowNs, memory_order_release);
	hfsTraceEmitDetail(HFS_TRACE_METADATA_PUBLISH,
		HFS_TRACE_OPERATION_METADATA, HFS_TRACE_BUFFER_NONE, 0, 0,
		openFile->inodeId, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		HFS_TRACE_META_OP_PUBLISH,
		traceStartNs != 0 && hfsTraceNowNs() >= traceStartNs
			? hfsTraceNowNs() - traceStartNs : 0,
		0);
	return true;
}

/* Update the in-memory inode while the metadata write lock is held. */
static void hfsPublishWriteEndLocked(HfsOpenFile *openFile, uint64_t end) {
	struct hfs_inode *in;
	uint64_t nowNs = hfs_now_ns();
	uint64_t traceStartNs = hfsTraceNowNs();
	bool published = false;

	/* A size-changing write supersedes any older deferred timestamp. */
	hfsInodeRuntimeDiscardPending(openFile == NULL ? NULL : openFile->runtime);

	in = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
	if (in != NULL) {
		if (end > in->size) {
			in->size = end;
		}
		/* Every successful content write changes mtime and ctime. */
		in->mtimeNs = nowNs;
		in->ctimeNs = nowNs;
		hfsMetaMarkInodeDirty(g_fs, in->id);
		hfsInodeRuntimeSetLogicalSize(openFile->runtime, in->size);
		atomic_store_explicit(&openFile->size, in->size,
				      memory_order_relaxed);
		atomic_store_explicit(&openFile->mtimeNs, in->mtimeNs,
				      memory_order_relaxed);
		atomic_store_explicit(&openFile->ctimeNs, nowNs, memory_order_relaxed);
		published = true;
	} else if (in == NULL && hfsFdGetOrphanInfo(openFile, NULL, NULL, NULL)) {
		uint64_t size = atomic_load_explicit(&openFile->size,
					     memory_order_relaxed);

		if (end > size) {
			atomic_store_explicit(&openFile->size, end,
					      memory_order_relaxed);
		}
		hfsInodeRuntimeSetLogicalSize(openFile->runtime,
					      atomic_load_explicit(&openFile->size,
								memory_order_relaxed));
		atomic_store_explicit(&openFile->mtimeNs, nowNs,
				      memory_order_relaxed);
		atomic_store_explicit(&openFile->ctimeNs, nowNs, memory_order_relaxed);
		published = true;
	}
	hfsTraceEmitDetail(HFS_TRACE_METADATA_PUBLISH,
		HFS_TRACE_OPERATION_METADATA, HFS_TRACE_BUFFER_NONE, 0, 0,
		openFile->inodeId, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		HFS_TRACE_META_OP_PUBLISH,
		traceStartNs != 0 && hfsTraceNowNs() >= traceStartNs
			? hfsTraceNowNs() - traceStartNs : 0,
		published ? 0 : -ENOENT);
}

/* Publish a successful write end for both linked and unlinked open files. */
static void hfsPublishWriteEnd(HfsOpenFile *openFile, uint64_t end) {
	if (hfsPublishWriteEndFast(openFile, end)) {
		return;
	}
	hfsMetaWriteLock();
	hfsPublishWriteEndLocked(openFile, end);
	pthread_rwlock_unlock(&g_metaLock);
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
	hfsTraceInit();
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
	if (hfs_durability_group_commit_enabled() &&
		hfsDurabilityCoordinatorInit(g_fs,
			hfs_lib_process_durability_batch) != 0) {
		HFS_LOG_ERROR("durability coordinator init failed");
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
	/* Stop new group commits before taking the final metadata lock. */
	if (g_fs != NULL) {
		hfsDurabilityCoordinatorStop(g_fs);
	}
	hfsMetaWriteLock();
	if (g_fs == NULL) {
		hfsDurabilityTraceFini();
		hfsTraceFini();
		hfsStatsFini();
		pthread_rwlock_unlock(&g_metaLock);
		return;
	}
	/* Persist dirty inode sizes before closing the last open descriptions. */
	if (atomic_load_explicit(&g_fs->metaDirty, memory_order_acquire) &&
	    hfsMetaSaveAll(g_fs) != 0) {
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
	hfsTraceFini();
	hfsStatsFini();
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
		hfsInodeRuntimeDiscardPending(file->runtime);
	}
	if (current != NULL) {
		/* Seed the shared EOF before publishing the new open description. */
		hfsInodeRuntimeSetLogicalSize(file->runtime, current->size);
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
	bool writeGapHeld = false;
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
	if (hfsPrepareWriteGap(openFile, writeOffset,
		writeOffset + (uint64_t)count, &writeGapHeld) != 0) {
		pthread_mutex_unlock(&openFile->ioLock);
		if (append) {
			pthread_mutex_unlock(&openFile->runtime->appendLock);
		}
		hfs_release_open_file(openFile);
		return -1;
	}
	n = hfsWriteAt(openFile->file, buf, count, writeOffset);
	if (n <= 0) {
		savedErrno = errno;
		HFS_LOG_ERROR("sequential write failed n=%zd errno=%d", n, savedErrno);
		hfsReleaseWriteGap(openFile, writeGapHeld);
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
	hfsReleaseWriteGap(openFile, writeGapHeld);
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
	HfsIoProfile profile = {0};

	if (count == 0) {
		return 0;
	}
	if (hfsProfileBegin(&profile, false, count)) {
		hfsProfileSetCurrent(&profile);
	}
	if (offset < 0) {
		errno = EINVAL;
		hfsProfileEndCall(&profile, -1);
		return -1;
	}

	openFile = hfs_acquire_file(fd, true);
	if (openFile == NULL) {
		hfsProfileEndCall(&profile, -1);
		return -1;
	}
	hfsProfileParentStage(HFS_PROFILE_T1);
	hfsMetaReadLock();
	in = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
	if (g_fs == NULL) {
		pthread_rwlock_unlock(&g_metaLock);
		hfs_release_open_file(openFile);
		errno = ENODEV;
		hfsProfileEndCall(&profile, -1);
		return -1;
	}
	if (in == NULL && !hfsFdGetOrphanInfo(openFile, &fileSize, NULL, NULL)) {
		pthread_rwlock_unlock(&g_metaLock);
		hfs_release_open_file(openFile);
		errno = ENOENT;
		hfsProfileEndCall(&profile, -1);
		return -1;
	}
	if (in != NULL) {
		fileSize = in->size;
	}
	if ((uint64_t)offset >= fileSize) {
		pthread_rwlock_unlock(&g_metaLock);
		hfs_release_open_file(openFile);
		hfsProfileEndCall(&profile, 0);
		return 0;
	}
	if ((uint64_t)offset + count > fileSize) {
		count = (size_t)(fileSize - (uint64_t)offset);
	}

	pthread_rwlock_unlock(&g_metaLock);
	n = hfsReadAt(openFile->file, buf, count, (uint64_t)offset);
	hfs_release_open_file(openFile);
	hfsProfileEndCall(&profile, n);
	return n;
}

/* Write at an explicit offset without moving the fd offset. */
ssize_t hfs_lib_pwrite(int fd, const void *buf, size_t count, off_t offset) {
	HfsOpenFile *openFile;
	struct hfs_inode *in;
	uint64_t writeOffset;
	bool append;
	bool writeGapHeld = false;
	ssize_t n;
	int savedErrno;
	HfsIoProfile profile = {0};

	if (count == 0) {
		return 0;
	}
	if (hfsProfileBegin(&profile, true, count)) {
		hfsProfileSetCurrent(&profile);
	}
	if (offset < 0) {
		errno = EINVAL;
		hfsProfileEndCall(&profile, -1);
		return -1;
	}

	openFile = hfs_acquire_file(fd, true);
	if (openFile == NULL) {
		hfsProfileEndCall(&profile, -1);
		return -1;
	}
	hfsProfileParentStage(HFS_PROFILE_T1);
	if (!openFile->writable) {
		hfs_release_open_file(openFile);
		errno = EBADF;
		hfsProfileEndCall(&profile, -1);
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
			hfsProfileEndCall(&profile, -1);
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
			hfsProfileEndCall(&profile, -1);
			return -1;
		}
		pthread_rwlock_unlock(&g_metaLock);
	}
	if (hfsPrepareWriteGap(openFile, writeOffset,
		writeOffset + (uint64_t)count, &writeGapHeld) != 0) {
		if (append) {
			pthread_mutex_unlock(&openFile->runtime->appendLock);
		}
		hfs_release_open_file(openFile);
		return -1;
	}
	n = hfsWriteAt(openFile->file, buf, count, writeOffset);
	if (n <= 0) {
		savedErrno = errno;
		HFS_LOG_ERROR("positioned write failed n=%zd errno=%d", n, savedErrno);
		hfsReleaseWriteGap(openFile, writeGapHeld);
		if (append) {
			pthread_mutex_unlock(&openFile->runtime->appendLock);
		}
		hfs_release_open_file(openFile);
		errno = n < 0 ? savedErrno : EIO;
		hfsProfileEndCall(&profile, -1);
		return -1;
	}
	hfsPublishWriteEnd(openFile, writeOffset + (uint64_t)n);
	hfsReleaseWriteGap(openFile, writeGapHeld);
	if (append) {
		pthread_mutex_unlock(&openFile->runtime->appendLock);
	}
	hfs_release_open_file(openFile);
	hfsProfileEndCall(&profile, n);
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
	bool writeGapHeld = false;

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
	if (hfsPrepareWriteGap(openFile, writeOffset,
		writeOffset + (uint64_t)total, &writeGapHeld) != 0) {
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
	hfsReleaseWriteGap(openFile, writeGapHeld);
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
	bool writeGapHeld = false;

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
	if (hfsPrepareWriteGap(openFile, writeOffset,
		writeOffset + (uint64_t)total, &writeGapHeld) != 0) {
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
	hfsReleaseWriteGap(openFile, writeGapHeld);
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
	hfsLibSyncWriteLock(runtime);
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
			hfsInodeRuntimeDiscardPending(runtime);
			hfsInodeRuntimeSetLogicalSize(runtime, (uint64_t)length);
			atomic_store_explicit(&openFile->size, (uint64_t)length,
					      memory_order_relaxed);
			atomic_store_explicit(&openFile->mtimeNs, in->mtimeNs,
					      memory_order_relaxed);
		}
	} else if (hfsFdGetOrphanInfo(openFile, NULL, NULL, NULL)) {
		hfsInodeRuntimeDiscardPending(runtime);
		hfsInodeRuntimeSetLogicalSize(runtime, (uint64_t)length);
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
	HfsInodeRuntime *runtime;
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
		runtime = openFile->runtime;
		if (runtime != NULL) {
			uint64_t logicalSize = hfsInodeRuntimeGetLogicalSize(runtime);
			uint64_t pendingMtimeNs;
			uint64_t pendingCtimeNs;

			if (logicalSize > snapshot.size) {
				snapshot.size = logicalSize;
			}
			if (hfsInodeRuntimeGetPendingMetadata(runtime,
						       &pendingMtimeNs,
						       &pendingCtimeNs)) {
				if (pendingMtimeNs > snapshot.mtimeNs) {
					snapshot.mtimeNs = pendingMtimeNs;
				}
				if (pendingCtimeNs > snapshot.ctimeNs) {
					snapshot.ctimeNs = pendingCtimeNs;
				}
			}
		}
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
 * Flush one HFS fd, optionally stopping after its data barrier.
 * The second sync flushes the metadata transaction committed after data.
 */
static int hfs_lib_fsync_open_file(HfsOpenFile *openFile, bool dataOnly) {
	struct hfs_inode *inode;
	HfsInodeRuntime *runtime = NULL;
	bool ioLocked = false;
	int savedErrno;

	pthread_mutex_lock(&openFile->ioLock);
	if (openFile->file != NULL) {
		runtime = openFile->runtime;
		if (runtime == NULL) {
			pthread_mutex_unlock(&openFile->ioLock);
			hfs_release_open_file(openFile);
			errno = EIO;
			return -1;
		}
		hfsLibSyncWriteLock(runtime);
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
	if (dataOnly && openFile->file != NULL && runtime != NULL) {
		bool needsMetadata;

		/* Size/extent changes are queued in the inode dirty set; timestamps alone
		 * are intentionally allowed to remain pending for fdatasync. */
		hfsMetaReadLock();
		inode = g_fs == NULL ? NULL : hfsMetaFindById(g_fs, openFile->inodeId);
		needsMetadata = g_fs == NULL || inode == NULL ||
			hfsMetaInodeDirty(g_fs, openFile->inodeId) ||
			hfsInodeRuntimeGetLogicalSize(runtime) != inode->size;
		pthread_rwlock_unlock(&g_metaLock);
		if (!needsMetadata) {
			hfsStatsRecordFdatasyncFastPath();
			hfsDurabilityTraceRecord(openFile->inodeId, "data-only-skip", 0);
			if (ioLocked) {
				pthread_rwlock_unlock(&runtime->syncLock);
			}
			pthread_mutex_unlock(&openFile->ioLock);
			hfs_release_open_file(openFile);
			return 0;
		}
	}
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
	/* Fold lock-free write timestamps into the next durable metadata epoch. */
	hfsInodeRuntimeDrainPendingLocked(g_fs);
	if (!atomic_load_explicit(&g_fs->metaDirty, memory_order_acquire)) {
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

/* One unique runtime/blob target admitted into a durability epoch. */
typedef struct HfsDurabilityTarget {
	HfsOpenFile *openFile;
	HfsFile *file;
	HfsInodeRuntime *runtime;
	uint64_t inodeId;
} HfsDurabilityTarget;

/* Process one epoch without holding per-file locks across device waits. */
static int hfs_lib_process_durability_batch(
	Hfs *fs, HfsDurabilityRequest **requests, size_t requestCount) {
	HfsDurabilityTarget *targets;
	size_t targetCount = 0;
	size_t i;
	int savedErrno = 0;
	int rc = 0;
	bool metadataNeeded = false;
	bool strongRequest = false;
	bool allDataOnly = true;
	bool combinedBarrier = false;

	if (fs == NULL || requests == NULL || requestCount == 0) {
		return -EINVAL;
	}
	for (i = 0; i < requestCount; i++) {
		if (!requests[i]->dataOnly) {
			allDataOnly = false;
			break;
		}
	}
	hfsStatsRecordDurabilityBatchFast(requestCount);
	targets = calloc(requestCount, sizeof(*targets));
	if (targets == NULL) {
		return -ENOMEM;
	}

	/* Admission waits for already-running data I/O, then releases all locks. */
	for (i = 0; i < requestCount; i++) {
		HfsOpenFile *openFile = requests[i]->openFile;
		HfsInodeRuntime *runtime;
		size_t j;

		if (openFile == NULL) {
			savedErrno = EIO;
			rc = -1;
			break;
		}
		pthread_mutex_lock(&openFile->ioLock);
		runtime = openFile->runtime;
		if (runtime != NULL && hfsLibSyncWriteLock(runtime) != 0) {
			savedErrno = errno != 0 ? errno : EBUSY;
			pthread_mutex_unlock(&openFile->ioLock);
			rc = -1;
			break;
		}
		for (j = 0; j < targetCount; j++) {
			if ((runtime != NULL && targets[j].runtime == runtime) ||
				(runtime == NULL && targets[j].openFile == openFile)) {
				break;
			}
		}
		if (j == targetCount) {
			targets[targetCount].openFile = openFile;
			targets[targetCount].file = openFile->file;
			targets[targetCount].runtime = runtime;
			targets[targetCount].inodeId = openFile->inodeId;
			targetCount++;
		}
		if (runtime != NULL) {
			hfsLibRwlockUnlock(&runtime->syncLock);
		}
		pthread_mutex_unlock(&openFile->ioLock);
	}
	hfsStatsRecordDurabilityDataTargetFast(targetCount);

	/* Stage A: synchronize each unique blob without holding POSIX locks. */
	if (rc == 0) {
		hfsStatsRecordDurabilityDataBatchFast();
		/* Same-size data-only writes need only one device barrier, not blob metadata sync. */
		if (allDataOnly) {
			bool needsMetadata = false;

			hfsMetaReadLock();
			for (i = 0; i < targetCount; i++) {
				struct hfs_inode *inode = hfsMetaFindById(fs,
					targets[i].inodeId);

				if (inode == NULL ||
					hfsMetaInodeDirty(fs, targets[i].inodeId) ||
					(targets[i].runtime != NULL &&
					 hfsInodeRuntimeGetLogicalSize(targets[i].runtime) !=
					 inode->size)) {
					needsMetadata = true;
					break;
				}
			}
			hfsLibRwlockUnlock(&g_metaLock);
			if (!needsMetadata) {
				hfsStatsRecordDurabilityFlushFast();
				if (hfsDeviceFlush(fs) != 0) {
					savedErrno = errno != 0 ? errno : EIO;
					rc = -1;
				} else {
					combinedBarrier = true;
				}
			}
		}
		if (rc == 0 && !combinedBarrier) {
			for (i = 0; i < targetCount; i++) {
				if (targets[i].file != NULL && hfsSyncData(targets[i].file) != 0) {
					savedErrno = errno != 0 ? errno : EIO;
					rc = -1;
					break;
				}
			}
		}
		for (i = 0; i < requestCount; i++) {
			hfsDurabilityTraceRecordEpoch(requests[i]->epochId,
					requests[i]->openFile->inodeId,
					"data-barrier", rc == 0 ? 0 : -savedErrno);
		}
	}

	/* Stage B: publish one metadata image for the whole epoch. */
	if (rc == 0) {
		hfsFsyncCrashPoint(1);
		if (!combinedBarrier) {
			hfsMetaWriteLock();
			if (g_fs == NULL || g_fs != fs) {
				savedErrno = ENODEV;
				rc = -1;
			} else {
				for (i = 0; i < requestCount; i++) {
					if (!requests[i]->dataOnly) {
						strongRequest = true;
						break;
					}
				}
				/* A data-only barrier needs metadata only for this inode's size/extent. */
				if (!strongRequest) {
					for (i = 0; i < targetCount; i++) {
						struct hfs_inode *inode = hfsMetaFindById(fs,
							targets[i].inodeId);

						if (inode == NULL ||
							hfsMetaInodeDirty(fs, targets[i].inodeId) ||
							(targets[i].runtime != NULL &&
							 hfsInodeRuntimeGetLogicalSize(targets[i].runtime) !=
							 inode->size)) {
							metadataNeeded = true;
							break;
						}
					}
				} else {
					metadataNeeded = true;
				}
				/* Merge timestamp overlays only when a metadata image is required. */
				if (metadataNeeded) {
					hfsInodeRuntimeDrainPendingLocked(fs);
				}
				if (metadataNeeded) {
					hfsStatsRecordDurabilityMetadataCommitFast();
					if (hfsMetaCommitDirty(fs) != 0) {
						savedErrno = errno != 0 ? errno : EIO;
						rc = -1;
					}
				}
				if (!metadataNeeded && !strongRequest) {
					hfsStatsRecordFdatasyncFastPath();
				}
			}
			hfsLibRwlockUnlock(&g_metaLock);
		} else {
			/* The combined data-only barrier already proved metadata is unchanged. */
			hfsStatsRecordFdatasyncFastPath();
			for (i = 0; i < requestCount; i++) {
				hfsDurabilityTraceRecordEpoch(requests[i]->epochId,
					requests[i]->openFile->inodeId,
					"metadata-skip", 0);
			}
		}
		for (i = 0; i < requestCount; i++) {
			if (!combinedBarrier) {
				hfsDurabilityTraceRecordEpoch(requests[i]->epochId,
					requests[i]->openFile->inodeId,
					metadataNeeded ? "metadata-commit" : "metadata-skip",
					rc == 0 ? 0 : -savedErrno);
			}
		}
	}

	/* Stage C: issue one durable device barrier without per-file locks. */
	if (rc == 0) {
		hfsFsyncCrashPoint(2);
		if (!combinedBarrier) {
			hfsStatsRecordDurabilityFlushFast();
			if (hfsDeviceFlush(fs) != 0) {
				savedErrno = errno != 0 ? errno : EIO;
				rc = -1;
			}
		}
		for (i = 0; i < requestCount; i++) {
			hfsDurabilityTraceRecordEpoch(requests[i]->epochId,
				requests[i]->openFile->inodeId,
				"post-metadata-barrier", rc == 0 ? 0 : -savedErrno);
		}
	}
	if (rc == 0) {
		hfsFsyncCrashPoint(3);
	}

	for (i = 0; i < requestCount; i++) {
		if (rc == 0) {
			requests[i]->result = 0;
			requests[i]->error = 0;
		} else {
			requests[i]->result = -1;
			requests[i]->error = savedErrno != 0 ? savedErrno : EIO;
		}
	}
	free(targets);
	return 0;
}

/* Join overlapping flushes for one open-file description into one epoch. */
static int hfs_lib_durability_call_legacy(int fd, bool dataOnly) {
	HfsOpenFile *probe;
	uint64_t inodeId;
	bool strong = !dataOnly;

	probe = hfsFdAcquire(fd);
	if (probe == NULL) {
		errno = EBADF;
		return -1;
	}
	inodeId = probe->inodeId;

	for (;;) {
		uint64_t generation;
		int rc;
		int savedErrno;

		pthread_mutex_lock(&g_durabilityLock);
		if (!g_durabilityRunning) {
			g_durabilityRunning = true;
			g_durabilityStrong = strong;
			g_durabilityInodeId = inodeId;
			generation = g_durabilityGeneration;
			hfsStatsRecordDurabilityLeaderFast();
			pthread_mutex_unlock(&g_durabilityLock);

			rc = hfs_lib_fsync_open_file(probe, dataOnly);
			probe = NULL;
			savedErrno = rc == 0 ? 0 : errno;
			pthread_mutex_lock(&g_durabilityLock);
			g_durabilityResult = rc;
			g_durabilityErrno = savedErrno;
			g_durabilityRunning = false;
			g_durabilityGeneration = generation + 1U;
			hfsTraceThreadWake(HFS_TRACE_OPERATION_METADATA,
			HFS_TRACE_WAKE_DURABILITY, HFS_TRACE_META_OP_FLUSH, 0,
			g_durabilityInodeId, 0, 0, 0, 0, 0, 0, 0);
			pthread_cond_broadcast(&g_durabilityDone);
			pthread_mutex_unlock(&g_durabilityLock);
			if (rc != 0) {
				errno = savedErrno;
			}
			return rc;
		}

		if (g_durabilityInodeId == inodeId &&
		    (!strong || g_durabilityStrong)) {
			hfsStatsRecordDurabilityWaiterFast();
			generation = g_durabilityGeneration;
			while (g_durabilityRunning &&
			       g_durabilityGeneration == generation) {
				hfsDurabilityCondWait();
			}
			rc = g_durabilityResult;
			savedErrno = g_durabilityErrno;
			pthread_mutex_unlock(&g_durabilityLock);
			hfs_release_open_file(probe);
			if (rc != 0) {
				errno = savedErrno;
			}
			return rc;
		}

		/* A different inode, or a stronger request, waits then starts its own epoch. */
		while (g_durabilityRunning) {
			hfsDurabilityCondWait();
		}
		pthread_mutex_unlock(&g_durabilityLock);
	}
}

/* Submit one POSIX durability request to the mount-level group-commit worker. */
static int hfs_lib_durability_call(int fd, bool dataOnly) {
	HfsOpenFile *openFile;
	HfsDurabilityRequest request;
	int rc;

	if (g_fs == NULL || g_fs->durability == NULL) {
		return hfs_lib_durability_call_legacy(fd, dataOnly);
	}
	openFile = hfsFdAcquire(fd);
	if (openFile == NULL) {
		errno = EBADF;
		return -1;
	}
	if (hfsDurabilityRequestInit(&request, openFile, dataOnly) != 0) {
		hfs_release_open_file(openFile);
		return -1;
	}
	rc = hfsDurabilityRequestSubmit(g_fs, &request);
	hfsDurabilityRequestFini(&request);
	hfs_release_open_file(openFile);
	return rc;
}

/* Flush data and metadata, coalescing concurrent callers on the same inode. */
int hfs_lib_fsync(int fd) {
	return hfs_lib_durability_call(fd, false);
}

/* Flush data and only size/extent metadata when a checkpoint is required. */
int hfs_lib_fdatasync(int fd) {
	return hfs_lib_durability_call(fd, true);
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
		hfsLibSyncWriteLock(runtime);
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
	hfsLibSyncWriteLock(runtime);
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
	HfsInodeRuntime *runtime;
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
	runtime = hfsInodeRuntimeGet(g_fs, in->id);
	if (runtime != NULL) {
		uint64_t pendingMtimeNs;
		uint64_t pendingCtimeNs;

		if (hfsInodeRuntimeGetPendingMetadata(runtime, &pendingMtimeNs,
						       &pendingCtimeNs)) {
			if (pendingMtimeNs > snapshot.mtimeNs) {
				snapshot.mtimeNs = pendingMtimeNs;
			}
			if (pendingCtimeNs > snapshot.ctimeNs) {
				snapshot.ctimeNs = pendingCtimeNs;
			}
		}
	}
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
