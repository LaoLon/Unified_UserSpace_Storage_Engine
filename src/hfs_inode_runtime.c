#include <errno.h>
#include <stdlib.h>

#include "hfs_inode_runtime.h"
#include "hfs_meta.h"

/* Keep concurrent write timestamps monotonic within one inode runtime. */
static void hfsInodeRuntimeAtomicMax(_Atomic uint64_t *value,
					     uint64_t candidate) {
	uint64_t current = atomic_load_explicit(value, memory_order_relaxed);

	while (current < candidate &&
	       !atomic_compare_exchange_weak_explicit(value, &current, candidate,
						       memory_order_release,
						       memory_order_relaxed)) {
		/* The compare-exchange updates current before the next attempt. */
	}
}

/* Hash persistent ids without depending on the inode table's compact layout. */
static uint32_t hfsInodeRuntimeBucket(uint64_t inodeId) {
	inodeId ^= inodeId >> 33;
	inodeId *= UINT64_C(0xff51afd7ed558ccd);
	inodeId ^= inodeId >> 33;
	return (uint32_t)inodeId & (HFS_INODE_RUNTIME_BUCKET_COUNT - 1U);
}

/* Initialize runtime synchronization before the first file is opened. */
int hfsInodeRuntimeInit(Hfs *fs) {
	if (fs == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (pthread_mutex_init(&fs->inodeRuntimeLock, NULL) != 0) {
		errno = ENOMEM;
		return -1;
	}
	if (pthread_mutex_init(&fs->compatibilityResizeLock, NULL) != 0) {
		pthread_mutex_destroy(&fs->inodeRuntimeLock);
		errno = ENOMEM;
		return -1;
	}
	fs->compatibilityRuntime = hfsInodeRuntimeGet(fs, UINT64_MAX);
	if (fs->compatibilityRuntime == NULL) {
		pthread_mutex_destroy(&fs->compatibilityResizeLock);
		pthread_mutex_destroy(&fs->inodeRuntimeLock);
		return -1;
	}
	return 0;
}

/* Destroy lazy runtime entries after all open files have been released. */
void hfsInodeRuntimeFini(Hfs *fs) {
	uint32_t i;

	if (fs == NULL) {
		return;
	}
	for (i = 0; i < HFS_INODE_RUNTIME_BUCKET_COUNT; i++) {
		HfsInodeRuntime *runtime = fs->inodeRuntimeBuckets[i];

		while (runtime != NULL) {
			HfsInodeRuntime *next = runtime->next;
			uint32_t stripe;

			pthread_mutex_lock(&runtime->ticketLock);
			runtime->ticketStopping = true;
			pthread_cond_broadcast(&runtime->ticketAvailable);
			pthread_mutex_unlock(&runtime->ticketLock);
			/* The runtime is destroyed only after HFS has stopped accepting I/O. */
			pthread_cond_destroy(&runtime->ticketAvailable);
			pthread_mutex_destroy(&runtime->ticketLock);
			hfsRangeLockDestroy(&runtime->rangeGate);
			for (stripe = 0; stripe < HFS_INODE_RANGE_STRIPE_COUNT; stripe++) {
				hfsRangeLockDestroy(&runtime->rangeStripes[stripe]);
			}
			pthread_rwlock_destroy(&runtime->syncLock);
			pthread_mutex_destroy(&runtime->appendLock);
			pthread_mutex_destroy(&runtime->resizeLock);
			free(runtime);
			runtime = next;
		}
		fs->inodeRuntimeBuckets[i] = NULL;
	}
	fs->compatibilityRuntime = NULL;
	pthread_mutex_destroy(&fs->compatibilityResizeLock);
	pthread_mutex_destroy(&fs->inodeRuntimeLock);
}

/* Return an existing entry or create one while holding the short registry lock. */
HfsInodeRuntime *hfsInodeRuntimeGet(Hfs *fs, uint64_t inodeId) {
	HfsInodeRuntime *runtime;
	uint32_t bucket;
	int rc;

	if (fs == NULL || inodeId == 0) {
		errno = EINVAL;
		return NULL;
	}
	bucket = hfsInodeRuntimeBucket(inodeId);
	pthread_mutex_lock(&fs->inodeRuntimeLock);
	for (runtime = fs->inodeRuntimeBuckets[bucket]; runtime != NULL;
	     runtime = runtime->next) {
		if (runtime->inodeId == inodeId) {
			pthread_mutex_unlock(&fs->inodeRuntimeLock);
			return runtime;
		}
	}
	runtime = calloc(1, sizeof(*runtime));
	if (runtime == NULL) {
		pthread_mutex_unlock(&fs->inodeRuntimeLock);
		errno = ENOMEM;
		return NULL;
	}
	runtime->inodeId = inodeId;
	rc = pthread_mutex_init(&runtime->resizeLock, NULL);
	if (rc != 0) {
		free(runtime);
		pthread_mutex_unlock(&fs->inodeRuntimeLock);
		errno = rc;
		return NULL;
	}
	rc = pthread_mutex_init(&runtime->appendLock, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&runtime->resizeLock);
		free(runtime);
		pthread_mutex_unlock(&fs->inodeRuntimeLock);
		errno = rc;
		return NULL;
	}
	rc = pthread_rwlock_init(&runtime->syncLock, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&runtime->appendLock);
		pthread_mutex_destroy(&runtime->resizeLock);
		free(runtime);
		pthread_mutex_unlock(&fs->inodeRuntimeLock);
		errno = rc;
		return NULL;
	}
	rc = pthread_mutex_init(&runtime->ticketLock, NULL);
	if (rc != 0) {
		pthread_rwlock_destroy(&runtime->syncLock);
		pthread_mutex_destroy(&runtime->appendLock);
		pthread_mutex_destroy(&runtime->resizeLock);
		free(runtime);
		pthread_mutex_unlock(&fs->inodeRuntimeLock);
		errno = rc;
		return NULL;
	}
	rc = pthread_cond_init(&runtime->ticketAvailable, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&runtime->ticketLock);
		pthread_rwlock_destroy(&runtime->syncLock);
		pthread_mutex_destroy(&runtime->appendLock);
		pthread_mutex_destroy(&runtime->resizeLock);
		free(runtime);
		pthread_mutex_unlock(&fs->inodeRuntimeLock);
		errno = rc;
		return NULL;
	}
	runtime->ticketLimit = 64U;
	runtime->ticketStopping = false;
	rc = hfsRangeLockInit(&runtime->rangeGate);
	if (rc != 0) {
		pthread_cond_destroy(&runtime->ticketAvailable);
		pthread_mutex_destroy(&runtime->ticketLock);
		pthread_rwlock_destroy(&runtime->syncLock);
		pthread_mutex_destroy(&runtime->appendLock);
		pthread_mutex_destroy(&runtime->resizeLock);
		free(runtime);
		pthread_mutex_unlock(&fs->inodeRuntimeLock);
		errno = rc;
		return NULL;
	}
	{
		uint32_t stripe;

		for (stripe = 0; stripe < HFS_INODE_RANGE_STRIPE_COUNT; stripe++) {
			rc = hfsRangeLockInit(&runtime->rangeStripes[stripe]);
			if (rc != 0) {
				while (stripe > 0) {
					stripe--;
				hfsRangeLockDestroy(&runtime->rangeStripes[stripe]);
				}
				hfsRangeLockDestroy(&runtime->rangeGate);
				pthread_cond_destroy(&runtime->ticketAvailable);
				pthread_mutex_destroy(&runtime->ticketLock);
				pthread_rwlock_destroy(&runtime->syncLock);
				pthread_mutex_destroy(&runtime->appendLock);
				pthread_mutex_destroy(&runtime->resizeLock);
				free(runtime);
				errno = rc;
				pthread_mutex_unlock(&fs->inodeRuntimeLock);
				return NULL;
			}
		}
	}
	runtime->next = fs->inodeRuntimeBuckets[bucket];
	fs->inodeRuntimeBuckets[bucket] = runtime;
	pthread_mutex_unlock(&fs->inodeRuntimeLock);
	return runtime;
}

/* Return the runtime associated with a file, creating it for normal inode files. */
HfsInodeRuntime *hfsInodeRuntimeForFile(HfsFile *file) {
	if (file == NULL || file->fs == NULL) {
		errno = EINVAL;
		return NULL;
	}
	if (file->runtime != NULL) {
		return file->runtime;
	}
	if (file->inodeId == 0) {
		return file->fs->compatibilityRuntime;
	}
	if (file->inodeId != 0) {
		file->runtime = hfsInodeRuntimeGet(file->fs, file->inodeId);
	}
	return file->runtime;
}

/* Publish a metadata-authoritative EOF after truncate or open initialization. */
void hfsInodeRuntimeSetLogicalSize(HfsInodeRuntime *runtime, uint64_t size) {
	if (runtime != NULL) {
		atomic_store_explicit(&runtime->logicalSize, size, memory_order_release);
	}
}

/* Read the shared EOF used to decide whether a write needs metadata locking. */
uint64_t hfsInodeRuntimeGetLogicalSize(const HfsInodeRuntime *runtime) {
	return runtime == NULL ? 0 :
		atomic_load_explicit(&runtime->logicalSize, memory_order_acquire);
}

/* Record a write timestamp for a later group commit without blocking I/O. */
void hfsInodeRuntimeRecordMetadata(HfsInodeRuntime *runtime,
					   uint64_t mtimeNs, uint64_t ctimeNs) {
	if (runtime == NULL) {
		return;
	}
	hfsInodeRuntimeAtomicMax(&runtime->pendingMtimeNs, mtimeNs);
	hfsInodeRuntimeAtomicMax(&runtime->pendingCtimeNs, ctimeNs);
	atomic_store_explicit(&runtime->pendingMetadata, true, memory_order_release);
}

/* Read a stable-enough pending timestamp overlay for a metadata snapshot. */
bool hfsInodeRuntimeGetPendingMetadata(const HfsInodeRuntime *runtime,
					       uint64_t *mtimeNs, uint64_t *ctimeNs) {
	if (runtime == NULL ||
	    !atomic_load_explicit(&runtime->pendingMetadata, memory_order_acquire)) {
		return false;
	}
	if (mtimeNs != NULL) {
		*mtimeNs = atomic_load_explicit(&runtime->pendingMtimeNs,
						memory_order_acquire);
	}
	if (ctimeNs != NULL) {
		*ctimeNs = atomic_load_explicit(&runtime->pendingCtimeNs,
						memory_order_acquire);
	}
	return true;
}

/* Discard a coalesced overlay after a size-changing write updates the inode. */
void hfsInodeRuntimeDiscardPending(HfsInodeRuntime *runtime) {
	if (runtime != NULL) {
		atomic_store_explicit(&runtime->pendingMetadata, false,
					      memory_order_release);
	}
}

/* Merge coalesced timestamps into inodes while the caller owns metadata write lock. */
void hfsInodeRuntimeDrainPendingLocked(Hfs *fs) {
	uint32_t bucket;

	if (fs == NULL) {
		return;
	}
	/* The metadata lock must be held before entering this registry walk. */
	pthread_mutex_lock(&fs->inodeRuntimeLock);
	for (bucket = 0; bucket < HFS_INODE_RUNTIME_BUCKET_COUNT; bucket++) {
		HfsInodeRuntime *runtime;

		for (runtime = fs->inodeRuntimeBuckets[bucket]; runtime != NULL;
		     runtime = runtime->next) {
			struct hfs_inode *inode;
			uint64_t mtimeNs;
			uint64_t ctimeNs;

			if (!atomic_exchange_explicit(&runtime->pendingMetadata, false,
						      memory_order_acq_rel)) {
				continue;
			}
			mtimeNs = atomic_load_explicit(&runtime->pendingMtimeNs,
						       memory_order_acquire);
			ctimeNs = atomic_load_explicit(&runtime->pendingCtimeNs,
						       memory_order_acquire);
			inode = hfsMetaFindById(fs, runtime->inodeId);
			if (inode == NULL) {
				continue;
			}
			if (mtimeNs > inode->mtimeNs) {
				inode->mtimeNs = mtimeNs;
			}
			if (ctimeNs > inode->ctimeNs) {
				inode->ctimeNs = ctimeNs;
			}
			hfsMetaMarkInodeDirty(fs, inode->id);
		}
	}
	pthread_mutex_unlock(&fs->inodeRuntimeLock);
}
