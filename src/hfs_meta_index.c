#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "hfs_meta.h"
#include "hfs_meta_index.h"

/* Open-addressed inode-id slot; the table stores indexes, not unstable pointers. */
typedef struct HfsMetaIdSlot {
	uint64_t id;
	uint32_t inodeIndex;
	bool used;
} HfsMetaIdSlot;

/* Open-addressed dentry slot keyed by parent id and name hash. */
typedef struct HfsMetaDentrySlot {
	uint64_t hash;
	uint64_t parentId;
	uint32_t inodeIndex;
	bool used;
} HfsMetaDentrySlot;

/* Ordered child indexes for one parent directory. */
typedef struct HfsMetaDirSlot {
	uint64_t parentId;
	uint32_t *children;
	uint32_t count;
	uint32_t capacity;
	bool used;
} HfsMetaDirSlot;

struct hfs_meta_index {
	HfsMetaIdSlot *ids;
	uint32_t idCapacity;
	HfsMetaDentrySlot *dentries;
	uint32_t dentryCapacity;
	HfsMetaDirSlot *dirs;
	uint32_t dirCapacity;
	bool dirty;
};

/* Mix one integer into a stable hash for power-of-two tables. */
static uint64_t hfsMetaIndexMix(uint64_t value) {
	value ^= value >> 30;
	value *= UINT64_C(0xbf58476d1ce4e5b9);
	value ^= value >> 27;
	value *= UINT64_C(0x94d049bb133111eb);
	return value ^ (value >> 31);
}

/* Hash a parent/name pair without allocating a temporary string. */
static uint64_t hfsMetaIndexNameHash(uint64_t parentId, const char *name) {
	uint64_t hash = hfsMetaIndexMix(parentId ^ UINT64_C(0x9e3779b97f4a7c15));

	while (name != NULL && *name != '\0') {
		hash = hfsMetaIndexMix(hash ^ (unsigned char)*name++);
	}
	return hash;
}

/* Round a requested slot count up to a safe power of two. */
static int hfsMetaIndexCapacity(uint32_t count, uint32_t *capacity) {
	uint32_t value = 16U;

	if (count > UINT32_MAX / 2U) {
		errno = EOVERFLOW;
		return -1;
	}
	while (value < count * 2U) {
		if (value > UINT32_MAX / 2U) {
			errno = EOVERFLOW;
			return -1;
		}
		value <<= 1;
	}
	*capacity = value;
	return 0;
}

/* Free every allocation owned by an index instance. */
static void hfsMetaIndexFree(struct hfs_meta_index *index) {
	uint32_t i;

	if (index == NULL) {
		return;
	}
	if (index->dirs != NULL) {
		for (i = 0; i < index->dirCapacity; i++) {
			free(index->dirs[i].children);
		}
	}
	free(index->dirs);
	free(index->dentries);
	free(index->ids);
	free(index);
}

/* Allocate the empty index container before metadata is loaded. */
int hfsMetaIndexInit(Hfs *fs) {
	if (fs == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (fs->metaIndex != NULL) {
		return 0;
	}
	fs->metaIndex = calloc(1, sizeof(*fs->metaIndex));
	if (fs->metaIndex == NULL) {
		return -1;
	}
	fs->metaIndex->dirty = true;
	return 0;
}

/* Release all indexed path and directory state. */
void hfsMetaIndexFini(Hfs *fs) {
	if (fs == NULL) {
		return;
	}
	hfsMetaIndexFree(fs->metaIndex);
	fs->metaIndex = NULL;
}

/* Mark the current index stale after an in-memory inode-table mutation. */
void hfsMetaIndexMarkDirty(Hfs *fs) {
	if (fs != NULL && fs->metaIndex != NULL) {
		fs->metaIndex->dirty = true;
	}
}

/* Return true when indexed lookups can be trusted without a linear fallback. */
bool hfsMetaIndexReady(const Hfs *fs) {
	return fs != NULL && fs->metaIndex != NULL && !fs->metaIndex->dirty;
}

/* Insert one id into a freshly allocated open-addressed table. */
static int hfsMetaIndexInsertId(struct hfs_meta_index *index,
					uint64_t id, uint32_t inodeIndex) {
	uint32_t slot = (uint32_t)hfsMetaIndexMix(id) & (index->idCapacity - 1U);

	while (index->ids[slot].used) {
		if (index->ids[slot].id == id) {
			errno = EIO;
			return -1;
		}
		slot = (slot + 1U) & (index->idCapacity - 1U);
	}
	index->ids[slot].used = true;
	index->ids[slot].id = id;
	index->ids[slot].inodeIndex = inodeIndex;
	return 0;
}

/* Insert one dentry into a freshly allocated open-addressed table. */
static int hfsMetaIndexInsertDentry(struct hfs_meta_index *index,
					    const Hfs *fs,
					    const struct hfs_inode *inode,
					    uint32_t inodeIndex) {
	uint64_t hash = hfsMetaIndexNameHash(inode->parentId, inode->name);
	uint32_t slot = (uint32_t)hash & (index->dentryCapacity - 1U);

	while (index->dentries[slot].used) {
		if (index->dentries[slot].hash == hash &&
		    index->dentries[slot].parentId == inode->parentId &&
		    strcmp(fs->inodes[index->dentries[slot].inodeIndex].name,
			   inode->name) == 0) {
			errno = EIO;
			return -1;
		}
		slot = (slot + 1U) & (index->dentryCapacity - 1U);
	}
	index->dentries[slot].used = true;
	index->dentries[slot].hash = hash;
	index->dentries[slot].parentId = inode->parentId;
	index->dentries[slot].inodeIndex = inodeIndex;
	return 0;
}

/* Find or allocate one directory child vector in a fresh index. */
static HfsMetaDirSlot *hfsMetaIndexGetDir(struct hfs_meta_index *index,
						 uint64_t parentId) {
	uint32_t slot = (uint32_t)hfsMetaIndexMix(parentId) &
		(index->dirCapacity - 1U);

	while (index->dirs[slot].used) {
		if (index->dirs[slot].parentId == parentId) {
			return &index->dirs[slot];
		}
		slot = (slot + 1U) & (index->dirCapacity - 1U);
	}
	index->dirs[slot].used = true;
	index->dirs[slot].parentId = parentId;
	return &index->dirs[slot];
}

/* Append one inode index to its parent's child vector. */
static int hfsMetaIndexAddChild(struct hfs_meta_index *index,
					const struct hfs_inode *inode,
					uint32_t inodeIndex) {
	HfsMetaDirSlot *dir = hfsMetaIndexGetDir(index, inode->parentId);
	uint32_t capacity;
	uint32_t *children;

	if (dir == NULL) {
		errno = ENOMEM;
		return -1;
	}
	if (dir->count == dir->capacity) {
		capacity = dir->capacity == 0 ? 8U : dir->capacity * 2U;
		if (capacity < dir->capacity) {
			errno = EOVERFLOW;
			return -1;
		}
		children = realloc(dir->children, (size_t)capacity * sizeof(*children));
		if (children == NULL) {
			return -1;
		}
		dir->children = children;
		dir->capacity = capacity;
	}
	dir->children[dir->count++] = inodeIndex;
	return 0;
}

/* Sort child indexes by name while preserving the inode table as-is. */
static int hfsMetaIndexCompareChild(const void *lhs, const void *rhs,
						 void *arg) {
	const Hfs *fs = arg;
	uint32_t left = *(const uint32_t *)lhs;
	uint32_t right = *(const uint32_t *)rhs;

	return strcmp(fs->inodes[left].name, fs->inodes[right].name);
}

/* Rebuild all indexes from the current inode table atomically. */
int hfsMetaIndexRebuild(Hfs *fs) {
	struct hfs_meta_index *next;
	uint32_t capacity;
	uint32_t i;

	if (fs == NULL || fs->inodes == NULL) {
		errno = EINVAL;
		return -1;
	}
	next = calloc(1, sizeof(*next));
	if (next == NULL) {
		return -1;
	}
	if (hfsMetaIndexCapacity(fs->inodeCount, &capacity) != 0) {
		hfsMetaIndexFree(next);
		return -1;
	}
	next->idCapacity = capacity;
	next->dentryCapacity = capacity;
	next->dirCapacity = capacity;
	next->ids = calloc(next->idCapacity, sizeof(*next->ids));
	next->dentries = calloc(next->dentryCapacity, sizeof(*next->dentries));
	next->dirs = calloc(next->dirCapacity, sizeof(*next->dirs));
	if (next->ids == NULL || next->dentries == NULL || next->dirs == NULL) {
		hfsMetaIndexFree(next);
		return -1;
	}
	for (i = 0; i < fs->inodeCount; i++) {
		if (fs->inodes[i].id == 0 ||
		    hfsMetaIndexInsertId(next, fs->inodes[i].id, i) != 0 ||
		    hfsMetaIndexInsertDentry(next, fs, &fs->inodes[i], i) != 0 ||
		    hfsMetaIndexAddChild(next, &fs->inodes[i], i) != 0) {
			hfsMetaIndexFree(next);
			return -1;
		}
	}
	for (i = 0; i < next->dirCapacity; i++) {
		if (next->dirs[i].used && next->dirs[i].count > 1) {
			qsort_r(next->dirs[i].children, next->dirs[i].count,
				sizeof(*next->dirs[i].children),
				hfsMetaIndexCompareChild, fs);
		}
	}
	next->dirty = false;
	hfsMetaIndexFree(fs->metaIndex);
	fs->metaIndex = next;
	return 0;
}

/* Find an inode index in the id table. */
static bool hfsMetaIndexLookupId(const struct hfs_meta_index *index,
					 uint64_t id, uint32_t *inodeIndex) {
	uint32_t slot;

	if (index == NULL || index->dirty || index->idCapacity == 0) {
		return false;
	}
	slot = (uint32_t)hfsMetaIndexMix(id) & (index->idCapacity - 1U);
	while (index->ids[slot].used) {
		if (index->ids[slot].id == id) {
			*inodeIndex = index->ids[slot].inodeIndex;
			return true;
		}
		slot = (slot + 1U) & (index->idCapacity - 1U);
	}
	return false;
}

/* Look up an inode through the id index, with callers holding metadata lock. */
struct hfs_inode *hfsMetaIndexFindById(Hfs *fs, uint64_t id) {
	uint32_t inodeIndex;

	if (hfsMetaIndexLookupId(fs != NULL ? fs->metaIndex : NULL, id,
					&inodeIndex) && inodeIndex < fs->inodeCount &&
	    fs->inodes[inodeIndex].id == id) {
		return &fs->inodes[inodeIndex];
	}
	return NULL;
}

/* Look up an inode through the dentry index, with callers holding metadata lock. */
struct hfs_inode *hfsMetaIndexFindByName(Hfs *fs, uint64_t parentId,
					 const char *name) {
	const struct hfs_meta_index *index = fs != NULL ? fs->metaIndex : NULL;
	uint64_t hash;
	uint32_t slot;

	if (index == NULL || index->dirty || index->dentryCapacity == 0 ||
	    name == NULL) {
		return NULL;
	}
	hash = hfsMetaIndexNameHash(parentId, name);
	slot = (uint32_t)hash & (index->dentryCapacity - 1U);
	while (index->dentries[slot].used) {
		uint32_t inodeIndex = index->dentries[slot].inodeIndex;

		if (index->dentries[slot].hash == hash &&
		    index->dentries[slot].parentId == parentId &&
		    inodeIndex < fs->inodeCount &&
		    strcmp(fs->inodes[inodeIndex].name, name) == 0) {
			return &fs->inodes[inodeIndex];
		}
		slot = (slot + 1U) & (index->dentryCapacity - 1U);
	}
	return NULL;
}

/* Find one directory child in indexed name order. */
struct hfs_inode *hfsMetaIndexChildAt(Hfs *fs, uint64_t parentId,
					       uint32_t ordinal) {
	const struct hfs_meta_index *index = fs != NULL ? fs->metaIndex : NULL;
	uint32_t slot;

	if (index == NULL || index->dirty || index->dirCapacity == 0) {
		return NULL;
	}
	slot = (uint32_t)hfsMetaIndexMix(parentId) & (index->dirCapacity - 1U);
	while (index->dirs[slot].used) {
		if (index->dirs[slot].parentId == parentId) {
			uint32_t inodeIndex;

			if (ordinal >= index->dirs[slot].count) {
				return NULL;
			}
			inodeIndex = index->dirs[slot].children[ordinal];
			return inodeIndex < fs->inodeCount
			? &fs->inodes[inodeIndex] : NULL;
		}
		slot = (slot + 1U) & (index->dirCapacity - 1U);
	}
	return NULL;
}

/* Return the number of children from the indexed directory vector. */
uint32_t hfsMetaIndexChildCount(Hfs *fs, uint64_t parentId) {
	const struct hfs_meta_index *index = fs != NULL ? fs->metaIndex : NULL;
	uint32_t slot;

	if (index == NULL || index->dirty || index->dirCapacity == 0) {
		return 0;
	}
	slot = (uint32_t)hfsMetaIndexMix(parentId) & (index->dirCapacity - 1U);
	while (index->dirs[slot].used) {
		if (index->dirs[slot].parentId == parentId) {
			return index->dirs[slot].count;
		}
		slot = (slot + 1U) & (index->dirCapacity - 1U);
	}
	return 0;
}
