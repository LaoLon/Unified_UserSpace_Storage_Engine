#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hfs_fd.h"
#include "hfs_lib.h"
#include "hfs_stats.h"

/* A bucketized registry keeps fd lookup contention local to a small fd range. */
#define HFS_FD_BUCKET_COUNT 256U

typedef struct HfsFdSlot {
	_Atomic(HfsOpenFile *) openFile;
	_Atomic uint64_t generation;
} HfsFdSlot;

static HfsFdSlot g_fdSlots[HFS_FD_TABLE_SIZE];
static pthread_mutex_t g_fdBuckets[HFS_FD_BUCKET_COUNT];
static pthread_once_t g_fdBucketsOnce = PTHREAD_ONCE_INIT;

/* Initialize bucket locks once before the first registry operation. */
static void hfsFdInitBuckets(void) {
	uint32_t i;

	for (i = 0; i < HFS_FD_BUCKET_COUNT; i++) {
		(void)pthread_mutex_init(&g_fdBuckets[i], NULL);
	}
}

/* Make bucket initialization visible to every caller, including preload users. */
static void hfsFdEnsureBuckets(void) {
	(void)pthread_once(&g_fdBucketsOnce, hfsFdInitBuckets);
}

/* Map an fd to a stable bucket without requiring a process-wide lock. */
static uint32_t hfsFdBucketIndex(int fd) {
	return ((uint32_t)fd) % HFS_FD_BUCKET_COUNT;
}

/* Acquire one bucket and account for lookup or mutation timing. */
static void hfsFdBucketLock(int fd, bool mutation) {
	hfsFdEnsureBuckets();
	if (mutation) {
		HFS_STATS_FD_WRITE_LOCK();
	} else {
		HFS_STATS_FD_READ_LOCK();
	}
	hfsStatsLockBeforeFast(HFS_STATS_LOCK_FD);
	(void)pthread_mutex_lock(&g_fdBuckets[hfsFdBucketIndex(fd)]);
	hfsStatsLockAfterFast(HFS_STATS_LOCK_FD);
}

/* Release one bucket and finish its timing sample. */
static void hfsFdBucketUnlock(int fd) {
	hfsStatsLockReleaseFast(HFS_STATS_LOCK_FD);
	(void)pthread_mutex_unlock(&g_fdBuckets[hfsFdBucketIndex(fd)]);
}

/* Lock all buckets in a fixed order for rare registry-wide metadata operations. */
static void hfsFdLockAllBuckets(void) {
	uint32_t i;

	hfsFdEnsureBuckets();
	for (i = 0; i < HFS_FD_BUCKET_COUNT; i++) {
		(void)pthread_mutex_lock(&g_fdBuckets[i]);
	}
}

/* Unlock all buckets in reverse order to preserve lock hierarchy. */
static void hfsFdUnlockAllBuckets(void) {
	uint32_t i = HFS_FD_BUCKET_COUNT;

	while (i-- > 0) {
		(void)pthread_mutex_unlock(&g_fdBuckets[i]);
	}
}

/* Increment a live description reference without allowing a zero-count revival. */
static bool hfsFdTryAcquireRef(HfsOpenFile *openFile) {
	uint32_t refs;

	refs = atomic_load_explicit(&openFile->refs, memory_order_acquire);
	while (refs != 0) {
		if (atomic_compare_exchange_weak_explicit(&openFile->refs, &refs,
			refs + 1U, memory_order_acquire, memory_order_relaxed)) {
			return true;
		}
	}
	return false;
}

/* Allocate a description shared by all duplicate fd slots. */
int hfsFdCreate(HfsOpenFile **out, HfsFile *file, uint64_t inodeId,
		int flags, bool writable, bool isDirectory, const char *dirPath) {
	HfsOpenFile *openFile;

	if (out == NULL) {
		errno = EINVAL;
		return -1;
	}
	openFile = calloc(1, sizeof(*openFile));
	if (openFile == NULL) {
		errno = ENOMEM;
		return -1;
	}
	if (pthread_mutex_init(&openFile->ioLock, NULL) != 0) {
		free(openFile);
		errno = ENOMEM;
		return -1;
	}
	if (pthread_mutex_init(&openFile->orphanLock, NULL) != 0) {
		pthread_mutex_destroy(&openFile->ioLock);
		free(openFile);
		errno = ENOMEM;
		return -1;
	}
	openFile->file = file;
	openFile->inodeId = inodeId;
	openFile->flags = flags;
	openFile->writable = writable;
	openFile->isDirectory = isDirectory;
	atomic_init(&openFile->refs, 1U);
	if (dirPath != NULL) {
		snprintf(openFile->dirPath, sizeof(openFile->dirPath), "%s", dirPath);
	}
	*out = openFile;
	return 0;
}

/* Release the private synchronization state of an unused description. */
void hfsFdDestroy(HfsOpenFile *openFile) {
	if (openFile == NULL) {
		return;
	}
	pthread_mutex_destroy(&openFile->orphanLock);
	pthread_mutex_destroy(&openFile->ioLock);
	free(openFile);
}

/* Install one description in an empty fd slot. */
int hfsFdRegister(int fd, HfsOpenFile *openFile) {
	int rc = 0;

	if (fd < 0 || fd >= HFS_FD_TABLE_SIZE || openFile == NULL) {
		errno = EBADF;
		return -1;
	}
	hfsFdBucketLock(fd, true);
	if (atomic_load_explicit(&g_fdSlots[fd].openFile,
			memory_order_acquire) != NULL) {
		rc = -1;
		errno = EBUSY;
	} else {
		atomic_fetch_add_explicit(&g_fdSlots[fd].generation, 1,
			memory_order_relaxed);
		atomic_store_explicit(&g_fdSlots[fd].openFile, openFile,
			memory_order_release);
	}
	hfsFdBucketUnlock(fd);
	return rc;
}

/* Acquire a stable slot reference with an increment-if-not-zero CAS. */
HfsOpenFile *hfsFdAcquire(int fd) {
	HfsOpenFile *openFile;
	uint64_t generation;

	if (fd < 0 || fd >= HFS_FD_TABLE_SIZE) {
		return NULL;
	}
	hfsFdBucketLock(fd, false);
	openFile = atomic_load_explicit(&g_fdSlots[fd].openFile,
		memory_order_acquire);
	generation = atomic_load_explicit(&g_fdSlots[fd].generation,
		memory_order_relaxed);
	if (openFile == NULL || !hfsFdTryAcquireRef(openFile) ||
	    atomic_load_explicit(&g_fdSlots[fd].openFile,
			memory_order_acquire) != openFile ||
	    atomic_load_explicit(&g_fdSlots[fd].generation,
			memory_order_relaxed) != generation) {
		if (openFile != NULL &&
		    atomic_load_explicit(&openFile->refs, memory_order_acquire) != 0) {
			/* The bucket lock makes this compensation safe after a failed check. */
			(void)atomic_fetch_sub_explicit(&openFile->refs, 1,
				memory_order_release);
		}
		openFile = NULL;
	}
	hfsFdBucketUnlock(fd);
	return openFile;
}

/* Detach a slot before the kernel placeholder fd is closed or reused. */
HfsOpenFile *hfsFdDetach(int fd) {
	HfsOpenFile *openFile;

	if (fd < 0 || fd >= HFS_FD_TABLE_SIZE) {
		return NULL;
	}
	hfsFdBucketLock(fd, true);
	openFile = atomic_exchange_explicit(&g_fdSlots[fd].openFile, NULL,
		memory_order_acq_rel);
	atomic_fetch_add_explicit(&g_fdSlots[fd].generation, 1,
		memory_order_relaxed);
	hfsFdBucketUnlock(fd);
	return openFile;
}

/* Detach one arbitrary slot while the runtime is still available. */
HfsOpenFile *hfsFdDetachAny(int *fdOut) {
	HfsOpenFile *openFile = NULL;
	uint32_t bucket;
	uint32_t i;

	hfsFdEnsureBuckets();
	for (bucket = 0; bucket < HFS_FD_BUCKET_COUNT && openFile == NULL;
	     bucket++) {
		(void)pthread_mutex_lock(&g_fdBuckets[bucket]);
		for (i = bucket; i < HFS_FD_TABLE_SIZE; i += HFS_FD_BUCKET_COUNT) {
			openFile = atomic_load_explicit(&g_fdSlots[i].openFile,
				memory_order_acquire);
			if (openFile != NULL) {
				openFile = atomic_exchange_explicit(&g_fdSlots[i].openFile,
					NULL, memory_order_acq_rel);
				atomic_fetch_add_explicit(&g_fdSlots[i].generation, 1,
					memory_order_relaxed);
				if (fdOut != NULL) {
					*fdOut = (int)i;
				}
				break;
			}
		}
		(void)pthread_mutex_unlock(&g_fdBuckets[bucket]);
	}
	return openFile;
}

/* Lock two buckets in fd order so dup2 replacement cannot deadlock. */
static void hfsFdCopyLock(int oldfd, int newfd, uint32_t *first,
		uint32_t *second) {
	*first = hfsFdBucketIndex(oldfd);
	*second = hfsFdBucketIndex(newfd);
	if (*first > *second) {
		uint32_t swap = *first;
		*first = *second;
		*second = swap;
	}
	hfsFdEnsureBuckets();
	(void)pthread_mutex_lock(&g_fdBuckets[*first]);
	if (*second != *first) {
		(void)pthread_mutex_lock(&g_fdBuckets[*second]);
	}
}

/* Unlock the ordered bucket pair. */
static void hfsFdCopyUnlock(uint32_t first, uint32_t second) {
	if (second != first) {
		(void)pthread_mutex_unlock(&g_fdBuckets[second]);
	}
	(void)pthread_mutex_unlock(&g_fdBuckets[first]);
}

/* Replace the target slot and make both descriptors share one description. */
int hfsFdCopy(int oldfd, int newfd, HfsOpenFile **replacedFile) {
	HfsOpenFile *oldFile;
	HfsOpenFile *replaced;
	uint32_t first;
	uint32_t second;

	if (replacedFile != NULL) {
		*replacedFile = NULL;
	}
	if (oldfd < 0 || oldfd >= HFS_FD_TABLE_SIZE ||
	    newfd < 0 || newfd >= HFS_FD_TABLE_SIZE) {
		errno = EBADF;
		return -1;
	}
	hfsFdCopyLock(oldfd, newfd, &first, &second);
	oldFile = atomic_load_explicit(&g_fdSlots[oldfd].openFile,
		memory_order_acquire);
	if (oldFile == NULL) {
		hfsFdCopyUnlock(first, second);
		errno = EBADF;
		return -1;
	}
	if (oldfd == newfd) {
		hfsFdCopyUnlock(first, second);
		return 0;
	}
	if (!hfsFdTryAcquireRef(oldFile)) {
		hfsFdCopyUnlock(first, second);
		errno = EBADF;
		return -1;
	}
	replaced = atomic_exchange_explicit(&g_fdSlots[newfd].openFile,
		oldFile, memory_order_acq_rel);
	atomic_fetch_add_explicit(&g_fdSlots[newfd].generation, 1,
		memory_order_relaxed);
	hfsFdCopyUnlock(first, second);
	if (replacedFile != NULL) {
		*replacedFile = replaced;
	} else if (replaced != NULL) {
		(void)hfsFdRelease(replaced, NULL);
	}
	return 0;
}

/* Drop one slot or in-flight operation reference without taking a global lock. */
bool hfsFdRelease(HfsOpenFile *openFile, HfsOrphan **readyOrphan) {
	uint32_t refs;
	HfsOrphan *orphan;

	if (readyOrphan != NULL) {
		*readyOrphan = NULL;
	}
	if (openFile == NULL) {
		return false;
	}
	refs = atomic_load_explicit(&openFile->refs, memory_order_acquire);
	while (refs != 0 && !atomic_compare_exchange_weak_explicit(
		&openFile->refs, &refs, refs - 1U, memory_order_acq_rel,
		memory_order_acquire)) {
		/* Retry until the reference count reaches zero or is decremented. */
	}
	if (refs == 0) {
		return false;
	}
	if (refs != 1U) {
		return false;
	}
	(void)pthread_mutex_lock(&openFile->orphanLock);
	orphan = openFile->orphan;
	openFile->orphan = NULL;
	if (orphan != NULL &&
	    atomic_fetch_sub_explicit(&orphan->openRefs, 1,
		memory_order_acq_rel) == 1U) {
		if (atomic_load_explicit(&orphan->cancelled, memory_order_acquire)) {
			/* A rolled-back orphan may have outlived the registry scan. */
			free(orphan);
		} else if (readyOrphan != NULL) {
			*readyOrphan = orphan;
		}
	}
	(void)pthread_mutex_unlock(&openFile->orphanLock);
	return true;
}

/* Check one slot atomically so non-HFS preload calls avoid all HFS locks. */
bool hfsFdIsHfs(int fd) {
	if (fd < 0 || fd >= HFS_FD_TABLE_SIZE) {
		return false;
	}
	return atomic_load_explicit(&g_fdSlots[fd].openFile,
		memory_order_acquire) != NULL;
}

/* Clear the registry during a fresh library initialization. */
void hfsFdReset(void) {
	uint32_t i;

	hfsFdLockAllBuckets();
	for (i = 0; i < HFS_FD_TABLE_SIZE; i++) {
		(void)atomic_exchange_explicit(&g_fdSlots[i].openFile, NULL,
			memory_order_acq_rel);
		atomic_fetch_add_explicit(&g_fdSlots[i].generation, 1,
			memory_order_relaxed);
	}
	hfsFdUnlockAllBuckets();
}

/* Attach one orphan record to every unique open description for an inode. */
int hfsFdMarkOrphan(uint64_t inodeId, uint64_t blobId,
		   uint64_t size, uint64_t ctimeNs, uint64_t mtimeNs,
		   HfsOrphan **orphanOut) {
	HfsOrphan *orphan = NULL;
	uint32_t i;
	uint32_t j;

	if (orphanOut != NULL) {
		*orphanOut = NULL;
	}
	hfsFdLockAllBuckets();
	for (i = 0; i < HFS_FD_TABLE_SIZE; i++) {
		HfsOpenFile *openFile = atomic_load_explicit(
			&g_fdSlots[i].openFile, memory_order_acquire);
		bool seen = false;

		if (openFile == NULL || openFile->inodeId != inodeId) {
			continue;
		}
		for (j = 0; j < i; j++) {
			HfsOpenFile *previous = atomic_load_explicit(
				&g_fdSlots[j].openFile, memory_order_relaxed);
			if (previous == openFile) {
				seen = true;
				break;
			}
		}
		if (seen) {
			continue;
		}
		(void)pthread_mutex_lock(&openFile->orphanLock);
		if (openFile->orphan != NULL) {
			(void)pthread_mutex_unlock(&openFile->orphanLock);
			continue;
		}
		if (orphan == NULL) {
			orphan = calloc(1, sizeof(*orphan));
			if (orphan == NULL) {
				(void)pthread_mutex_unlock(&openFile->orphanLock);
				hfsFdUnlockAllBuckets();
				errno = ENOMEM;
				return -1;
			}
			orphan->inodeId = inodeId;
			orphan->blobId = blobId;
			atomic_init(&orphan->cancelled, false);
			atomic_init(&orphan->openRefs, 0U);
		}
		openFile->orphan = orphan;
		atomic_store_explicit(&openFile->size, size, memory_order_relaxed);
		atomic_store_explicit(&openFile->ctimeNs, ctimeNs, memory_order_relaxed);
		atomic_store_explicit(&openFile->mtimeNs, mtimeNs,
			memory_order_relaxed);
		atomic_fetch_add_explicit(&orphan->openRefs, 1,
			memory_order_relaxed);
		(void)pthread_mutex_unlock(&openFile->orphanLock);
	}
	hfsFdUnlockAllBuckets();
	if (orphanOut != NULL) {
		*orphanOut = orphan;
	}
	return 0;
}

/* Detach a provisional orphan from all descriptions and release its record. */
void hfsFdClearOrphan(HfsOrphan *orphan) {
	uint32_t i;
	uint32_t remaining;

	if (orphan == NULL) {
		return;
	}
	/* Hold the caller's orphan reference while detached descriptions drain. */
	atomic_fetch_add_explicit(&orphan->openRefs, 1, memory_order_acquire);
	atomic_store_explicit(&orphan->cancelled, true, memory_order_release);
	hfsFdLockAllBuckets();
	for (i = 0; i < HFS_FD_TABLE_SIZE; i++) {
		HfsOpenFile *openFile = atomic_load_explicit(
			&g_fdSlots[i].openFile, memory_order_acquire);

		if (openFile == NULL) {
			continue;
		}
		(void)pthread_mutex_lock(&openFile->orphanLock);
		if (openFile->orphan == orphan) {
			openFile->orphan = NULL;
			(void)atomic_fetch_sub_explicit(&orphan->openRefs, 1,
				memory_order_acq_rel);
		}
		(void)pthread_mutex_unlock(&openFile->orphanLock);
	}
	hfsFdUnlockAllBuckets();
	remaining = atomic_fetch_sub_explicit(&orphan->openRefs, 1,
		memory_order_acq_rel);
	if (remaining == 1) {
		free(orphan);
	}
}

/* Read metadata retained by an unlinked open-file description. */
bool hfsFdGetOrphanInfo(HfsOpenFile *openFile, uint64_t *size,
			uint64_t *ctimeNs, uint64_t *mtimeNs) {
	bool result;

	if (openFile == NULL) {
		return false;
	}
	(void)pthread_mutex_lock(&openFile->orphanLock);
	result = openFile->orphan != NULL;
	if (result) {
		if (size != NULL) {
			*size = atomic_load_explicit(&openFile->size,
				memory_order_relaxed);
		}
		if (ctimeNs != NULL) {
			*ctimeNs = atomic_load_explicit(&openFile->ctimeNs,
				memory_order_relaxed);
		}
		if (mtimeNs != NULL) {
			*mtimeNs = atomic_load_explicit(&openFile->mtimeNs,
				memory_order_relaxed);
		}
	}
	(void)pthread_mutex_unlock(&openFile->orphanLock);
	return result;
}

/* Free an orphan record after its deferred blob deletion completes. */
void hfsFdOrphanDestroy(HfsOrphan *orphan) {
	free(orphan);
}
