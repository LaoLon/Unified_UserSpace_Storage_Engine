#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

#include <spdk/blob.h>
#include <spdk/env.h>

#include "hfs_internal.h"
#include "hfs_inode_runtime.h"
#include "hfs_io.h"
#include "hfs_log.h"
#include "hfs_stats.h"

/* Keep the hot path bounded while avoiding a 1 MiB lease for tiny requests. */
#define HFS_IO_POOL_CLASS_COUNT	4U
#define HFS_IO_POOL_CLASS_MAX_COUNT	32U
#define HFS_IO_POOL_4K_BYTES	(4U << 10)
#define HFS_IO_POOL_16K_BYTES	(16U << 10)
#define HFS_IO_POOL_64K_BYTES	(64U << 10)
#define HFS_IO_POOL_1M_BYTES	(1U << 20)
#define HFS_IO_MAX_PIPELINE	16U
#define HFS_IO_MAX_VECTOR	32U
#define HFS_IO_DEFAULT_PIPELINE	4U
#define HFS_IO_ASYNC_MAGIC	UINT64_C(0x4846534153594e43)

/* One size-class DMA buffer leased by one request at a time. */
typedef struct HfsIoPoolClass HfsIoPoolClass;
typedef struct hfs_io_pool HfsIoPool;

typedef struct HfsIoLease {
	HfsIoPool *pool;
	HfsIoPoolClass *classPool;
	uint32_t slot;
	uint8_t *buffer;
} HfsIoLease;

/* Storage and free-list state for one DMA size class. */
struct HfsIoPoolClass {
	uint8_t *storage;
	size_t bufferBytes;
	uint32_t count;
	uint32_t freeTop;
	uint32_t freeSlots[HFS_IO_POOL_CLASS_MAX_COUNT];
};

/* Bounded DMA storage shared by all request-local data I/O. */
struct hfs_io_pool {
	pthread_mutex_t lock;
	pthread_cond_t available;
	HfsIoPoolClass classes[HFS_IO_POOL_CLASS_COUNT];
	uint32_t active;
	bool closing;
};

static const size_t g_hfsIoPoolTargetBytes[HFS_IO_POOL_CLASS_COUNT] = {
	HFS_IO_POOL_4K_BYTES, HFS_IO_POOL_16K_BYTES,
	HFS_IO_POOL_64K_BYTES, HFS_IO_POOL_1M_BYTES
};
static const uint32_t g_hfsIoPoolTargetCounts[HFS_IO_POOL_CLASS_COUNT] = {
	32U, 16U, 8U, 16U
};

/* Operation submitted by one request-local SPDK transfer context. */
typedef enum HfsIoOperation {
	HFS_IO_READ,
	HFS_IO_WRITE,
	HFS_IO_UNMAP,
	HFS_IO_WRITE_ZEROES
} HfsIoOperation;

/* Request state retained until the owner-thread callback completes. */
typedef struct HfsIoRequest {
	HfsFile *file;
	void *buffer;
	uint64_t offset;
	uint64_t units;
	HfsIoOperation operation;
	struct iovec *iov;
	int iovcnt;
	bool vector;
	struct spdk_io_channel *channel;
	uint32_t dataReactor;
	bool dataPath;
	HfsRuntimeBatchItem batchItem;
	HfsRequestResult result;
	/* Distinguish worker-free child requests from synchronous stack requests. */
	uint64_t asyncMagic;
	void (*asyncDone)(struct HfsIoRequest *request);
	void *asyncContext;
	uint32_t asyncIndex;
} HfsIoRequest;

/* Set of hashed range buckets held by one request in ascending order. */
typedef struct HfsIoRangeGuard {
	uint16_t buckets[HFS_RANGE_LOCK_COUNT];
	uint16_t count;
	bool write;
	HfsInodeRuntime *runtime;
} HfsIoRangeGuard;

/* Use a per-inode gate when a range would otherwise lock most global buckets. */
#define HFS_IO_RANGE_GATE_THRESHOLD (HFS_RANGE_LOCK_COUNT / 4U)

/* Parent state for one worker-free aligned request and its bounded child batches. */
typedef struct HfsIoAsyncParent {
	HfsFile *file;
	uint8_t *readBuffer;
	const uint8_t *writeBuffer;
	size_t count;
	uint64_t offset;
	uint64_t totalUnits;
	uint64_t nextUnit;
	uint32_t pending;
	uint32_t launched;
	uint32_t depth;
	bool write;
	bool launching;
	bool finished;
	int firstError;
	HfsIoAsyncCallback callback;
	void *context;
	HfsInodeRuntime *runtime;
	struct spdk_io_channel *channel;
	uint32_t dataReactor;
	HfsIoRangeGuard guard;
	HfsIoRequest requests[HFS_IO_MAX_PIPELINE];
	HfsIoLease leases[HFS_IO_MAX_PIPELINE];
	pthread_mutex_t lock;
} HfsIoAsyncParent;

/* Hash a blob/unit pair so unrelated files normally use independent locks. */
static uint32_t hfsIoRangeBucket(const HfsFile *file, uint64_t unitIndex) {
	uint64_t hash = (uint64_t)file->blobId ^ (unitIndex * UINT64_C(0x9e3779b97f4a7c15));

	hash ^= hash >> 30;
	hash *= UINT64_C(0xbf58476d1ce4e5b9);
	hash ^= hash >> 27;
	return (uint32_t)hash & (HFS_RANGE_LOCK_COUNT - 1U);
}

/* Lock all hashed buckets covering a byte range in a deadlock-free order. */
static int hfsIoRangeLock(HfsFile *file, uint64_t offset, uint64_t length,
			  bool write, HfsIoRangeGuard *guard) {
	uint64_t firstUnit;
	uint64_t lastUnit;
	uint64_t unit;
	uint64_t seen[HFS_RANGE_LOCK_COUNT / 64U] = {0};
	uint32_t bucket;
	uint32_t seenCount = 0;
	HfsInodeRuntime *runtime;

	if (file == NULL || file->fs == NULL || file->fs->unitSize == 0 ||
	    guard == NULL || length == 0 || offset > UINT64_MAX - (length - 1)) {
		errno = EINVAL;
		return -1;
	}
	guard->count = 0;
	runtime = hfsInodeRuntimeForFile(file);
	if (runtime == NULL) {
		return -1;
	}
	guard->runtime = runtime;
	firstUnit = offset / file->fs->unitSize;
	lastUnit = (offset + length - 1) / file->fs->unitSize;
	for (unit = firstUnit; unit <= lastUnit; unit++) {
		bucket = hfsIoRangeBucket(file, unit);
		if ((seen[bucket / 64U] & (UINT64_C(1) << (bucket % 64U))) == 0) {
			seen[bucket / 64U] |= UINT64_C(1) << (bucket % 64U);
			seenCount++;
		}
		if (unit == UINT64_MAX || seenCount == HFS_RANGE_LOCK_COUNT) {
			break;
		}
	}
	guard->write = write;
	if (seenCount >= HFS_IO_RANGE_GATE_THRESHOLD) {
		/* A large range is exclusive per inode, but does not block other files. */
		pthread_rwlock_wrlock(&runtime->rangeGate);
		return 0;
	}
	pthread_rwlock_rdlock(&runtime->rangeGate);
	for (bucket = 0; bucket < HFS_RANGE_LOCK_COUNT; bucket++) {
		if ((seen[bucket / 64U] & (UINT64_C(1) << (bucket % 64U))) == 0) {
			continue;
		}
		guard->buckets[guard->count++] = (uint16_t)bucket;
		hfsStatsLockBeforeFast(HFS_STATS_LOCK_IO);
		if (write) {
			pthread_rwlock_wrlock(&file->fs->rangeLocks[bucket]);
		} else {
			pthread_rwlock_rdlock(&file->fs->rangeLocks[bucket]);
		}
		hfsStatsLockAfterFast(HFS_STATS_LOCK_IO);
	}
	return 0;
}

/* Forward declaration used by the non-blocking range-lock rollback path. */
static void hfsIoRangeUnlock(HfsFile *file, HfsIoRangeGuard *guard);

/* Acquire range buckets without sleeping for a worker-free async submit. */
static int hfsIoRangeTryLock(HfsFile *file, uint64_t offset, uint64_t length,
			      bool write, HfsIoRangeGuard *guard) {
	uint64_t firstUnit;
	uint64_t lastUnit;
	uint64_t unit;
	uint64_t seen[HFS_RANGE_LOCK_COUNT / 64U] = {0};
	uint32_t bucket;
	uint32_t seenCount = 0;
	int rc;
	HfsInodeRuntime *runtime;

	if (file == NULL || file->fs == NULL || file->fs->unitSize == 0 ||
	    guard == NULL || length == 0 || offset > UINT64_MAX - (length - 1)) {
		errno = EINVAL;
		return -1;
	}
	guard->count = 0;
	runtime = hfsInodeRuntimeForFile(file);
	if (runtime == NULL) {
		return -1;
	}
	guard->runtime = runtime;
	firstUnit = offset / file->fs->unitSize;
	lastUnit = (offset + length - 1) / file->fs->unitSize;
	for (unit = firstUnit; unit <= lastUnit; unit++) {
		bucket = hfsIoRangeBucket(file, unit);
		if ((seen[bucket / 64U] & (UINT64_C(1) << (bucket % 64U))) == 0) {
			seen[bucket / 64U] |= UINT64_C(1) << (bucket % 64U);
			seenCount++;
		}
		if (unit == UINT64_MAX || seenCount == HFS_RANGE_LOCK_COUNT) {
			break;
		}
	}
	guard->write = write;
	if (seenCount >= HFS_IO_RANGE_GATE_THRESHOLD) {
		rc = pthread_rwlock_trywrlock(&runtime->rangeGate);
		if (rc != 0) {
			errno = EAGAIN;
			return -1;
		}
		return 0;
	}
	rc = pthread_rwlock_tryrdlock(&runtime->rangeGate);
	if (rc != 0) {
		errno = EAGAIN;
		return -1;
	}
	for (bucket = 0; bucket < HFS_RANGE_LOCK_COUNT; bucket++) {
		if ((seen[bucket / 64U] & (UINT64_C(1) << (bucket % 64U))) == 0) {
			continue;
		}
		hfsStatsLockBeforeFast(HFS_STATS_LOCK_IO);
		rc = write ? pthread_rwlock_trywrlock(&file->fs->rangeLocks[bucket])
			    : pthread_rwlock_tryrdlock(&file->fs->rangeLocks[bucket]);
		if (rc != 0) {
			hfsIoRangeUnlock(file, guard);
			errno = EAGAIN;
			return -1;
		}
		hfsStatsLockAfterFast(HFS_STATS_LOCK_IO);
		guard->buckets[guard->count++] = (uint16_t)bucket;
	}
	return 0;
}

/* Release all buckets acquired by one request. */
static void hfsIoRangeUnlock(HfsFile *file, HfsIoRangeGuard *guard) {
	uint16_t i;

	if (file == NULL || file->fs == NULL || guard == NULL) {
		return;
	}
	for (i = guard->count; i > 0; i--) {
		hfsStatsLockReleaseFast(HFS_STATS_LOCK_IO);
		pthread_rwlock_unlock(&file->fs->rangeLocks[guard->buckets[i - 1]]);
	}
	if (guard->runtime != NULL) {
		pthread_rwlock_unlock(&guard->runtime->rangeGate);
	}
	guard->count = 0;
	guard->runtime = NULL;
}

/* Enter the per-inode read side so fsync/truncate can take an exclusive barrier. */
static HfsInodeRuntime *hfsIoSyncReadLock(HfsFile *file) {
	HfsInodeRuntime *runtime = hfsInodeRuntimeForFile(file);

	if (runtime == NULL) {
		return NULL;
	}
	pthread_rwlock_rdlock(&runtime->syncLock);
	return runtime;
}

/* Enter the inode read side without waiting for an async submit. */
static HfsInodeRuntime *hfsIoSyncReadTryLock(HfsFile *file) {
	HfsInodeRuntime *runtime = hfsInodeRuntimeForFile(file);

	if (runtime == NULL) {
		return NULL;
	}
	if (pthread_rwlock_tryrdlock(&runtime->syncLock) != 0) {
		errno = EAGAIN;
		return NULL;
	}
	return runtime;
}

/* Leave the per-inode read side after a data operation. */
static void hfsIoSyncReadUnlock(HfsInodeRuntime *runtime) {
	if (runtime != NULL) {
		pthread_rwlock_unlock(&runtime->syncLock);
	}
}

/* Capture an SPDK completion code before waking the synchronous caller. */
static void hfsIoDone(void *arg, int bserrno);

/* Map a data transfer opcode to the shared request result vocabulary. */
static HfsRequestOperation hfsIoRequestOperation(HfsIoOperation operation) {
	switch (operation) {
	case HFS_IO_READ:
		return HFS_REQUEST_BLOB_READ;
	case HFS_IO_WRITE:
		return HFS_REQUEST_BLOB_WRITE;
	case HFS_IO_UNMAP:
		return HFS_REQUEST_BLOB_UNMAP;
	case HFS_IO_WRITE_ZEROES:
		return HFS_REQUEST_BLOB_WRITE_ZEROES;
	default:
		return HFS_REQUEST_BLOB_READ;
	}
}

/* Return the number of complete I/O units one pool buffer can hold. */
static uint64_t hfsIoMaxUnits(const Hfs *fs) {
	return (uint64_t)(fs->ioPool->classes[HFS_IO_POOL_CLASS_COUNT - 1U].bufferBytes /
		(size_t)fs->unitSize);
}

/* Select the smallest DMA class that can hold one request. */
static HfsIoPoolClass *hfsIoSelectClass(Hfs *fs, size_t requestedBytes) {
	uint32_t i;

	if (fs == NULL || fs->ioPool == NULL || requestedBytes == 0) {
		return NULL;
	}
	for (i = 0; i < HFS_IO_POOL_CLASS_COUNT; i++) {
		if (requestedBytes <= fs->ioPool->classes[i].bufferBytes) {
			return &fs->ioPool->classes[i];
		}
	}
	return NULL;
}

static pthread_once_t g_hfsIoPipelineOnce = PTHREAD_ONCE_INIT;
static uint32_t g_hfsIoPipelineConfigured;

/* Parse the bounded aligned-I/O child depth once per process. */
static void hfsIoPipelineInit(void) {
	const char *value = getenv("HFS_IO_PIPELINE_DEPTH");
	char *end = NULL;
	unsigned long parsed;

	g_hfsIoPipelineConfigured = HFS_IO_DEFAULT_PIPELINE;
	if (value == NULL || value[0] == '\0') {
		return;
	}
	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno == 0 && end != value && *end == '\0' &&
	    parsed >= 1U && parsed <= HFS_IO_MAX_PIPELINE) {
		g_hfsIoPipelineConfigured = (uint32_t)parsed;
	}
}

/* Return the maximum number of aligned child requests in one parent batch. */
static uint32_t hfsIoPipelineDepth(void) {
	(void)pthread_once(&g_hfsIoPipelineOnce, hfsIoPipelineInit);
	return g_hfsIoPipelineConfigured;
}

/* Acquire the smallest bounded DMA buffer, waiting until a matching buffer returns. */
static int hfsIoAcquire(Hfs *fs, HfsIoLease *lease, size_t requestedBytes) {
	HfsIoPool *pool = fs->ioPool;
	HfsIoPoolClass *classPool = hfsIoSelectClass(fs, requestedBytes);
	bool waited = false;

	if (classPool == NULL) {
		errno = E2BIG;
		return -1;
	}
	HFS_STATS_DMA_ACQUIRE();
	memset(lease, 0, sizeof(*lease));
	pthread_mutex_lock(&pool->lock);
	if (classPool->freeTop == 0 && !pool->closing) {
		waited = true;
		HFS_STATS_DMA_WAIT();
	}
	while (classPool->freeTop == 0 && !pool->closing) {
		pthread_cond_wait(&pool->available, &pool->lock);
	}
	if (pool->closing) {
		pthread_mutex_unlock(&pool->lock);
		errno = ESHUTDOWN;
		return -1;
	}
	lease->pool = pool;
	lease->classPool = classPool;
	lease->slot = classPool->freeSlots[--classPool->freeTop];
	lease->buffer = classPool->storage +
		(size_t)lease->slot * classPool->bufferBytes;
	pool->active++;
	HFS_STATS_DMA_ACTIVE(pool->active);
	hfsStatsRecordDmaFast(classPool->bufferBytes, waited, pool->active);
	pthread_mutex_unlock(&pool->lock);
	return 0;
}

/* Acquire the smallest DMA buffer without waiting for an async submit. */
static int hfsIoTryAcquire(Hfs *fs, HfsIoLease *lease, size_t requestedBytes) {
	HfsIoPool *pool = fs->ioPool;
	HfsIoPoolClass *classPool = hfsIoSelectClass(fs, requestedBytes);
	int rc;

	if (classPool == NULL) {
		errno = E2BIG;
		return -1;
	}
	HFS_STATS_DMA_ACQUIRE();
	memset(lease, 0, sizeof(*lease));
	rc = pthread_mutex_trylock(&pool->lock);
	if (rc != 0) {
		errno = EAGAIN;
		return -1;
	}
	if (classPool->freeTop == 0 || pool->closing) {
		pthread_mutex_unlock(&pool->lock);
		errno = pool->closing ? ESHUTDOWN : EAGAIN;
		return -1;
	}
	lease->pool = pool;
	lease->classPool = classPool;
	lease->slot = classPool->freeSlots[--classPool->freeTop];
	lease->buffer = classPool->storage +
		(size_t)lease->slot * classPool->bufferBytes;
	pool->active++;
	HFS_STATS_DMA_ACTIVE(pool->active);
	hfsStatsRecordDmaFast(classPool->bufferBytes, false, pool->active);
	pthread_mutex_unlock(&pool->lock);
	return 0;
}

/* Return one DMA buffer to the pool and wake the next waiting request. */
static void hfsIoRelease(HfsIoLease *lease) {
	struct hfs_io_pool *pool;

	if (lease == NULL || lease->pool == NULL) {
		return;
	}
	pool = lease->pool;
	pthread_mutex_lock(&pool->lock);
	if (lease->classPool != NULL &&
	    lease->classPool->freeTop < lease->classPool->count) {
		lease->classPool->freeSlots[lease->classPool->freeTop++] = lease->slot;
	}
	if (pool->active > 0) {
		pool->active--;
	}
	HFS_STATS_DMA_RELEASE();
	pthread_cond_broadcast(&pool->available);
	pthread_mutex_unlock(&pool->lock);
	memset(lease, 0, sizeof(*lease));
}

/* Start one SPDK blob operation on the dedicated owner thread. */
static void hfsIoStart(void *arg) {
	HfsIoRequest *request = arg;
	HfsFile *file = request->file;
	struct spdk_io_channel *channel = request->channel != NULL
		? request->channel : file->fs->channel;

	if (hfsRequestCompletionInjected(request->result.operation)) {
		hfsIoDone(request, -EIO);
		return;
	}
	if (request->vector) {
		if (request->operation == HFS_IO_READ) {
			spdk_blob_io_readv(file->blob, channel, request->iov,
					 request->iovcnt, request->offset, request->units,
					 hfsIoDone, request);
		} else {
			spdk_blob_io_writev(file->blob, channel, request->iov,
					  request->iovcnt, request->offset, request->units,
					  hfsIoDone, request);
		}
		return;
	}
	switch (request->operation) {
	case HFS_IO_READ:
		spdk_blob_io_read(file->blob, channel, request->buffer,
				  request->offset, request->units,
				  hfsIoDone, request);
		break;
	case HFS_IO_WRITE:
		spdk_blob_io_write(file->blob, channel, request->buffer,
				   request->offset, request->units,
				   hfsIoDone, request);
		break;
	case HFS_IO_UNMAP:
		spdk_blob_io_unmap(file->blob, channel, request->offset,
				   request->units, hfsIoDone, request);
		break;
	case HFS_IO_WRITE_ZEROES:
		spdk_blob_io_write_zeroes(file->blob, channel,
					 request->offset, request->units,
					 hfsIoDone, request);
		break;
	}
}

/* Capture an SPDK completion code before waking the synchronous caller. */
static void hfsIoDone(void *arg, int bserrno) {
	HfsIoRequest *request = arg;

	hfsRequestComplete(&request->result, bserrno, request->units);
	if (request->dataPath) {
		hfsStatsRecordDataReactorCompleteFast(request->dataReactor);
	}
	if (request->asyncMagic == HFS_IO_ASYNC_MAGIC && request->asyncDone != NULL) {
		request->asyncDone(request);
	}
}

/* Submit one child request without waiting for its owner-thread completion. */
static int hfsIoStartRequest(HfsIoRequest *request) {
	int rc;

	hfsRequestInit(&request->result, hfsIoRequestOperation(request->operation));
	if (request->dataPath && request->dataReactor != 0) {
		rc = hfsRequestSubmitData(&request->result, hfsIoStart, request,
					   request->dataReactor);
	} else {
		rc = hfsRequestSubmitBatch(&request->result, hfsIoStart, request,
					     &request->batchItem);
	}
	if (rc == 0 && request->dataPath) {
		hfsStatsRecordDataReactorSubmitFast(request->dataReactor);
	}
	return rc;
}

/* Forward declaration used by direct and vector synchronous submissions. */
static int hfsIoWaitRequest(HfsIoRequest *request);

/* Check that SPDK can address one caller buffer without a bounce copy. */
static bool hfsIoBufferDmaCapable(const void *buffer, size_t bytes) {
	uint64_t span = bytes;

	return buffer != NULL && bytes != 0 &&
		spdk_vtophys(buffer, &span) != SPDK_VTOPHYS_ERROR && span >= bytes;
}

/* Check vector shape and DMA reachability before using SPDK native vector I/O. */
static bool hfsIoVectorDmaCapable(const HfsFile *file,
				  const struct iovec *iov, int iovcnt,
				  size_t total, uint64_t offset) {
	int i;

	if (file == NULL || file->fs == NULL || iov == NULL || iovcnt <= 0 ||
	    iovcnt > (int)HFS_IO_MAX_VECTOR || total == 0 ||
	    offset % file->fs->unitSize != 0 ||
	    total % (size_t)file->fs->unitSize != 0) {
		return false;
	}
	for (i = 0; i < iovcnt; i++) {
		if (iov[i].iov_len == 0) {
			continue;
		}
		if (iov[i].iov_base == NULL ||
		    iov[i].iov_len % (size_t)file->fs->unitSize != 0 ||
		    !hfsIoBufferDmaCapable(iov[i].iov_base, iov[i].iov_len)) {
			return false;
		}
	}
	return true;
}

/* Submit one DMA-capable contiguous range directly to its selected channel. */
static int hfsIoSubmitDirect(HfsFile *file, void *buffer, uint64_t offset,
				     uint64_t units, bool write) {
	HfsIoRequest request;

	memset(&request, 0, sizeof(request));
	request.file = file;
	request.buffer = buffer;
	request.offset = offset;
	request.units = units;
	request.operation = write ? HFS_IO_WRITE : HFS_IO_READ;
	request.channel = hfsRuntimeDataChannel(file->fs,
		hfsRuntimeDataReactorFor(file->fs, file->blobId));
	request.dataReactor = hfsRuntimeDataReactorFor(file->fs, file->blobId);
	request.dataPath = true;
	if (request.channel == NULL || hfsIoStartRequest(&request) != 0 ||
	    hfsIoWaitRequest(&request) != 0) {
		return -1;
	}
	hfsStatsRecordIoDirectFast(units * file->fs->unitSize);
	return 0;
}

/* Submit one native vector request while retaining inode/range ordering. */
static int hfsIoSubmitVector(HfsFile *file, const struct iovec *iov,
				     int iovcnt, size_t total, uint64_t offset,
				     bool write) {
	struct iovec vector[HFS_IO_MAX_VECTOR];
	HfsIoRangeGuard guard = {0};
	HfsInodeRuntime *runtime;
	HfsIoRequest request;
	uint32_t dataReactor;
	int rc;

	if (!hfsIoVectorDmaCapable(file, iov, iovcnt, total, offset)) {
		errno = EAGAIN;
		return -1;
	}
	memcpy(vector, iov, (size_t)iovcnt * sizeof(*iov));
	runtime = hfsIoSyncReadLock(file);
	if (runtime == NULL) {
		return -1;
	}
	if (write && hfsResizeTo(file, offset + total) != 0) {
		hfsIoSyncReadUnlock(runtime);
		return -1;
	}
	if (hfsIoRangeLock(file, offset, total, write, &guard) != 0) {
		hfsIoSyncReadUnlock(runtime);
		return -1;
	}
	dataReactor = hfsRuntimeDataReactorFor(file->fs, file->blobId);
	memset(&request, 0, sizeof(request));
	request.file = file;
	request.iov = vector;
	request.iovcnt = iovcnt;
	request.vector = true;
	request.offset = offset / file->fs->unitSize;
	request.units = total / file->fs->unitSize;
	request.operation = write ? HFS_IO_WRITE : HFS_IO_READ;
	request.channel = hfsRuntimeDataChannel(file->fs, dataReactor);
	request.dataReactor = dataReactor;
	request.dataPath = true;
	rc = request.channel == NULL ? -1 : hfsIoStartRequest(&request);
	if (rc == 0) {
		rc = hfsIoWaitRequest(&request);
	}
	hfsIoRangeUnlock(file, &guard);
	hfsIoSyncReadUnlock(runtime);
	if (rc == 0) {
		hfsStatsRecordIoDirectFast(total);
		hfsStatsRecordIoVectorFast(total);
	}
	return rc;
}

/* Wait for one child request and retain the existing error logging contract. */
static int hfsIoWaitRequest(HfsIoRequest *request) {
	if (hfsRequestWaitResult(&request->result) != 0) {
		HFS_STATS_ERROR();
		HFS_LOG_ERROR("data I/O failed operation=%d offset=%"PRIu64
			      " units=%"PRIu64" submitRc=%d completionRc=%d bserrno=%d",
			      request->operation, request->offset, request->units,
			      request->result.submitRc, request->result.completionRc,
			      request->result.bserrno);
		return -1;
	}
	return 0;
}

/* Wait for every submitted child before parent storage is allowed to unwind. */
static int hfsIoWaitPipeline(HfsIoRequest *requests, uint32_t count) {
	int rc = 0;
	int firstErrno = 0;
	uint32_t i;

	for (i = 0; i < count; i++) {
		if (hfsIoWaitRequest(&requests[i]) != 0) {
			if (firstErrno == 0) {
				firstErrno = errno;
			}
			rc = -1;
		}
	}
	if (firstErrno != 0) {
		errno = firstErrno;
	}
	return rc;
}

/* Release all DMA leases owned by one completed or failed parent batch. */
static void hfsIoReleasePipeline(HfsIoLease *leases, uint32_t count) {
	uint32_t i;

	for (i = 0; i < count; i++) {
		hfsIoRelease(&leases[i]);
	}
}

/* Start one request-local transfer and wait for its owner-thread callback. */
static int hfsIoSubmit(HfsFile *file, void *buffer, uint64_t offset,
		       uint64_t units, HfsIoOperation operation) {
	HfsIoRequest request;

	/* The callback owns only this request, so the HfsFile has no I/O scratch state. */
	memset(&request, 0, sizeof(request));
	request.file = file;
	request.buffer = buffer;
	request.offset = offset;
	request.units = units;
	request.operation = operation;
	if (hfsIoStartRequest(&request) != 0) {
		HFS_STATS_ERROR();
		return -1;
	}
	if (hfsIoWaitRequest(&request) != 0) {
		return -1;
	}
	return 0;
}

static void hfsIoAsyncChildDone(HfsIoRequest *request);
static int hfsIoAsyncStartBatch(HfsIoAsyncParent *parent);

/* Finish a worker-free parent after every child and synchronization guard retire. */
static void hfsIoAsyncFinish(HfsIoAsyncParent *parent) {
	ssize_t result;
	int error;

	pthread_mutex_lock(&parent->lock);
	if (parent->finished) {
		pthread_mutex_unlock(&parent->lock);
		return;
	}
	parent->finished = true;
	result = parent->firstError == 0 ? (ssize_t)parent->count : -1;
	error = parent->firstError;
	pthread_mutex_unlock(&parent->lock);
	hfsIoRangeUnlock(parent->file, &parent->guard);
	hfsIoSyncReadUnlock(parent->runtime);
	parent->callback(parent->context, result, error);
	pthread_mutex_destroy(&parent->lock);
	free(parent);
}

/* Decide whether a completed batch should launch another batch or finish. */
static void hfsIoAsyncAfterBatch(HfsIoAsyncParent *parent) {
	bool advance = false;
	bool finish = false;

	pthread_mutex_lock(&parent->lock);
	if (!parent->launching && parent->pending == 0 && !parent->finished) {
		if (parent->firstError != 0 || parent->nextUnit >= parent->totalUnits) {
			finish = true;
		} else {
			advance = true;
		}
	}
	pthread_mutex_unlock(&parent->lock);
	if (advance) {
		if (hfsIoAsyncStartBatch(parent) != 0) {
			pthread_mutex_lock(&parent->lock);
			if (parent->firstError == 0) {
				parent->firstError = errno != 0 ? errno : EIO;
			}
			pthread_mutex_unlock(&parent->lock);
			hfsIoAsyncFinish(parent);
		}
	} else if (finish) {
		hfsIoAsyncFinish(parent);
	}
}

/* Submit one bounded child batch while keeping parent storage owner-stable. */
static int hfsIoAsyncStartBatch(HfsIoAsyncParent *parent) {
	uint64_t batchStart;
	uint64_t batchUnits = 0;
	uint64_t remaining;
	uint64_t maxUnits = hfsIoMaxUnits(parent->file->fs);
	uint32_t count;
	uint32_t i;
	int savedErrno = 0;
	bool finish = false;

	if (maxUnits == 0) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&parent->lock);
	if (parent->finished || parent->launching || parent->pending != 0) {
		pthread_mutex_unlock(&parent->lock);
		return 0;
	}
	remaining = parent->totalUnits - parent->nextUnit;
	if (remaining == 0) {
		pthread_mutex_unlock(&parent->lock);
		return 0;
	}
	count = (uint32_t)((remaining + maxUnits - 1U) / maxUnits);
	if (count > parent->depth) {
		count = parent->depth;
	}
	if ((uint64_t)count > HFS_IO_MAX_PIPELINE) {
		count = HFS_IO_MAX_PIPELINE;
	}
	batchStart = parent->nextUnit;
	for (i = 0; i < count; i++) {
		uint64_t units = remaining - batchUnits;

		if (units > maxUnits) {
			units = maxUnits;
		}
		batchUnits += units;
	}
	parent->nextUnit += batchUnits;
	parent->launching = true;
	parent->launched = count;
	pthread_mutex_unlock(&parent->lock);

	/* Reserve the complete batch before submitting the first child. */
	{
		uint64_t relativeUnit = batchStart;

		for (i = 0; i < count; i++) {
			uint64_t units;
			size_t bytes;

			units = parent->totalUnits - relativeUnit;
			if (units > maxUnits) {
				units = maxUnits;
			}
			bytes = (size_t)(units * parent->file->fs->unitSize);
			if (hfsIoTryAcquire(parent->file->fs, &parent->leases[i], bytes) != 0) {
				savedErrno = errno != 0 ? errno : EAGAIN;
				hfsIoReleasePipeline(parent->leases, i);
				pthread_mutex_lock(&parent->lock);
				parent->nextUnit = batchStart;
				parent->launching = false;
				pthread_mutex_unlock(&parent->lock);
				errno = savedErrno;
				return -1;
			}
			relativeUnit += units;
		}
	}

	/* Reuse the same linear cursor while assigning each child range. */
	{
		uint64_t relativeUnit = batchStart;

		for (i = 0; i < count; i++) {
			uint64_t units;
			size_t bytes;

			units = parent->totalUnits - relativeUnit;
			if (units > maxUnits) {
				units = maxUnits;
			}
			bytes = (size_t)(units * parent->file->fs->unitSize);
			memset(&parent->requests[i], 0, sizeof(parent->requests[i]));
			parent->requests[i].file = parent->file;
			parent->requests[i].buffer = parent->leases[i].buffer;
			parent->requests[i].offset = parent->offset /
				parent->file->fs->unitSize + relativeUnit;
			parent->requests[i].units = units;
			parent->requests[i].operation = parent->write ? HFS_IO_WRITE : HFS_IO_READ;
			parent->requests[i].channel = parent->channel;
			parent->requests[i].dataReactor = parent->dataReactor;
			parent->requests[i].dataPath = true;
			parent->requests[i].asyncMagic = HFS_IO_ASYNC_MAGIC;
			parent->requests[i].asyncDone = hfsIoAsyncChildDone;
			parent->requests[i].asyncContext = parent;
			parent->requests[i].asyncIndex = i;
			if (parent->write) {
				memcpy(parent->leases[i].buffer,
				       parent->writeBuffer + (size_t)(relativeUnit *
						parent->file->fs->unitSize), bytes);
				hfsStatsRecordCopyFast(bytes);
			}
			pthread_mutex_lock(&parent->lock);
			parent->pending++;
			pthread_mutex_unlock(&parent->lock);
			if (hfsIoStartRequest(&parent->requests[i]) != 0) {
				savedErrno = errno != 0 ? errno : EIO;
				hfsIoRelease(&parent->leases[i]);
				pthread_mutex_lock(&parent->lock);
				parent->pending--;
				if (parent->firstError == 0) {
					parent->firstError = savedErrno;
				}
				pthread_mutex_unlock(&parent->lock);
			}
			relativeUnit += units;
		}
	}
	hfsStatsRecordIoPipelineFast(count);
	pthread_mutex_lock(&parent->lock);
	parent->launching = false;
	finish = parent->pending == 0;
	pthread_mutex_unlock(&parent->lock);
	if (finish) {
		hfsIoAsyncAfterBatch(parent);
	}
	return 0;
}

/* Complete one child, copy read data, and advance the parent state machine. */
static void hfsIoAsyncChildDone(HfsIoRequest *request) {
	HfsIoAsyncParent *parent = request->asyncContext;
	uint64_t relativeUnit;
	ssize_t bytes;
	int error = request->result.completionRc;

	if (error != 0) {
		error = error < 0 ? -error : error;
	} else if (!parent->write) {
		relativeUnit = request->offset - parent->offset /
			parent->file->fs->unitSize;
		bytes = (ssize_t)(request->units * parent->file->fs->unitSize);
		memcpy(parent->readBuffer + (size_t)(relativeUnit *
			parent->file->fs->unitSize), request->buffer, (size_t)bytes);
		hfsStatsRecordCopyFast((size_t)bytes);
	}
	hfsIoRelease(&parent->leases[request->asyncIndex]);
	pthread_mutex_lock(&parent->lock);
	if (error != 0 && parent->firstError == 0) {
		parent->firstError = error;
	}
	if (parent->pending > 0) {
		parent->pending--;
	}
	pthread_mutex_unlock(&parent->lock);
	hfsIoAsyncAfterBatch(parent);
}

/* Submit one aligned read while retaining synchronization until completion. */
int hfsIoReadAtAsync(HfsFile *file, uint8_t *buffer, size_t count,
		     uint64_t offset, HfsIoAsyncCallback callback, void *context) {
	HfsIoAsyncParent *parent;

	if (callback == NULL || file == NULL || file->fs == NULL ||
	    file->blob == NULL || file->fs->ioPool == NULL ||
	    file->fs->unitSize == 0 || buffer == NULL || count == 0 ||
	    count > (size_t)SSIZE_MAX || offset > UINT64_MAX - count ||
	    offset % file->fs->unitSize != 0 ||
	    count % (size_t)file->fs->unitSize != 0) {
		errno = EINVAL;
		return -1;
	}
	parent = calloc(1, sizeof(*parent));
	if (parent == NULL) {
		errno = ENOMEM;
		return -1;
	}
	parent->file = file;
	parent->dataReactor = hfsRuntimeDataReactorFor(file->fs, file->blobId);
	parent->channel = hfsRuntimeDataChannel(file->fs, parent->dataReactor);
	if (parent->channel == NULL) {
		free(parent);
		errno = EAGAIN;
		return -1;
	}
	parent->readBuffer = buffer;
	parent->count = count;
	parent->offset = offset;
	parent->totalUnits = count / file->fs->unitSize;
	parent->depth = hfsIoPipelineDepth();
	parent->callback = callback;
	parent->context = context;
	parent->runtime = hfsIoSyncReadTryLock(file);
	if (parent->runtime == NULL ||
	    hfsIoRangeTryLock(file, offset, count, false, &parent->guard) != 0) {
		if (parent->runtime != NULL) {
			hfsIoSyncReadUnlock(parent->runtime);
		}
		free(parent);
		if (errno != EAGAIN) {
			errno = EAGAIN;
		}
		return -1;
	}
	if (pthread_mutex_init(&parent->lock, NULL) != 0) {
		hfsIoRangeUnlock(file, &parent->guard);
		hfsIoSyncReadUnlock(parent->runtime);
		free(parent);
		errno = ENOMEM;
		return -1;
	}
	if (hfsIoAsyncStartBatch(parent) != 0) {
		hfsIoRangeUnlock(file, &parent->guard);
		hfsIoSyncReadUnlock(parent->runtime);
		pthread_mutex_destroy(&parent->lock);
		free(parent);
		return -1;
	}
	return 0;
}

/* Submit one aligned write without resizing, retaining synchronization to completion. */
int hfsIoWriteAtAsync(HfsFile *file, const uint8_t *buffer, size_t count,
		      uint64_t offset, HfsIoAsyncCallback callback, void *context) {
	HfsIoAsyncParent *parent;
	uint64_t end;
	uint64_t clusters;
	uint64_t capacity;

	if (callback == NULL || file == NULL || file->fs == NULL ||
	    file->blob == NULL || file->fs->ioPool == NULL ||
	    file->fs->unitSize == 0 || buffer == NULL || count == 0 ||
	    count > (size_t)SSIZE_MAX || offset > UINT64_MAX - count ||
	    offset % file->fs->unitSize != 0 ||
	    count % (size_t)file->fs->unitSize != 0) {
		errno = EINVAL;
		return -1;
	}
	if (file->fs->clusterSize == 0) {
		errno = EAGAIN;
		return -1;
	}
	clusters = spdk_blob_get_num_clusters(file->blob);
	if (clusters > UINT64_MAX / file->fs->clusterSize) {
		errno = EOVERFLOW;
		return -1;
	}
	capacity = clusters * file->fs->clusterSize;
	end = offset + count;
	if (end > capacity) {
		errno = EAGAIN;
		return -1;
	}
	parent = calloc(1, sizeof(*parent));
	if (parent == NULL) {
		errno = ENOMEM;
		return -1;
	}
	parent->file = file;
	parent->dataReactor = hfsRuntimeDataReactorFor(file->fs, file->blobId);
	parent->channel = hfsRuntimeDataChannel(file->fs, parent->dataReactor);
	if (parent->channel == NULL) {
		free(parent);
		errno = EAGAIN;
		return -1;
	}
	parent->writeBuffer = buffer;
	parent->count = count;
	parent->offset = offset;
	parent->totalUnits = count / file->fs->unitSize;
	parent->depth = hfsIoPipelineDepth();
	parent->write = true;
	parent->callback = callback;
	parent->context = context;
	parent->runtime = hfsIoSyncReadTryLock(file);
	if (parent->runtime == NULL ||
	    hfsIoRangeTryLock(file, offset, count, true, &parent->guard) != 0) {
		if (parent->runtime != NULL) {
			hfsIoSyncReadUnlock(parent->runtime);
		}
		free(parent);
		if (errno != EAGAIN) {
			errno = EAGAIN;
		}
		return -1;
	}
	if (pthread_mutex_init(&parent->lock, NULL) != 0) {
		hfsIoRangeUnlock(file, &parent->guard);
		hfsIoSyncReadUnlock(parent->runtime);
		free(parent);
		errno = ENOMEM;
		return -1;
	}
	if (hfsIoAsyncStartBatch(parent) != 0) {
		hfsIoRangeUnlock(file, &parent->guard);
		hfsIoSyncReadUnlock(parent->runtime);
		pthread_mutex_destroy(&parent->lock);
		free(parent);
		return -1;
	}
	return 0;
}

/* Initialize the bounded DMA pool and its synchronization primitives. */
int hfsIoInit(Hfs *fs) {
	HfsIoPool *pool;
	size_t bufferBytes;
	size_t storageBytes;
	int rc;
	uint32_t rangeLocksReady = 0;
	uint32_t classesReady = 0;
	bool runtimeReady = false;

	if (fs == NULL || fs->unitSize == 0 ||
	    (size_t)fs->unitSize != fs->unitSize) {
		errno = EINVAL;
		return -1;
	}
	pool = calloc(1, sizeof(*pool));
	if (pool == NULL) {
		errno = ENOMEM;
		return -1;
	}
	rc = pthread_mutex_init(&pool->lock, NULL);
	if (rc != 0) {
		errno = rc;
		goto fail_pool;
	}
	rc = pthread_cond_init(&pool->available, NULL);
	if (rc != 0) {
		errno = rc;
		goto fail_pool_mutex;
	}
	for (rangeLocksReady = 0; rangeLocksReady < HFS_RANGE_LOCK_COUNT;
	     rangeLocksReady++) {
		rc = pthread_rwlock_init(&fs->rangeLocks[rangeLocksReady], NULL);
		if (rc != 0) {
			errno = rc;
			goto fail_range_locks;
		}
	}
	if (hfsInodeRuntimeInit(fs) != 0) {
		goto fail_range_locks;
	}
	runtimeReady = true;

	for (uint32_t i = 0; i < HFS_IO_POOL_CLASS_COUNT; i++) {
		HfsIoPoolClass *classPool = &pool->classes[i];
		uint32_t j;

		bufferBytes = g_hfsIoPoolTargetBytes[i];
		if (bufferBytes < (size_t)fs->unitSize) {
			bufferBytes = (size_t)fs->unitSize;
		}
		if (bufferBytes > SIZE_MAX - ((size_t)fs->unitSize - 1U)) {
			errno = EOVERFLOW;
			goto fail_classes;
		}
		bufferBytes += (size_t)fs->unitSize - 1U;
		bufferBytes -= bufferBytes % (size_t)fs->unitSize;
		if (bufferBytes == 0 ||
		    g_hfsIoPoolTargetCounts[i] > SIZE_MAX / bufferBytes) {
			errno = EOVERFLOW;
			goto fail_classes;
		}
		storageBytes = bufferBytes * g_hfsIoPoolTargetCounts[i];
		classPool->storage = spdk_malloc(storageBytes, 0x1000, NULL,
						SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (classPool->storage == NULL) {
			errno = ENOMEM;
			goto fail_classes;
		}
		classPool->bufferBytes = bufferBytes;
		classPool->count = g_hfsIoPoolTargetCounts[i];
		classPool->freeTop = classPool->count;
		for (j = 0; j < classPool->count; j++) {
			classPool->freeSlots[j] = j;
		}
		classesReady++;
	}
	fs->ioPool = pool;
	return 0;

fail_classes:
	while (classesReady > 0) {
		classesReady--;
		spdk_free(pool->classes[classesReady].storage);
	}
	if (runtimeReady) {
		hfsInodeRuntimeFini(fs);
	}
fail_range_locks:
	while (rangeLocksReady > 0) {
		rangeLocksReady--;
		pthread_rwlock_destroy(&fs->rangeLocks[rangeLocksReady]);
	}
	pthread_cond_destroy(&pool->available);
fail_pool_mutex:
	pthread_mutex_destroy(&pool->lock);
fail_pool:
	free(pool);
	return -1;
}

/* Drain active leases and release the DMA pool during filesystem teardown. */
void hfsIoFini(Hfs *fs) {
	HfsIoPool *pool;

	if (fs == NULL || fs->ioPool == NULL) {
		return;
	}
	pool = fs->ioPool;
	pthread_mutex_lock(&pool->lock);
	pool->closing = true;
	pthread_cond_broadcast(&pool->available);
	while (pool->active != 0) {
		pthread_cond_wait(&pool->available, &pool->lock);
	}
	pthread_mutex_unlock(&pool->lock);
	for (uint32_t i = 0; i < HFS_IO_POOL_CLASS_COUNT; i++) {
		spdk_free(pool->classes[i].storage);
	}
	pthread_cond_destroy(&pool->available);
	pthread_mutex_destroy(&pool->lock);
	free(pool);
	fs->ioPool = NULL;
	hfsInodeRuntimeFini(fs);
	for (uint32_t i = 0; i < HFS_RANGE_LOCK_COUNT; i++) {
		pthread_rwlock_destroy(&fs->rangeLocks[i]);
	}
}

/* Read one complete I/O unit into a leased DMA buffer. */
static int hfsIoReadUnit(HfsFile *file, HfsIoLease *lease,
			 uint64_t unitOffset) {
	return hfsIoSubmit(file, lease->buffer, unitOffset, 1, HFS_IO_READ);
}

/* Write one complete I/O unit from a leased DMA buffer. */
static int hfsIoWriteUnit(HfsFile *file, HfsIoLease *lease,
			  uint64_t unitOffset) {
	return hfsIoSubmit(file, lease->buffer, unitOffset, 1, HFS_IO_WRITE);
}

/* Preserve bytes outside an edge and replace only the requested subrange. */
static int hfsIoModifyPartial(HfsFile *file, uint64_t unitOffset,
			       size_t unitIndex, const uint8_t *source,
			       size_t length) {
	HfsIoLease lease;
	int rc;

	if (hfsIoAcquire(file->fs, &lease, (size_t)file->fs->unitSize) != 0) {
		return -1;
	}
	rc = hfsIoReadUnit(file, &lease, unitOffset);
	if (rc == 0) {
		hfsStatsRecordRmwFast(file->fs->unitSize);
		memcpy(lease.buffer + unitIndex, source, length);
		hfsStatsRecordCopyFast(length);
		rc = hfsIoWriteUnit(file, &lease, unitOffset);
	}
	hfsIoRelease(&lease);
	return rc;
}

/* Zero an aligned range through pooled full-unit writes. */
static int hfsIoWriteZeroUnits(HfsFile *file, uint64_t offset, uint64_t units) {
	uint64_t maxUnits = hfsIoMaxUnits(file->fs);

	if (maxUnits == 0) {
		errno = EINVAL;
		return -1;
	}
	while (units != 0) {
		HfsIoLease lease;
		uint64_t chunkUnits = units > maxUnits ? maxUnits : units;
		int rc;

		if (hfsIoAcquire(file->fs, &lease,
				(size_t)(chunkUnits * file->fs->unitSize)) != 0) {
			return -1;
		}
		memset(lease.buffer, 0, (size_t)(chunkUnits * file->fs->unitSize));
		rc = hfsIoSubmit(file, lease.buffer, offset, chunkUnits, HFS_IO_WRITE);
		hfsIoRelease(&lease);
		if (rc != 0) {
			return -1;
		}
		offset += chunkUnits;
		units -= chunkUnits;
	}
	return 0;
}

/* Read a positioned range while the caller holds the filesystem read lock. */
static ssize_t hfsIoReadAtLocked(HfsFile *file, uint8_t *buffer,
				 size_t count, uint64_t offset) {
	uint64_t unitSize = file->fs->unitSize;
	uint64_t cursor = offset;
	size_t remaining = count;
	size_t done = 0;
	if (offset % unitSize == 0 && count % (size_t)unitSize == 0 &&
	    hfsIoBufferDmaCapable(buffer, count)) {
		uint64_t maxUnits = hfsIoMaxUnits(file->fs);
		uint64_t units = count / unitSize;
		uint64_t cursorUnit = offset / unitSize;
		size_t bytesDone = 0;

		while (units != 0) {
			uint64_t chunkUnits = units > maxUnits ? maxUnits : units;
			size_t chunkBytes = (size_t)(chunkUnits * unitSize);

			if (hfsIoSubmitDirect(file, buffer + bytesDone, cursorUnit,
					      chunkUnits, false) != 0) {
				return -1;
			}
			cursorUnit += chunkUnits;
			units -= chunkUnits;
			bytesDone += chunkBytes;
		}
		return (ssize_t)count;
	}

	if (cursor % unitSize != 0) {
		uint64_t unitOffset = cursor / unitSize;
		size_t unitIndex = (size_t)(cursor % unitSize);
		size_t take = (size_t)unitSize - unitIndex;
		HfsIoLease lease;

		if (take > remaining) {
			take = remaining;
		}
		if (hfsIoAcquire(file->fs, &lease, (size_t)file->fs->unitSize) != 0) {
			return -1;
		}
		if (hfsIoReadUnit(file, &lease, unitOffset) != 0) {
			hfsIoRelease(&lease);
			return -1;
		}
		memcpy(buffer, lease.buffer + unitIndex, take);
		hfsStatsRecordCopyFast(take);
		hfsIoRelease(&lease);
		cursor += take;
		buffer += take;
		remaining -= take;
		done += take;
	}

	while (remaining >= (size_t)unitSize) {
		uint64_t fullUnits = remaining / (size_t)unitSize;
		uint64_t maxUnits = hfsIoMaxUnits(file->fs);
		uint64_t pendingUnits = 0;
		uint32_t pipelineDepth = hfsIoPipelineDepth();
		uint32_t requestCount = 0;
		HfsIoRequest requests[HFS_IO_MAX_PIPELINE] = {0};
		HfsIoLease leases[HFS_IO_MAX_PIPELINE] = {0};
		uint32_t i;

		while (requestCount < pipelineDepth && pendingUnits < fullUnits) {
			uint64_t chunkUnits = fullUnits - pendingUnits;

			if (chunkUnits > maxUnits) {
				chunkUnits = maxUnits;
			}
			if (hfsIoAcquire(file->fs, &leases[requestCount],
					(size_t)(chunkUnits * unitSize)) != 0) {
				hfsIoWaitPipeline(requests, requestCount);
				hfsIoReleasePipeline(leases, requestCount);
				return -1;
			}
			requests[requestCount].file = file;
			requests[requestCount].buffer = leases[requestCount].buffer;
			requests[requestCount].offset = cursor / unitSize + pendingUnits;
			requests[requestCount].units = chunkUnits;
			requests[requestCount].operation = HFS_IO_READ;
			if (hfsIoStartRequest(&requests[requestCount]) != 0) {
				hfsIoRelease(&leases[requestCount]);
				hfsIoWaitPipeline(requests, requestCount);
				hfsIoReleasePipeline(leases, requestCount);
				return -1;
			}
			requestCount++;
			pendingUnits += chunkUnits;
		}
		hfsStatsRecordIoPipelineFast(requestCount);
		if (hfsIoWaitPipeline(requests, requestCount) != 0) {
			hfsIoReleasePipeline(leases, requestCount);
			return -1;
		}
		for (i = 0; i < requestCount; i++) {
			size_t bytes = (size_t)(requests[i].units * unitSize);

			memcpy(buffer, leases[i].buffer, bytes);
			hfsStatsRecordCopyFast(bytes);
			buffer += bytes;
		}
		hfsIoReleasePipeline(leases, requestCount);
		cursor += pendingUnits * unitSize;
		remaining -= (size_t)(pendingUnits * unitSize);
		done += (size_t)(pendingUnits * unitSize);
	}

	if (remaining != 0) {
		HfsIoLease lease;

		if (hfsIoAcquire(file->fs, &lease, (size_t)unitSize) != 0) {
			return -1;
		}
		if (hfsIoReadUnit(file, &lease, cursor / unitSize) != 0) {
			hfsIoRelease(&lease);
			return -1;
		}
		memcpy(buffer, lease.buffer, remaining);
		hfsStatsRecordCopyFast(remaining);
		hfsIoRelease(&lease);
		done += remaining;
	}
	return (ssize_t)done;
}

/* Write a positioned range while the caller holds the selected data lock. */
static ssize_t hfsIoWriteAtLocked(HfsFile *file, const uint8_t *buffer,
				  size_t count, uint64_t offset) {
	uint64_t unitSize = file->fs->unitSize;
	uint64_t cursor = offset;
	size_t remaining = count;
	size_t done = 0;
	if (offset % unitSize == 0 && count % (size_t)unitSize == 0 &&
	    hfsIoBufferDmaCapable(buffer, count)) {
		uint64_t maxUnits = hfsIoMaxUnits(file->fs);
		uint64_t units = count / unitSize;
		uint64_t cursorUnit = offset / unitSize;
		size_t bytesDone = 0;

		while (units != 0) {
			uint64_t chunkUnits = units > maxUnits ? maxUnits : units;
			size_t chunkBytes = (size_t)(chunkUnits * unitSize);

			if (hfsIoSubmitDirect(file, (void *)(buffer + bytesDone),
					      cursorUnit, chunkUnits, true) != 0) {
				return -1;
			}
			cursorUnit += chunkUnits;
			units -= chunkUnits;
			bytesDone += chunkBytes;
		}
		return (ssize_t)count;
	}

	if (cursor % unitSize != 0) {
		uint64_t unitOffset = cursor / unitSize;
		size_t unitIndex = (size_t)(cursor % unitSize);
		size_t take = (size_t)unitSize - unitIndex;

		if (take > remaining) {
			take = remaining;
		}
		if (hfsIoModifyPartial(file, unitOffset, unitIndex, buffer, take) != 0) {
			return -1;
		}
		cursor += take;
		buffer += take;
		remaining -= take;
		done += take;
	}

	while (remaining >= (size_t)unitSize) {
		uint64_t fullUnits = remaining / (size_t)unitSize;
		uint64_t maxUnits = hfsIoMaxUnits(file->fs);
		uint64_t pendingUnits = 0;
		uint32_t pipelineDepth = hfsIoPipelineDepth();
		uint32_t requestCount = 0;
		HfsIoRequest requests[HFS_IO_MAX_PIPELINE] = {0};
		HfsIoLease leases[HFS_IO_MAX_PIPELINE] = {0};

		while (requestCount < pipelineDepth && pendingUnits < fullUnits) {
			uint64_t chunkUnits = fullUnits - pendingUnits;
			size_t bytes;

			if (chunkUnits > maxUnits) {
				chunkUnits = maxUnits;
			}
			if (hfsIoAcquire(file->fs, &leases[requestCount],
					(size_t)(chunkUnits * unitSize)) != 0) {
				hfsIoWaitPipeline(requests, requestCount);
				hfsIoReleasePipeline(leases, requestCount);
				return -1;
			}
			bytes = (size_t)(chunkUnits * unitSize);
			memcpy(leases[requestCount].buffer,
			       buffer + (size_t)(pendingUnits * unitSize), bytes);
			hfsStatsRecordCopyFast(bytes);
			requests[requestCount].file = file;
			requests[requestCount].buffer = leases[requestCount].buffer;
			requests[requestCount].offset = cursor / unitSize + pendingUnits;
			requests[requestCount].units = chunkUnits;
			requests[requestCount].operation = HFS_IO_WRITE;
			if (hfsIoStartRequest(&requests[requestCount]) != 0) {
				hfsIoRelease(&leases[requestCount]);
				hfsIoWaitPipeline(requests, requestCount);
				hfsIoReleasePipeline(leases, requestCount);
				return -1;
			}
			requestCount++;
			pendingUnits += chunkUnits;
		}
		hfsStatsRecordIoPipelineFast(requestCount);
		if (hfsIoWaitPipeline(requests, requestCount) != 0) {
			hfsIoReleasePipeline(leases, requestCount);
			return -1;
		}
		hfsIoReleasePipeline(leases, requestCount);
		cursor += pendingUnits * unitSize;
		buffer += (size_t)(pendingUnits * unitSize);
		remaining -= (size_t)(pendingUnits * unitSize);
		done += (size_t)(pendingUnits * unitSize);
	}

	if (remaining != 0 &&
	    hfsIoModifyPartial(file, cursor / unitSize, 0, buffer, remaining) != 0) {
		return -1;
	}
	return (ssize_t)(done + remaining);
}

/* Validate and read a positioned byte range through the bounded pool. */
ssize_t hfsIoReadAt(HfsFile *file, uint8_t *buffer, size_t count,
		    uint64_t offset) {
	HfsInodeRuntime *runtime;
	HfsIoRangeGuard guard = {0};
	ssize_t result;
	bool aligned;

	if (count == 0) {
		return 0;
	}
	if (file == NULL || file->fs == NULL || file->blob == NULL ||
	    file->fs->ioPool == NULL || file->fs->unitSize == 0 ||
	    buffer == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (count > (size_t)SSIZE_MAX || offset > UINT64_MAX - count) {
		errno = EOVERFLOW;
		return -1;
	}
	aligned = (offset % file->fs->unitSize) == 0 &&
		(count % (size_t)file->fs->unitSize) == 0;
	hfsStatsRecordIoFast(false, count, aligned);
	runtime = hfsIoSyncReadLock(file);
	if (runtime == NULL) {
		return -1;
	}
	if (hfsIoRangeLock(file, offset, count, false, &guard) != 0) {
		hfsIoSyncReadUnlock(runtime);
		return -1;
	}
	result = hfsIoReadAtLocked(file, buffer, count, offset);
	hfsIoRangeUnlock(file, &guard);
	hfsIoSyncReadUnlock(runtime);
	return result;
}

/* Validate and write a positioned byte range with direct aligned writes. */
ssize_t hfsIoWriteAt(HfsFile *file, const uint8_t *buffer, size_t count,
			     uint64_t offset) {
	HfsInodeRuntime *runtime;
	HfsIoRangeGuard guard = {0};
	ssize_t result;
	bool aligned;

	if (count == 0) {
		return 0;
	}
	if (file == NULL || file->fs == NULL || file->blob == NULL ||
	    file->fs->ioPool == NULL || file->fs->unitSize == 0 ||
	    buffer == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (count > (size_t)SSIZE_MAX || offset > UINT64_MAX - count) {
		errno = EOVERFLOW;
		return -1;
	}
	aligned = (offset % file->fs->unitSize) == 0 &&
		(count % (size_t)file->fs->unitSize) == 0;
	hfsStatsRecordIoFast(true, count, aligned);
	runtime = hfsIoSyncReadLock(file);
	if (runtime == NULL) {
		return -1;
	}
	if (hfsResizeTo(file, offset + count) != 0) {
		hfsIoSyncReadUnlock(runtime);
		return -1;
	}
	if (hfsIoRangeLock(file, offset, count, true, &guard) != 0) {
		hfsIoSyncReadUnlock(runtime);
		return -1;
	}
	result = hfsIoWriteAtLocked(file, buffer, count, offset);
	hfsIoRangeUnlock(file, &guard);
	hfsIoSyncReadUnlock(runtime);
	return result;
}

/* Attempt one native vector read and preserve EAGAIN as the fallback signal. */
ssize_t hfsIoReadvAt(HfsFile *file, const struct iovec *iov, int iovcnt,
			     size_t total, uint64_t offset) {
	if (file == NULL || file->fs == NULL || file->blob == NULL ||
	    offset > UINT64_MAX - total || total > (size_t)SSIZE_MAX ||
	    !hfsIoVectorDmaCapable(file, iov, iovcnt, total, offset)) {
		errno = EAGAIN;
		return -1;
	}
	if (hfsIoSubmitVector(file, iov, iovcnt, total, offset, false) != 0) {
		return -1;
	}
	return (ssize_t)total;
}

/* Attempt one native vector write and preserve EAGAIN as the fallback signal. */
ssize_t hfsIoWritevAt(HfsFile *file, const struct iovec *iov, int iovcnt,
			      size_t total, uint64_t offset) {
	if (file == NULL || file->fs == NULL || file->blob == NULL ||
	    offset > UINT64_MAX - total || total > (size_t)SSIZE_MAX ||
	    !hfsIoVectorDmaCapable(file, iov, iovcnt, total, offset)) {
		errno = EAGAIN;
		return -1;
	}
	if (hfsIoSubmitVector(file, iov, iovcnt, total, offset, true) != 0) {
		return -1;
	}
	return (ssize_t)total;
}

/* Zero one partial unit without modifying the bytes outside the range. */
static int hfsIoZeroPartial(HfsFile *file, uint64_t unitOffset,
			    size_t unitIndex, size_t length) {
	HfsIoLease lease;
	int rc;

	if (hfsIoAcquire(file->fs, &lease, (size_t)file->fs->unitSize) != 0) {
		return -1;
	}
	rc = hfsIoReadUnit(file, &lease, unitOffset);
	if (rc == 0) {
		hfsStatsRecordRmwFast(file->fs->unitSize);
		memset(lease.buffer + unitIndex, 0, length);
		rc = hfsIoWriteUnit(file, &lease, unitOffset);
	}
	hfsIoRelease(&lease);
	return rc;
}

/* Unmap first, then commit the logical zero range through a reliable fallback. */
static int hfsIoUnmapUnits(HfsFile *file, uint64_t offset, uint64_t units) {
	int savedErrno;

	if (hfsIoSubmit(file, NULL, offset, units, HFS_IO_UNMAP) != 0) {
		savedErrno = errno;
		if (savedErrno != EOPNOTSUPP && savedErrno != ENOTSUP) {
			return -1;
		}
	}
	if (hfsIoSubmit(file, NULL, offset, units, HFS_IO_WRITE_ZEROES) == 0) {
		return 0;
	}
	/*
	 * An unmap completion does not promise reads of zero on every bdev. If the
	 * accelerated zeroes path fails, retry through the DMA write path and only
	 * report success after the observable hole contents are actually zero.
	 */
	return hfsIoWriteZeroUnits(file, offset, units);
}

/* Clear a range with edge-preserving zeroes and aligned unmap operations. */
static int hfsIoZeroRangeCore(HfsFile *file, uint64_t offset, uint64_t length) {
	uint64_t unitSize;
	uint64_t end;
	uint64_t cursor;
	uint64_t alignedEnd;
	HfsIoRangeGuard guard = {0};

	if (length == 0) {
		return 0;
	}
	if (file == NULL || file->fs == NULL || file->blob == NULL ||
	    file->fs->ioPool == NULL || file->fs->unitSize == 0 ||
	    offset > UINT64_MAX - length) {
		errno = EINVAL;
		return -1;
	}
	unitSize = file->fs->unitSize;
	end = offset + length;
	if (hfsIoRangeLock(file, offset, length, true, &guard) != 0) {
		return -1;
	}
	cursor = offset;
	if (cursor % unitSize != 0) {
		size_t unitIndex = (size_t)(cursor % unitSize);
		size_t take = (size_t)unitSize - unitIndex;

		if ((uint64_t)take > end - cursor) {
			take = (size_t)(end - cursor);
		}
		if (hfsIoZeroPartial(file, cursor / unitSize, unitIndex, take) != 0) {
			hfsIoRangeUnlock(file, &guard);
			return -1;
		}
		cursor += take;
	}
	alignedEnd = (end / unitSize) * unitSize;
	if (cursor < alignedEnd) {
		if (hfsIoUnmapUnits(file, cursor / unitSize,
				    (alignedEnd - cursor) / unitSize) != 0) {
				hfsIoRangeUnlock(file, &guard);
				return -1;
		}
		cursor = alignedEnd;
	}
	if (cursor < end &&
	    hfsIoZeroPartial(file, cursor / unitSize, 0,
			     (size_t)(end - cursor)) != 0) {
		hfsIoRangeUnlock(file, &guard);
		return -1;
	}
	hfsIoRangeUnlock(file, &guard);
	return 0;
}

/* Zero a range while the caller holds the inode synchronization barrier. */
int hfsIoZeroRangeLocked(HfsFile *file, uint64_t offset, uint64_t length) {
	return hfsIoZeroRangeCore(file, offset, length);
}

/* Zero a range under the inode read side for ordinary callers. */
int hfsIoZeroRange(HfsFile *file, uint64_t offset, uint64_t length) {
	HfsInodeRuntime *runtime;
	int rc;

	if (length == 0) {
		return 0;
	}
	runtime = hfsIoSyncReadLock(file);
	if (runtime == NULL) {
		return -1;
	}
	rc = hfsIoZeroRangeCore(file, offset, length);
	hfsIoSyncReadUnlock(runtime);
	return rc;
}

/* Punching a logical hole uses the same zero-read guarantee as truncate. */
int hfsIoPunchHole(HfsFile *file, uint64_t offset, uint64_t length) {
	return hfsIoZeroRange(file, offset, length);
}

/* Punch a hole while the caller owns the inode synchronization barrier. */
int hfsIoPunchHoleLocked(HfsFile *file, uint64_t offset, uint64_t length) {
	return hfsIoZeroRangeCore(file, offset, length);
}
