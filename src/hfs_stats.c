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
#define HFS_STATS_MAX_LOCK_DEPTH 8U
#define HFS_STATS_TICKET_SCAN_BUCKETS 7U
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
_Atomic uint_fast64_t hfsStatsDurabilityBatches;
_Atomic uint_fast64_t hfsStatsDurabilityBatchRequests;
_Atomic uint_fast64_t hfsStatsDurabilityBatchMax;
_Atomic uint_fast64_t hfsStatsDurabilityEpochs;
_Atomic uint_fast64_t hfsStatsDurabilityEpochWaiters;
_Atomic uint_fast64_t hfsStatsDurabilityDataTargets;
_Atomic uint_fast64_t hfsStatsDurabilityDataBatches;
_Atomic uint_fast64_t hfsStatsDurabilityMetadataCommits;
_Atomic uint_fast64_t hfsStatsDurabilityFlushes;
_Atomic uint_fast64_t hfsStatsFdatasyncFast;
static _Atomic uint_fast64_t g_hfsStatsDataReactorSubmit[HFS_STATS_MAX_DATA_REACTORS];
static _Atomic uint_fast64_t g_hfsStatsDataReactorComplete[HFS_STATS_MAX_DATA_REACTORS];
#define HFS_STATS_REACTOR_METRIC_COUNT 8U
static _Atomic uint_fast64_t g_hfsStatsReactorMetricSum[HFS_STATS_MAX_DATA_REACTORS][HFS_STATS_REACTOR_METRIC_COUNT];
static _Atomic uint_fast64_t g_hfsStatsReactorMetricCount[HFS_STATS_MAX_DATA_REACTORS][HFS_STATS_REACTOR_METRIC_COUNT];
static _Atomic uint_fast64_t g_hfsStatsReactorMetricMax[HFS_STATS_MAX_DATA_REACTORS][HFS_STATS_REACTOR_METRIC_COUNT];
static _Atomic uint_fast64_t g_hfsStatsReactorMetricHist[HFS_STATS_MAX_DATA_REACTORS][HFS_STATS_REACTOR_METRIC_COUNT][HFS_STATS_HIST_BUCKETS];
static _Atomic uint_fast64_t g_hfsStatsReactorPollCalls[HFS_STATS_MAX_DATA_REACTORS];
static _Atomic uint_fast64_t g_hfsStatsReactorPollBusy[HFS_STATS_MAX_DATA_REACTORS];
static _Atomic uint_fast64_t g_hfsStatsReactorPollGapSumNs[HFS_STATS_MAX_DATA_REACTORS];
static _Atomic uint_fast64_t g_hfsStatsReactorPollGapMaxNs[HFS_STATS_MAX_DATA_REACTORS];
_Atomic uint_fast64_t hfsStatsSpdkSubmits;
_Atomic uint_fast64_t hfsStatsSpdkCompletions;
_Atomic uint_fast64_t hfsStatsSpdkRejects;
_Atomic uint_fast64_t hfsStatsIoOutstanding;
_Atomic uint_fast64_t hfsStatsIoOutstandingPeak;
_Atomic uint_fast64_t hfsStatsErrors;
_Atomic uint_fast64_t hfsStatsQueueCreditWaits[HFS_STATS_MAX_DATA_REACTORS];
_Atomic uint_fast64_t hfsStatsQueueCreditWaitNs[HFS_STATS_MAX_DATA_REACTORS];
_Atomic uint_fast64_t hfsStatsQueueDepthPeak[HFS_STATS_MAX_DATA_REACTORS];
_Atomic uint_fast64_t hfsStatsQueueDrainBatches[HFS_STATS_MAX_DATA_REACTORS];
_Atomic uint_fast64_t hfsStatsQueueDrainItems[HFS_STATS_MAX_DATA_REACTORS];
_Atomic uint_fast64_t hfsStatsRingDoorbells;
_Atomic uint_fast64_t hfsStatsRingReservations;
_Atomic uint_fast64_t hfsStatsRingItems;
_Atomic uint_fast64_t hfsStatsLegacyDataSubmits;
_Atomic uint_fast64_t hfsStatsTicketAdmits;
_Atomic uint_fast64_t hfsStatsTicketReadies;
_Atomic uint_fast64_t hfsStatsTicketCompletes;
_Atomic uint_fast64_t hfsStatsTicketDependencyWaits;
_Atomic uint_fast64_t hfsStatsTicketDependencyWaitNs;
_Atomic uint_fast64_t hfsStatsTicketPromotes;
_Atomic uint_fast64_t hfsStatsTicketPendingPeak;
_Atomic uint_fast64_t hfsStatsTicketWakes;
_Atomic uint_fast64_t hfsStatsMessageSubmitAcquires;
_Atomic uint_fast64_t hfsStatsMessageSubmitWaitNs;
_Atomic uint_fast64_t hfsStatsMessageSubmitHoldNs;
_Atomic uint_fast64_t hfsStatsOwnerMessages;
_Atomic uint_fast64_t hfsStatsDataMessages;
_Atomic uint_fast64_t hfsStatsTicketConflicts;
_Atomic uint_fast64_t hfsStatsTicketDependencies;
_Atomic uint_fast64_t hfsStatsTicketRealWaits;
_Atomic uint_fast64_t hfsStatsTicketScanItems;
_Atomic uint_fast64_t hfsStatsTicketActiveSum;
_Atomic uint_fast64_t hfsStatsTicketActiveSamples;
_Atomic uint_fast64_t hfsStatsTicketActivePeak;
_Atomic uint_fast64_t hfsStatsTicketWaitNs;
_Atomic uint_fast64_t hfsStatsTicketAdmitNs;
_Atomic uint_fast64_t hfsStatsTicketCompleteNs;
_Atomic uint_fast64_t hfsStatsFutexWaitAttempts;
_Atomic uint_fast64_t hfsStatsFutexWaitSuccess;
_Atomic uint_fast64_t hfsStatsFutexEagain;
_Atomic uint_fast64_t hfsStatsFutexEintr;
_Atomic uint_fast64_t hfsStatsFutexSleepNs;
_Atomic uint_fast64_t hfsStatsCompletionToWakeNs;
_Atomic uint_fast64_t hfsStatsWakeToResumeNs;
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
	uint64_t messageWaitHist[HFS_STATS_HIST_BUCKETS];
	uint64_t messageHoldHist[HFS_STATS_HIST_BUCKETS];
	uint64_t messageWaitCount;
	uint64_t messageHoldCount;
	uint64_t messageWaitNs;
	uint64_t messageHoldNs;
	uint64_t copyInOps;
	uint64_t copyInBytes;
	uint64_t copyInTicks;
	uint64_t copyOutOps;
	uint64_t copyOutBytes;
	uint64_t copyOutTicks;
	uint64_t dmaDirectRequests;
	uint64_t dmaDirectRequestBytes;
	uint64_t dmaStagingRequests;
	uint64_t dmaStagingRequestBytes;
	uint64_t ticketMetricSum[HFS_STATS_TICKET_METRIC_COUNT];
	uint64_t ticketMetricCount[HFS_STATS_TICKET_METRIC_COUNT];
	uint64_t ticketMetricHist[HFS_STATS_TICKET_METRIC_COUNT][HFS_STATS_HIST_BUCKETS];
	uint64_t ticketAdmissionScanLength[HFS_STATS_TICKET_SCAN_BUCKETS];
	uint64_t ticketCompletionScanLength[HFS_STATS_TICKET_SCAN_BUCKETS];
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
static _Thread_local uint64_t g_hfsStatsMessageWaitStart;
static _Thread_local uint64_t g_hfsStatsMessageHoldStart;
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

/* Keep the reactor metric indexes stable for the key-value snapshot. */
enum HfsStatsReactorMetric {
	HFS_STATS_REACTOR_TB0_TB1 = 0,
	HFS_STATS_REACTOR_TB1_TB2,
	HFS_STATS_REACTOR_T6_T7,
	HFS_STATS_REACTOR_INFLIGHT_SUBMIT,
	HFS_STATS_REACTOR_INFLIGHT_COMPLETE,
	HFS_STATS_REACTOR_QUEUE_SUBMIT,
	HFS_STATS_REACTOR_QUEUE_COMPLETE,
	HFS_STATS_REACTOR_POLL_GAP_COMPLETE
};

/* Update one lock-free reactor metric sample and its percentile histogram. */
static void hfsStatsRecordReactorMetric(uint32_t reactorId,
						unsigned int metric, uint64_t value) {
	uint_fast64_t current;
	unsigned int bucket;

	if (reactorId >= HFS_STATS_MAX_DATA_REACTORS ||
	    metric >= HFS_STATS_REACTOR_METRIC_COUNT ||
	    !hfsStatsIsEnabled()) {
		return;
	}
	atomic_fetch_add_explicit(&g_hfsStatsReactorMetricSum[reactorId][metric],
		value, memory_order_relaxed);
	atomic_fetch_add_explicit(&g_hfsStatsReactorMetricCount[reactorId][metric],
		1, memory_order_relaxed);
	current = atomic_load_explicit(
		&g_hfsStatsReactorMetricMax[reactorId][metric], memory_order_relaxed);
	while (current < value && !atomic_compare_exchange_weak_explicit(
		&g_hfsStatsReactorMetricMax[reactorId][metric], &current, value,
		memory_order_relaxed, memory_order_relaxed)) {
		/* Retry with the refreshed maximum. */
	}
	bucket = hfsStatsBucket(value);
	atomic_fetch_add_explicit(
		&g_hfsStatsReactorMetricHist[reactorId][metric][bucket], 1,
		memory_order_relaxed);
}

/* Record the split backend stages and depths observed by one data reactor. */
void hfsStatsRecordBackendMetrics(uint32_t reactorId,
					  uint64_t tb0Tb1Ns, uint64_t tb1Tb2Ns,
					  uint64_t t6T7Ns, uint64_t submitInflight,
					  uint64_t completeInflight,
					  uint64_t submitQueueDepth,
					  uint64_t completeQueueDepth,
					  uint64_t pollGapAtCompleteNs) {
	hfsStatsRecordReactorMetric(reactorId, HFS_STATS_REACTOR_TB0_TB1,
		tb0Tb1Ns);
	hfsStatsRecordReactorMetric(reactorId, HFS_STATS_REACTOR_TB1_TB2,
		tb1Tb2Ns);
	hfsStatsRecordReactorMetric(reactorId, HFS_STATS_REACTOR_T6_T7,
		t6T7Ns);
	hfsStatsRecordReactorMetric(reactorId, HFS_STATS_REACTOR_INFLIGHT_SUBMIT,
		submitInflight);
	hfsStatsRecordReactorMetric(reactorId,
		HFS_STATS_REACTOR_INFLIGHT_COMPLETE, completeInflight);
	hfsStatsRecordReactorMetric(reactorId, HFS_STATS_REACTOR_QUEUE_SUBMIT,
		submitQueueDepth);
	hfsStatsRecordReactorMetric(reactorId, HFS_STATS_REACTOR_QUEUE_COMPLETE,
		completeQueueDepth);
	hfsStatsRecordReactorMetric(reactorId,
		HFS_STATS_REACTOR_POLL_GAP_COMPLETE, pollGapAtCompleteNs);
}

/* Record one data-reactor poll and the gap since its previous poll. */
void hfsStatsRecordReactorPoll(uint32_t reactorId, bool busy,
					 uint64_t gapNs) {
	if (reactorId >= HFS_STATS_MAX_DATA_REACTORS || !hfsStatsIsEnabled()) {
		return;
	}
	atomic_fetch_add_explicit(&g_hfsStatsReactorPollCalls[reactorId], 1,
		memory_order_relaxed);
	if (busy) {
		atomic_fetch_add_explicit(&g_hfsStatsReactorPollBusy[reactorId], 1,
			memory_order_relaxed);
	}
	atomic_fetch_add_explicit(&g_hfsStatsReactorPollGapSumNs[reactorId], gapNs,
		memory_order_relaxed);
	{
		uint_fast64_t current = atomic_load_explicit(
			&g_hfsStatsReactorPollGapMaxNs[reactorId], memory_order_relaxed);
		while (current < gapNs && !atomic_compare_exchange_weak_explicit(
			&g_hfsStatsReactorPollGapMaxNs[reactorId], &current, gapNs,
			memory_order_relaxed, memory_order_relaxed)) {
			/* Retry with the refreshed maximum. */
		}
	}
}

/* Map a ticket scan length to the report's fixed diagnostic buckets. */
static unsigned int hfsStatsTicketScanBucket(uint32_t scanItems) {
	if (scanItems == 0) {
		return 0;
	}
	if (scanItems <= 4U) {
		return 1;
	}
	if (scanItems <= 8U) {
		return 2;
	}
	if (scanItems <= 16U) {
		return 3;
	}
	if (scanItems <= 32U) {
		return 4;
	}
	if (scanItems <= 64U) {
		return 5;
	}
	return 6;
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
	atomic_store_explicit(&hfsStatsDurabilityBatches, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsDurabilityBatchRequests, 0,
		memory_order_relaxed);
	atomic_store_explicit(&hfsStatsDurabilityBatchMax, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsDurabilityEpochs, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsDurabilityEpochWaiters, 0,
		memory_order_relaxed);
	atomic_store_explicit(&hfsStatsDurabilityDataTargets, 0,
		memory_order_relaxed);
	atomic_store_explicit(&hfsStatsDurabilityDataBatches, 0,
		memory_order_relaxed);
	atomic_store_explicit(&hfsStatsDurabilityMetadataCommits, 0,
		memory_order_relaxed);
	atomic_store_explicit(&hfsStatsDurabilityFlushes, 0,
		memory_order_relaxed);
	atomic_store_explicit(&hfsStatsFdatasyncFast, 0, memory_order_relaxed);
	for (unsigned int i = 0; i < HFS_STATS_MAX_DATA_REACTORS; i++) {
		atomic_store_explicit(&g_hfsStatsDataReactorSubmit[i], 0,
			memory_order_relaxed);
		atomic_store_explicit(&g_hfsStatsDataReactorComplete[i], 0,
			memory_order_relaxed);
		atomic_store_explicit(&g_hfsStatsReactorPollCalls[i], 0,
			memory_order_relaxed);
		atomic_store_explicit(&g_hfsStatsReactorPollBusy[i], 0,
			memory_order_relaxed);
		atomic_store_explicit(&g_hfsStatsReactorPollGapSumNs[i], 0,
			memory_order_relaxed);
		atomic_store_explicit(&g_hfsStatsReactorPollGapMaxNs[i], 0,
			memory_order_relaxed);
		for (unsigned int metric = 0;
		     metric < HFS_STATS_REACTOR_METRIC_COUNT; metric++) {
			atomic_store_explicit(&g_hfsStatsReactorMetricSum[i][metric], 0,
				memory_order_relaxed);
			atomic_store_explicit(&g_hfsStatsReactorMetricCount[i][metric], 0,
				memory_order_relaxed);
			atomic_store_explicit(&g_hfsStatsReactorMetricMax[i][metric], 0,
				memory_order_relaxed);
			for (unsigned int bucket = 0;
			     bucket < HFS_STATS_HIST_BUCKETS; bucket++) {
				atomic_store_explicit(
					&g_hfsStatsReactorMetricHist[i][metric][bucket], 0,
					memory_order_relaxed);
			}
		}
	}
	atomic_store_explicit(&hfsStatsSpdkSubmits, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsSpdkCompletions, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsSpdkRejects, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsIoOutstanding, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsIoOutstandingPeak, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsErrors, 0, memory_order_relaxed);
	for (unsigned int i = 0; i < HFS_STATS_MAX_DATA_REACTORS; i++) {
		atomic_store_explicit(&hfsStatsQueueCreditWaits[i], 0,
			memory_order_relaxed);
		atomic_store_explicit(&hfsStatsQueueCreditWaitNs[i], 0,
			memory_order_relaxed);
		atomic_store_explicit(&hfsStatsQueueDepthPeak[i], 0,
			memory_order_relaxed);
		atomic_store_explicit(&hfsStatsQueueDrainBatches[i], 0,
			memory_order_relaxed);
		atomic_store_explicit(&hfsStatsQueueDrainItems[i], 0,
			memory_order_relaxed);
	}
	atomic_store_explicit(&hfsStatsRingDoorbells, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsRingReservations, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsRingItems, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsLegacyDataSubmits, 0,
		memory_order_relaxed);
	atomic_store_explicit(&hfsStatsTicketAdmits, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsTicketReadies, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsTicketCompletes, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsTicketDependencyWaits, 0,
		memory_order_relaxed);
	atomic_store_explicit(&hfsStatsTicketDependencyWaitNs, 0,
		memory_order_relaxed);
	atomic_store_explicit(&hfsStatsTicketPromotes, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsTicketPendingPeak, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsTicketWakes, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsMessageSubmitAcquires, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsMessageSubmitWaitNs, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsMessageSubmitHoldNs, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsOwnerMessages, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsDataMessages, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsTicketConflicts, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsTicketDependencies, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsTicketRealWaits, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsTicketScanItems, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsTicketActiveSum, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsTicketActiveSamples, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsTicketActivePeak, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsTicketWaitNs, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsTicketAdmitNs, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsTicketCompleteNs, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsFutexWaitAttempts, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsFutexWaitSuccess, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsFutexEagain, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsFutexEintr, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsFutexSleepNs, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsCompletionToWakeNs, 0, memory_order_relaxed);
	atomic_store_explicit(&hfsStatsWakeToResumeNs, 0, memory_order_relaxed);
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

/* Start timing the global SPDK message-submit mutex. */
void hfsStatsMessageSubmitLockBefore(void) {
	if (!hfsStatsTimingEnabled()) {
		return;
	}
	(void)hfsStatsShardGet();
	g_hfsStatsMessageWaitStart = hfsStatsNowNs();
}

/* Finish message-submit mutex wait timing after pthread_mutex_lock returns. */
void hfsStatsMessageSubmitLockAfter(void) {
	HfsStatsShard *shard;
	uint64_t now;
	uint64_t waitNs;

	if (!hfsStatsTimingEnabled() || g_hfsStatsMessageWaitStart == 0) {
		return;
	}
	now = hfsStatsNowNs();
	waitNs = now >= g_hfsStatsMessageWaitStart ?
		now - g_hfsStatsMessageWaitStart : 0;
	g_hfsStatsMessageHoldStart = now;
	atomic_fetch_add_explicit(&hfsStatsMessageSubmitAcquires, 1,
		memory_order_relaxed);
	atomic_fetch_add_explicit(&hfsStatsMessageSubmitWaitNs, waitNs,
		memory_order_relaxed);
	shard = hfsStatsShardGet();
	if (shard != NULL) {
		shard->messageWaitCount++;
		shard->messageWaitNs += waitNs;
		shard->messageWaitHist[hfsStatsBucket(waitNs)]++;
	}
}

/* Finish message-submit mutex hold timing on the sending thread. */
void hfsStatsMessageSubmitLockRelease(void) {
	HfsStatsShard *shard;
	uint64_t now;
	uint64_t holdNs;

	if (!hfsStatsTimingEnabled() || g_hfsStatsMessageHoldStart == 0) {
		return;
	}
	now = hfsStatsNowNs();
	holdNs = now >= g_hfsStatsMessageHoldStart ?
		now - g_hfsStatsMessageHoldStart : 0;
	atomic_fetch_add_explicit(&hfsStatsMessageSubmitHoldNs, holdNs,
		memory_order_relaxed);
	shard = hfsStatsShardGet();
	if (shard != NULL) {
		shard->messageHoldCount++;
		shard->messageHoldNs += holdNs;
		shard->messageHoldHist[hfsStatsBucket(holdNs)]++;
	}
	g_hfsStatsMessageWaitStart = 0;
	g_hfsStatsMessageHoldStart = 0;
}

/* Count one SPDK message by its destination class. */
void hfsStatsRecordMessage(bool dataMessage) {
	if (!hfsStatsIsEnabled()) {
		return;
	}
	if (dataMessage) {
		atomic_fetch_add_explicit(&hfsStatsDataMessages, 1,
			memory_order_relaxed);
	} else {
		atomic_fetch_add_explicit(&hfsStatsOwnerMessages, 1,
			memory_order_relaxed);
	}
}

/* Record one scheduler scan with conflict and active-count information. */
void hfsStatsRecordTicketAdmission(uint32_t scanItems,
					uint32_t pendingDependencies,
					uint32_t activeTickets,
					uint64_t admitNs) {
	if (!hfsStatsIsEnabled()) {
		return;
	}
	atomic_fetch_add_explicit(&hfsStatsTicketScanItems, scanItems,
		memory_order_relaxed);
	atomic_fetch_add_explicit(&hfsStatsTicketDependencies,
		pendingDependencies, memory_order_relaxed);
	if (pendingDependencies != 0) {
		atomic_fetch_add_explicit(&hfsStatsTicketConflicts, 1,
			memory_order_relaxed);
	}
	atomic_fetch_add_explicit(&hfsStatsTicketActiveSum, activeTickets,
		memory_order_relaxed);
	atomic_fetch_add_explicit(&hfsStatsTicketActiveSamples, 1,
		memory_order_relaxed);
	hfsStatsPeak(&hfsStatsTicketActivePeak, activeTickets);
	atomic_fetch_add_explicit(&hfsStatsTicketAdmitNs, admitNs,
			memory_order_relaxed);
	hfsStatsRecordTicketScanLength(false, scanItems);
}

/* Record only a ticket wait that reached pthread_cond_wait. */
void hfsStatsRecordTicketRealWait(uint64_t waitNs) {
	if (hfsStatsIsEnabled()) {
		atomic_fetch_add_explicit(&hfsStatsTicketRealWaits, 1,
			memory_order_relaxed);
		atomic_fetch_add_explicit(&hfsStatsTicketWaitNs, waitNs,
			memory_order_relaxed);
	}
}

/* Record one fine-grained ticket phase in a thread-local histogram. */
void hfsStatsRecordTicketMetric(HfsStatsTicketMetric metric, uint64_t durationNs) {
	HfsStatsShard *shard;

	if (!hfsStatsIsEnabled() || metric >= HFS_STATS_TICKET_METRIC_COUNT) {
		return;
	}
	shard = hfsStatsShardGet();
	if (shard == NULL) {
		return;
	}
	shard->ticketMetricSum[metric] += durationNs;
	shard->ticketMetricCount[metric]++;
	shard->ticketMetricHist[metric][hfsStatsBucket(durationNs)]++;
}

/* Record the admission or completion scheduler scan-length bucket. */
void hfsStatsRecordTicketScanLength(bool completion, uint32_t scanItems) {
	HfsStatsShard *shard;
	unsigned int bucket;

	if (!hfsStatsIsEnabled()) {
		return;
	}
	shard = hfsStatsShardGet();
	if (shard == NULL) {
		return;
	}
	bucket = hfsStatsTicketScanBucket(scanItems);
	if (completion) {
		shard->ticketCompletionScanLength[bucket]++;
	} else {
		shard->ticketAdmissionScanLength[bucket]++;
	}
}

/* Record the time spent scanning overlapping scheduler nodes during completion. */
void hfsStatsRecordTicketCompletion(uint64_t completeNs, uint64_t scanNs,
					uint32_t scanItems) {
	if (hfsStatsIsEnabled()) {
		atomic_fetch_add_explicit(&hfsStatsTicketCompleteNs, completeNs,
			memory_order_relaxed);
		hfsStatsRecordTicketMetric(HFS_STATS_TICKET_COMPLETE_SCAN, scanNs);
		hfsStatsRecordTicketScanLength(true, scanItems);
	}
}

/* Record one futex wait syscall attempt. */
void hfsStatsRecordFutexAttempt(void) {
	if (hfsStatsIsEnabled()) {
		atomic_fetch_add_explicit(&hfsStatsFutexWaitAttempts, 1,
			memory_order_relaxed);
	}
}

/* Record a futex syscall that slept until a wake. */
void hfsStatsRecordFutexSuccess(uint64_t sleepNs) {
	if (hfsStatsIsEnabled()) {
		atomic_fetch_add_explicit(&hfsStatsFutexWaitSuccess, 1,
			memory_order_relaxed);
		atomic_fetch_add_explicit(&hfsStatsFutexSleepNs, sleepNs,
			memory_order_relaxed);
	}
}

/* Record a futex EAGAIN race that did not sleep. */
void hfsStatsRecordFutexEagain(void) {
	if (hfsStatsIsEnabled()) {
		atomic_fetch_add_explicit(&hfsStatsFutexEagain, 1,
			memory_order_relaxed);
	}
}

/* Record a futex EINTR return that did not complete the wait. */
void hfsStatsRecordFutexEintr(void) {
	if (hfsStatsIsEnabled()) {
		atomic_fetch_add_explicit(&hfsStatsFutexEintr, 1,
			memory_order_relaxed);
	}
}

/* Accumulate completion callback to completion-wake handoff ticks. */
void hfsStatsRecordCompletionToWake(uint64_t ticks) {
	if (hfsStatsIsEnabled()) {
		atomic_fetch_add_explicit(&hfsStatsCompletionToWakeNs, ticks,
			memory_order_relaxed);
	}
}

/* Accumulate wake-to-application-resume ticks. */
void hfsStatsRecordWakeToResume(uint64_t ticks) {
	if (hfsStatsIsEnabled()) {
		atomic_fetch_add_explicit(&hfsStatsWakeToResumeNs, ticks,
			memory_order_relaxed);
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

/* Record a copy from application memory into a DMA staging buffer. */
void hfsStatsRecordCopyIn(uint64_t bytes) {
	HfsStatsShard *shard = hfsStatsShardGet();

	if (shard != NULL) {
		shard->copyInOps++;
		shard->copyInBytes += bytes;
	}
}

/* Record a copy from a DMA staging buffer back to application memory. */
void hfsStatsRecordCopyOut(uint64_t bytes) {
	HfsStatsShard *shard = hfsStatsShardGet();

	if (shard != NULL) {
		shard->copyOutOps++;
		shard->copyOutBytes += bytes;
	}
}

/* Record the duration of one application-to-DMA copy in SPDK ticks. */
void hfsStatsRecordCopyInTicks(uint64_t ticks) {
	HfsStatsShard *shard = hfsStatsShardGet();

	if (shard != NULL) {
		shard->copyInTicks += ticks;
	}
}

/* Record the duration of one DMA-to-application copy in SPDK ticks. */
void hfsStatsRecordCopyOutTicks(uint64_t ticks) {
	HfsStatsShard *shard = hfsStatsShardGet();

	if (shard != NULL) {
		shard->copyOutTicks += ticks;
	}
}

/* Record one logical request whose caller buffer was DMA-addressable. */
void hfsStatsRecordDmaDirectRequest(uint64_t bytes) {
	HfsStatsShard *shard = hfsStatsShardGet();

	if (shard != NULL) {
		shard->dmaDirectRequests++;
		shard->dmaDirectRequestBytes += bytes;
	}
}

/* Record one logical request that needed a bounded staging buffer. */
void hfsStatsRecordDmaStagingRequest(uint64_t bytes) {
	HfsStatsShard *shard = hfsStatsShardGet();

	if (shard != NULL) {
		shard->dmaStagingRequests++;
		shard->dmaStagingRequestBytes += bytes;
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

/* Record one bounded queue credit wait and its elapsed duration. */
void hfsStatsRecordQueueCreditWait(uint32_t reactorId, uint64_t waitNs) {
	if (reactorId >= HFS_STATS_MAX_DATA_REACTORS) {
		return;
	}
	atomic_fetch_add_explicit(&hfsStatsQueueCreditWaits[reactorId], 1,
		memory_order_relaxed);
	atomic_fetch_add_explicit(&hfsStatsQueueCreditWaitNs[reactorId], waitNs,
		memory_order_relaxed);
}

/* Record the queue high-water mark for one data reactor. */
void hfsStatsRecordQueueDepth(uint32_t reactorId, uint64_t depth) {
	uint_fast64_t current;

	if (reactorId >= HFS_STATS_MAX_DATA_REACTORS) {
		return;
	}
	current = atomic_load_explicit(&hfsStatsQueueDepthPeak[reactorId],
		memory_order_relaxed);
	while (current < depth &&
	       !atomic_compare_exchange_weak_explicit(
			&hfsStatsQueueDepthPeak[reactorId], &current, depth,
			memory_order_relaxed, memory_order_relaxed)) {
		/* Retry only when another producer advanced the same peak. */
	}
}

/* Record one reactor queue drain and the number of tickets it consumed. */
void hfsStatsRecordQueueDrain(uint32_t reactorId, uint64_t batchSize) {
	if (reactorId >= HFS_STATS_MAX_DATA_REACTORS) {
		return;
	}
	atomic_fetch_add_explicit(&hfsStatsQueueDrainBatches[reactorId], 1,
		memory_order_relaxed);
	atomic_fetch_add_explicit(&hfsStatsQueueDrainItems[reactorId], batchSize,
		memory_order_relaxed);
}

/* Record one actor doorbell edge rather than one notification per request. */
void hfsStatsRecordRingDoorbell(void) {
	if (!hfsStatsIsEnabled()) {
		return;
	}
	atomic_fetch_add_explicit(&hfsStatsRingDoorbells, 1, memory_order_relaxed);
}

/* Record one successful fixed-ring reservation. */
void hfsStatsRecordRingReservation(void) {
	if (!hfsStatsIsEnabled()) {
		return;
	}
	atomic_fetch_add_explicit(&hfsStatsRingReservations, 1,
		memory_order_relaxed);
}

/* Record the number of operations consumed by one actor dispatch. */
void hfsStatsRecordRingItems(uint64_t count) {
	if (!hfsStatsIsEnabled()) {
		return;
	}
	atomic_fetch_add_explicit(&hfsStatsRingItems, count, memory_order_relaxed);
}

/* Record a request that bypassed the actor ring through the legacy path. */
void hfsStatsRecordLegacyDataSubmit(void) {
	if (!hfsStatsIsEnabled()) {
		return;
	}
	atomic_fetch_add_explicit(&hfsStatsLegacyDataSubmits, 1,
		memory_order_relaxed);
}

/* Record one ticket admission and its active high-water mark. */
void hfsStatsRecordTicketAdmit(uint32_t pendingDependencies,
				       uint32_t activeTickets) {
	uint_fast64_t current;

	(void)pendingDependencies;
	if (!hfsStatsIsEnabled()) {
		return;
	}
	atomic_fetch_add_explicit(&hfsStatsTicketAdmits, 1, memory_order_relaxed);
	current = atomic_load_explicit(&hfsStatsTicketPendingPeak,
		memory_order_relaxed);
	while (current < activeTickets &&
	       !atomic_compare_exchange_weak_explicit(&hfsStatsTicketPendingPeak,
			&current, activeTickets, memory_order_relaxed,
			memory_order_relaxed)) {
		/* Retry with the refreshed peak. */
	}
}

/* Record one ticket that became executable. */
void hfsStatsRecordTicketReady(void) {
	if (hfsStatsIsEnabled()) {
		atomic_fetch_add_explicit(&hfsStatsTicketReadies, 1,
			memory_order_relaxed);
	}
}

/* Record one ticket removed from the active inode scheduler. */
void hfsStatsRecordTicketComplete(void) {
	if (hfsStatsIsEnabled()) {
		atomic_fetch_add_explicit(&hfsStatsTicketCompletes, 1,
			memory_order_relaxed);
	}
}

/* Record one dependency wait and its elapsed duration. */
void hfsStatsRecordTicketDependencyWait(uint64_t waitNs) {
	if (hfsStatsIsEnabled()) {
		atomic_fetch_add_explicit(&hfsStatsTicketDependencyWaits, 1,
			memory_order_relaxed);
		atomic_fetch_add_explicit(&hfsStatsTicketDependencyWaitNs, waitNs,
			memory_order_relaxed);
	}
}

/* Record a completion that promoted one or more dependent tickets. */
void hfsStatsRecordTicketPromote(uint32_t count) {
	if (hfsStatsIsEnabled()) {
		atomic_fetch_add_explicit(&hfsStatsTicketPromotes, count,
			memory_order_relaxed);
	}
}

/* Record one logical POSIX waiter wake. */
void hfsStatsRecordTicketWake(void) {
	if (hfsStatsIsEnabled()) {
		atomic_fetch_add_explicit(&hfsStatsTicketWakes, 1,
			memory_order_relaxed);
	}
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

/* Record one coordinator batch and its coalesced request count. */
void hfsStatsRecordDurabilityBatch(uint64_t requestCount) {
	uint_fast64_t current;

	atomic_fetch_add_explicit(&hfsStatsDurabilityBatches, 1,
		memory_order_relaxed);
	atomic_fetch_add_explicit(&hfsStatsDurabilityBatchRequests, requestCount,
		memory_order_relaxed);
	current = atomic_load_explicit(&hfsStatsDurabilityBatchMax,
		memory_order_relaxed);
	while (current < requestCount && !atomic_compare_exchange_weak_explicit(
		&hfsStatsDurabilityBatchMax, &current, requestCount,
		memory_order_relaxed, memory_order_relaxed)) {
		/* Retry until another producer publishes an equal or larger batch. */
	}
}

/* Record one mount-level durability epoch created by the coordinator. */
void hfsStatsRecordDurabilityEpoch(void) {
	atomic_fetch_add_explicit(&hfsStatsDurabilityEpochs, 1,
		memory_order_relaxed);
}

/* Record callers that wait on an already admitted durability epoch. */
void hfsStatsRecordDurabilityEpochWaiter(uint64_t count) {
	atomic_fetch_add_explicit(&hfsStatsDurabilityEpochWaiters, count,
		memory_order_relaxed);
}

/* Record unique blob/runtime targets handled by one epoch. */
void hfsStatsRecordDurabilityDataTarget(uint64_t count) {
	atomic_fetch_add_explicit(&hfsStatsDurabilityDataTargets, count,
		memory_order_relaxed);
}

/* Record one deduplicated data barrier stage. */
void hfsStatsRecordDurabilityDataBatch(void) {
	atomic_fetch_add_explicit(&hfsStatsDurabilityDataBatches, 1,
		memory_order_relaxed);
}

/* Record one metadata commit stage. */
void hfsStatsRecordDurabilityMetadataCommit(void) {
	atomic_fetch_add_explicit(&hfsStatsDurabilityMetadataCommits, 1,
		memory_order_relaxed);
}

/* Record one device flush stage. */
void hfsStatsRecordDurabilityFlush(void) {
	atomic_fetch_add_explicit(&hfsStatsDurabilityFlushes, 1,
		memory_order_relaxed);
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
	for (i = 0; i < HFS_STATS_HIST_BUCKETS; i++) {
		total->messageWaitHist[i] += shard->messageWaitHist[i];
		total->messageHoldHist[i] += shard->messageHoldHist[i];
	}
	total->messageWaitCount += shard->messageWaitCount;
	total->messageHoldCount += shard->messageHoldCount;
	total->messageWaitNs += shard->messageWaitNs;
	total->messageHoldNs += shard->messageHoldNs;
	total->copyInOps += shard->copyInOps;
	total->copyInBytes += shard->copyInBytes;
	total->copyInTicks += shard->copyInTicks;
	total->copyOutOps += shard->copyOutOps;
	total->copyOutBytes += shard->copyOutBytes;
	total->copyOutTicks += shard->copyOutTicks;
	total->dmaDirectRequests += shard->dmaDirectRequests;
	total->dmaDirectRequestBytes += shard->dmaDirectRequestBytes;
	total->dmaStagingRequests += shard->dmaStagingRequests;
	total->dmaStagingRequestBytes += shard->dmaStagingRequestBytes;
	for (i = 0; i < HFS_STATS_TICKET_METRIC_COUNT; i++) {
		total->ticketMetricSum[i] += shard->ticketMetricSum[i];
		total->ticketMetricCount[i] += shard->ticketMetricCount[i];
		for (j = 0; j < HFS_STATS_HIST_BUCKETS; j++) {
			total->ticketMetricHist[i][j] += shard->ticketMetricHist[i][j];
		}
	}
	for (i = 0; i < HFS_STATS_TICKET_SCAN_BUCKETS; i++) {
		total->ticketAdmissionScanLength[i] +=
			shard->ticketAdmissionScanLength[i];
		total->ticketCompletionScanLength[i] +=
			shard->ticketCompletionScanLength[i];
	}
}

/* Return the upper bound of a logarithmic histogram quantile. */
static uint64_t hfsStatsHistogramQuantile(const uint64_t *hist,
					 uint64_t total,
					 unsigned int percentile) {
	uint64_t target;
	uint64_t seen = 0;
	unsigned int i;

	if (hist == NULL || total == 0) {
		return 0;
	}
	target = (total * percentile + 99U) / 100U;
	if (target == 0) {
		target = 1;
	}
	for (i = 0; i < HFS_STATS_HIST_BUCKETS; i++) {
		seen += hist[i];
		if (seen >= target) {
			return UINT64_C(1) << i;
		}
	}
	return UINT64_C(1) << (HFS_STATS_HIST_BUCKETS - 1U);
}

/* Write an aggregated key-value report without depending on HFS logging. */
static void hfsStatsWriteSnapshot(void) {
	static const char *const lockNames[HFS_STATS_LOCK_CLASS_COUNT] = {
		"fd", "meta", "io", "range_gate", "resize", "append", "runtime", "other"
	};
	static const char *const ticketMetricNames[HFS_STATS_TICKET_METRIC_COUNT] = {
		"admit_lock_wait", "admit_scan", "admit_insert", "dependency_wait",
		"complete_lock_wait", "complete_scan", "wake_processing"
	};
	static const char *const ticketScanNames[HFS_STATS_TICKET_SCAN_BUCKETS] = {
		"0", "1_4", "5_8", "9_16", "17_32", "33_64", "gt64"
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
	HFS_STATS_APPEND("durability_group_batches=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsDurabilityBatches, memory_order_relaxed));
	HFS_STATS_APPEND("durability_group_requests=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsDurabilityBatchRequests,
		memory_order_relaxed));
	HFS_STATS_APPEND("durability_group_batch_max=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsDurabilityBatchMax, memory_order_relaxed));
	HFS_STATS_APPEND("durability_epochs=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsDurabilityEpochs, memory_order_relaxed));
	HFS_STATS_APPEND("durability_epoch_waiters=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsDurabilityEpochWaiters,
		memory_order_relaxed));
	HFS_STATS_APPEND("durability_data_targets=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsDurabilityDataTargets,
		memory_order_relaxed));
	HFS_STATS_APPEND("durability_data_batches=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsDurabilityDataBatches,
		memory_order_relaxed));
	HFS_STATS_APPEND("durability_metadata_commits=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsDurabilityMetadataCommits,
		memory_order_relaxed));
	HFS_STATS_APPEND("durability_flushes=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsDurabilityFlushes,
		memory_order_relaxed));
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
	HFS_STATS_APPEND("ticket_admit=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsTicketAdmits, memory_order_relaxed));
	HFS_STATS_APPEND("ticket_ready=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsTicketReadies, memory_order_relaxed));
	HFS_STATS_APPEND("ticket_complete=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsTicketCompletes, memory_order_relaxed));
	HFS_STATS_APPEND("ticket_dependency_waits=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsTicketDependencyWaits,
		memory_order_relaxed));
	HFS_STATS_APPEND("ticket_dependency_wait_ns=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsTicketDependencyWaitNs,
		memory_order_relaxed));
	HFS_STATS_APPEND("ticket_promotes=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsTicketPromotes, memory_order_relaxed));
	HFS_STATS_APPEND("ticket_pending_peak=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsTicketPendingPeak,
		memory_order_relaxed));
	HFS_STATS_APPEND("ticket_wakes=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsTicketWakes, memory_order_relaxed));
	HFS_STATS_APPEND("message_submit_lock_acquire=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsMessageSubmitAcquires, memory_order_relaxed));
	HFS_STATS_APPEND("message_submit_lock_wait_ticks=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsMessageSubmitWaitNs, memory_order_relaxed));
	HFS_STATS_APPEND("message_submit_lock_hold_ticks=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsMessageSubmitHoldNs, memory_order_relaxed));
	HFS_STATS_APPEND("owner_message_count=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsOwnerMessages, memory_order_relaxed));
	HFS_STATS_APPEND("data_message_count=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsDataMessages, memory_order_relaxed));
	HFS_STATS_APPEND("message_submit_lock_wait_p50_ns=%"PRIu64"\n",
		hfsStatsHistogramQuantile(total.messageWaitHist, total.messageWaitCount, 50));
	HFS_STATS_APPEND("message_submit_lock_wait_p95_ns=%"PRIu64"\n",
		hfsStatsHistogramQuantile(total.messageWaitHist, total.messageWaitCount, 95));
	HFS_STATS_APPEND("message_submit_lock_wait_p99_ns=%"PRIu64"\n",
		hfsStatsHistogramQuantile(total.messageWaitHist, total.messageWaitCount, 99));
	HFS_STATS_APPEND("message_submit_lock_hold_p50_ns=%"PRIu64"\n",
		hfsStatsHistogramQuantile(total.messageHoldHist, total.messageHoldCount, 50));
	HFS_STATS_APPEND("message_submit_lock_hold_p95_ns=%"PRIu64"\n",
		hfsStatsHistogramQuantile(total.messageHoldHist, total.messageHoldCount, 95));
	HFS_STATS_APPEND("message_submit_lock_hold_p99_ns=%"PRIu64"\n",
		hfsStatsHistogramQuantile(total.messageHoldHist, total.messageHoldCount, 99));
	HFS_STATS_APPEND("ticket_conflict_count=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsTicketConflicts, memory_order_relaxed));
	HFS_STATS_APPEND("ticket_dependency_count=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsTicketDependencies, memory_order_relaxed));
	HFS_STATS_APPEND("ticket_real_wait_count=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsTicketRealWaits, memory_order_relaxed));
	HFS_STATS_APPEND("ticket_scan_items_total=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsTicketScanItems, memory_order_relaxed));
	HFS_STATS_APPEND("ticket_active_list_length_sum=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsTicketActiveSum, memory_order_relaxed));
	HFS_STATS_APPEND("ticket_active_list_length_samples=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsTicketActiveSamples, memory_order_relaxed));
	HFS_STATS_APPEND("ticket_active_list_length_peak=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsTicketActivePeak, memory_order_relaxed));
	HFS_STATS_APPEND("ticket_wait_ticks=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsTicketWaitNs, memory_order_relaxed));
	HFS_STATS_APPEND("ticket_admit_ticks=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsTicketAdmitNs, memory_order_relaxed));
	HFS_STATS_APPEND("ticket_complete_ticks=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsTicketCompleteNs, memory_order_relaxed));
	for (i = 0; i < HFS_STATS_TICKET_METRIC_COUNT; i++) {
		uint64_t count = total.ticketMetricCount[i];
		uint64_t sum = total.ticketMetricSum[i];

		HFS_STATS_APPEND("ticket_%s_count=%"PRIu64"\n",
			ticketMetricNames[i], count);
		HFS_STATS_APPEND("ticket_%s_sum_ns=%"PRIu64"\n",
			ticketMetricNames[i], sum);
		HFS_STATS_APPEND("ticket_%s_avg_ns=%"PRIu64"\n",
			ticketMetricNames[i], count == 0 ? 0 : sum / count);
		HFS_STATS_APPEND("ticket_%s_p50_ns=%"PRIu64"\n",
			ticketMetricNames[i], hfsStatsHistogramQuantile(
				total.ticketMetricHist[i], count, 50));
		HFS_STATS_APPEND("ticket_%s_p95_ns=%"PRIu64"\n",
			ticketMetricNames[i], hfsStatsHistogramQuantile(
				total.ticketMetricHist[i], count, 95));
		HFS_STATS_APPEND("ticket_%s_p99_ns=%"PRIu64"\n",
			ticketMetricNames[i], hfsStatsHistogramQuantile(
				total.ticketMetricHist[i], count, 99));
	}
	for (i = 0; i < HFS_STATS_TICKET_SCAN_BUCKETS; i++) {
		HFS_STATS_APPEND("ticket_admission_scan_length_%s=%"PRIu64"\n",
			ticketScanNames[i], total.ticketAdmissionScanLength[i]);
		HFS_STATS_APPEND("ticket_completion_scan_length_%s=%"PRIu64"\n",
			ticketScanNames[i], total.ticketCompletionScanLength[i]);
	}
	HFS_STATS_APPEND("futex_wait_attempt=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsFutexWaitAttempts, memory_order_relaxed));
	HFS_STATS_APPEND("futex_wait_success=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsFutexWaitSuccess, memory_order_relaxed));
	HFS_STATS_APPEND("futex_eagain=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsFutexEagain, memory_order_relaxed));
	HFS_STATS_APPEND("futex_eintr=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsFutexEintr, memory_order_relaxed));
	HFS_STATS_APPEND("futex_sleep_ticks=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsFutexSleepNs, memory_order_relaxed));
	HFS_STATS_APPEND("completion_to_wake_ticks=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsCompletionToWakeNs, memory_order_relaxed));
	HFS_STATS_APPEND("wake_to_application_resume_ticks=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsWakeToResumeNs, memory_order_relaxed));
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
	HFS_STATS_APPEND("ring_doorbell=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsRingDoorbells, memory_order_relaxed));
	HFS_STATS_APPEND("ring_reservation=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsRingReservations, memory_order_relaxed));
	HFS_STATS_APPEND("ring_items=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsRingItems, memory_order_relaxed));
	HFS_STATS_APPEND("legacy_data_submit=%"PRIuFAST64"\n",
		atomic_load_explicit(&hfsStatsLegacyDataSubmits, memory_order_relaxed));
	for (i = 0; i < HFS_STATS_MAX_DATA_REACTORS; i++) {
		HFS_STATS_APPEND("data_reactor_%u_submit=%"PRIu64"\n", i,
			atomic_load_explicit(&g_hfsStatsDataReactorSubmit[i],
			memory_order_relaxed));
		HFS_STATS_APPEND("data_reactor_%u_complete=%"PRIu64"\n", i,
			atomic_load_explicit(&g_hfsStatsDataReactorComplete[i],
			memory_order_relaxed));
		HFS_STATS_APPEND("queue_reactor_%u_credit_waits=%"PRIuFAST64"\n", i,
			atomic_load_explicit(&hfsStatsQueueCreditWaits[i],
			memory_order_relaxed));
		HFS_STATS_APPEND("queue_reactor_%u_credit_wait_ns=%"PRIuFAST64"\n", i,
			atomic_load_explicit(&hfsStatsQueueCreditWaitNs[i],
			memory_order_relaxed));
		HFS_STATS_APPEND("queue_reactor_%u_depth_peak=%"PRIuFAST64"\n", i,
			atomic_load_explicit(&hfsStatsQueueDepthPeak[i],
			memory_order_relaxed));
		HFS_STATS_APPEND("queue_reactor_%u_drain_batches=%"PRIuFAST64"\n", i,
			atomic_load_explicit(&hfsStatsQueueDrainBatches[i],
			memory_order_relaxed));
		HFS_STATS_APPEND("queue_reactor_%u_drain_items=%"PRIuFAST64"\n", i,
			atomic_load_explicit(&hfsStatsQueueDrainItems[i],
			memory_order_relaxed));
	}
	{
		static const char *const reactorMetricNames[HFS_STATS_REACTOR_METRIC_COUNT] = {
			"tb0_tb1_ns", "tb1_tb2_ns", "t6_t7_ns", "inflight_submit",
			"inflight_complete", "queue_submit", "queue_complete",
			"poll_gap_complete_ns"
		};
		for (i = 0; i < HFS_STATS_MAX_DATA_REACTORS; i++) {
			HFS_STATS_APPEND("reactor_%u_poll_calls=%"PRIuFAST64"\n", i,
				atomic_load_explicit(&g_hfsStatsReactorPollCalls[i],
				memory_order_relaxed));
			HFS_STATS_APPEND("reactor_%u_poll_busy=%"PRIuFAST64"\n", i,
				atomic_load_explicit(&g_hfsStatsReactorPollBusy[i],
				memory_order_relaxed));
			HFS_STATS_APPEND("reactor_%u_poll_gap_sum_ns=%"PRIuFAST64"\n", i,
				atomic_load_explicit(&g_hfsStatsReactorPollGapSumNs[i],
				memory_order_relaxed));
			HFS_STATS_APPEND("reactor_%u_poll_gap_max_ns=%"PRIuFAST64"\n", i,
				atomic_load_explicit(&g_hfsStatsReactorPollGapMaxNs[i],
				memory_order_relaxed));
			for (j = 0; j < HFS_STATS_REACTOR_METRIC_COUNT; j++) {
				uint64_t hist[HFS_STATS_HIST_BUCKETS];
				uint64_t count = atomic_load_explicit(
					&g_hfsStatsReactorMetricCount[i][j], memory_order_relaxed);
				uint64_t sum = atomic_load_explicit(
					&g_hfsStatsReactorMetricSum[i][j], memory_order_relaxed);
				unsigned int bucket;

				for (bucket = 0; bucket < HFS_STATS_HIST_BUCKETS; bucket++) {
					hist[bucket] = atomic_load_explicit(
						&g_hfsStatsReactorMetricHist[i][j][bucket],
						memory_order_relaxed);
				}
				HFS_STATS_APPEND("reactor_%u_%s_count=%"PRIu64"\n", i,
					reactorMetricNames[j], count);
				HFS_STATS_APPEND("reactor_%u_%s_sum=%"PRIu64"\n", i,
					reactorMetricNames[j], sum);
				HFS_STATS_APPEND("reactor_%u_%s_avg=%"PRIu64"\n", i,
					reactorMetricNames[j], count == 0 ? 0 : sum / count);
				HFS_STATS_APPEND("reactor_%u_%s_max=%"PRIuFAST64"\n", i,
					reactorMetricNames[j], atomic_load_explicit(
						&g_hfsStatsReactorMetricMax[i][j], memory_order_relaxed));
				HFS_STATS_APPEND("reactor_%u_%s_p50=%"PRIu64"\n", i,
					reactorMetricNames[j], hfsStatsHistogramQuantile(hist, count, 50));
				HFS_STATS_APPEND("reactor_%u_%s_p95=%"PRIu64"\n", i,
					reactorMetricNames[j], hfsStatsHistogramQuantile(hist, count, 95));
				HFS_STATS_APPEND("reactor_%u_%s_p99=%"PRIu64"\n", i,
					reactorMetricNames[j], hfsStatsHistogramQuantile(hist, count, 99));
			}
		}
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
	HFS_STATS_APPEND("dma_direct_count=%"PRIu64"\n",
		total.dmaDirectRequests);
	HFS_STATS_APPEND("dma_direct_bytes=%"PRIu64"\n",
		total.dmaDirectRequestBytes);
	HFS_STATS_APPEND("staging_count=%"PRIu64"\n",
		total.dmaStagingRequests);
	HFS_STATS_APPEND("staging_bytes=%"PRIu64"\n",
		total.dmaStagingRequestBytes);
	HFS_STATS_APPEND("io_vector_bytes=%"PRIu64"\n", total.ioVectorBytes);
	HFS_STATS_APPEND("io_vector_ops=%"PRIu64"\n", total.ioVectorOps);
	HFS_STATS_APPEND("copy_in_ops=%"PRIu64"\n", total.copyInOps);
	HFS_STATS_APPEND("copy_in_bytes=%"PRIu64"\n", total.copyInBytes);
	HFS_STATS_APPEND("copy_in_ticks=%"PRIu64"\n", total.copyInTicks);
	HFS_STATS_APPEND("copy_out_ops=%"PRIu64"\n", total.copyOutOps);
	HFS_STATS_APPEND("copy_out_bytes=%"PRIu64"\n", total.copyOutBytes);
	HFS_STATS_APPEND("copy_out_ticks=%"PRIu64"\n", total.copyOutTicks);
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
	const char *profile;

	if (g_hfsStatsInitialized) {
		return;
	}
	hfsStatsReset();
	g_hfsStatsPath[0] = '\0';
	hfsStatsTimingState = false;
	hfsStatsFastEnabledState = false;
	path = getenv("HFS_STATS_PATH");
	enabled = getenv("HFS_STATS");
	profile = getenv("HFS_PROFILE");
	if (path != NULL && path[0] != '\0') {
		if (snprintf(g_hfsStatsPath, sizeof(g_hfsStatsPath), "%s", path) >=
		    (int)sizeof(g_hfsStatsPath)) {
			g_hfsStatsPath[0] = '\0';
		}
	} else if (hfsStatsBool(enabled)) {
		snprintf(g_hfsStatsPath, sizeof(g_hfsStatsPath),
			"/var/tmp/hfs-stats-%ld.txt", (long)getpid());
	}
	hfsStatsTimingState = (hfsStatsBool(getenv("HFS_STATS_TIMING")) ||
		(profile != NULL && !strcasecmp(profile, "timing"))) &&
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
