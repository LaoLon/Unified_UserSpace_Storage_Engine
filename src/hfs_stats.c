#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "hfs_stats.h"

#define HFS_STATS_HIST_BUCKETS 16U
#define HFS_STATS_IO_SIZE_BUCKETS 16U
#define HFS_STATS_DMA_SIZE_CLASSES 8U
#define HFS_STATS_DMA_POOL_CLASSES 4U
#define HFS_STATS_MAX_DATA_REACTORS 8U
#define HFS_STATS_MAX_LOCK_DEPTH 8U
#define HFS_STATS_SNAPSHOT_BYTES (128U * 1024U)

_Atomic bool hfsStatsEnabledState;
_Atomic uint_fast64_t hfsStatsFdReadLocks;
_Atomic uint_fast64_t hfsStatsFdWriteLocks;
_Atomic uint_fast64_t hfsStatsMetaReadLocks;
_Atomic uint_fast64_t hfsStatsMetaWriteLocks;
_Atomic uint_fast64_t hfsStatsDmaAcquires;
_Atomic uint_fast64_t hfsStatsDmaWaits;
_Atomic uint_fast64_t hfsStatsDmaReleases;
_Atomic uint_fast64_t hfsStatsDmaActivePeak;
_Atomic uint_fast64_t hfsStatsDmaExhausts;
_Atomic uint_fast64_t hfsStatsDmaFastAcquires;
_Atomic uint_fast64_t hfsStatsDurabilityLeaders;
_Atomic uint_fast64_t hfsStatsDurabilityWaiters;
_Atomic uint_fast64_t hfsStatsFdatasyncFast;
static _Atomic uint_fast64_t g_hfsStatsDataReactorSubmit[HFS_STATS_MAX_DATA_REACTORS];
static _Atomic uint_fast64_t g_hfsStatsDataReactorComplete[HFS_STATS_MAX_DATA_REACTORS];
_Atomic uint_fast64_t hfsStatsSpdkSubmits;
_Atomic uint_fast64_t hfsStatsSpdkCompletions;
_Atomic uint_fast64_t hfsStatsSpdkRejects;
_Atomic uint_fast64_t hfsStatsIoOutstanding;
_Atomic uint_fast64_t hfsStatsIoOutstandingPeak;
_Atomic uint_fast64_t hfsStatsErrors;
bool hfsStatsFastEnabledState;
bool hfsStatsTimingState;

/* One statistics shard is owned by one producer thread and aggregated at exit. */
typedef struct HfsStatsShard {
	struct HfsStatsShard *next;
	uint64_t threadId;
	uint64_t lockWaitCount[HFS_STATS_LOCK_CLASS_COUNT];
	uint64_t lockHoldCount[HFS_STATS_LOCK_CLASS_COUNT];
	uint64_t lockContention[HFS_STATS_LOCK_CLASS_COUNT];
	uint64_t lockWaitNs[HFS_STATS_LOCK_CLASS_COUNT];
	uint64_t lockHoldNs[HFS_STATS_LOCK_CLASS_COUNT];
	uint64_t lockWaitHist[HFS_STATS_LOCK_CLASS_COUNT][HFS_STATS_HIST_BUCKETS];
	uint64_t lockHoldHist[HFS_STATS_LOCK_CLASS_COUNT][HFS_STATS_HIST_BUCKETS];
	uint64_t runtimeSubmit;
	uint64_t runtimeComplete;
	uint64_t runtimeQueueDepthSum;
	uint64_t runtimeQueueDepthPeak;
	uint64_t runtimeInflightPeak;
	uint64_t runtimeQueueWaitNs;
	uint64_t runtimeOwnerNs;
	uint64_t ioPipelinePeak;
	uint64_t dataReactorSubmit[HFS_STATS_MAX_DATA_REACTORS];
	uint64_t dataReactorComplete[HFS_STATS_MAX_DATA_REACTORS];
	uint64_t ioReadOps;
	uint64_t ioWriteOps;
	uint64_t ioReadBytes;
	uint64_t ioWriteBytes;
	uint64_t ioAlignedOps;
	uint64_t ioUnalignedOps;
	uint64_t ioRmwOps;
	uint64_t ioRmwBytes;
	uint64_t ioCopyBytes;
	uint64_t ioCopyOps;
	uint64_t ioDirectBytes;
	uint64_t ioDirectOps;
	uint64_t ioVectorBytes;
	uint64_t ioVectorOps;
	uint64_t resizeChecks;
	uint64_t resizeFastHits;
	uint64_t resizeRequests;
	uint64_t punchRequests;
	uint64_t punchUnmapOnly;
	uint64_t punchZeroFallbacks;
	uint64_t ioSizeHist[HFS_STATS_IO_SIZE_BUCKETS];
	uint64_t metadataLogBytes;
	uint64_t metadataImageBytes;
	uint64_t metadataPageBytes;
	uint64_t metadataTransactions;
	uint64_t metadataBatchTotal;
	uint64_t metadataBatchMax;
	uint64_t metadataSyncCount;
	uint64_t flushCount;
	uint64_t dmaSizeClass[HFS_STATS_DMA_SIZE_CLASSES];
	uint64_t dmaPoolClass[HFS_STATS_DMA_POOL_CLASSES];
	uint64_t dmaWaits;
	uint64_t dmaWaitNs;
	uint64_t dmaExhausts;
	uint64_t dmaActivePeak;
} HfsStatsShard;

/* A lock frame lets wait and hold time be measured without a global lock. */
typedef struct HfsStatsLockFrame {
	uint64_t waitStartNs;
	uint64_t holdStartNs;
} HfsStatsLockFrame;

static pthread_mutex_t g_hfsStatsShardLock = PTHREAD_MUTEX_INITIALIZER;
static HfsStatsShard *g_hfsStatsShards;
static _Thread_local HfsStatsShard *g_hfsStatsTlsShard;
static _Thread_local HfsStatsLockFrame
	g_hfsStatsLockFrames[HFS_STATS_LOCK_CLASS_COUNT][HFS_STATS_MAX_LOCK_DEPTH];
static _Thread_local unsigned int
	g_hfsStatsLockDepth[HFS_STATS_LOCK_CLASS_COUNT];
static bool g_hfsStatsInitialized;
static char g_hfsStatsPath[PATH_MAX];

/* Return the current monotonic time in nanoseconds. */
uint64_t hfsStatsNowNs(void) {
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Return true only while optional instrumentation is enabled. */
bool hfsStatsIsEnabled(void) {
	return atomic_load_explicit(&hfsStatsEnabledState, memory_order_relaxed);
}

/* Keep detailed timing opt-in so normal statistics add only counter overhead. */
bool hfsStatsTimingEnabled(void) {
	return hfsStatsTimingState && hfsStatsFastEnabledState;
}

/* Map a duration or size into a bounded logarithmic histogram bucket. */
static unsigned int hfsStatsBucket(uint64_t value) {
	unsigned int bucket = 0;

	while (value > 1 && bucket + 1U < HFS_STATS_HIST_BUCKETS) {
		value >>= 1;
		bucket++;
	}
	return bucket;
}

/* Return a stable Linux thread id for correlating per-thread shards. */
static uint64_t hfsStatsThreadId(void) {
	long value = syscall(SYS_gettid);

	return value > 0 ? (uint64_t)value : (uint64_t)(uintptr_t)pthread_self();
}

/* Lazily register one shard; no global lock is taken after registration. */
static HfsStatsShard *hfsStatsShardGet(void) {
	HfsStatsShard *shard;

	if (!hfsStatsIsEnabled()) {
		return NULL;
	}
	if (g_hfsStatsTlsShard != NULL) {
		return g_hfsStatsTlsShard;
	}
	shard = calloc(1, sizeof(*shard));
	if (shard == NULL) {
		return NULL;
	}
	shard->threadId = hfsStatsThreadId();
	pthread_mutex_lock(&g_hfsStatsShardLock);
	shard->next = g_hfsStatsShards;
	g_hfsStatsShards = shard;
	pthread_mutex_unlock(&g_hfsStatsShardLock);
	g_hfsStatsTlsShard = shard;
	return shard;
}

/* Reset process counters and every registered per-thread shard. */
static void hfsStatsReset(void) {
	HfsStatsShard *shard;

	atomic_store_explicit(&hfsStatsFdReadLocks, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsFdWriteLocks, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsMetaReadLocks, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsMetaWriteLocks, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsDmaAcquires, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsDmaWaits, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsDmaReleases, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsDmaActivePeak, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsDmaExhausts, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsDmaFastAcquires, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsDurabilityLeaders, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsDurabilityWaiters, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsFdatasyncFast, 0, memory_order_relaxed);
	for (unsigned int i = 0; i < HFS_STATS_MAX_DATA_REACTORS; i++) {
		atomic_store_explicit(&g_hfsStatsDataReactorSubmit[i], 0,
			memory_order_relaxed);
		atomic_store_explicit(&g_hfsStatsDataReactorComplete[i], 0,
			memory_order_relaxed);
	}
	atomic_store_explicit(&hfsStatsSpdkSubmits, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsSpdkCompletions, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsSpdkRejects, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsIoOutstanding, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsIoOutstandingPeak, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsErrors, 0, memory_order_relaxed);
	pthread_mutex_lock(&g_hfsStatsShardLock);
	for (shard = g_hfsStatsShards; shard != NULL; shard = shard->next) {
		struct HfsStatsShard *next = shard->next;

		memset(shard, 0, sizeof(*shard));
		shard->next = next;
	}
	pthread_mutex_unlock(&g_hfsStatsShardLock);
}

/* Return true for the small set of accepted boolean environment values. */
static bool hfsStatsBool(const char *value) {
	return value != NULL &&
		(!strcmp(value, "1") || !strcasecmp(value, "true") ||
		 !strcasecmp(value, "yes") || !strcasecmp(value, "on"));
}

/* Add one lock event to the current thread's wait histogram. */
void hfsStatsLockBefore(HfsStatsLockClass lockClass) {
	unsigned int depth;

	if (!hfsStatsTimingEnabled() || lockClass >= HFS_STATS_LOCK_CLASS_COUNT) {
		return;
	}
	depth = g_hfsStatsLockDepth[lockClass];
	if (depth >= HFS_STATS_MAX_LOCK_DEPTH) {
		return;
	}
	(void)hfsStatsShardGet();
	g_hfsStatsLockFrames[lockClass][depth].waitStartNs = hfsStatsNowNs();
	g_hfsStatsLockFrames[lockClass][depth].holdStartNs = 0;
	g_hfsStatsLockDepth[lockClass] = depth + 1U;
}

/* Finish a lock acquisition and account for contention and wait latency. */
void hfsStatsLockAfter(HfsStatsLockClass lockClass) {
	HfsStatsShard *shard;
	unsigned int depth;
	uint64_t now;
	uint64_t waitNs;

	if (!hfsStatsTimingEnabled() || lockClass >= HFS_STATS_LOCK_CLASS_COUNT) {
		return;
	}
	depth = g_hfsStatsLockDepth[lockClass];
	if (depth == 0) {
		return;
	}
	depth--;
	now = hfsStatsNowNs();
	waitNs = now - g_hfsStatsLockFrames[lockClass][depth].waitStartNs;
	g_hfsStatsLockFrames[lockClass][depth].holdStartNs = now;
	shard = hfsStatsShardGet();
	if (shard != NULL) {
		shard->lockWaitCount[lockClass]++;
		shard->lockWaitNs[lockClass] += waitNs;
		shard->lockWaitHist[lockClass][hfsStatsBucket(waitNs)]++;
		if (waitNs > 1000U) {
			shard->lockContention[lockClass]++;
		}
	}
}

/* Drop a timing frame when a try-lock failed before the lock was acquired. */
void hfsStatsLockCancel(HfsStatsLockClass lockClass) {
	unsigned int depth;

	if (!hfsStatsTimingEnabled() || lockClass >= HFS_STATS_LOCK_CLASS_COUNT) {
		return;
	}
	depth = g_hfsStatsLockDepth[lockClass];
	if (depth == 0) {
		return;
	}
	depth--;
	g_hfsStatsLockDepth[lockClass] = depth;
	g_hfsStatsLockFrames[lockClass][depth].waitStartNs = 0;
	g_hfsStatsLockFrames[lockClass][depth].holdStartNs = 0;
}

/* Record a hold duration on the releasing thread without using TLS frames. */
void hfsStatsLockRecordHold(HfsStatsLockClass lockClass, uint64_t holdNs) {
	HfsStatsShard *shard;

	if (!hfsStatsTimingEnabled() || lockClass >= HFS_STATS_LOCK_CLASS_COUNT) {
		return;
	}
	shard = hfsStatsShardGet();
	if (shard != NULL) {
		shard->lockHoldCount[lockClass]++;
		shard->lockHoldNs[lockClass] += holdNs;
		shard->lockHoldHist[lockClass][hfsStatsBucket(holdNs)]++;
	}
}

/* Account for the protected section when the lock is released. */
void hfsStatsLockRelease(HfsStatsLockClass lockClass) {
	HfsStatsShard *shard;
	unsigned int depth;
	uint64_t holdNs;

	if (!hfsStatsTimingEnabled() || lockClass >= HFS_STATS_LOCK_CLASS_COUNT) {
		return;
	}
	depth = g_hfsStatsLockDepth[lockClass];
	if (depth == 0) {
		return;
	}
	depth--;
	g_hfsStatsLockDepth[lockClass] = depth;
	if (g_hfsStatsLockFrames[lockClass][depth].holdStartNs == 0) {
		return;
	}
	holdNs = hfsStatsNowNs() -
		g_hfsStatsLockFrames[lockClass][depth].holdStartNs;
	shard = hfsStatsShardGet();
	if (shard != NULL) {
		shard->lockHoldCount[lockClass]++;
		shard->lockHoldNs[lockClass] += holdNs;
		shard->lockHoldHist[lockClass][hfsStatsBucket(holdNs)]++;
	}
}

/* Record one public read or write request and its alignment class. */
void hfsStatsRecordIo(bool write, size_t bytes, bool aligned) {
	HfsStatsShard *shard = hfsStatsShardGet();
	unsigned int bucket;

	if (shard == NULL) {
		return;
	}
	if (write) {
		shard->ioWriteOps++;
		shard->ioWriteBytes += bytes;
	} else {
		shard->ioReadOps++;
		shard->ioReadBytes += bytes;
	}
	if (aligned) {
		shard->ioAlignedOps++;
	} else {
		shard->ioUnalignedOps++;
	}
	bucket = hfsStatsBucket(bytes);
	if (bucket >= HFS_STATS_IO_SIZE_BUCKETS) {
		bucket = HFS_STATS_IO_SIZE_BUCKETS - 1U;
	}
	shard->ioSizeHist[bucket]++;
}

/* Record a DMA-capable caller buffer that bypassed the bounce pool. */
void hfsStatsRecordIoDirect(uint64_t bytes) {
	HfsStatsShard *shard = hfsStatsShardGet();

	if (shard != NULL) {
		shard->ioDirectOps++;
		shard->ioDirectBytes += bytes;
	}
}

/* Record one native vector submission and its covered bytes. */
void hfsStatsRecordIoVector(uint64_t bytes) {
	HfsStatsShard *shard = hfsStatsShardGet();

	if (shard != NULL) {
		shard->ioVectorOps++;
		shard->ioVectorBytes += bytes;
	}
}

/* Record user/DMA copies, which are often the hidden cost of the fast path. */
void hfsStatsRecordCopy(uint64_t bytes) {
	HfsStatsShard *shard = hfsStatsShardGet();

	if (shard != NULL) {
		shard->ioCopyBytes += bytes;
		shard->ioCopyOps++;
	}
}

/* Record one partial-unit read-modify-write operation. */
void hfsStatsRecordRmw(uint64_t bytes) {
	HfsStatsShard *shard = hfsStatsShardGet();

	if (shard != NULL) {
		shard->ioRmwOps++;
		shard->ioRmwBytes += bytes;
	}
}

/* Record whether a resize was absorbed by the runtime allocation cache. */
void hfsStatsRecordResize(bool fastHit) {
	HfsStatsShard *shard = hfsStatsShardGet();

	if (shard != NULL) {
		shard->resizeChecks++;
		if (fastHit) {
			shard->resizeFastHits++;
		} else {
			shard->resizeRequests++;
		}
	}
}

/* Record the selected aligned punch implementation and fallback activity. */
void hfsStatsRecordPunch(bool unmapOnly, bool zeroFallback) {
	HfsStatsShard *shard = hfsStatsShardGet();

	if (shard != NULL) {
		shard->punchRequests++;
		if (unmapOnly) {
			shard->punchUnmapOnly++;
		}
		if (zeroFallback) {
			shard->punchZeroFallbacks++;
		}
	}
}

/* Record queue depth at the point a request is handed to SPDK. */
void hfsStatsRecordRuntimeSubmit(uint64_t queueDepth) {
	HfsStatsShard *shard = hfsStatsShardGet();

	if (shard != NULL) {
		shard->runtimeSubmit++;
		shard->runtimeQueueDepthSum += queueDepth;
		if (queueDepth > shard->runtimeQueueDepthPeak) {
			shard->runtimeQueueDepthPeak = queueDepth;
		}
	}
}

/* Record owner dispatch latency and the effective in-flight depth. */
void hfsStatsRecordRuntimeDispatch(uint64_t queueWaitNs,
					   uint64_t ownerNs, uint64_t queueDepth) {
	HfsStatsShard *shard = hfsStatsShardGet();

	if (shard != NULL) {
		shard->runtimeComplete++;
		shard->runtimeQueueWaitNs += queueWaitNs;
		shard->runtimeOwnerNs += ownerNs;
		if (queueDepth > shard->runtimeInflightPeak) {
			shard->runtimeInflightPeak = queueDepth;
		}
	}
}

/* Record one direct child submission on a bounded data reactor. */
void hfsStatsRecordDataReactorSubmit(uint32_t reactorId) {
	HfsStatsShard *shard;

	if (reactorId >= HFS_STATS_MAX_DATA_REACTORS) {
		return;
	}
	shard = hfsStatsShardGet();
	if (shard != NULL) {
		shard->dataReactorSubmit[reactorId]++;
	}
	atomic_fetch_add_explicit(&g_hfsStatsDataReactorSubmit[reactorId], 1,
		memory_order_relaxed);
}

/* Record one direct child completion on a bounded data reactor. */
void hfsStatsRecordDataReactorComplete(uint32_t reactorId) {
	HfsStatsShard *shard;

	if (reactorId >= HFS_STATS_MAX_DATA_REACTORS) {
		return;
	}
	shard = hfsStatsShardGet();
	if (shard != NULL) {
		shard->dataReactorComplete[reactorId]++;
	}
	atomic_fetch_add_explicit(&g_hfsStatsDataReactorComplete[reactorId], 1,
		memory_order_relaxed);
}

/* Record the actual SPDK data callback outstanding depth, not dispatcher depth. */
void hfsStatsRecordIoSubmit(void) {
	uint64_t current;
	uint64_t peak;

	current = atomic_fetch_add_explicit(&hfsStatsIoOutstanding, 1,
		memory_order_relaxed) + 1U;
	peak = atomic_load_explicit(&hfsStatsIoOutstandingPeak,
		memory_order_relaxed);
	while (current > peak &&
	       !atomic_compare_exchange_weak_explicit(&hfsStatsIoOutstandingPeak,
			&peak, current, memory_order_relaxed, memory_order_relaxed)) {
		/* Retry with the peak value refreshed by the failed compare-exchange. */
	}
}

/* Retire one SPDK data callback from the outstanding-depth counter. */
void hfsStatsRecordIoComplete(void) {
	uint64_t current;

	current = atomic_load_explicit(&hfsStatsIoOutstanding, memory_order_relaxed);
	while (current != 0 &&
	       !atomic_compare_exchange_weak_explicit(&hfsStatsIoOutstanding,
			&current, current - 1U, memory_order_relaxed,
			memory_order_relaxed)) {
		/* Retry with the current value refreshed by the failed compare-exchange. */
	}
}

/* Record the child fan-out observed by one aligned parent request. */
void hfsStatsRecordIoPipeline(uint64_t depth) {
	HfsStatsShard *shard = hfsStatsShardGet();

	if (shard != NULL && depth > shard->ioPipelinePeak) {
		shard->ioPipelinePeak = depth;
	}
}

/* Record serialized metadata bytes and the image/page split. */
void hfsStatsRecordMetadataWrite(uint64_t bytes, uint64_t imageBytes,
					 uint64_t pageBytes) {
	HfsStatsShard *shard = hfsStatsShardGet();

	if (shard != NULL) {
		shard->metadataLogBytes += bytes;
		shard->metadataImageBytes += imageBytes;
		shard->metadataPageBytes += pageBytes;
	}
}

/* Record one metadata transaction and its number of changed inodes. */
void hfsStatsRecordMetadataTransaction(uint32_t batchSize) {
	HfsStatsShard *shard = hfsStatsShardGet();

	if (shard != NULL) {
		shard->metadataTransactions++;
		shard->metadataBatchTotal += batchSize;
		if (batchSize > shard->metadataBatchMax) {
			shard->metadataBatchMax = batchSize;
		}
	}
}

/* Record an attempted device durability barrier. */
void hfsStatsRecordFlush(void) {
	HfsStatsShard *shard = hfsStatsShardGet();

	if (shard != NULL) {
		shard->flushCount++;
	}
}

/* Record one metadata blob sync separately from the device flush counter. */
void hfsStatsRecordMetadataSync(void) {
	HfsStatsShard *shard = hfsStatsShardGet();

	if (shard != NULL) {
		shard->metadataSyncCount++;
	}
}

/* Record a DMA size class, wait, and active high-water mark. */
void hfsStatsRecordDma(size_t bytes, bool waited, uint64_t active) {
	HfsStatsShard *shard = hfsStatsShardGet();
	unsigned int bucket;
	unsigned int poolClass;

	if (shard == NULL) {
		return;
	}
	bucket = hfsStatsBucket(bytes);
	if (bucket >= HFS_STATS_DMA_SIZE_CLASSES) {
		bucket = HFS_STATS_DMA_SIZE_CLASSES - 1U;
	}
	shard->dmaSizeClass[bucket]++;
	if (bytes <= (4U << 10)) {
		poolClass = 0;
	} else if (bytes <= (16U << 10)) {
		poolClass = 1;
	} else if (bytes <= (64U << 10)) {
		poolClass = 2;
	} else {
		poolClass = 3;
	}
	shard->dmaPoolClass[poolClass]++;
	if (waited) {
		shard->dmaWaits++;
	}
	if (active > shard->dmaActivePeak) {
		shard->dmaActivePeak = active;
	}
}

/* Record the elapsed wait for one reactor-local DMA lease. */
void hfsStatsRecordDmaWaitNs(uint64_t waitNs) {
	HfsStatsShard *shard = hfsStatsShardGet();

	if (shard != NULL) {
		shard->dmaWaitNs += waitNs;
	}
}

/* Record one failed non-blocking lease acquisition in the thread shard. */
void hfsStatsRecordDmaExhaust(void) {
	HfsStatsShard *shard = hfsStatsShardGet();

	if (shard != NULL) {
		shard->dmaExhausts++;
	}
}

/* Record one successful lock-free DMA slot claim. */
void hfsStatsRecordDmaFastAcquire(void) {
	atomic_fetch_add_explicit(&hfsStatsDmaFastAcquires, 1, memory_order_relaxed);
}

/* Record one durability epoch leader. */
void hfsStatsRecordDurabilityLeader(void) {
	atomic_fetch_add_explicit(&hfsStatsDurabilityLeaders, 1, memory_order_relaxed);
}

/* Record one caller that joined an in-flight durability epoch. */
void hfsStatsRecordDurabilityWaiter(void) {
	atomic_fetch_add_explicit(&hfsStatsDurabilityWaiters, 1, memory_order_relaxed);
}

/* Record a fdatasync that completed after only the data barrier. */
void hfsStatsRecordFdatasyncFast(void) {
	atomic_fetch_add_explicit(&hfsStatsFdatasyncFast, 1, memory_order_relaxed);
}

/* Add one shard into the process aggregate used by the final report. */
static void hfsStatsAggregateShard(HfsStatsShard *total,
					const HfsStatsShard *shard) {
	unsigned int i;
	unsigned int j;

	for (i = 0; i < HFS_STATS_LOCK_CLASS_COUNT; i++) {
		total->lockWaitCount[i] += shard->lockWaitCount[i];
		total->lockHoldCount[i] += shard->lockHoldCount[i];
		total->lockContention[i] += shard->lockContention[i];
		total->lockWaitNs[i] += shard->lockWaitNs[i];
		total->lockHoldNs[i] += shard->lockHoldNs[i];
		for (j = 0; j < HFS_STATS_HIST_BUCKETS; j++) {
			total->lockWaitHist[i][j] += shard->lockWaitHist[i][j];
			total->lockHoldHist[i][j] += shard->lockHoldHist[i][j];
		}
	}
	total->runtimeSubmit += shard->runtimeSubmit;
	total->runtimeComplete += shard->runtimeComplete;
	total->runtimeQueueDepthSum += shard->runtimeQueueDepthSum;
	if (shard->runtimeQueueDepthPeak > total->runtimeQueueDepthPeak) {
		total->runtimeQueueDepthPeak = shard->runtimeQueueDepthPeak;
	}
	if (shard->runtimeInflightPeak > total->runtimeInflightPeak) {
		total->runtimeInflightPeak = shard->runtimeInflightPeak;
	}
	total->runtimeQueueWaitNs += shard->runtimeQueueWaitNs;
	total->runtimeOwnerNs += shard->runtimeOwnerNs;
	if (shard->ioPipelinePeak > total->ioPipelinePeak) {
		total->ioPipelinePeak = shard->ioPipelinePeak;
	}
	for (i = 0; i < HFS_STATS_MAX_DATA_REACTORS; i++) {
		total->dataReactorSubmit[i] += shard->dataReactorSubmit[i];
		total->dataReactorComplete[i] += shard->dataReactorComplete[i];
	}
	total->ioReadOps += shard->ioReadOps;
	total->ioWriteOps += shard->ioWriteOps;
	total->ioReadBytes += shard->ioReadBytes;
	total->ioWriteBytes += shard->ioWriteBytes;
	total->ioAlignedOps += shard->ioAlignedOps;
	total->ioUnalignedOps += shard->ioUnalignedOps;
	total->ioRmwOps += shard->ioRmwOps;
	total->ioRmwBytes += shard->ioRmwBytes;
	total->ioCopyBytes += shard->ioCopyBytes;
	total->ioCopyOps += shard->ioCopyOps;
	total->ioDirectBytes += shard->ioDirectBytes;
	total->ioDirectOps += shard->ioDirectOps;
	total->ioVectorBytes += shard->ioVectorBytes;
	total->ioVectorOps += shard->ioVectorOps;
	total->resizeChecks += shard->resizeChecks;
	total->resizeFastHits += shard->resizeFastHits;
	total->resizeRequests += shard->resizeRequests;
	total->punchRequests += shard->punchRequests;
	total->punchUnmapOnly += shard->punchUnmapOnly;
	total->punchZeroFallbacks += shard->punchZeroFallbacks;
	for (i = 0; i < HFS_STATS_IO_SIZE_BUCKETS; i++) {
		total->ioSizeHist[i] += shard->ioSizeHist[i];
	}
	total->metadataLogBytes += shard->metadataLogBytes;
	total->metadataImageBytes += shard->metadataImageBytes;
	total->metadataPageBytes += shard->metadataPageBytes;
	total->metadataTransactions += shard->metadataTransactions;
	total->metadataBatchTotal += shard->metadataBatchTotal;
	if (shard->metadataBatchMax > total->metadataBatchMax) {
		total->metadataBatchMax = shard->metadataBatchMax;
	}
	total->metadataSyncCount += shard->metadataSyncCount;
	total->flushCount += shard->flushCount;
	for (i = 0; i < HFS_STATS_DMA_SIZE_CLASSES; i++) {
		total->dmaSizeClass[i] += shard->dmaSizeClass[i];
	}
	total->dmaWaits += shard->dmaWaits;
	total->dmaWaitNs += shard->dmaWaitNs;
	total->dmaExhausts += shard->dmaExhausts;
	if (shard->dmaActivePeak > total->dmaActivePeak) {
		total->dmaActivePeak = shard->dmaActivePeak;
	}
	for (i = 0; i < HFS_STATS_DMA_POOL_CLASSES; i++) {
		total->dmaPoolClass[i] += shard->dmaPoolClass[i];
	}
}

/* Write an aggregated key-value report without depending on HFS logging. */
static void hfsStatsWriteSnapshot(void) {
	static const char *const lockNames[HFS_STATS_LOCK_CLASS_COUNT] = {
		"fd", "meta", "io", "range_gate", "resize", "append", "runtime", "other"
	};
	HfsStatsShard total = {0};
	HfsStatsShard *shard;
	char *buffer;
	int fd;
	size_t used = 0;
	uint64_t shardCount = 0;
	unsigned int i;
	unsigned int j;

	if (g_hfsStatsPath[0] == '\0') {
		return;
	}
	pthread_mutex_lock(&g_hfsStatsShardLock);
	for (shard = g_hfsStatsShards; shard != NULL; shard = shard->next) {
		hfsStatsAggregateShard(&total, shard);
		shardCount++;
	}
	pthread_mutex_unlock(&g_hfsStatsShardLock);
	buffer = calloc(1, HFS_STATS_SNAPSHOT_BYTES);
	if (buffer == NULL) {
		return;
	}
	fd = open(g_hfsStatsPath, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		free(buffer);
		return;
	}
#define HFS_STATS_APPEND(fmt, ...) do { \
		if (used < HFS_STATS_SNAPSHOT_BYTES) { \
			int _n = snprintf(buffer + used, HFS_STATS_SNAPSHOT_BYTES - used, \
				fmt, __VA_ARGS__); \
			if (_n > 0) used += (size_t)_n; \
		} \
	} while (0)
	HFS_STATS_APPEND("fd_read_lock=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsFdReadLocks, memory_order_relaxed));
	HFS_STATS_APPEND("fd_write_lock=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsFdWriteLocks, memory_order_relaxed));
	HFS_STATS_APPEND("meta_read_lock=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsMetaReadLocks, memory_order_relaxed));
	HFS_STATS_APPEND("meta_write_lock=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsMetaWriteLocks, memory_order_relaxed));
	HFS_STATS_APPEND("dma_acquire=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsDmaAcquires, memory_order_relaxed));
	HFS_STATS_APPEND("dma_wait=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsDmaWaits, memory_order_relaxed));
	HFS_STATS_APPEND("dma_release=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsDmaReleases, memory_order_relaxed));
	HFS_STATS_APPEND("dma_active_peak=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsDmaActivePeak, memory_order_relaxed));
	HFS_STATS_APPEND("dma_exhaust=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsDmaExhausts, memory_order_relaxed));
	HFS_STATS_APPEND("dma_fast_acquire=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsDmaFastAcquires, memory_order_relaxed));
	HFS_STATS_APPEND("durability_leader=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsDurabilityLeaders, memory_order_relaxed));
	HFS_STATS_APPEND("durability_waiter=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsDurabilityWaiters, memory_order_relaxed));
	HFS_STATS_APPEND("fdatasync_fast=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsFdatasyncFast, memory_order_relaxed));
	HFS_STATS_APPEND("spdk_submit=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsSpdkSubmits, memory_order_relaxed));
	HFS_STATS_APPEND("spdk_complete=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsSpdkCompletions, memory_order_relaxed));
	HFS_STATS_APPEND("spdk_reject=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsSpdkRejects, memory_order_relaxed));
	HFS_STATS_APPEND("io_backend_outstanding=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsIoOutstanding, memory_order_relaxed));
	HFS_STATS_APPEND("io_backend_outstanding_peak=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsIoOutstandingPeak, memory_order_relaxed));
	HFS_STATS_APPEND("errors=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsErrors, memory_order_relaxed));
	HFS_STATS_APPEND("stats_shards=%"PRIu64"\n", shardCount);
	for (i = 0; i < HFS_STATS_LOCK_CLASS_COUNT; i++) {
		HFS_STATS_APPEND("lock_%s_wait_count=%"PRIu64"\n", lockNames[i],
			total.lockWaitCount[i]);
		HFS_STATS_APPEND("lock_%s_hold_count=%"PRIu64"\n", lockNames[i],
			total.lockHoldCount[i]);
		HFS_STATS_APPEND("lock_%s_contention=%"PRIu64"\n", lockNames[i],
			total.lockContention[i]);
		HFS_STATS_APPEND("lock_%s_wait_ns=%"PRIu64"\n", lockNames[i],
			total.lockWaitNs[i]);
		HFS_STATS_APPEND("lock_%s_hold_ns=%"PRIu64"\n", lockNames[i],
			total.lockHoldNs[i]);
		for (j = 0; j < HFS_STATS_HIST_BUCKETS; j++) {
			HFS_STATS_APPEND("lock_%s_wait_hist_%u=%"PRIu64"\n",
				lockNames[i], j, total.lockWaitHist[i][j]);
			HFS_STATS_APPEND("lock_%s_hold_hist_%u=%"PRIu64"\n",
				lockNames[i], j, total.lockHoldHist[i][j]);
		}
	}
	HFS_STATS_APPEND("runtime_submit=%"PRIu64"\n", total.runtimeSubmit);
	HFS_STATS_APPEND("runtime_complete=%"PRIu64"\n", total.runtimeComplete);
	HFS_STATS_APPEND("runtime_queue_depth_sum=%"PRIu64"\n",
		total.runtimeQueueDepthSum);
	HFS_STATS_APPEND("runtime_queue_depth_peak=%"PRIu64"\n",
		total.runtimeQueueDepthPeak);
	HFS_STATS_APPEND("runtime_inflight_peak=%"PRIu64"\n",
		total.runtimeInflightPeak);
	HFS_STATS_APPEND("runtime_queue_wait_ns=%"PRIu64"\n",
		total.runtimeQueueWaitNs);
	HFS_STATS_APPEND("runtime_owner_ns=%"PRIu64"\n", total.runtimeOwnerNs);
	HFS_STATS_APPEND("io_pipeline_peak=%"PRIu64"\n",
		total.ioPipelinePeak);
	for (i = 0; i < HFS_STATS_MAX_DATA_REACTORS; i++) {
		HFS_STATS_APPEND("data_reactor_%u_submit=%"PRIu64"\n", i,
			atomic_load_explicit(&g_hfsStatsDataReactorSubmit[i],
			memory_order_relaxed));
		HFS_STATS_APPEND("data_reactor_%u_complete=%"PRIu64"\n", i,
			atomic_load_explicit(&g_hfsStatsDataReactorComplete[i],
			memory_order_relaxed));
	}
	HFS_STATS_APPEND("io_read_ops=%"PRIu64"\n", total.ioReadOps);
	HFS_STATS_APPEND("io_write_ops=%"PRIu64"\n", total.ioWriteOps);
	HFS_STATS_APPEND("io_read_bytes=%"PRIu64"\n", total.ioReadBytes);
	HFS_STATS_APPEND("io_write_bytes=%"PRIu64"\n", total.ioWriteBytes);
	HFS_STATS_APPEND("io_aligned_ops=%"PRIu64"\n", total.ioAlignedOps);
	HFS_STATS_APPEND("io_unaligned_ops=%"PRIu64"\n", total.ioUnalignedOps);
	HFS_STATS_APPEND("io_rmw_ops=%"PRIu64"\n", total.ioRmwOps);
	HFS_STATS_APPEND("io_rmw_bytes=%"PRIu64"\n", total.ioRmwBytes);
	HFS_STATS_APPEND("io_copy_bytes=%"PRIu64"\n", total.ioCopyBytes);
	HFS_STATS_APPEND("io_copy_ops=%"PRIu64"\n", total.ioCopyOps);
	HFS_STATS_APPEND("io_direct_bytes=%"PRIu64"\n", total.ioDirectBytes);
	HFS_STATS_APPEND("io_direct_ops=%"PRIu64"\n", total.ioDirectOps);
	HFS_STATS_APPEND("io_vector_bytes=%"PRIu64"\n", total.ioVectorBytes);
	HFS_STATS_APPEND("io_vector_ops=%"PRIu64"\n", total.ioVectorOps);
	HFS_STATS_APPEND("resize_checks=%"PRIu64"\n", total.resizeChecks);
	HFS_STATS_APPEND("resize_fast_hits=%"PRIu64"\n", total.resizeFastHits);
	HFS_STATS_APPEND("resize_requests=%"PRIu64"\n", total.resizeRequests);
	HFS_STATS_APPEND("punch_requests=%"PRIu64"\n", total.punchRequests);
	HFS_STATS_APPEND("punch_unmap_only=%"PRIu64"\n", total.punchUnmapOnly);
	HFS_STATS_APPEND("punch_zero_fallbacks=%"PRIu64"\n",
		total.punchZeroFallbacks);
	for (i = 0; i < HFS_STATS_IO_SIZE_BUCKETS; i++) {
		HFS_STATS_APPEND("io_size_hist_%u=%"PRIu64"\n", i,
			total.ioSizeHist[i]);
	}
	HFS_STATS_APPEND("metadata_log_bytes=%"PRIu64"\n",
		total.metadataLogBytes);
	HFS_STATS_APPEND("metadata_image_bytes=%"PRIu64"\n",
		total.metadataImageBytes);
	HFS_STATS_APPEND("metadata_page_bytes=%"PRIu64"\n",
		total.metadataPageBytes);
	HFS_STATS_APPEND("metadata_transactions=%"PRIu64"\n",
		total.metadataTransactions);
	HFS_STATS_APPEND("metadata_batch_total=%"PRIu64"\n",
		total.metadataBatchTotal);
	HFS_STATS_APPEND("metadata_batch_max=%"PRIu64"\n",
		total.metadataBatchMax);
	HFS_STATS_APPEND("metadata_sync_count=%"PRIu64"\n",
		total.metadataSyncCount);
	HFS_STATS_APPEND("flush_count=%"PRIu64"\n", total.flushCount);
	HFS_STATS_APPEND("dma_wait_shard=%"PRIu64"\n", total.dmaWaits);
	HFS_STATS_APPEND("dma_wait_ns=%"PRIu64"\n", total.dmaWaitNs);
	HFS_STATS_APPEND("dma_exhaust_shard=%"PRIu64"\n", total.dmaExhausts);
	HFS_STATS_APPEND("dma_active_peak_shard=%"PRIu64"\n",
		total.dmaActivePeak);
	for (i = 0; i < HFS_STATS_DMA_SIZE_CLASSES; i++) {
		HFS_STATS_APPEND("dma_size_class_%u=%"PRIu64"\n", i,
			total.dmaSizeClass[i]);
	}
	for (i = 0; i < HFS_STATS_DMA_POOL_CLASSES; i++) {
		HFS_STATS_APPEND("dma_pool_class_%u=%"PRIu64"\n", i,
			total.dmaPoolClass[i]);
	}
#undef HFS_STATS_APPEND
	if (used > HFS_STATS_SNAPSHOT_BYTES) {
		used = HFS_STATS_SNAPSHOT_BYTES;
	}
	/* Best-effort snapshot output; shutdown must not block on reporting errors. */
	ssize_t written = write(fd, buffer, used);
	(void)written;
	close(fd);
	free(buffer);
}

/* Enable optional counters only when a report path or explicit switch exists. */
void hfsStatsInit(void) {
	const char *path;
	const char *enabled;

	if (g_hfsStatsInitialized) {
		return;
	}
	hfsStatsReset();
	g_hfsStatsPath[0] = '\0';
	hfsStatsTimingState = false;
	hfsStatsFastEnabledState = false;
	path = getenv("HFS_STATS_PATH");
	enabled = getenv("HFS_STATS");
	if (path != NULL && path[0] != '\0') {
		if (snprintf(g_hfsStatsPath, sizeof(g_hfsStatsPath), "%s", path) >=
		    (int)sizeof(g_hfsStatsPath)) {
			g_hfsStatsPath[0] = '\0';
		}
	} else if (hfsStatsBool(enabled)) {
		snprintf(g_hfsStatsPath, sizeof(g_hfsStatsPath),
			"/var/tmp/hfs-stats-%ld.txt", (long)getpid());
	}
	hfsStatsTimingState = hfsStatsBool(getenv("HFS_STATS_TIMING")) &&
		g_hfsStatsPath[0] != '\0';
	hfsStatsFastEnabledState = g_hfsStatsPath[0] != '\0';
	g_hfsStatsInitialized = true;
	atomic_store_explicit(&hfsStatsEnabledState, g_hfsStatsPath[0] != '\0',
				      memory_order_release);
}

/* Stop counting before writing a coherent final report. */
void hfsStatsFini(void) {
	if (!g_hfsStatsInitialized) {
		return;
	}
	atomic_store_explicit(&hfsStatsEnabledState, false, memory_order_release);
	hfsStatsTimingState = false;
	hfsStatsFastEnabledState = false;
	hfsStatsWriteSnapshot();
	g_hfsStatsInitialized = false;
}
