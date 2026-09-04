#ifndef HFS_STATS_H
#define HFS_STATS_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* Process-local counters used only when HFS_STATS_PATH is configured. */
extern _Atomic bool hfsStatsEnabledState;
extern _Atomic uint_fast64_t hfsStatsFdReadLocks;
extern _Atomic uint_fast64_t hfsStatsFdWriteLocks;
extern _Atomic uint_fast64_t hfsStatsMetaReadLocks;
extern _Atomic uint_fast64_t hfsStatsMetaWriteLocks;
extern _Atomic uint_fast64_t hfsStatsDmaAcquires;
extern _Atomic uint_fast64_t hfsStatsDmaWaits;
extern _Atomic uint_fast64_t hfsStatsDmaReleases;
extern _Atomic uint_fast64_t hfsStatsDmaActivePeak;
extern _Atomic uint_fast64_t hfsStatsSpdkSubmits;
extern _Atomic uint_fast64_t hfsStatsSpdkCompletions;
extern _Atomic uint_fast64_t hfsStatsSpdkRejects;
extern _Atomic uint_fast64_t hfsStatsErrors;

/* Read HFS_STATS_PATH/HFS_STATS and reset counters for one process run. */
void hfsStatsInit(void);
/* Disable counters and write the final key-value snapshot if requested. */
void hfsStatsFini(void);

/* Add one event without taking a lock when statistics are disabled. */
static inline void hfsStatsAdd(_Atomic uint_fast64_t *counter) {
	if (atomic_load_explicit(&hfsStatsEnabledState, memory_order_relaxed)) {
		atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
	}
}

/* Record a high-water mark without serializing otherwise independent requests. */
static inline void hfsStatsPeak(_Atomic uint_fast64_t *counter,
				uint64_t value) {
	uint_fast64_t current;

	if (!atomic_load_explicit(&hfsStatsEnabledState, memory_order_relaxed)) {
		return;
	}
	current = atomic_load_explicit(counter, memory_order_relaxed);
	while (current < value &&
	       !atomic_compare_exchange_weak_explicit(counter, &current, value,
						      memory_order_relaxed,
						      memory_order_relaxed)) {
		/* Retry only when another request advanced the same high-water mark. */
	}
}

#define HFS_STATS_FD_READ_LOCK() hfsStatsAdd(&hfsStatsFdReadLocks)
#define HFS_STATS_FD_WRITE_LOCK() hfsStatsAdd(&hfsStatsFdWriteLocks)
#define HFS_STATS_META_READ_LOCK() hfsStatsAdd(&hfsStatsMetaReadLocks)
#define HFS_STATS_META_WRITE_LOCK() hfsStatsAdd(&hfsStatsMetaWriteLocks)
#define HFS_STATS_DMA_ACQUIRE() hfsStatsAdd(&hfsStatsDmaAcquires)
#define HFS_STATS_DMA_WAIT() hfsStatsAdd(&hfsStatsDmaWaits)
#define HFS_STATS_DMA_RELEASE() hfsStatsAdd(&hfsStatsDmaReleases)
#define HFS_STATS_DMA_ACTIVE(value) hfsStatsPeak(&hfsStatsDmaActivePeak, value)
#define HFS_STATS_SPDK_SUBMIT() hfsStatsAdd(&hfsStatsSpdkSubmits)
#define HFS_STATS_SPDK_COMPLETE() hfsStatsAdd(&hfsStatsSpdkCompletions)
#define HFS_STATS_SPDK_REJECT() hfsStatsAdd(&hfsStatsSpdkRejects)
#define HFS_STATS_ERROR() hfsStatsAdd(&hfsStatsErrors)

#endif /* HFS_STATS_H */
