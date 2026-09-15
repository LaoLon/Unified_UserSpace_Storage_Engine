#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/uio.h>

#include <spdk/bdev.h>
#include <spdk/blob.h>
#include <spdk/env.h>

#include "hfs_internal.h"
#include "hfs_inode_runtime.h"
#include "hfs_io.h"
#include "hfs_log.h"
#include "hfs_profile.h"
#include "hfs_stats.h"
#include "hfs_ticket.h"
#include "hfs_trace.h"

/* Keep the hot path bounded while avoiding a 1 MiB lease for tiny requests. */
#define HFS_IO_POOL_CLASS_COUNT	4U
#define HFS_IO_POOL_CLASS_MAX_COUNT	32U
#define HFS_IO_POOL_API_CLOSING	(UINT32_C(1) << 31)
#define HFS_IO_MAX_POOL_REACTORS	8U
#define HFS_IO_POOL_4K_BYTES	(4U << 10)
#define HFS_IO_POOL_16K_BYTES	(16U << 10)
#define HFS_IO_POOL_64K_BYTES	(64U << 10)
#define HFS_IO_POOL_1M_BYTES	(1U << 20)
#define HFS_IO_MAX_PIPELINE	16U
#define HFS_IO_MAX_VECTOR	32U
#define HFS_IO_DEFAULT_PIPELINE	8U

static pthread_once_t g_hfsIoUnmapCapabilityOnce = PTHREAD_ONCE_INIT;
static bool g_hfsIoUnmapZeroSafe;

/* Parse the explicit operator declaration for an unmap-to-zero backend. */
static void hfsIoInitUnmapCapability(void) {
	const char *value = getenv("HFS_UNMAP_ZERO_SAFE");

	g_hfsIoUnmapZeroSafe = value != NULL &&
		(!strcmp(value, "1") || !strcasecmp(value, "true") ||
		 !strcasecmp(value, "yes") || !strcasecmp(value, "on"));
}

/* Require both an explicit declaration and an SPDK UNMAP capability bit. */
static bool hfsIoUnmapGuaranteesZero(const HfsFile *file) {
	struct spdk_bdev *base;

	pthread_once(&g_hfsIoUnmapCapabilityOnce, hfsIoInitUnmapCapability);
	if (!g_hfsIoUnmapZeroSafe || file == NULL || file->fs == NULL ||
	    file->fs->bsDev == NULL || file->fs->bsDev->get_base_bdev == NULL) {
		return false;
	}
	base = file->fs->bsDev->get_base_bdev(file->fs->bsDev);
	return base != NULL &&
		spdk_bdev_io_type_supported(base, SPDK_BDEV_IO_TYPE_UNMAP);
}

/* One size-class DMA buffer leased by one request at a time. */
typedef struct HfsIoPoolClass HfsIoPoolClass;
typedef struct hfs_io_pool HfsIoPool;

typedef struct HfsIoLease {
	HfsIoPool *pool;
	struct HfsIoPoolShard *shard;
	HfsIoPoolClass *classPool;
	uint32_t slot;
	uint8_t *buffer;
	uint8_t poolClass;
} HfsIoLease;

/* Storage and free-list state for one DMA size class. */
struct HfsIoPoolClass {
	uint8_t *storage;
	size_t bufferBytes;
	uint32_t count;
	/* Each set bit represents one available DMA slot. */
	_Atomic uint32_t freeMask;
};

/* One reactor-local DMA pool with its own wait queue and free lists. */
typedef struct HfsIoPoolShard {
	pthread_mutex_t lock;
	pthread_cond_t available;
	HfsIoPoolClass classes[HFS_IO_POOL_CLASS_COUNT];
	_Atomic uint32_t active;
	_Atomic uint32_t apiUsers;
	_Atomic uint32_t waiters;
	_Atomic bool closing;
} HfsIoPoolShard;

/* Bounded DMA storage partitioned by the selected data reactor. */
struct hfs_io_pool {
	uint32_t shardCount;
	HfsIoPoolShard shards[HFS_IO_MAX_POOL_REACTORS];
};

static const size_t g_hfsIoPoolTargetBytes[HFS_IO_POOL_CLASS_COUNT] = {
	HFS_IO_POOL_4K_BYTES, HFS_IO_POOL_16K_BYTES,
	HFS_IO_POOL_64K_BYTES, HFS_IO_POOL_1M_BYTES
};
static const uint32_t g_hfsIoPoolTargetCounts[HFS_IO_POOL_CLASS_COUNT] = {
	32U, 16U, 32U, 8U
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
	HfsIoChildProfile profileChild;
	uint64_t traceParentId;
	uint64_t traceSubmitNs;
	HfsTraceBufferKind traceBufferKind;
} HfsIoRequest;

static _Thread_local uint64_t g_hfsIoTraceParentId;
static _Thread_local HfsTraceOperation g_hfsIoTraceOperation;

/* Convert an internal I/O operation to the stable trace vocabulary. */
static HfsTraceOperation hfsIoTraceOperation(HfsIoOperation operation)
{
	switch (operation) {
	case HFS_IO_READ: return HFS_TRACE_OPERATION_READ;
	case HFS_IO_WRITE: return HFS_TRACE_OPERATION_WRITE;
	case HFS_IO_UNMAP: return HFS_TRACE_OPERATION_UNMAP;
	case HFS_IO_WRITE_ZEROES: return HFS_TRACE_OPERATION_WRITE_ZEROES;
	default: return HFS_TRACE_OPERATION_NONE;
	}
}

/* Set of hashed range buckets held by one request in ascending order. */
typedef struct HfsIoRangeGuard {
	uint16_t buckets[HFS_RANGE_LOCK_COUNT];
	HfsTraceLockToken traceLocks[HFS_INODE_RANGE_STRIPE_COUNT];
	uint16_t count;
	bool write;
	bool gateWrite;
	/* Track whether the blocking per-inode gate has a timing frame. */
	bool gateStatsTimed;
	/* Preserve the gate hold start across submit/completion reactor threads. */
	uint64_t gateHoldStartNs;
	HfsInodeRuntime *runtime;
} HfsIoRangeGuard;

/* Map logical units to a stable per-inode stripe. */
static uint32_t hfsIoRangeBucket(const HfsFile *file, uint64_t unitIndex) {
	(void)file;
	return (uint32_t)((unitIndex / HFS_INODE_RANGE_STRIPE_UNITS) &
		(HFS_INODE_RANGE_STRIPE_COUNT - 1U));
}

/* Return the per-inode stripe selected by one range guard. */
static HfsRangeLock *hfsIoRangePrimitive(const HfsIoRangeGuard *guard,
						 HfsFile *file, uint16_t bucket) {
	if (guard != NULL && guard->runtime != NULL) {
		return &guard->runtime->rangeStripes[bucket];
	}
	return &file->fs->rangeLocks[bucket];
}

/* Lock all per-inode stripes covering a byte range in a deadlock-free order. */
static int hfsIoRangeLock(HfsFile *file, uint64_t offset, uint64_t length,
			  bool write, HfsIoRangeGuard *guard) {
	uint64_t firstUnit;
	uint64_t lastUnit;
	uint64_t unit;
	uint64_t seen[HFS_INODE_RANGE_STRIPE_COUNT / 64U] = {0};
	uint32_t bucket;
	uint32_t seenCount = 0;
	HfsInodeRuntime *runtime;

	if (file == NULL || file->fs == NULL || file->fs->unitSize == 0 ||
	    guard == NULL || length == 0 || offset > UINT64_MAX - (length - 1)) {
		errno = EINVAL;
		return -1;
	}
	guard->count = 0;
	memset(guard->traceLocks, 0, sizeof(guard->traceLocks));
	guard->gateHoldStartNs = 0;
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
		if (unit == UINT64_MAX || seenCount == HFS_INODE_RANGE_STRIPE_COUNT) {
			break;
		}
	}
	guard->write = write;
	guard->gateWrite = false;
	guard->gateStatsTimed = false;
	guard->gateHoldStartNs = 0;
	for (bucket = 0; bucket < HFS_INODE_RANGE_STRIPE_COUNT; bucket++) {
		if ((seen[bucket / 64U] & (UINT64_C(1) << (bucket % 64U))) == 0) {
			continue;
		}
		guard->buckets[guard->count] = (uint16_t)bucket;
		hfsTraceLockWaitBegin(&guard->traceLocks[guard->count],
			HFS_TRACE_LOCK_RANGE_STRIPE, bucket,
			g_hfsIoTraceOperation, g_hfsIoTraceParentId, file->inodeId,
			offset, length, seenCount);
		hfsStatsLockBeforeFast(HFS_STATS_LOCK_IO);
		if (write) {
			hfsRangeLockWriteLock(hfsIoRangePrimitive(guard, file,
													 (uint16_t)bucket));
		} else {
			hfsRangeLockReadLock(hfsIoRangePrimitive(guard, file,
													 (uint16_t)bucket));
		}
		hfsStatsLockAfterFast(HFS_STATS_LOCK_IO);
		hfsTraceLockAcquired(&guard->traceLocks[guard->count], 0);
		guard->count++;
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
		hfsRangeLockUnlock(hfsIoRangePrimitive(guard, file,
							 guard->buckets[i - 1]), guard->write);
		hfsTraceLockReleased(&guard->traceLocks[i - 1], 0);
	}
	/* The per-inode stripe locks are released in reverse order. */
	guard->count = 0;
	guard->gateWrite = false;
	guard->gateStatsTimed = false;
	guard->gateHoldStartNs = 0;
	guard->runtime = NULL;
}

/* Enter the per-inode read side so fsync/truncate can take an exclusive barrier. */
static HfsInodeRuntime *hfsIoSyncReadLock(HfsFile *file,
						 HfsTraceLockToken *traceToken) {
	HfsInodeRuntime *runtime = hfsInodeRuntimeForFile(file);

	if (runtime == NULL) {
		return NULL;
	}
	hfsTraceLockWaitBegin(traceToken, HFS_TRACE_LOCK_SYNC, 0,
			g_hfsIoTraceOperation, g_hfsIoTraceParentId, file->inodeId,
			0, 0, 0);
	pthread_rwlock_rdlock(&runtime->syncLock);
	hfsTraceLockAcquired(traceToken, 0);
	return runtime;
}

/* Leave the per-inode read side after a data operation. */
static void hfsIoSyncReadUnlock(HfsInodeRuntime *runtime,
						HfsTraceLockToken *traceToken) {
	if (runtime != NULL) {
		pthread_rwlock_unlock(&runtime->syncLock);
		hfsTraceLockReleased(traceToken, 0);
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

/* Return the pool shard selected by a data reactor, with owner fallback. */
static HfsIoPoolShard *hfsIoPoolShardFor(Hfs *fs, uint32_t reactorId) {
	if (fs == NULL || fs->ioPool == NULL || fs->ioPool->shardCount == 0) {
		return NULL;
	}
	if (reactorId >= fs->ioPool->shardCount) {
		reactorId = 0;
	}
	return &fs->ioPool->shards[reactorId];
}

static pthread_once_t g_hfsIoMaxUnitsOnce = PTHREAD_ONCE_INIT;
static uint64_t g_hfsIoMaxUnitsConfigured;

/* Parse an optional direct-child unit cap used to tune device request size. */
static void hfsIoMaxUnitsInit(void) {
	const char *value = getenv("HFS_IO_MAX_DIRECT_UNITS");
	char *end = NULL;
	unsigned long long parsed;

	g_hfsIoMaxUnitsConfigured = 0;
	if (value == NULL || value[0] == '\0') {
		return;
	}
	errno = 0;
	parsed = strtoull(value, &end, 10);
	if (errno == 0 && end != value && *end == '\0' && parsed != 0) {
		g_hfsIoMaxUnitsConfigured = (uint64_t)parsed;
	}
}

/* Return the number of complete I/O units one pool buffer can hold. */
static uint64_t hfsIoMaxUnits(const Hfs *fs) {
	HfsIoPoolShard *shard = hfsIoPoolShardFor((Hfs *)fs, 0);
	uint64_t maxUnits;

	maxUnits = shard == NULL ? 0 : (uint64_t)(
		shard->classes[HFS_IO_POOL_CLASS_COUNT - 1U].bufferBytes /
		(size_t)fs->unitSize);
	(void)pthread_once(&g_hfsIoMaxUnitsOnce, hfsIoMaxUnitsInit);
	if (g_hfsIoMaxUnitsConfigured != 0 &&
		g_hfsIoMaxUnitsConfigured < maxUnits) {
		maxUnits = g_hfsIoMaxUnitsConfigured;
	}
	return maxUnits;
}

/* Select the smallest DMA class that can hold one request. */
static HfsIoPoolClass *hfsIoSelectClass(HfsIoPoolShard *shard,
						 size_t requestedBytes) {
	uint32_t i;

	if (shard == NULL || requestedBytes == 0) {
		return NULL;
	}
	for (i = 0; i < HFS_IO_POOL_CLASS_COUNT; i++) {
		if (requestedBytes <= shard->classes[i].bufferBytes) {
			return &shard->classes[i];
		}
	}
	return NULL;
}

/* Parse per-reactor class counts from HFS_IO_POOL_CLASS_COUNTS. */
static int hfsIoPoolCountsEnv(uint32_t counts[HFS_IO_POOL_CLASS_COUNT]) {
	const char *value = getenv("HFS_IO_POOL_CLASS_COUNTS");
	char *end;
	unsigned long parsed;
	uint32_t i;

	for (i = 0; i < HFS_IO_POOL_CLASS_COUNT; i++) {
		counts[i] = g_hfsIoPoolTargetCounts[i];
	}
	if (value == NULL || value[0] == '\0') {
		return 0;
	}
	for (i = 0; i < HFS_IO_POOL_CLASS_COUNT; i++) {
		parsed = strtoul(value, &end, 10);
		if (end == value || parsed == 0 ||
		    parsed > HFS_IO_POOL_CLASS_MAX_COUNT) {
			errno = EINVAL;
			return -1;
		}
		counts[i] = (uint32_t)parsed;
		if (i + 1U == HFS_IO_POOL_CLASS_COUNT) {
			if (*end != '\0') {
				errno = EINVAL;
				return -1;
			}
			break;
		}
		if (*end != ',') {
			errno = EINVAL;
			return -1;
		}
		value = end + 1;
	}
	return 0;
}

/* Parse an optional per-reactor byte cap with K/M/G suffixes. */
static size_t hfsIoPoolBytesEnv(void) {
	const char *value = getenv("HFS_IO_POOL_BYTES");
	char *end;
	unsigned long long parsed;
	unsigned long long multiplier = 1;

	if (value == NULL || value[0] == '\0') {
		return 0;
	}
	errno = 0;
	parsed = strtoull(value, &end, 10);
	if (errno != 0 || end == value) {
		return 0;
	}
	if (*end == 'k' || *end == 'K') {
		multiplier = 1ULL << 10;
	} else if (*end == 'm' || *end == 'M') {
		multiplier = 1ULL << 20;
	} else if (*end == 'g' || *end == 'G') {
		multiplier = 1ULL << 30;
	} else if (*end != '\0') {
		return 0;
	}
	if (parsed > SIZE_MAX / multiplier) {
		return 0;
	}
	return (size_t)(parsed * multiplier);
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

/* Enter the pool API gate while preventing entry after teardown starts. */
static bool hfsIoPoolApiEnter(HfsIoPoolShard *shard) {
	uint32_t mask;

	mask = atomic_load_explicit(&shard->apiUsers, memory_order_acquire);
	for (;;) {
		if ((mask & HFS_IO_POOL_API_CLOSING) != 0U) {
			return false;
		}
		if (atomic_compare_exchange_weak_explicit(&shard->apiUsers, &mask,
								mask + 1U, memory_order_acq_rel,
								memory_order_acquire)) {
			return true;
		}
	}
}

/* Leave the pool API gate and wake teardown once the final caller exits. */
static void hfsIoPoolApiExit(HfsIoPoolShard *shard) {
	uint32_t previous;

	previous = atomic_fetch_sub_explicit(&shard->apiUsers, 1U,
						memory_order_release);
	if ((previous & HFS_IO_POOL_API_CLOSING) != 0U &&
	    (previous & ~HFS_IO_POOL_API_CLOSING) == 1U) {
		pthread_cond_broadcast(&shard->available);
	}
}

/* Claim one free slot without taking the pool mutex. */
static int hfsIoPoolFastTake(HfsIoPoolShard *shard, HfsIoPoolClass *classPool,
				     uint32_t *slot) {
	uint32_t mask;
	uint32_t bit = 0;

	if (atomic_load_explicit(&shard->closing, memory_order_acquire)) {
		return -1;
	}
	for (;;) {
		mask = atomic_load_explicit(&classPool->freeMask, memory_order_acquire);
		if (mask == 0) {
			return 1;
		}
		bit = mask & (0U - mask);
		if (atomic_compare_exchange_weak_explicit(&classPool->freeMask, &mask,
								mask & ~bit,
									memory_order_acq_rel,
									memory_order_acquire)) {
			*slot = (uint32_t)__builtin_ctz(bit);
			atomic_fetch_add_explicit(&shard->active, 1, memory_order_relaxed);
			return 0;
		}
	}
}

/* Populate a lease after the slot has been removed from the free bitmap. */
static void hfsIoLeaseFill(HfsIoPool *pool, HfsIoPoolShard *shard,
				   HfsIoPoolClass *classPool, HfsIoLease *lease,
				   uint32_t slot) {
	lease->pool = pool;
	lease->shard = shard;
	lease->classPool = classPool;
	lease->poolClass = (uint8_t)(classPool - shard->classes);
	lease->slot = slot;
	lease->buffer = classPool->storage + (size_t)slot * classPool->bufferBytes;
}

/* Acquire a buffer through the condvar slow path after the bitmap is empty. */
static int hfsIoAcquireSlow(HfsIoPool *pool, HfsIoPoolShard *shard,
				    HfsIoPoolClass *classPool, HfsIoLease *lease,
				    size_t requestedBytes, uint32_t reactorId,
				    uint64_t waitStartNs) {
	uint32_t mask;
	uint32_t bit = 0;
	bool waited = true;
	HfsTraceWaitToken traceWait = {0};
	(void)requestedBytes;

	atomic_fetch_add_explicit(&shard->waiters, 1, memory_order_relaxed);
	pthread_mutex_lock(&shard->lock);
	for (;;) {
		if (atomic_load_explicit(&shard->closing, memory_order_acquire)) {
			break;
		}
		mask = atomic_load_explicit(&classPool->freeMask, memory_order_acquire);
		if (mask == 0) {
			hfsTraceThreadWaitBegin(&traceWait, HFS_TRACE_OPERATION_THREAD,
				HFS_TRACE_WAKE_DMA_LEASE, HFS_TRACE_META_OP_NONE,
				g_hfsIoTraceParentId, 0, 0, classPool->bufferBytes, 0,
				atomic_load_explicit(&shard->active, memory_order_relaxed),
				reactorId, (uint32_t)(classPool - shard->classes));
			{
				int waitRc = pthread_cond_wait(&shard->available, &shard->lock);
				hfsTraceThreadWaitEnd(&traceWait, waitRc);
			}
			continue;
		}
		bit = mask & (0U - mask);
		if (atomic_compare_exchange_weak_explicit(&classPool->freeMask, &mask,
								mask & ~bit,
									memory_order_acq_rel,
									memory_order_acquire)) {
			break;
		}
	}
	atomic_fetch_sub_explicit(&shard->waiters, 1, memory_order_relaxed);
	if (atomic_load_explicit(&shard->closing, memory_order_acquire)) {
		pthread_mutex_unlock(&shard->lock);
		errno = ESHUTDOWN;
		return -1;
	}
	atomic_fetch_add_explicit(&shard->active, 1, memory_order_relaxed);
	hfsIoLeaseFill(pool, shard, classPool, lease, (uint32_t)__builtin_ctz(bit));
	pthread_mutex_unlock(&shard->lock);

	HFS_STATS_DMA_ACTIVE(atomic_load_explicit(&shard->active, memory_order_relaxed));
	hfsStatsRecordDmaFast(classPool->bufferBytes, waited,
			      atomic_load_explicit(&shard->active, memory_order_relaxed));
	hfsStatsRecordDmaWaitNs(hfsStatsNowNs() - waitStartNs);
	hfsTraceEmit(HFS_TRACE_LEASE_WAIT_END, g_hfsIoTraceOperation,
			     HFS_TRACE_BUFFER_DMA_LEASE, 0, g_hfsIoTraceParentId,
			     0, 0, classPool->bufferBytes, 0,
			     atomic_load_explicit(&shard->active, memory_order_relaxed),
			     reactorId, 0, lease->poolClass,
			     hfsStatsNowNs() - waitStartNs, 0);
	return 0;
}

/* Acquire a reactor-local DMA buffer, waiting only when its class is empty. */
static int hfsIoAcquire(Hfs *fs, HfsIoLease *lease, size_t requestedBytes,
				uint32_t reactorId) {
	HfsIoPool *pool = fs->ioPool;
	HfsIoPoolShard *shard = hfsIoPoolShardFor(fs, reactorId);
	HfsIoPoolClass *classPool = hfsIoSelectClass(shard, requestedBytes);
	uint32_t slot;
	int fastRc;
	uint64_t waitStartNs;

	if (classPool == NULL) {
		errno = E2BIG;
		return -1;
	}
	if (!hfsIoPoolApiEnter(shard)) {
		errno = ESHUTDOWN;
		return -1;
	}
	HFS_STATS_DMA_ACQUIRE();
	memset(lease, 0, sizeof(*lease));
	fastRc = hfsIoPoolFastTake(shard, classPool, &slot);
	if (fastRc == -1) {
		hfsIoPoolApiExit(shard);
		errno = ESHUTDOWN;
		return -1;
	}
	if (fastRc == 0) {
		hfsIoLeaseFill(pool, shard, classPool, lease, slot);
		hfsStatsRecordDmaFastAcquireFast();
		HFS_STATS_DMA_ACTIVE(atomic_load_explicit(&shard->active, memory_order_relaxed));
		hfsStatsRecordDmaFast(classPool->bufferBytes, false,
				      atomic_load_explicit(&shard->active, memory_order_relaxed));
		hfsTraceEmit(HFS_TRACE_LEASE_ACQUIRE, g_hfsIoTraceOperation,
			     HFS_TRACE_BUFFER_DMA_LEASE, 0, g_hfsIoTraceParentId,
			     0, 0, classPool->bufferBytes, 0,
			     atomic_load_explicit(&shard->active, memory_order_relaxed),
			     reactorId, 0, lease->poolClass, 0, 0);
		hfsIoPoolApiExit(shard);
		return 0;
	}
	waitStartNs = hfsStatsNowNs();
	HFS_STATS_DMA_WAIT();
	hfsTraceEmit(HFS_TRACE_LEASE_WAIT_BEGIN, g_hfsIoTraceOperation,
			     HFS_TRACE_BUFFER_DMA_LEASE, 0, g_hfsIoTraceParentId,
			     0, 0, requestedBytes, 0,
			     atomic_load_explicit(&shard->active, memory_order_relaxed),
			     reactorId, 0, (uint32_t)(classPool - shard->classes), 0, 0);
	fastRc = hfsIoAcquireSlow(pool, shard, classPool, lease, requestedBytes,
					 reactorId, waitStartNs);
	hfsIoPoolApiExit(shard);
	return fastRc;
}

/* Return one DMA buffer atomically and wake waiters only when needed. */
static void hfsIoRelease(HfsIoLease *lease) {
	HfsIoPoolShard *shard;
	HfsIoPoolClass *classPool;
	uint32_t previousActive;
	bool wokeWaiter = false;

	if (lease == NULL || lease->pool == NULL) {
		return;
	}
	if (lease->shard == NULL || lease->classPool == NULL) {
		memset(lease, 0, sizeof(*lease));
		return;
	}
	shard = lease->shard;
	classPool = lease->classPool;
	atomic_fetch_or_explicit(&classPool->freeMask, 1U << lease->slot,
					memory_order_release);
	previousActive = atomic_fetch_sub_explicit(&shard->active, 1,
							 memory_order_relaxed);
	HFS_STATS_DMA_RELEASE();
	hfsTraceEmit(HFS_TRACE_LEASE_RELEASE, g_hfsIoTraceOperation,
			     HFS_TRACE_BUFFER_DMA_LEASE, 0, g_hfsIoTraceParentId,
			     0, 0, classPool->bufferBytes, 0, previousActive - 1U,
			     0, 0, lease->poolClass, 0, 0);
	if (atomic_load_explicit(&shard->closing, memory_order_acquire)) {
		/* The fini path owns the mutex while waiting; signal without relocking. */
		pthread_cond_broadcast(&shard->available);
		wokeWaiter = true;
	} else if (atomic_load_explicit(&shard->waiters, memory_order_relaxed) != 0) {
		pthread_mutex_lock(&shard->lock);
		pthread_cond_broadcast(&shard->available);
		pthread_mutex_unlock(&shard->lock);
		wokeWaiter = true;
	}
	if (wokeWaiter) {
		hfsTraceThreadWake(HFS_TRACE_OPERATION_THREAD,
			HFS_TRACE_WAKE_DMA_LEASE, HFS_TRACE_META_OP_NONE,
			g_hfsIoTraceParentId, 0, 0, classPool->bufferBytes, 0,
			previousActive - 1U, 0, lease->poolClass, 0);
	}
	memset(lease, 0, sizeof(*lease));
}

/* Select the pool shard for a logical unit in one file. */
static uint32_t hfsIoReactorForUnit(const HfsFile *file, uint64_t unitOffset) {
	if (file == NULL || file->fs == NULL) {
		return 0;
	}
	return hfsRuntimeDataReactorFor(file->fs, file->blobId, unitOffset);
}

/* Acquire a DMA lease from the reactor that will execute the request. */
static int hfsIoAcquireAt(HfsFile *file, HfsIoLease *lease,
				  size_t requestedBytes, uint64_t unitOffset) {
	return hfsIoAcquire(file->fs, lease, requestedBytes,
			    hfsIoReactorForUnit(file, unitOffset));
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
	hfsProfileChildSubmit(&request->profileChild);
	/* Count one real SPDK data callback window, separate from queue dispatch depth. */
	hfsStatsRecordIoSubmitFast();
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
	uint64_t durationNs = 0;
	uint64_t traceRequestId;
	uint64_t traceParentId;
	uint64_t traceInodeId;
	uint64_t traceOffset;
	uint64_t traceBytes;
	uint32_t traceUnits;
	uint32_t traceReactor;
	HfsTraceOperation traceOperation;
	HfsTraceBufferKind traceBufferKind;
	HfsFile *traceFile;
	uint64_t completedUnits;

	/* Publish reactor accounting before waking waiters or shutdown snapshots. */
	hfsProfileChildComplete(&request->profileChild);
	hfsStatsRecordIoCompleteFast();
	if (request->dataPath) {
		hfsStatsRecordDataReactorCompleteFast(request->dataReactor);
	}
	/* Snapshot all request fields before waking a waiter that may unwind a stack request. */
	traceRequestId = request->result.requestId;
	traceParentId = request->traceParentId;
	traceFile = request->file;
	traceInodeId = traceFile != NULL ? traceFile->inodeId : 0;
	traceOffset = request->offset;
	traceBytes = request->units *
		(traceFile != NULL && traceFile->fs != NULL ? traceFile->fs->unitSize : 0);
	traceUnits = (uint32_t)request->units;
	traceReactor = request->dataReactor;
	traceOperation = hfsIoTraceOperation(request->operation);
	traceBufferKind = request->traceBufferKind;
	completedUnits = request->units;
	if (request->traceSubmitNs != 0) {
		uint64_t nowNs = hfsTraceNowNs();
		if (nowNs >= request->traceSubmitNs) {
			durationNs = nowNs - request->traceSubmitNs;
		}
	}
	hfsTraceEmit(HFS_TRACE_SPDK_COMPLETE,
		     traceOperation, traceBufferKind, traceRequestId, traceParentId,
		     traceInodeId, traceOffset, traceBytes, traceUnits, 0,
		     traceReactor, 0,
		     0, durationNs, bserrno);
	/* Publish completion only after tracing stops touching the caller-owned request. */
	hfsRequestComplete(&request->result, bserrno, completedUnits);
}

/* Submit one child request without waiting for its owner-thread completion. */
static int hfsIoStartRequest(HfsIoRequest *request) {
	int rc;

	if (request == NULL || request->file == NULL || request->file->fs == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (request->traceParentId == 0) {
		request->traceParentId = g_hfsIoTraceParentId;
	}
	if (request->traceBufferKind == HFS_TRACE_BUFFER_NONE) {
		request->traceBufferKind = request->buffer != NULL ?
			HFS_TRACE_BUFFER_DMA_LEASE : HFS_TRACE_BUFFER_NONE;
	}
	hfsRequestInit(&request->result, hfsIoRequestOperation(request->operation));
	hfsProfileChildInit(&request->profileChild, hfsProfileCurrent(),
		request->dataReactor);
	request->profileChild.requestId = request->result.requestId;
	request->batchItem.profileChild = &request->profileChild;
	if (request->profileChild.parent != NULL &&
		request->profileChild.parent->enabled) {
		request->profileChild.parent->flags |= HFS_PROFILE_FLAG_DATA;
		hfsProfileStage(request->profileChild.parent, HFS_PROFILE_T3);
	}
	/* Copy the logical request identity into the completion for wait/wake traces. */
	request->result.completion.traceParentId = request->traceParentId;
	request->result.completion.traceInodeId = request->file->inodeId;
	request->result.completion.traceOffset = request->offset;
	request->result.completion.traceBytes =
		request->units * request->file->fs->unitSize;
	request->result.completion.traceBufferKind =
		(uint8_t)request->traceBufferKind;
	request->result.completion.profileChild = &request->profileChild;
	request->traceSubmitNs = hfsTraceNowNs();
	hfsTraceEmit(HFS_TRACE_SPDK_SUBMIT,
		     hfsIoTraceOperation(request->operation),
		     request->traceBufferKind,
		     request->result.requestId, request->traceParentId,
		     request->file->inodeId, request->offset,
		     request->units * request->file->fs->unitSize,
		     (uint32_t)request->units, 0, request->dataReactor, 0,
		     0, 0, 0);
	request->batchItem.traceParentId = request->traceParentId;
	if (request->dataPath && request->dataReactor != 0) {
		rc = hfsRequestSubmitDataBatch(&request->result, hfsIoStart,
						request, request->dataReactor,
						&request->batchItem);
	} else {
		rc = hfsRequestSubmitBatch(&request->result, hfsIoStart, request,
					     &request->batchItem);
	}
	if (rc != 0) {
		hfsStatsRecordIoCompleteFast();
	}
	if (rc == 0) {
		hfsProfileChildSubmitted(request->profileChild.parent);
	}
	if (rc == 0 && request->dataPath) {
		hfsStatsRecordDataReactorSubmitFast(request->dataReactor);
	}
	return rc;
}

/* Forward declaration used by direct and vector synchronous submissions. */
static int hfsIoWaitRequest(HfsIoRequest *request);

/* Allocate internal DMA memory for POSIX staging and metadata operations. */
void *hfsIoDmaAlloc(size_t size, size_t alignment) {
	void *buffer;

	if (size == 0) {
		errno = EINVAL;
		return NULL;
	}
	if (alignment == 0) {
		alignment = 0x1000;
	}
	if ((alignment & (alignment - 1U)) != 0 || alignment < sizeof(void *)) {
		errno = EINVAL;
		return NULL;
	}
	buffer = spdk_malloc(size, alignment, NULL, SPDK_ENV_LCORE_ID_ANY,
				    SPDK_MALLOC_DMA);
	if (buffer == NULL) {
		errno = ENOMEM;
	}
	return buffer;
}

/* Free internal DMA memory returned by hfsIoDmaAlloc. */
void hfsIoDmaFree(void *buffer) {
	if (buffer != NULL) {
		spdk_free(buffer);
	}
}

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
	request.traceBufferKind = HFS_TRACE_BUFFER_CALLER;
	request.dataReactor = hfsRuntimeDataReactorFor(file->fs, file->blobId,
								 offset);
	request.channel = hfsRuntimeDataChannel(file->fs, request.dataReactor);
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
	HfsTraceLockToken syncTrace = {0};
	HfsInodeRuntime *runtime;
	HfsIoRequest request;
	uint32_t dataReactor;
	int rc;

	if (!hfsIoVectorDmaCapable(file, iov, iovcnt, total, offset)) {
		errno = EAGAIN;
		return -1;
	}
	memcpy(vector, iov, (size_t)iovcnt * sizeof(*iov));
	runtime = hfsIoSyncReadLock(file, &syncTrace);
	if (runtime == NULL) {
		return -1;
	}
	if (write && hfsResizeTo(file, offset + total) != 0) {
		hfsIoSyncReadUnlock(runtime, &syncTrace);
		return -1;
	}
	if (hfsIoRangeLock(file, offset, total, write, &guard) != 0) {
		hfsIoSyncReadUnlock(runtime, &syncTrace);
		return -1;
	}
	dataReactor = hfsRuntimeDataReactorFor(file->fs, file->blobId,
							 offset / file->fs->unitSize);
	memset(&request, 0, sizeof(request));
	request.file = file;
	request.iov = vector;
	request.iovcnt = iovcnt;
	request.vector = true;
	request.offset = offset / file->fs->unitSize;
	request.units = total / file->fs->unitSize;
	request.operation = write ? HFS_IO_WRITE : HFS_IO_READ;
	request.traceBufferKind = HFS_TRACE_BUFFER_VECTOR;
	request.channel = hfsRuntimeDataChannel(file->fs, dataReactor);
	request.dataReactor = dataReactor;
	request.dataPath = true;
	rc = request.channel == NULL ? -1 : hfsIoStartRequest(&request);
	if (rc == 0) {
		rc = hfsIoWaitRequest(&request);
	}
	hfsIoRangeUnlock(file, &guard);
	hfsIoSyncReadUnlock(runtime, &syncTrace);
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

/* Wait for read children, copy each result, and release its lease immediately. */
static int hfsIoWaitReadPipeline(HfsIoRequest *requests, HfsIoLease *leases,
				 uint32_t count, uint8_t *buffer,
				 uint64_t baseUnit, size_t unitSize) {
	int rc = 0;
	int firstErrno = 0;
	uint32_t i;

	for (i = 0; i < count; i++) {
		size_t bytes = (size_t)(requests[i].units * unitSize);

		if (hfsIoWaitRequest(&requests[i]) != 0) {
			if (firstErrno == 0) {
				firstErrno = errno;
			}
			hfsIoRelease(&leases[i]);
			rc = -1;
			continue;
		}
		{
			uint64_t copyStartNs = hfsTraceNowNs();
			uint64_t copyStartTick = hfsProfileNow();
			size_t destinationOffset = (size_t)(requests[i].offset - baseUnit) *
				unitSize;

			hfsTraceEmit(HFS_TRACE_COPY_OUT_BEGIN, HFS_TRACE_OPERATION_READ,
				     HFS_TRACE_BUFFER_DMA_LEASE, 0,
				     g_hfsIoTraceParentId, requests[i].file->inodeId,
				     requests[i].offset * unitSize, bytes, 0, 0, 0, 0,
				     leases[i].poolClass, 0, 0);
			memcpy(buffer + destinationOffset, leases[i].buffer, bytes);
			hfsStatsRecordCopyOutTicksFast(hfsProfileNow() - copyStartTick);
			hfsTraceEmit(HFS_TRACE_COPY_OUT_END, HFS_TRACE_OPERATION_READ,
				     HFS_TRACE_BUFFER_DMA_LEASE, 0,
				     g_hfsIoTraceParentId, requests[i].file->inodeId,
				     requests[i].offset * unitSize, bytes, 0, 0, 0, 0,
				     leases[i].poolClass,
				     hfsTraceNowNs() - copyStartNs, 0);
		}
		hfsStatsRecordCopyOutFast(bytes);
		hfsIoRelease(&leases[i]);
	}
	if (firstErrno != 0) {
		errno = firstErrno;
	}
	return rc;
}

/* Start one request-local transfer and wait for its owner-thread callback. */
static int hfsIoSubmit(HfsFile *file, void *buffer, uint64_t offset,
		       uint64_t units, HfsIoOperation operation) {
	HfsIoRequest request;
	uint32_t dataReactor;

	/* The callback owns only this request, so the HfsFile has no I/O scratch state. */
	memset(&request, 0, sizeof(request));
	/* Keep bounce-buffer I/O on the same logical stripe as direct I/O. */
	dataReactor = hfsRuntimeDataReactorFor(file->fs, file->blobId,
							 offset);
	request.file = file;
	request.buffer = buffer;
	request.offset = offset;
	request.units = units;
	request.operation = operation;
	request.channel = hfsRuntimeDataChannel(file->fs, dataReactor);
	request.dataReactor = dataReactor;
	request.dataPath = true;
	if (request.channel == NULL) {
		errno = EIO;
		return -1;
	}
	if (hfsIoStartRequest(&request) != 0) {
		HFS_STATS_ERROR();
		return -1;
	}
	if (hfsIoWaitRequest(&request) != 0) {
		return -1;
	}
	return 0;
}

/* Initialize one reactor-local DMA pool shard. */
static int hfsIoPoolShardInit(HfsIoPoolShard *shard,
				      const size_t bufferBytes[HFS_IO_POOL_CLASS_COUNT],
				      const uint32_t counts[HFS_IO_POOL_CLASS_COUNT]) {
	uint32_t i;

	if (pthread_mutex_init(&shard->lock, NULL) != 0) {
		errno = ENOMEM;
		return -1;
	}
	if (pthread_cond_init(&shard->available, NULL) != 0) {
		pthread_mutex_destroy(&shard->lock);
		errno = ENOMEM;
		return -1;
	}
	atomic_init(&shard->active, 0);
	atomic_init(&shard->apiUsers, 0);
	atomic_init(&shard->waiters, 0);
	atomic_init(&shard->closing, false);
	for (i = 0; i < HFS_IO_POOL_CLASS_COUNT; i++) {
		HfsIoPoolClass *classPool = &shard->classes[i];
		size_t storageBytes;

		if (bufferBytes[i] == 0 || counts[i] == 0 ||
		    counts[i] > SIZE_MAX / bufferBytes[i]) {
			errno = EOVERFLOW;
			goto fail;
		}
		storageBytes = bufferBytes[i] * counts[i];
		classPool->storage = spdk_malloc(storageBytes, 0x1000, NULL,
					SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (classPool->storage == NULL) {
			errno = ENOMEM;
			goto fail;
		}
		classPool->bufferBytes = bufferBytes[i];
		classPool->count = counts[i];
		atomic_init(&classPool->freeMask,
				counts[i] == 32U ? UINT32_MAX : ((1U << counts[i]) - 1U));
	}
	return 0;

fail:
	while (i > 0) {
		i--;
		spdk_free(shard->classes[i].storage);
		shard->classes[i].storage = NULL;
	}
	pthread_cond_destroy(&shard->available);
	pthread_mutex_destroy(&shard->lock);
	return -1;
}

/* Drain one reactor-local pool and release its DMA storage. */
static void hfsIoPoolShardFini(HfsIoPoolShard *shard) {
	uint32_t i;

	/* Close the API gate before waiting so no new acquire can touch the shard. */
	atomic_fetch_or_explicit(&shard->apiUsers, HFS_IO_POOL_API_CLOSING,
					memory_order_acq_rel);
	atomic_store_explicit(&shard->closing, true, memory_order_release);
	pthread_mutex_lock(&shard->lock);
	pthread_cond_broadcast(&shard->available);
	hfsTraceThreadWake(HFS_TRACE_OPERATION_THREAD, HFS_TRACE_WAKE_DMA_LEASE,
		HFS_TRACE_META_OP_NONE, 0, 0, 0, 0, 0,
		atomic_load_explicit(&shard->active, memory_order_relaxed), 0,
		UINT32_MAX, 0);
	while (atomic_load_explicit(&shard->active, memory_order_acquire) != 0 ||
	       (atomic_load_explicit(&shard->apiUsers, memory_order_acquire) &
		~HFS_IO_POOL_API_CLOSING) != 0U) {
		HfsTraceWaitToken traceWait = {0};
		hfsTraceThreadWaitBegin(&traceWait, HFS_TRACE_OPERATION_THREAD,
			HFS_TRACE_WAKE_DMA_LEASE, HFS_TRACE_META_OP_NONE, 0, 0, 0, 0,
			0, atomic_load_explicit(&shard->active, memory_order_relaxed), 0,
			UINT32_MAX);
		{
			int waitRc = pthread_cond_wait(&shard->available, &shard->lock);
			hfsTraceThreadWaitEnd(&traceWait, waitRc);
		}
	}
	pthread_mutex_unlock(&shard->lock);
	for (i = 0; i < HFS_IO_POOL_CLASS_COUNT; i++) {
		spdk_free(shard->classes[i].storage);
		shard->classes[i].storage = NULL;
	}
	pthread_cond_destroy(&shard->available);
	pthread_mutex_destroy(&shard->lock);
}

/* Align configured class sizes and enforce the optional per-reactor cap. */
static int hfsIoPoolLayout(Hfs *fs, size_t bufferBytes[HFS_IO_POOL_CLASS_COUNT],
				   uint32_t counts[HFS_IO_POOL_CLASS_COUNT]) {
	size_t limit = hfsIoPoolBytesEnv();
	size_t total = 0;
	uint32_t i;

	for (i = 0; i < HFS_IO_POOL_CLASS_COUNT; i++) {
		bufferBytes[i] = g_hfsIoPoolTargetBytes[i];
		if (bufferBytes[i] < (size_t)fs->unitSize) {
			bufferBytes[i] = (size_t)fs->unitSize;
		}
		if (bufferBytes[i] > SIZE_MAX - ((size_t)fs->unitSize - 1U)) {
			errno = EOVERFLOW;
			return -1;
		}
		bufferBytes[i] += (size_t)fs->unitSize - 1U;
		bufferBytes[i] -= bufferBytes[i] % (size_t)fs->unitSize;
		if (bufferBytes[i] == 0 || counts[i] > SIZE_MAX / bufferBytes[i]) {
			errno = EOVERFLOW;
			return -1;
		}
		total += bufferBytes[i] * counts[i];
	}
	if (limit == 0 || total <= limit) {
		return 0;
	}
	for (i = HFS_IO_POOL_CLASS_COUNT; i > 0 && total > limit; i--) {
		uint32_t classIndex = i - 1U;

		while (counts[classIndex] > 1U && total > limit) {
			counts[classIndex]--;
			total -= bufferBytes[classIndex];
		}
	}
	if (total > limit) {
		errno = E2BIG;
		return -1;
	}
	return 0;
}

/* Initialize per-reactor DMA pools and data synchronization state. */
int hfsIoInit(Hfs *fs) {
	HfsIoPool *pool;
	size_t bufferBytes[HFS_IO_POOL_CLASS_COUNT];
	uint32_t counts[HFS_IO_POOL_CLASS_COUNT];
	int rc;
	uint32_t rangeLocksReady = 0;
	uint32_t shardsReady = 0;
	bool runtimeReady = false;

	if (fs == NULL || fs->unitSize == 0 ||
	    (size_t)fs->unitSize != fs->unitSize) {
		errno = EINVAL;
		return -1;
	}
	if (hfsIoPoolCountsEnv(counts) != 0 ||
	    hfsIoPoolLayout(fs, bufferBytes, counts) != 0) {
		return -1;
	}
	pool = calloc(1, sizeof(*pool));
	if (pool == NULL) {
		errno = ENOMEM;
		return -1;
	}
	pool->shardCount = fs->dataReactorCount == 0 ? 1 : fs->dataReactorCount;
	if (pool->shardCount > HFS_IO_MAX_POOL_REACTORS) {
		pool->shardCount = HFS_IO_MAX_POOL_REACTORS;
	}
	for (shardsReady = 0; shardsReady < pool->shardCount; shardsReady++) {
		if (hfsIoPoolShardInit(&pool->shards[shardsReady], bufferBytes,
					counts) != 0) {
			goto fail_pool_shards;
		}
	}
	for (rangeLocksReady = 0; rangeLocksReady < HFS_RANGE_LOCK_COUNT;
	     rangeLocksReady++) {
		rc = hfsRangeLockInit(&fs->rangeLocks[rangeLocksReady]);
		if (rc != 0) {
			errno = rc;
			goto fail_range_locks;
		}
	}
	if (hfsInodeRuntimeInit(fs) != 0) {
		goto fail_range_locks;
	}
	runtimeReady = true;
	fs->ioPool = pool;
	return 0;

fail_range_locks:
	if (runtimeReady) {
		hfsInodeRuntimeFini(fs);
	}
	while (rangeLocksReady > 0) {
		rangeLocksReady--;
		hfsRangeLockDestroy(&fs->rangeLocks[rangeLocksReady]);
	}
fail_pool_shards:
	while (shardsReady > 0) {
		shardsReady--;
		hfsIoPoolShardFini(&pool->shards[shardsReady]);
	}
	free(pool);
	return -1;
}

/* Drain all reactor-local pools and release filesystem I/O state. */
void hfsIoFini(Hfs *fs) {
	HfsIoPool *pool;
	uint32_t i;

	if (fs == NULL || fs->ioPool == NULL) {
		return;
	}
	pool = fs->ioPool;
	for (i = 0; i < pool->shardCount; i++) {
		hfsIoPoolShardFini(&pool->shards[i]);
	}
	free(pool);
	fs->ioPool = NULL;
	hfsInodeRuntimeFini(fs);
	for (i = 0; i < HFS_RANGE_LOCK_COUNT; i++) {
		hfsRangeLockDestroy(&fs->rangeLocks[i]);
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

	if (hfsIoAcquireAt(file, &lease, (size_t)file->fs->unitSize,
			   unitOffset) != 0) {
		return -1;
	}
	rc = hfsIoReadUnit(file, &lease, unitOffset);
	if (rc == 0) {
		uint64_t copyStartNs = hfsTraceNowNs();
		uint64_t copyStartTick = hfsProfileNow();
		hfsTraceEmit(HFS_TRACE_COPY_IN_BEGIN, HFS_TRACE_OPERATION_WRITE,
			     HFS_TRACE_BUFFER_RMW, 0, g_hfsIoTraceParentId,
			     file->inodeId, unitOffset * file->fs->unitSize,
			     length, 0, 0, 0, 0, lease.poolClass, 0, 0);
		hfsStatsRecordRmwFast(file->fs->unitSize);
		memcpy(lease.buffer + unitIndex, source, length);
		hfsStatsRecordCopyInFast(length);
		hfsStatsRecordCopyInTicksFast(hfsProfileNow() - copyStartTick);
		hfsTraceEmit(HFS_TRACE_COPY_IN_END, HFS_TRACE_OPERATION_WRITE,
			     HFS_TRACE_BUFFER_RMW, 0, g_hfsIoTraceParentId,
			     file->inodeId, unitOffset * file->fs->unitSize,
			     length, 0, 0, 0, 0, lease.poolClass,
			     hfsTraceNowNs() - copyStartNs, 0);
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

		if (hfsIoAcquireAt(file, &lease,
				(size_t)(chunkUnits * file->fs->unitSize), offset) != 0) {
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
				 size_t count, uint64_t offset,
				 bool windowedReservation,
				 bool directEligible) {
	uint64_t unitSize = file->fs->unitSize;
	uint64_t cursor = offset;
	size_t remaining = count;
	size_t done = 0;
	if (directEligible) {
		uint64_t maxUnits = hfsIoMaxUnits(file->fs);
		uint64_t units = count / unitSize;
		uint64_t cursorUnit = offset / unitSize;
		size_t bytesDone = 0;

		while (units != 0) {
			uint64_t chunkUnits = units > maxUnits ? maxUnits : units;
			size_t chunkBytes = (size_t)(chunkUnits * unitSize);
			HfsIoRangeGuard windowGuard = {0};
			int rc;

			if (windowedReservation && hfsIoRangeLock(file,
					cursorUnit * unitSize, chunkBytes, false,
					&windowGuard) != 0) {
				return -1;
			}
			rc = hfsIoSubmitDirect(file, buffer + bytesDone, cursorUnit,
					chunkUnits, false);
			if (windowedReservation) {
				hfsIoRangeUnlock(file, &windowGuard);
			}
			if (rc != 0) {
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
		if (hfsIoAcquireAt(file, &lease, (size_t)file->fs->unitSize,
				   unitOffset) != 0) {
			return -1;
		}
		if (hfsIoReadUnit(file, &lease, unitOffset) != 0) {
			hfsIoRelease(&lease);
			return -1;
		}
		{
			uint64_t copyStartNs = hfsTraceNowNs();
			uint64_t copyStartTick = hfsProfileNow();
			hfsTraceEmit(HFS_TRACE_COPY_OUT_BEGIN, HFS_TRACE_OPERATION_READ,
				     HFS_TRACE_BUFFER_RMW, 0, g_hfsIoTraceParentId,
				     file->inodeId, cursor, take, 0, 0, 0, 0,
				     lease.poolClass, 0, 0);
			memcpy(buffer, lease.buffer + unitIndex, take);
			hfsStatsRecordCopyOutTicksFast(hfsProfileNow() - copyStartTick);
			hfsTraceEmit(HFS_TRACE_COPY_OUT_END, HFS_TRACE_OPERATION_READ,
				     HFS_TRACE_BUFFER_RMW, 0, g_hfsIoTraceParentId,
				     file->inodeId, cursor, take, 0, 0, 0, 0,
				     lease.poolClass, hfsTraceNowNs() - copyStartNs, 0);
		}
		hfsStatsRecordCopyOutFast(take);
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
		HfsIoRangeGuard windowGuard = {0};
		uint64_t windowUnits = fullUnits;

		if (windowUnits > (uint64_t)pipelineDepth * maxUnits) {
			windowUnits = (uint64_t)pipelineDepth * maxUnits;
		}
		if (windowedReservation && hfsIoRangeLock(file, cursor,
				(size_t)(windowUnits * unitSize), false,
				&windowGuard) != 0) {
			return -1;
		}

		while (requestCount < pipelineDepth && pendingUnits < fullUnits) {
			uint64_t chunkUnits = fullUnits - pendingUnits;

			if (chunkUnits > maxUnits) {
				chunkUnits = maxUnits;
			}
			if (hfsIoAcquireAt(file, &leases[requestCount],
					(size_t)(chunkUnits * unitSize),
					cursor / unitSize + pendingUnits) != 0) {
					hfsIoWaitPipeline(requests, requestCount);
				hfsIoReleasePipeline(leases, requestCount);
				if (windowedReservation) {
					hfsIoRangeUnlock(file, &windowGuard);
				}
				return -1;
			}
			requests[requestCount].file = file;
			requests[requestCount].buffer = leases[requestCount].buffer;
			requests[requestCount].offset = cursor / unitSize + pendingUnits;
			requests[requestCount].units = chunkUnits;
			requests[requestCount].operation = HFS_IO_READ;
			requests[requestCount].dataReactor = hfsRuntimeDataReactorFor(
				file->fs, file->blobId, requests[requestCount].offset);
			requests[requestCount].channel = hfsRuntimeDataChannel(file->fs,
											 requests[requestCount].dataReactor);
			requests[requestCount].dataPath = true;
			if (hfsIoStartRequest(&requests[requestCount]) != 0) {
				hfsIoRelease(&leases[requestCount]);
				hfsIoWaitPipeline(requests, requestCount);
				hfsIoReleasePipeline(leases, requestCount);
				if (windowedReservation) {
					hfsIoRangeUnlock(file, &windowGuard);
				}
				return -1;
			}
			requestCount++;
			pendingUnits += chunkUnits;
		}
		hfsStatsRecordIoPipelineFast(requestCount);
		if (hfsIoWaitReadPipeline(requests, leases, requestCount, buffer,
				cursor / unitSize, (size_t)unitSize) != 0) {
			if (windowedReservation) {
				hfsIoRangeUnlock(file, &windowGuard);
			}
			return -1;
		}
		buffer += (size_t)(pendingUnits * unitSize);
		if (windowedReservation) {
			hfsIoRangeUnlock(file, &windowGuard);
		}
		cursor += pendingUnits * unitSize;
		remaining -= (size_t)(pendingUnits * unitSize);
		done += (size_t)(pendingUnits * unitSize);
	}

	if (remaining != 0) {
		HfsIoLease lease;

		if (hfsIoAcquireAt(file, &lease, (size_t)unitSize,
				   cursor / unitSize) != 0) {
			return -1;
		}
		if (hfsIoReadUnit(file, &lease, cursor / unitSize) != 0) {
			hfsIoRelease(&lease);
			return -1;
		}
		{
			uint64_t copyStartNs = hfsTraceNowNs();
			uint64_t copyStartTick = hfsProfileNow();
			hfsTraceEmit(HFS_TRACE_COPY_OUT_BEGIN, HFS_TRACE_OPERATION_READ,
				     HFS_TRACE_BUFFER_DMA_LEASE, 0, g_hfsIoTraceParentId,
				     file->inodeId, cursor, remaining, 0, 0, 0, 0,
				     lease.poolClass, 0, 0);
			memcpy(buffer, lease.buffer, remaining);
			hfsStatsRecordCopyOutTicksFast(hfsProfileNow() - copyStartTick);
			hfsTraceEmit(HFS_TRACE_COPY_OUT_END, HFS_TRACE_OPERATION_READ,
				     HFS_TRACE_BUFFER_DMA_LEASE, 0, g_hfsIoTraceParentId,
				     file->inodeId, cursor, remaining, 0, 0, 0, 0,
				     lease.poolClass, hfsTraceNowNs() - copyStartNs, 0);
		}
		hfsStatsRecordCopyOutFast(remaining);
		hfsIoRelease(&lease);
		done += remaining;
	}
	return (ssize_t)done;
}

/* Write a positioned range while the caller holds the selected data lock. */
static ssize_t hfsIoWriteAtLocked(HfsFile *file, const uint8_t *buffer,
				  size_t count, uint64_t offset,
				  bool windowedReservation,
				  bool directEligible) {
	uint64_t unitSize = file->fs->unitSize;
	uint64_t cursor = offset;
	size_t remaining = count;
	size_t done = 0;
	if (directEligible) {
		uint64_t maxUnits = hfsIoMaxUnits(file->fs);
		uint64_t units = count / unitSize;
		uint64_t cursorUnit = offset / unitSize;
		size_t bytesDone = 0;

		while (units != 0) {
			uint64_t chunkUnits = units > maxUnits ? maxUnits : units;
			size_t chunkBytes = (size_t)(chunkUnits * unitSize);
			HfsIoRangeGuard windowGuard = {0};
			int rc;

			if (windowedReservation && hfsIoRangeLock(file,
					cursorUnit * unitSize, chunkBytes, true,
					&windowGuard) != 0) {
				return -1;
			}
			rc = hfsIoSubmitDirect(file, (void *)(buffer + bytesDone),
					cursorUnit, chunkUnits, true);
			if (windowedReservation) {
				hfsIoRangeUnlock(file, &windowGuard);
			}
			if (rc != 0) {
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
		HfsIoRangeGuard windowGuard = {0};
		uint64_t windowUnits = fullUnits;

		if (windowUnits > (uint64_t)pipelineDepth * maxUnits) {
			windowUnits = (uint64_t)pipelineDepth * maxUnits;
		}
		if (windowedReservation && hfsIoRangeLock(file, cursor,
				(size_t)(windowUnits * unitSize), true,
				&windowGuard) != 0) {
			return -1;
		}

		while (requestCount < pipelineDepth && pendingUnits < fullUnits) {
			uint64_t chunkUnits = fullUnits - pendingUnits;
			size_t bytes;

			if (chunkUnits > maxUnits) {
				chunkUnits = maxUnits;
			}
			if (hfsIoAcquireAt(file, &leases[requestCount],
					(size_t)(chunkUnits * unitSize),
					cursor / unitSize + pendingUnits) != 0) {
				hfsIoWaitPipeline(requests, requestCount);
				hfsIoReleasePipeline(leases, requestCount);
				if (windowedReservation) {
					hfsIoRangeUnlock(file, &windowGuard);
				}
				return -1;
			}
			bytes = (size_t)(chunkUnits * unitSize);
			{
				uint64_t copyStartNs = hfsTraceNowNs();
				uint64_t copyStartTick = hfsProfileNow();
				hfsTraceEmit(HFS_TRACE_COPY_IN_BEGIN,
					     HFS_TRACE_OPERATION_WRITE,
					     HFS_TRACE_BUFFER_DMA_LEASE, 0,
					     g_hfsIoTraceParentId, file->inodeId,
					     cursor + pendingUnits * unitSize, bytes,
					     0, 0, 0, 0,
					     leases[requestCount].poolClass, 0, 0);
				memcpy(leases[requestCount].buffer,
				       buffer + (size_t)(pendingUnits * unitSize), bytes);
				hfsStatsRecordCopyInTicksFast(hfsProfileNow() - copyStartTick);
				hfsTraceEmit(HFS_TRACE_COPY_IN_END,
					     HFS_TRACE_OPERATION_WRITE,
					     HFS_TRACE_BUFFER_DMA_LEASE, 0,
					     g_hfsIoTraceParentId, file->inodeId,
					     cursor + pendingUnits * unitSize, bytes,
					     0, 0, 0, 0,
					     leases[requestCount].poolClass,
					     hfsTraceNowNs() - copyStartNs, 0);
			}
			hfsStatsRecordCopyInFast(bytes);
			requests[requestCount].file = file;
			requests[requestCount].buffer = leases[requestCount].buffer;
			requests[requestCount].offset = cursor / unitSize + pendingUnits;
			requests[requestCount].units = chunkUnits;
			requests[requestCount].operation = HFS_IO_WRITE;
			requests[requestCount].dataReactor = hfsRuntimeDataReactorFor(
				file->fs, file->blobId, requests[requestCount].offset);
			requests[requestCount].channel = hfsRuntimeDataChannel(file->fs,
											  requests[requestCount].dataReactor);
			requests[requestCount].dataPath = true;
			if (hfsIoStartRequest(&requests[requestCount]) != 0) {
				hfsIoRelease(&leases[requestCount]);
				hfsIoWaitPipeline(requests, requestCount);
				hfsIoReleasePipeline(leases, requestCount);
				if (windowedReservation) {
					hfsIoRangeUnlock(file, &windowGuard);
				}
				return -1;
			}
			requestCount++;
			pendingUnits += chunkUnits;
		}
		hfsStatsRecordIoPipelineFast(requestCount);
		if (hfsIoWaitPipeline(requests, requestCount) != 0) {
			hfsIoReleasePipeline(leases, requestCount);
			if (windowedReservation) {
				hfsIoRangeUnlock(file, &windowGuard);
			}
			return -1;
		}
		hfsIoReleasePipeline(leases, requestCount);
		if (windowedReservation) {
			hfsIoRangeUnlock(file, &windowGuard);
		}
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
	HfsPosixIoTicket ticket = {0};
	HfsIoRangeGuard guard = {0};
	HfsTraceLockToken syncTrace = {0};
	ssize_t result;
	bool aligned;
	bool directEligible;
	bool ticketActive = false;
	bool ticketInitialized = false;
	bool rangeActive = false;
	HfsPosixOrderingMode ordering;
	uint64_t traceId;
	uint64_t traceStartNs;
	uint64_t previousTraceParent;
	HfsTraceOperation previousTraceOperation;
	HfsIoProfile *profile = hfsProfileCurrent();

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
	directEligible = aligned && hfsIoBufferDmaCapable(buffer, count);
	traceId = profile != NULL && profile->enabled ? profile->parentId :
		hfsTraceNextRequestId();
	traceStartNs = hfsTraceNowNs();
	previousTraceParent = g_hfsIoTraceParentId;
	previousTraceOperation = g_hfsIoTraceOperation;
	g_hfsIoTraceParentId = traceId;
	g_hfsIoTraceOperation = HFS_TRACE_OPERATION_READ;
	hfsTraceEmit(HFS_TRACE_IO_ENTER, HFS_TRACE_OPERATION_READ,
		     directEligible ? HFS_TRACE_BUFFER_CALLER :
		     (aligned ? HFS_TRACE_BUFFER_DMA_LEASE : HFS_TRACE_BUFFER_RMW),
		     0, traceId, file->inodeId, offset, count,
		     (uint32_t)(count / file->fs->unitSize), 0, 0, 0, 0, 0, 0);
	hfsTraceEmit(HFS_TRACE_BUFFER_CLASSIFY, HFS_TRACE_OPERATION_READ,
		     directEligible ? HFS_TRACE_BUFFER_CALLER :
		     (aligned ? HFS_TRACE_BUFFER_DMA_LEASE : HFS_TRACE_BUFFER_RMW),
		     0, traceId, file->inodeId, offset, count,
		     (uint32_t)(count / file->fs->unitSize), 0, 0, 0, 0, 0, 0);
	hfsStatsRecordIoFast(false, count, aligned);
	if (directEligible) {
		hfsStatsRecordDmaDirectRequestFast(count);
	} else {
		hfsStatsRecordDmaStagingRequestFast(count);
	}
	if (profile != NULL && profile->enabled) {
		profile->flags |= directEligible ? HFS_PROFILE_FLAG_DIRECT :
			HFS_PROFILE_FLAG_STAGING;
	}
	runtime = hfsIoSyncReadLock(file, &syncTrace);
	if (runtime == NULL) {
		hfsTraceEmit(HFS_TRACE_IO_EXIT, HFS_TRACE_OPERATION_READ,
			     HFS_TRACE_BUFFER_NONE, 0, traceId, file->inodeId,
			     offset, count, 0, 0, 0, 0, 0,
			     hfsTraceNowNs() - traceStartNs, -1);
		g_hfsIoTraceParentId = previousTraceParent;
		g_hfsIoTraceOperation = previousTraceOperation;
		return -1;
	}
	ordering = hfsPosixOrderingMode();
	if (ordering == HFS_POSIX_ORDER_TICKET) {
		ticketInitialized = hfsPosixTicketInit(&ticket, runtime, file,
			offset, count, HFS_POSIX_TICKET_READ, traceId) == 0;
		if (!ticketInitialized || hfsPosixTicketAdmit(&ticket) != 0 ||
		    hfsPosixTicketWaitReady(&ticket) != 0) {
			if (ticketInitialized) {
				if (ticket.admitted) {
					hfsPosixTicketComplete(&ticket, -1);
				}
				hfsPosixTicketDestroy(&ticket);
			}
			hfsIoSyncReadUnlock(runtime, &syncTrace);
			hfsTraceEmit(HFS_TRACE_IO_EXIT, HFS_TRACE_OPERATION_READ,
				     HFS_TRACE_BUFFER_NONE, 0, traceId, file->inodeId,
				     offset, count, 0, 0, 0, 0, 0,
				     hfsTraceNowNs() - traceStartNs, -1);
			g_hfsIoTraceParentId = previousTraceParent;
			g_hfsIoTraceOperation = previousTraceOperation;
			return -1;
		}
		ticketActive = true;
	} else if (ordering != HFS_POSIX_ORDER_NONE &&
			(ordering == HFS_POSIX_ORDER_RANGE || !aligned) &&
		hfsIoRangeLock(file, offset, count, false, &guard) != 0) {
		hfsIoSyncReadUnlock(runtime, &syncTrace);
		hfsTraceEmit(HFS_TRACE_IO_EXIT, HFS_TRACE_OPERATION_READ,
			     HFS_TRACE_BUFFER_RMW, 0, traceId, file->inodeId,
			     offset, count, 0, 0, 0, 0, 0,
			     hfsTraceNowNs() - traceStartNs, -1);
		g_hfsIoTraceParentId = previousTraceParent;
		g_hfsIoTraceOperation = previousTraceOperation;
		return -1;
	} else if (ordering != HFS_POSIX_ORDER_NONE &&
		(ordering == HFS_POSIX_ORDER_RANGE || !aligned)) {
		rangeActive = true;
	}
	hfsProfileStage(profile, HFS_PROFILE_T2);
	result = hfsIoReadAtLocked(file, buffer, count, offset,
					ordering != HFS_POSIX_ORDER_NONE && !ticketActive &&
					!rangeActive,
					directEligible);
	if (rangeActive) {
		hfsIoRangeUnlock(file, &guard);
	}
	if (ticketActive) {
		hfsPosixTicketComplete(&ticket, result < 0 ? -1 : 0);
		hfsPosixTicketDestroy(&ticket);
	}
	hfsProfileStage(profile, HFS_PROFILE_T10);
	hfsIoSyncReadUnlock(runtime, &syncTrace);
	hfsTraceEmit(HFS_TRACE_IO_EXIT, HFS_TRACE_OPERATION_READ,
		     directEligible ? HFS_TRACE_BUFFER_CALLER :
		     (aligned ? HFS_TRACE_BUFFER_DMA_LEASE : HFS_TRACE_BUFFER_RMW),
		     0, traceId, file->inodeId, offset, count, 0, 0, 0, 0, 0,
		     hfsTraceNowNs() - traceStartNs, result < 0 ? -1 : 0);
	g_hfsIoTraceParentId = previousTraceParent;
	g_hfsIoTraceOperation = previousTraceOperation;
	return result;
}

/* Validate and write a positioned byte range with direct aligned writes. */
ssize_t hfsIoWriteAt(HfsFile *file, const uint8_t *buffer, size_t count,
			     uint64_t offset) {
	HfsInodeRuntime *runtime;
	HfsPosixIoTicket ticket = {0};
	HfsIoRangeGuard guard = {0};
	HfsTraceLockToken syncTrace = {0};
	ssize_t result;
	bool aligned;
	bool directEligible;
	bool ticketActive = false;
	bool ticketInitialized = false;
	bool rangeActive = false;
	HfsPosixOrderingMode ordering;
	uint64_t traceId;
	uint64_t traceStartNs;
	uint64_t previousTraceParent;
	HfsTraceOperation previousTraceOperation;
	HfsIoProfile *profile = hfsProfileCurrent();

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
	directEligible = aligned && hfsIoBufferDmaCapable(buffer, count);
	traceId = profile != NULL && profile->enabled ? profile->parentId :
		hfsTraceNextRequestId();
	traceStartNs = hfsTraceNowNs();
	previousTraceParent = g_hfsIoTraceParentId;
	previousTraceOperation = g_hfsIoTraceOperation;
	g_hfsIoTraceParentId = traceId;
	g_hfsIoTraceOperation = HFS_TRACE_OPERATION_WRITE;
	hfsTraceEmit(HFS_TRACE_IO_ENTER, HFS_TRACE_OPERATION_WRITE,
		     directEligible ? HFS_TRACE_BUFFER_CALLER :
		     (aligned ? HFS_TRACE_BUFFER_DMA_LEASE : HFS_TRACE_BUFFER_RMW),
		     0, traceId, file->inodeId, offset, count,
		     (uint32_t)(count / file->fs->unitSize), 0, 0, 0, 0, 0, 0);
	hfsTraceEmit(HFS_TRACE_BUFFER_CLASSIFY, HFS_TRACE_OPERATION_WRITE,
		     directEligible ? HFS_TRACE_BUFFER_CALLER :
		     (aligned ? HFS_TRACE_BUFFER_DMA_LEASE : HFS_TRACE_BUFFER_RMW),
		     0, traceId, file->inodeId, offset, count,
		     (uint32_t)(count / file->fs->unitSize), 0, 0, 0, 0, 0, 0);
	hfsStatsRecordIoFast(true, count, aligned);
	if (directEligible) {
		hfsStatsRecordDmaDirectRequestFast(count);
	} else {
		hfsStatsRecordDmaStagingRequestFast(count);
	}
	if (profile != NULL && profile->enabled) {
		profile->flags |= directEligible ? HFS_PROFILE_FLAG_DIRECT :
			HFS_PROFILE_FLAG_STAGING;
	}
	runtime = hfsIoSyncReadLock(file, &syncTrace);
	if (runtime == NULL) {
		hfsTraceEmit(HFS_TRACE_IO_EXIT, HFS_TRACE_OPERATION_WRITE,
			     HFS_TRACE_BUFFER_NONE, 0, traceId, file->inodeId,
			     offset, count, 0, 0, 0, 0, 0,
			     hfsTraceNowNs() - traceStartNs, -1);
		g_hfsIoTraceParentId = previousTraceParent;
		g_hfsIoTraceOperation = previousTraceOperation;
		return -1;
	}
	if (hfsResizeTo(file, offset + count) != 0) {
		hfsIoSyncReadUnlock(runtime, &syncTrace);
		hfsTraceEmit(HFS_TRACE_IO_EXIT, HFS_TRACE_OPERATION_WRITE,
			     HFS_TRACE_BUFFER_NONE, 0, traceId, file->inodeId,
			     offset, count, 0, 0, 0, 0, 0,
			     hfsTraceNowNs() - traceStartNs, -1);
		g_hfsIoTraceParentId = previousTraceParent;
		g_hfsIoTraceOperation = previousTraceOperation;
		return -1;
	}
	ordering = hfsPosixOrderingMode();
	if (ordering == HFS_POSIX_ORDER_TICKET) {
		ticketInitialized = hfsPosixTicketInit(&ticket, runtime, file,
			offset, count, HFS_POSIX_TICKET_WRITE, traceId) == 0;
		if (!ticketInitialized || hfsPosixTicketAdmit(&ticket) != 0 ||
		    hfsPosixTicketWaitReady(&ticket) != 0) {
			if (ticketInitialized) {
				if (ticket.admitted) {
					hfsPosixTicketComplete(&ticket, -1);
				}
				hfsPosixTicketDestroy(&ticket);
			}
			hfsIoSyncReadUnlock(runtime, &syncTrace);
			hfsTraceEmit(HFS_TRACE_IO_EXIT, HFS_TRACE_OPERATION_WRITE,
				     HFS_TRACE_BUFFER_NONE, 0, traceId, file->inodeId,
				     offset, count, 0, 0, 0, 0, 0,
				     hfsTraceNowNs() - traceStartNs, -1);
			g_hfsIoTraceParentId = previousTraceParent;
			g_hfsIoTraceOperation = previousTraceOperation;
			return -1;
		}
		ticketActive = true;
	} else if (ordering != HFS_POSIX_ORDER_NONE &&
			(ordering == HFS_POSIX_ORDER_RANGE || !aligned) &&
		hfsIoRangeLock(file, offset, count, true, &guard) != 0) {
		hfsIoSyncReadUnlock(runtime, &syncTrace);
		hfsTraceEmit(HFS_TRACE_IO_EXIT, HFS_TRACE_OPERATION_WRITE,
			     HFS_TRACE_BUFFER_RMW, 0, traceId, file->inodeId,
			     offset, count, 0, 0, 0, 0, 0,
			     hfsTraceNowNs() - traceStartNs, -1);
		g_hfsIoTraceParentId = previousTraceParent;
		g_hfsIoTraceOperation = previousTraceOperation;
		return -1;
	} else if (ordering != HFS_POSIX_ORDER_NONE &&
		(ordering == HFS_POSIX_ORDER_RANGE || !aligned)) {
		rangeActive = true;
	}
	hfsProfileStage(profile, HFS_PROFILE_T2);
	result = hfsIoWriteAtLocked(file, buffer, count, offset,
					ordering != HFS_POSIX_ORDER_NONE && !ticketActive &&
					!rangeActive,
				    directEligible);
	if (rangeActive) {
		hfsIoRangeUnlock(file, &guard);
	}
	if (ticketActive) {
		hfsPosixTicketComplete(&ticket, result < 0 ? -1 : 0);
		hfsPosixTicketDestroy(&ticket);
	}
	hfsProfileStage(profile, HFS_PROFILE_T10);
	hfsIoSyncReadUnlock(runtime, &syncTrace);
	hfsTraceEmit(HFS_TRACE_IO_EXIT, HFS_TRACE_OPERATION_WRITE,
		     directEligible ? HFS_TRACE_BUFFER_CALLER :
		     (aligned ? HFS_TRACE_BUFFER_DMA_LEASE : HFS_TRACE_BUFFER_RMW),
		     0, traceId, file->inodeId, offset, count, 0, 0, 0, 0, 0,
		     hfsTraceNowNs() - traceStartNs, result < 0 ? -1 : 0);
	g_hfsIoTraceParentId = previousTraceParent;
	g_hfsIoTraceOperation = previousTraceOperation;
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

	if (hfsIoAcquireAt(file, &lease, (size_t)file->fs->unitSize,
			   unitOffset) != 0) {
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
		/* An unsupported UNMAP must retain the observable zero guarantee. */
		hfsStatsRecordPunchFast(false, true);
	} else if (hfsIoUnmapGuaranteesZero(file)) {
		/* The bdev capability and operator declaration make UNMAP sufficient. */
		hfsStatsRecordPunchFast(true, false);
		return 0;
	}
	if (hfsIoSubmit(file, NULL, offset, units, HFS_IO_WRITE_ZEROES) == 0) {
		hfsStatsRecordPunchFast(false, false);
		return 0;
	}
	/*
	 * An unmap completion does not promise reads of zero on every bdev. If the
	 * accelerated zeroes path fails, retry through the DMA write path and only
	 * report success after the observable hole contents are actually zero.
	 */
	hfsStatsRecordPunchFast(false, true);
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
	HfsTraceLockToken syncTrace = {0};
	int rc;

	if (length == 0) {
		return 0;
	}
	runtime = hfsIoSyncReadLock(file, &syncTrace);
	if (runtime == NULL) {
		return -1;
	}
	rc = hfsIoZeroRangeCore(file, offset, length);
	hfsIoSyncReadUnlock(runtime, &syncTrace);
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
