#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <spdk/blob.h>
#include <spdk/env.h>
#include <spdk/log.h>

#include "hfs_internal.h"
#include "hfs_meta.h"

/* Versioned metadata header used to keep inode IDs stable across compaction. */
#define HFS_META_MAGIC		0x32465348U
#define HFS_META_VERSION	2U

/* Return true only when the caller explicitly permits metadata formatting. */
static bool hfs_format_requested(void) {
	const char *format = getenv("HFS_FORMAT");

	return format != NULL && strcmp(format, "1") == 0;
}

/* Generic completion callback: marks the waiter as done. */
static void hfsMetaOpCb(void *arg, int bserrno) {
	Hfs *fs = (Hfs *)arg;

	hfsRuntimeCompletionComplete(&fs->completion);
}

/* Completion callback for spdk_bs_open_blob: stores the opened blob. */
static void hfsMetaOpenBlobCb(void *arg, struct spdk_blob *blob, int bserrno) {
	Hfs *fs = (Hfs *)arg;

	fs->metaBlob = blob;
	hfsRuntimeCompletionComplete(&fs->completion);
}

/* SPDK-thread task: open the metadata blob. */
static void hfsMetaDoOpenBlob(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_bs_open_blob(fs->blobstore, fs->metaBlobId, hfsMetaOpenBlobCb, fs);
}

/* SPDK-thread task: resize the metadata blob to the needed cluster count. */
static void hfsMetaDoResize(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_blob_resize(fs->metaBlob, fs->metaClusters, hfsMetaOpCb, fs);
}

/* SPDK-thread task: write the serialized metadata into the blob. */
static void hfsMetaDoWrite(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_blob_io_write(fs->metaBlob, fs->channel, fs->metaBuf,
		0, fs->metaUnits, hfsMetaOpCb, fs);
}

/* SPDK-thread task: read the metadata blob into the DMA buffer. */
static void hfsMetaDoRead(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_blob_io_read(fs->metaBlob, fs->channel, fs->metaBuf,
		0, fs->metaUnits, hfsMetaOpCb, fs);
}

/* SPDK-thread task: flush blob metadata to the backing device. */
static void hfsMetaDoSync(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_blob_sync_md(fs->metaBlob, hfsMetaOpCb, fs);
}

/* SPDK-thread task: close the metadata blob. */
static void hfsMetaDoClose(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_blob_close(fs->metaBlob, hfsMetaOpCb, fs);
}

/*
 * Write raw bytes into the metadata blob.
 * Runs the waiter chain: open -> resize -> write -> sync -> close.
 * 'buf' is copied into a DMA buffer first because SPDK requires
 * DMA-capable memory for blob I/O.
 */
static int hfsMetaBlobWrite(Hfs *fs, const void *buf, size_t len) {
	uint64_t clusterSize = spdk_bs_get_cluster_size(fs->blobstore);
	uint64_t ioUnitSize = fs->unitSize;
	uint64_t needClusters = (len + clusterSize - 1) / clusterSize;
	uint64_t needUnits = needClusters * clusterSize / ioUnitSize;
	void *tmp;
	int rc = -1;

	tmp = spdk_malloc(needUnits * ioUnitSize, 0x1000, NULL,
		SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (tmp == NULL) {
		return -1;
	}
	memset(tmp, 0, needUnits * ioUnitSize);
	memcpy(tmp, buf, len);

	fs->metaBuf = tmp;
	fs->metaUnits = needUnits;
	fs->metaClusters = needClusters;

	if (!waiter(globalThread, hfsMetaDoOpenBlob, fs, &fs->completion)) goto out;
	if (!waiter(globalThread, hfsMetaDoResize, fs, &fs->completion)) goto out;
	if (!waiter(globalThread, hfsMetaDoWrite, fs, &fs->completion)) goto out;
	if (!waiter(globalThread, hfsMetaDoSync, fs, &fs->completion)) goto out;
	if (!waiter(globalThread, hfsMetaDoClose, fs, &fs->completion)) goto out;

	rc = 0;
out:
	spdk_free(tmp);
	return rc;
}

/*
 * Read the whole metadata blob into a DMA buffer.
 * The caller receives the buffer via *outBuf and must release it with
 * spdk_free(). *outLen reports the buffer size in bytes.
 */
static int hfsMetaBlobRead(Hfs *fs, void **outBuf, size_t *outLen) {
	uint64_t clusterSize = spdk_bs_get_cluster_size(fs->blobstore);
	uint64_t ioUnitSize = fs->unitSize;
	uint64_t clusters;
	uint64_t units;
	void *tmp;
	int rc = -1;

	if (!waiter(globalThread, hfsMetaDoOpenBlob, fs, &fs->completion)) {
		return -1;
	}

	/* Derive the read size from the current blob size. */
	clusters = spdk_blob_get_num_clusters(fs->metaBlob);
	units = clusters * clusterSize / ioUnitSize;
	if (units == 0) {
		waiter(globalThread, hfsMetaDoClose, fs, &fs->completion);
		return -1;
	}

	tmp = spdk_malloc(units * ioUnitSize, 0x1000, NULL,
		SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (tmp == NULL) {
		waiter(globalThread, hfsMetaDoClose, fs, &fs->completion);
		return -1;
	}

	fs->metaBuf = tmp;
	fs->metaUnits = units;
	if (!waiter(globalThread, hfsMetaDoRead, fs, &fs->completion)) goto out;
	if (!waiter(globalThread, hfsMetaDoClose, fs, &fs->completion)) goto out;

	*outBuf = tmp;
	*outLen = units * ioUnitSize;
	return 0;
out:
	spdk_free(tmp);
	return rc;
}

/*
 * Serialize the versioned metadata header and inode table into the blob.
 * Layout: [magic][version][nextInodeId][count][inodes].
 */
int hfsMetaSaveAll(Hfs *fs) {
	const size_t headerSize = sizeof(uint32_t) * 2 + sizeof(uint64_t);
	const uint32_t magic = HFS_META_MAGIC;
	const uint32_t version = HFS_META_VERSION;
	uint32_t count = fs->inodeCount;
	uint64_t nextInodeId = fs->nextInodeId;
	size_t len = headerSize + (size_t)count * sizeof(struct hfs_inode);
	uint8_t *buf;
	int rc;

	if (nextInodeId == 0) {
		errno = EIO;
		return -1;
	}
	buf = malloc(len);
	if (buf == NULL) {
		return -1;
	}
	memcpy(buf, &magic, sizeof(magic));
	memcpy(buf + sizeof(magic), &version, sizeof(version));
	memcpy(buf + sizeof(magic) + sizeof(version), &nextInodeId,
	       sizeof(nextInodeId));
	memcpy(buf + headerSize - sizeof(count), &count, sizeof(count));
	memcpy(buf + headerSize, fs->inodes,
	       (size_t)count * sizeof(struct hfs_inode));

	rc = hfsMetaBlobWrite(fs, buf, len);
	free(buf);
	return rc;
}

/* Rebuild the in-memory inode table from the metadata blob. */
int hfsMetaLoadAll(Hfs *fs) {
	const size_t legacyHeaderSize = sizeof(uint32_t);
	const size_t headerSize = sizeof(uint32_t) * 2 + sizeof(uint64_t);
	uint8_t *buf;
	size_t len;
	size_t inodeOffset;
	uint32_t magic;
	uint32_t version;
	uint32_t count;
	uint64_t nextInodeId = 0;
	uint64_t maxInodeId = 0;
	uint32_t i;

	if (hfsMetaBlobRead(fs, (void **)&buf, &len) != 0) {
		return -1;
	}
	if (len < legacyHeaderSize) {
		spdk_free(buf);
		return -1;
	}
	memcpy(&count, buf, sizeof(count));
	inodeOffset = legacyHeaderSize;
	if (count == HFS_META_MAGIC) {
		if (len < headerSize) {
			spdk_free(buf);
			return -1;
		}
		memcpy(&magic, buf, sizeof(magic));
		memcpy(&version, buf + sizeof(magic), sizeof(version));
		memcpy(&nextInodeId, buf + sizeof(magic) + sizeof(version),
		       sizeof(nextInodeId));
		memcpy(&count, buf + headerSize - sizeof(count), sizeof(count));
		if (magic != HFS_META_MAGIC || version != HFS_META_VERSION) {
			spdk_free(buf);
			errno = EINVAL;
			return -1;
		}
		inodeOffset = headerSize;
	}
	if (count > HFS_MAX_INODES ||
	    len < inodeOffset + (size_t)count * sizeof(struct hfs_inode)) {
		spdk_free(buf);
		return -1;
	}

	memcpy(fs->inodes, buf + inodeOffset,
	       (size_t)count * sizeof(struct hfs_inode));
	fs->inodeCount = count;
	for (i = 0; i < count; i++) {
		if (fs->inodes[i].id > maxInodeId) {
			maxInodeId = fs->inodes[i].id;
		}
	}
	if (inodeOffset == headerSize) {
		fs->nextInodeId = nextInodeId;
	} else {
		fs->nextInodeId = maxInodeId == UINT64_MAX ? 0 : maxInodeId + 1;
	}
	if (fs->nextInodeId == 0 || fs->nextInodeId <= maxInodeId) {
		fs->nextInodeId = maxInodeId == UINT64_MAX ? 0 : maxInodeId + 1;
	}
	spdk_free(buf);
	if (fs->nextInodeId == 0) {
		errno = ENOSPC;
		return -1;
	}
	return 0;
}

/*
 * Append a zeroed inode to the table and return a pointer to it.
 * IDs are allocated monotonically so compacting the table cannot reuse one.
 */
struct hfs_inode *hfsMetaAddInode(Hfs *fs) {
	struct hfs_inode *inode;

	if (fs->inodeCount >= HFS_MAX_INODES) {
		return NULL;
	}
	if (fs->nextInodeId == 0 || fs->nextInodeId == UINT64_MAX) {
		return NULL;
	}
	inode = &fs->inodes[fs->inodeCount];
	memset(inode, 0, sizeof(*inode));
	inode->id = fs->nextInodeId++;
	fs->inodeCount++;
	return inode;
}

/* Linear search for an inode by (parentId, name). */
struct hfs_inode *hfsMetaFindByName(Hfs *fs, uint64_t parentId, const char *name) {
	uint32_t i;

	for (i = 0; i < fs->inodeCount; i++) {
		if (fs->inodes[i].parentId == parentId &&
		    strcmp(fs->inodes[i].name, name) == 0) {
			return &fs->inodes[i];
		}
	}
	return NULL;
}

/* Linear search for an inode by id. */
struct hfs_inode *hfsMetaFindById(Hfs *fs, uint64_t id) {
	uint32_t i;

	for (i = 0; i < fs->inodeCount; i++) {
		if (fs->inodes[i].id == id) {
			return &fs->inodes[i];
		}
	}
	return NULL;
}

/* Skip any leading '/' characters. */
static const char *hfsMetaSkipSlashes(const char *p) {
	while (*p == '/') {
		p++;
	}
	return p;
}

/*
 * Resolve an absolute path (e.g. "/hfs/a/b.txt") into its inode.
 * Walks each component with findByName starting from the root inode.
 * Returns NULL and sets errno=ENOENT for a missing component, or
 * errno=ENOTDIR when an intermediate component is not a directory.
 */
struct hfs_inode *hfsMetaResolvePath(Hfs *fs, const char *path) {
	struct hfs_inode *cur;
	const char *p;
	char comp[FILENAME_LENGTH];
	size_t len;

	cur = hfsMetaFindById(fs, HFS_ROOT_INODE_ID);
	if (cur == NULL) {
		errno = ENOENT;
		return NULL;
	}

	p = hfsMetaSkipSlashes(path);
	while (*p != '\0') {
		/* Extract the next path component. */
		len = 0;
		while (*p != '\0' && *p != '/' && len + 1 < sizeof(comp)) {
			comp[len++] = *p++;
		}
		comp[len] = '\0';
		if (len == 0) {
			p = hfsMetaSkipSlashes(p);
			continue;
		}
		/* Only directories can be descended into. */
		if (cur->type != HFS_INODE_DIR) {
			errno = ENOTDIR;
			return NULL;
		}
		/* "." keeps the current directory (POSIX). */
		if (strcmp(comp, ".") == 0) {
			p = hfsMetaSkipSlashes(p);
			continue;
		}
		/* ".." walks to the parent and clamps at the HFS root. */
		if (strcmp(comp, "..") == 0) {
			if (cur->id != HFS_ROOT_INODE_ID) {
				cur = hfsMetaFindById(fs, cur->parentId);
				if (cur == NULL) {
					errno = EIO;
					return NULL;
				}
			}
			p = hfsMetaSkipSlashes(p);
			continue;
		}
		cur = hfsMetaFindByName(fs, cur->id, comp);
		if (cur == NULL) {
			errno = ENOENT;
			return NULL;
		}
		p = hfsMetaSkipSlashes(p);
	}
	return cur;
}

/*
 * Split an absolute path into its parent directory inode and the leaf
 * name. The leaf name is returned in an internal buffer (valid until
 * the next call). Fails with EINVAL for the root itself and ENOENT
 * when the parent directory does not exist.
 */
int hfsMetaResolveParent(Hfs *fs, const char *path,
			 struct hfs_inode **parent, const char **leafName) {
	static char buf[PATH_MAX];
	char *slash;
	size_t len;

	*parent = NULL;
	*leafName = NULL;

	if (*hfsMetaSkipSlashes(path) == '\0') {
		errno = EINVAL;	/* the root directory has no parent */
		return -1;
	}

	snprintf(buf, sizeof(buf), "%s", path);

	/* Strip trailing slashes so the last component is the leaf. */
	len = strlen(buf);
	while (len > 1 && buf[len - 1] == '/') {
		buf[--len] = '\0';
	}

	slash = strrchr(buf, '/');
	if (slash == NULL || slash == buf) {
		/* Parent is the root directory. */
		*parent = hfsMetaFindById(fs, HFS_ROOT_INODE_ID);
		*leafName = slash ? slash + 1 : buf;
	} else {
		*slash = '\0';
		*parent = hfsMetaResolvePath(fs, buf);
		*leafName = slash + 1;
	}

	if (*parent == NULL || (*leafName)[0] == '\0') {
		errno = ENOENT;
		return -1;
	}
	return 0;
}

/*
 * Create a new directory inode under 'parent'.
 * Rejects duplicate names with EEXIST; persists the inode table.
 */
int hfsMetaMkdir(Hfs *fs, struct hfs_inode *parent, const char *name) {
	struct hfs_inode *dir;
	struct timespec ts;

	if (parent->type != HFS_INODE_DIR) {
		errno = ENOTDIR;
		return -1;
	}
	if (strlen(name) >= FILENAME_LENGTH) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (hfsMetaFindByName(fs, parent->id, name) != NULL) {
		errno = EEXIST;
		return -1;
	}

	dir = hfsMetaAddInode(fs);
	if (dir == NULL) {
		errno = ENOSPC;
		return -1;
	}

	clock_gettime(CLOCK_REALTIME, &ts);
	dir->type = HFS_INODE_DIR;
	dir->parentId = parent->id;
	dir->blobId = SPDK_BLOBID_INVALID;
	dir->ctimeNs = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
	dir->mtimeNs = dir->ctimeNs;
	snprintf(dir->name, sizeof(dir->name), "%s", name);

	return hfsMetaSaveAll(fs);
}

/*
 * Remove an empty directory resolved from 'path'.
 * Fails with ENOTDIR if it is a file, ENOTEMPTY if it has children.
 */
int hfsMetaRmdir(Hfs *fs, const char *path) {
	struct hfs_inode *dir;
	uint32_t i;

	dir = hfsMetaResolvePath(fs, path);
	if (dir == NULL) {
		return -1;
	}
	if (dir->type != HFS_INODE_DIR) {
		errno = ENOTDIR;
		return -1;
	}

	/* A directory is non-empty if any inode lists it as parent. */
	for (i = 0; i < fs->inodeCount; i++) {
		if (fs->inodes[i].parentId == dir->id) {
			errno = ENOTEMPTY;
			return -1;
		}
	}

	/* Remove by shifting the tail of the table over the entry. */
	for (i = 0; i < fs->inodeCount; i++) {
		if (fs->inodes[i].id == dir->id) {
			fs->inodes[i] = fs->inodes[fs->inodeCount - 1];
			fs->inodeCount--;
			break;
		}
	}

	return hfsMetaSaveAll(fs);
}

/* Async context for hfsMetaCreateFile (blob creation waiter). */
struct hfs_meta_create_ctx {
	Hfs *fs;
	HfsCompletion completion;
	int rc;
	spdk_blob_id blobId;
};

/* Completion callback for spdk_bs_create_blob (file data blob). */
static void hfsMetaCreateFileCb(void *arg, spdk_blob_id blobId, int bserrno) {
	struct hfs_meta_create_ctx *ctx = (struct hfs_meta_create_ctx *)arg;

	ctx->blobId = blobId;
	ctx->rc = bserrno;
	hfsRuntimeCompletionComplete(&ctx->completion);
}

/* SPDK-thread task: allocate a new data blob for a file. */
static void hfsMetaDoCreateFileBlob(void *arg) {
	struct hfs_meta_create_ctx *ctx = (struct hfs_meta_create_ctx *)arg;

	spdk_bs_create_blob(ctx->fs->blobstore, hfsMetaCreateFileCb, ctx);
}

/*
 * Create a new file: allocate a data blob, add a FILE inode under
 * 'parent' and persist the table. Fails with EEXIST if the name is taken.
 */
int hfsMetaCreateFile(Hfs *fs, struct hfs_inode *parent, const char *name) {
	struct hfs_meta_create_ctx ctx;
	struct hfs_inode *in;
	struct timespec ts;

	if (parent->type != HFS_INODE_DIR) {
		errno = ENOTDIR;
		return -1;
	}
	if (strlen(name) >= FILENAME_LENGTH) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (hfsMetaFindByName(fs, parent->id, name) != NULL) {
		errno = EEXIST;
		return -1;
	}

	ctx.rc = 0;
	ctx.blobId = SPDK_BLOBID_INVALID;
	ctx.fs = fs;

	if (!waiter(globalThread, hfsMetaDoCreateFileBlob, &ctx, &ctx.completion)) {
		errno = ETIMEDOUT;
		return -1;
	}
	if (ctx.rc != 0) {
		errno = -ctx.rc;
		return -1;
	}

	in = hfsMetaAddInode(fs);
	if (in == NULL) {
		errno = ENOSPC;
		return -1;
	}

	clock_gettime(CLOCK_REALTIME, &ts);
	in->type = HFS_INODE_FILE;
	in->parentId = parent->id;
	in->blobId = ctx.blobId;
	in->size = 0;
	in->ctimeNs = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
	in->mtimeNs = in->ctimeNs;
	snprintf(in->name, sizeof(in->name), "%s", name);

	return hfsMetaSaveAll(fs);
}

/*
 * Remove a file inode from the table; the caller deletes its blob.
 * Fails with EISDIR for a directory; returns the blob id to delete.
 */
int hfsMetaRemoveFile(Hfs *fs, struct hfs_inode *parent, const char *name,
		      uint64_t *blobId) {
	struct hfs_inode *in;
	uint32_t i;

	in = hfsMetaFindByName(fs, parent->id, name);
	if (in == NULL) {
		errno = ENOENT;
		return -1;
	}
	if (in->type != HFS_INODE_FILE) {
		errno = EISDIR;
		return -1;
	}

	*blobId = in->blobId;

	/* Remove by shifting the tail of the table over the entry. */
	for (i = 0; i < fs->inodeCount; i++) {
		if (fs->inodes[i].id == in->id) {
			fs->inodes[i] = fs->inodes[fs->inodeCount - 1];
			fs->inodeCount--;
			break;
		}
	}

	return 0;
}

/*
 * Rename 'oldPath' to 'newPath'. POSIX semantics: a plain-file target
 * is overwritten (its inode is dropped); a directory target must be
 * empty. The table is persisted once, so the rename is atomic.
 */
int hfsMetaRename(Hfs *fs, const char *oldPath, const char *newPath) {
	struct hfs_inode *src;
	struct hfs_inode *dst;
	struct hfs_inode *newParent;
	uint64_t srcId;
	uint64_t newParentId;
	const char *newLeaf;
	uint32_t i;

	src = hfsMetaResolvePath(fs, oldPath);
	if (src == NULL) {
		return -1;
	}
	srcId = src->id;

	/* Resolve the destination parent directory and leaf name. */
	if (hfsMetaResolveParent(fs, newPath, &newParent, &newLeaf) != 0) {
		return -1;
	}
	newParentId = newParent->id;
	if (strlen(newLeaf) >= FILENAME_LENGTH) {
		errno = ENAMETOOLONG;
		return -1;
	}

	/* A rename onto itself is a no-op. */
	if (src->parentId == newParentId &&
	    strcmp(src->name, newLeaf) == 0) {
		return 0;
	}

	dst = hfsMetaFindByName(fs, newParent->id, newLeaf);
	if (dst != NULL) {
		/* Type mismatches between file and directory. */
		if (src->type == HFS_INODE_DIR && dst->type != HFS_INODE_DIR) {
			errno = ENOTDIR;
			return -1;
		}
		if (src->type != HFS_INODE_DIR && dst->type == HFS_INODE_DIR) {
			errno = EISDIR;
			return -1;
		}
		if (dst->type == HFS_INODE_DIR) {
			/* A directory target must be empty. */
			for (i = 0; i < fs->inodeCount; i++) {
				if (fs->inodes[i].parentId == dst->id) {
					errno = ENOTEMPTY;
					return -1;
				}
			}
		}
		/* Overwrite: drop the destination inode (its blob is
		 * leaked for now; cleaned up by a later shard). */
		for (i = 0; i < fs->inodeCount; i++) {
			if (fs->inodes[i].id == dst->id) {
				fs->inodes[i] = fs->inodes[fs->inodeCount - 1];
				fs->inodeCount--;
				break;
			}
		}
	}

	/* Re-resolve moved entries after compacting the inode array. */
	src = hfsMetaFindById(fs, srcId);
	newParent = hfsMetaFindById(fs, newParentId);
	if (src == NULL || newParent == NULL) {
		errno = EIO;
		return -1;
	}

	/* Move the source inode into the destination slot. */
	src->parentId = newParent->id;
	snprintf(src->name, sizeof(src->name), "%s", newLeaf);

	return hfsMetaSaveAll(fs);
}

/* Update an inode's size and mtime, then persist the table. */
int hfsMetaSetSize(Hfs *fs, struct hfs_inode *in, uint64_t size) {
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	in->size = size;
	in->mtimeNs = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

	return hfsMetaSaveAll(fs);
}

/* Completion callback for spdk_bs_get_super: records the super blob id. */
static void hfsMetaGetSuperCb(void *arg, spdk_blob_id blobId, int bserrno) {
	Hfs *fs = (Hfs *)arg;

	if (bserrno != 0) {
		fs->metaBlobId = SPDK_BLOBID_INVALID;
		hfsRuntimeCompletionComplete(&fs->completion);
		return;
	}

	fs->metaBlobId = blobId;
	hfsRuntimeCompletionComplete(&fs->completion);
}

/* Completion callback for spdk_bs_set_super. */
static void hfsMetaSetSuperCb(void *arg, int bserrno) {
	Hfs *fs = (Hfs *)arg;

	hfsRuntimeCompletionComplete(&fs->completion);
}

/* Completion callback for spdk_bs_create_blob: registers the new blob
 * as the blobstore super blob. */
static void hfsMetaCreateBlobCb(void *arg, spdk_blob_id blobId, int bserrno) {
	Hfs *fs = (Hfs *)arg;

	if (bserrno != 0) {
		hfsRuntimeCompletionComplete(&fs->completion);
		return;
	}

	fs->metaBlobId = blobId;
	spdk_bs_set_super(fs->blobstore, blobId, hfsMetaSetSuperCb, fs);
}

/* SPDK-thread task: create the metadata blob (first-time format). */
static void hfsMetaDoFormat(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_bs_create_blob(fs->blobstore, hfsMetaCreateBlobCb, fs);
}

/* SPDK-thread task: fetch the blobstore super blob id. */
static void hfsMetaDoGetSuper(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_bs_get_super(fs->blobstore, hfsMetaGetSuperCb, fs);
}

/*
 * Load the super blob id from the blobstore.
 * Returns -1 with errno=ENOENT if the storage has never been formatted.
 */
int hfsMetaLoad(Hfs *fs) {
	if (!waiter(globalThread, hfsMetaDoGetSuper, fs, &fs->completion)) {
		return -1;
	}

	if (fs->metaBlobId == SPDK_BLOBID_INVALID) {
		errno = ENOENT;
		return -1;
	}

	SPDK_NOTICELOG("meta blobId: %"PRIu64"\n", fs->metaBlobId);
	return 0;
}

/*
 * First-time format: create a metadata blob and register it as the
 * blobstore super blob so it can be found on the next mount.
 */
int hfsMetaFormat(Hfs *fs) {
	if (!waiter(globalThread, hfsMetaDoFormat, fs, &fs->completion)) {
		return -1;
	}

	if (fs->metaBlobId == SPDK_BLOBID_INVALID) {
		return -1;
	}

	SPDK_NOTICELOG("formatted, meta blobId: %"PRIu64"\n", fs->metaBlobId);
	return 0;
}

/*
 * Entry point for metadata handling.
 * Allocates the inode table, then either loads an existing table or
 * formats a fresh one (with an empty table persisted).
 */
int hfsMetaInit(Hfs *fs) {
	struct hfs_inode *root;

	fs->inodes = calloc(HFS_MAX_INODES, sizeof(struct hfs_inode));
	if (fs->inodes == NULL) {
		return -1;
	}

	if (hfsMetaLoad(fs) == 0) {
		return hfsMetaLoadAll(fs);
	}
	if (errno != ENOENT) {
		free(fs->inodes);
		fs->inodes = NULL;
		return -1;
	}
	if (!hfs_format_requested()) {
		free(fs->inodes);
		fs->inodes = NULL;
		return -1;
	}

	if (hfsMetaFormat(fs) != 0) {
		free(fs->inodes);
		fs->inodes = NULL;
		return -1;
	}

	/* Create the root directory inode and persist the table. */
	fs->inodeCount = 0;
	fs->nextInodeId = 1;
	root = hfsMetaAddInode(fs);
	if (root == NULL) {
		free(fs->inodes);
		fs->inodes = NULL;
		return -1;
	}
	root->type = HFS_INODE_DIR;
	root->parentId = 0;
	root->blobId = SPDK_BLOBID_INVALID;
	snprintf(root->name, sizeof(root->name), "/");
	fs->rootInodeId = root->id;
	return hfsMetaSaveAll(fs);
}
