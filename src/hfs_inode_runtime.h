#ifndef HFS_INODE_RUNTIME_H
#define HFS_INODE_RUNTIME_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "hfs.h"

/* Per-inode range stripes remove cross-file collisions from the data path. */
#define HFS_INODE_RANGE_STRIPE_COUNT 64U
#define HFS_INODE_RANGE_STRIPE_UNITS 64U

/* Runtime-only state for one persistent inode; never serialized on disk. */
typedef struct hfs_inode_runtime {
	uint64_t inodeId;
	_Atomic uint64_t allocatedClusters;
	/* Logical EOF shared by all open descriptions of this inode. */
	_Atomic uint64_t logicalSize;
	/* Coalesce write timestamps until the next metadata checkpoint. */
	_Atomic uint64_t pendingMtimeNs;
	_Atomic uint64_t pendingCtimeNs;
	_Atomic bool pendingMetadata;
	pthread_mutex_t resizeLock;
	pthread_mutex_t appendLock;
	pthread_rwlock_t syncLock;
	/* Serialize overlapping ranges without taking the global hash buckets. */
	HfsRangeLock rangeStripes[HFS_INODE_RANGE_STRIPE_COUNT];
	/* Retained as a compatibility barrier for legacy callers. */
	HfsRangeLock rangeGate;
	struct hfs_inode_runtime *next;
} HfsInodeRuntime;

/* Initialize the runtime registry and compatibility fallback lock. */
int hfsInodeRuntimeInit(Hfs *fs);
/* Destroy all runtime state after the filesystem has stopped accepting I/O. */
void hfsInodeRuntimeFini(Hfs *fs);
/* Return or lazily create runtime state for one persistent inode. */
HfsInodeRuntime *hfsInodeRuntimeGet(Hfs *fs, uint64_t inodeId);
/* Return the runtime state attached to an open file, if any. */
HfsInodeRuntime *hfsInodeRuntimeForFile(HfsFile *file);
/* Publish one write's timestamp without taking the global metadata lock. */
void hfsInodeRuntimeRecordMetadata(HfsInodeRuntime *runtime,
					   uint64_t mtimeNs, uint64_t ctimeNs);
/* Publish or read the shared logical EOF used by the write fast path. */
void hfsInodeRuntimeSetLogicalSize(HfsInodeRuntime *runtime, uint64_t size);
uint64_t hfsInodeRuntimeGetLogicalSize(const HfsInodeRuntime *runtime);
/* Read a pending timestamp overlay for stat-style metadata snapshots. */
bool hfsInodeRuntimeGetPendingMetadata(const HfsInodeRuntime *runtime,
					       uint64_t *mtimeNs, uint64_t *ctimeNs);
/* Drop pending timestamps after a size-changing write is committed directly. */
void hfsInodeRuntimeDiscardPending(HfsInodeRuntime *runtime);
/* Apply all pending timestamp overlays while the metadata write lock is held. */
void hfsInodeRuntimeDrainPendingLocked(Hfs *fs);

#endif /* HFS_INODE_RUNTIME_H */
