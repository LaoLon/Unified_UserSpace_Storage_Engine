#ifndef HFS_STATS_H
#define HFS_STATS_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
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
extern bool hfsStatsFastEnabledState;
extern bool hfsStatsTimingState;

/* Lock classes are kept small so each thread can update a fixed shard. */
typedef enum HfsStatsLockClass {
	HFS_STATS_LOCK_FD = 0,
	HFS_STATS_LOCK_META,
	HFS_STATS_LOCK_IO,
	HFS_STATS_LOCK_RESIZE,
	HFS_STATS_LOCK_APPEND,
	HFS_STATS_LOCK_RUNTIME,
	HFS_STATS_LOCK_OTHER,
	HFS_STATS_LOCK_CLASS_COUNT
} HfsStatsLockClass;

/* Read HFS_STATS_PATH/HFS_STATS and reset counters for one process run. */
void hfsStatsInit(void);
/* Disable counters and write the final key-value snapshot if requested. */
void hfsStatsFini(void);

/* Return whether optional instrumentation is currently enabled. */
bool hfsStatsIsEnabled(void);
/* Return true when expensive lock/runtime timing is explicitly requested. */
bool hfsStatsTimingEnabled(void);
/* Inline gate used by hot-path call sites to avoid disabled-mode call overhead. */
static inline bool hfsStatsEnabledFast(void) {
	return hfsStatsFastEnabledState;
}
/* Inline gate for optional timestamp-heavy lock/runtime accounting. */
static inline bool hfsStatsTimingFastEnabled(void) {
	return hfsStatsTimingState;
}
/* Return a monotonic timestamp suitable for wait/hold measurements. */
uint64_t hfsStatsNowNs(void);

/* Record a lock acquisition without taking a statistics lock on the hot path. */
void hfsStatsLockBefore(HfsStatsLockClass lockClass);
/* Finish a lock acquisition and record wait time/contention. */
void hfsStatsLockAfter(HfsStatsLockClass lockClass);
/* Record lock hold time when the protected section is released. */
void hfsStatsLockRelease(HfsStatsLockClass lockClass);
static inline void hfsStatsLockBeforeFast(HfsStatsLockClass lockClass) {
	if (hfsStatsTimingFastEnabled()) {
		hfsStatsLockBefore(lockClass);
	}
}
static inline void hfsStatsLockAfterFast(HfsStatsLockClass lockClass) {
	if (hfsStatsTimingFastEnabled()) {
		hfsStatsLockAfter(lockClass);
	}
}
static inline void hfsStatsLockReleaseFast(HfsStatsLockClass lockClass) {
	if (hfsStatsTimingFastEnabled()) {
		hfsStatsLockRelease(lockClass);
	}
}

/* Record one public data I/O request and its copy/RMW accounting. */
void hfsStatsRecordIo(bool write, size_t bytes, bool aligned);
/* Record bytes copied between user memory and a DMA buffer. */
void hfsStatsRecordCopy(uint64_t bytes);
/* Record one read-modify-write unit and its byte cost. */
void hfsStatsRecordRmw(uint64_t bytes);

/* Record runtime queue depth and dispatch timing from a request wrapper. */
void hfsStatsRecordRuntimeSubmit(uint64_t queueDepth);
void hfsStatsRecordRuntimeDispatch(uint64_t queueWaitNs,
					   uint64_t ownerNs, uint64_t queueDepth);
/* Record bounded async executor queue and completion activity. */
void hfsStatsRecordAsyncSubmit(uint64_t queueDepth);
void hfsStatsRecordAsyncComplete(void);
void hfsStatsRecordAsyncCancel(void);

/* Record metadata image/page/log writes and transaction batch size. */
void hfsStatsRecordMetadataWrite(uint64_t bytes, uint64_t imageBytes,
					 uint64_t pageBytes);
void hfsStatsRecordMetadataTransaction(uint32_t batchSize);
/* Record one device or blobstore durability flush attempt. */
void hfsStatsRecordFlush(void);
/* Record one metadata blob sync that orders a metadata range write. */
void hfsStatsRecordMetadataSync(void);

/* Record a DMA size class, wait event and active high-water mark. */
void hfsStatsRecordDma(size_t bytes, bool waited, uint64_t active);

static inline void hfsStatsRecordIoFast(bool write, size_t bytes, bool aligned) {
	if (hfsStatsEnabledFast()) {
		hfsStatsRecordIo(write, bytes, aligned);
	}
}
static inline void hfsStatsRecordCopyFast(uint64_t bytes) {
	if (hfsStatsEnabledFast()) {
		hfsStatsRecordCopy(bytes);
	}
}
static inline void hfsStatsRecordRmwFast(uint64_t bytes) {
	if (hfsStatsEnabledFast()) {
		hfsStatsRecordRmw(bytes);
	}
}
static inline void hfsStatsRecordRuntimeSubmitFast(uint64_t queueDepth) {
	if (hfsStatsEnabledFast()) {
		hfsStatsRecordRuntimeSubmit(queueDepth);
	}
}
static inline void hfsStatsRecordRuntimeDispatchFast(uint64_t queueWaitNs,
									uint64_t ownerNs,
									uint64_t queueDepth) {
	if (hfsStatsEnabledFast()) {
		hfsStatsRecordRuntimeDispatch(queueWaitNs, ownerNs, queueDepth);
	}
}
static inline void hfsStatsRecordAsyncSubmitFast(uint64_t queueDepth) {
	if (hfsStatsEnabledFast()) {
		hfsStatsRecordAsyncSubmit(queueDepth);
	}
}
static inline void hfsStatsRecordAsyncCompleteFast(void) {
	if (hfsStatsEnabledFast()) {
		hfsStatsRecordAsyncComplete();
	}
}
static inline void hfsStatsRecordAsyncCancelFast(void) {
	if (hfsStatsEnabledFast()) {
		hfsStatsRecordAsyncCancel();
	}
}
static inline void hfsStatsRecordMetadataWriteFast(uint64_t bytes,
									uint64_t imageBytes,
									uint64_t pageBytes) {
	if (hfsStatsEnabledFast()) {
		hfsStatsRecordMetadataWrite(bytes, imageBytes, pageBytes);
	}
}
static inline void hfsStatsRecordMetadataTransactionFast(uint32_t batchSize) {
	if (hfsStatsEnabledFast()) {
		hfsStatsRecordMetadataTransaction(batchSize);
	}
}
static inline void hfsStatsRecordFlushFast(void) {
	if (hfsStatsEnabledFast()) {
		hfsStatsRecordFlush();
	}
}
static inline void hfsStatsRecordMetadataSyncFast(void) {
	if (hfsStatsEnabledFast()) {
		hfsStatsRecordMetadataSync();
	}
}
static inline void hfsStatsRecordDmaFast(size_t bytes, bool waited,
								 uint64_t active) {
	if (hfsStatsEnabledFast()) {
		hfsStatsRecordDma(bytes, waited, active);
	}
}

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
