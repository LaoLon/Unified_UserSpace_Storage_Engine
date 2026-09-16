#ifndef HFS_INODE_RUNTIME_H
#define HFS_INODE_RUNTIME_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "hfs.h"

struct hfs_posix_io_ticket;
typedef struct hfs_posix_io_ticket HfsPosixIoTicket;

/* One inode-local scheduler owns interval admission and dependency wakeups. */
typedef struct hfs_inode_scheduler {
	pthread_mutex_t lock;
	pthread_cond_t condition;
	HfsPosixIoTicket *root;
	uint64_t nextTicketId;
	uint64_t nextTicketSeq;
	/* Monotonic epoch shared by data and future barrier operations. */
	uint64_t epoch;
	uint32_t count;
	uint32_t limit;
	bool stopping;
} HfsInodeScheduler;

/* Runtime-only state for one persistent inode; never serialized on disk. */
typedef struct hfs_inode_runtime {
	uint64_t inodeId;
	Hfs *ownerFs;
	_Atomic uint64_t allocatedClusters;
	/* Logical EOF shared by all open descriptions of this inode. */
	_Atomic uint64_t logicalSize;
	/* Coalesce write timestamps until the next metadata checkpoint. */
	_Atomic uint64_t pendingMtimeNs;
	_Atomic uint64_t pendingCtimeNs;
	_Atomic bool pendingMetadata;
	/* Intrusive membership in the mount-level dirty inode set. */
	bool dirtyLinked;
	uint64_t dirtyEpoch;
	struct hfs_inode_runtime *dirtyPrev;
	struct hfs_inode_runtime *dirtyNext;
	pthread_mutex_t resizeLock;
	/* Serialize writes that must materialize an EOF gap before publication. */
	pthread_mutex_t writeGapLock;
	pthread_mutex_t appendLock;
	pthread_rwlock_t syncLock;
	/* Unified interval scheduler for read/write and future barrier operations. */
	HfsInodeScheduler scheduler;
	struct hfs_inode_runtime *next;
} HfsInodeRuntime;

/* Initialize the runtime registry and per-inode scheduler state. */
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
