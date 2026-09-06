#ifndef HFS_INODE_RUNTIME_H
#define HFS_INODE_RUNTIME_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "hfs.h"

/* Runtime-only state for one persistent inode; never serialized on disk. */
typedef struct hfs_inode_runtime {
	uint64_t inodeId;
	_Atomic uint64_t allocatedClusters;
	pthread_mutex_t resizeLock;
	pthread_mutex_t appendLock;
	pthread_rwlock_t syncLock;
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

#endif /* HFS_INODE_RUNTIME_H */
