#ifndef HFS_META_INDEX_H
#define HFS_META_INDEX_H

#include <stdint.h>

#include "hfs.h"

/* Allocate the empty index container before metadata is loaded. */
int hfsMetaIndexInit(Hfs *fs);
/* Release all indexed path and directory state. */
void hfsMetaIndexFini(Hfs *fs);
/* Mark the current index stale after an in-memory inode-table mutation. */
void hfsMetaIndexMarkDirty(Hfs *fs);
/* Return true when indexed lookups can be trusted without a linear fallback. */
bool hfsMetaIndexReady(const Hfs *fs);
/* Rebuild all indexes from the current inode table atomically. */
int hfsMetaIndexRebuild(Hfs *fs);
/* Look up an inode through the id index, with callers holding metadata lock. */
struct hfs_inode *hfsMetaIndexFindById(Hfs *fs, uint64_t id);
/* Look up an inode through the dentry index, with callers holding metadata lock. */
struct hfs_inode *hfsMetaIndexFindByName(Hfs *fs, uint64_t parentId,
					 const char *name);
/* Return one child in deterministic name order, or NULL at the end. */
struct hfs_inode *hfsMetaIndexChildAt(Hfs *fs, uint64_t parentId,
				       uint32_t ordinal);
/* Return the number of children for one indexed directory. */
uint32_t hfsMetaIndexChildCount(Hfs *fs, uint64_t parentId);

#endif /* HFS_META_INDEX_H */
