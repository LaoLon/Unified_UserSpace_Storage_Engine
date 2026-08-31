#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <spdk/blob.h>
#include <spdk/env.h>
#include <spdk/log.h>

#include "hfs_internal.h"
#include "hfs_meta.h"

static void hfsMetaOpCb(void *arg, int bserrno) {
	Hfs *fs = (Hfs *)arg;

	fs->finished = true;
}

static void hfsMetaOpenBlobCb(void *arg, struct spdk_blob *blob, int bserrno) {
	Hfs *fs = (Hfs *)arg;

	fs->metaBlob = blob;
	fs->finished = true;
}

static void hfsMetaDoOpenBlob(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_bs_open_blob(fs->blobstore, fs->metaBlobId, hfsMetaOpenBlobCb, fs);
}

static void hfsMetaDoResize(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_blob_resize(fs->metaBlob, fs->metaClusters, hfsMetaOpCb, fs);
}

static void hfsMetaDoWrite(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_blob_io_write(fs->metaBlob, fs->channel, fs->metaBuf,
		0, fs->metaUnits, hfsMetaOpCb, fs);
}

static void hfsMetaDoRead(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_blob_io_read(fs->metaBlob, fs->channel, fs->metaBuf,
		0, fs->metaUnits, hfsMetaOpCb, fs);
}

static void hfsMetaDoSync(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_blob_sync_md(fs->metaBlob, hfsMetaOpCb, fs);
}

static void hfsMetaDoClose(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_blob_close(fs->metaBlob, hfsMetaOpCb, fs);
}

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

	fs->finished = false;
	if (!waiter(globalThread, hfsMetaDoOpenBlob, fs, &fs->finished)) goto out;
	fs->finished = false;
	if (!waiter(globalThread, hfsMetaDoResize, fs, &fs->finished)) goto out;
	fs->finished = false;
	if (!waiter(globalThread, hfsMetaDoWrite, fs, &fs->finished)) goto out;
	fs->finished = false;
	if (!waiter(globalThread, hfsMetaDoSync, fs, &fs->finished)) goto out;
	fs->finished = false;
	if (!waiter(globalThread, hfsMetaDoClose, fs, &fs->finished)) goto out;

	rc = 0;
out:
	spdk_free(tmp);
	return rc;
}

static int hfsMetaBlobRead(Hfs *fs, void **outBuf, size_t *outLen) {
	uint64_t clusterSize = spdk_bs_get_cluster_size(fs->blobstore);
	uint64_t ioUnitSize = fs->unitSize;
	uint64_t clusters;
	uint64_t units;
	void *tmp;
	int rc = -1;

	fs->finished = false;
	if (!waiter(globalThread, hfsMetaDoOpenBlob, fs, &fs->finished)) {
		return -1;
	}

	clusters = spdk_blob_get_num_clusters(fs->metaBlob);
	units = clusters * clusterSize / ioUnitSize;
	if (units == 0) {
		fs->finished = false;
		waiter(globalThread, hfsMetaDoClose, fs, &fs->finished);
		return -1;
	}

	tmp = spdk_malloc(units * ioUnitSize, 0x1000, NULL,
		SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (tmp == NULL) {
		fs->finished = false;
		waiter(globalThread, hfsMetaDoClose, fs, &fs->finished);
		return -1;
	}

	fs->metaBuf = tmp;
	fs->metaUnits = units;
	fs->finished = false;
	if (!waiter(globalThread, hfsMetaDoRead, fs, &fs->finished)) goto out;
	fs->finished = false;
	if (!waiter(globalThread, hfsMetaDoClose, fs, &fs->finished)) goto out;

	*outBuf = tmp;
	*outLen = units * ioUnitSize;
	return 0;
out:
	spdk_free(tmp);
	return rc;
}

int hfsMetaSaveAll(Hfs *fs) {
	uint32_t count = fs->inodeCount;
	size_t len = sizeof(count) + (size_t)count * sizeof(struct hfs_inode);
	uint8_t *buf;
	int rc;

	buf = malloc(len);
	if (buf == NULL) {
		return -1;
	}
	memcpy(buf, &count, sizeof(count));
	memcpy(buf + sizeof(count), fs->inodes,
	       (size_t)count * sizeof(struct hfs_inode));

	rc = hfsMetaBlobWrite(fs, buf, len);
	free(buf);
	return rc;
}

int hfsMetaLoadAll(Hfs *fs) {
	uint8_t *buf;
	size_t len;
	uint32_t count;

	if (hfsMetaBlobRead(fs, (void **)&buf, &len) != 0) {
		return -1;
	}
	if (len < sizeof(count)) {
		spdk_free(buf);
		return -1;
	}
	memcpy(&count, buf, sizeof(count));
	if (count > HFS_MAX_INODES) {
		spdk_free(buf);
		return -1;
	}

	memcpy(fs->inodes, buf + sizeof(count),
	       (size_t)count * sizeof(struct hfs_inode));
	fs->inodeCount = count;
	spdk_free(buf);
	return 0;
}

struct hfs_inode *hfsMetaAddInode(Hfs *fs) {
	struct hfs_inode *inode;

	if (fs->inodeCount >= HFS_MAX_INODES) {
		return NULL;
	}
	inode = &fs->inodes[fs->inodeCount];
	memset(inode, 0, sizeof(*inode));
	inode->id = (uint64_t)fs->inodeCount + 1;
	fs->inodeCount++;
	return inode;
}

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

struct hfs_inode *hfsMetaFindById(Hfs *fs, uint64_t id) {
	uint32_t i;

	for (i = 0; i < fs->inodeCount; i++) {
		if (fs->inodes[i].id == id) {
			return &fs->inodes[i];
		}
	}
	return NULL;
}

static void hfsMetaGetSuperCb(void *arg, spdk_blob_id blobId, int bserrno) {
	Hfs *fs = (Hfs *)arg;

	if (bserrno != 0) {
		fs->metaBlobId = SPDK_BLOBID_INVALID;
		fs->finished = true;
		return;
	}

	fs->metaBlobId = blobId;
	fs->finished = true;
}

static void hfsMetaSetSuperCb(void *arg, int bserrno) {
	Hfs *fs = (Hfs *)arg;

	fs->finished = true;
}

static void hfsMetaCreateBlobCb(void *arg, spdk_blob_id blobId, int bserrno) {
	Hfs *fs = (Hfs *)arg;

	if (bserrno != 0) {
		fs->finished = true;
		return;
	}

	fs->metaBlobId = blobId;
	spdk_bs_set_super(fs->blobstore, blobId, hfsMetaSetSuperCb, fs);
}

static void hfsMetaDoFormat(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_bs_create_blob(fs->blobstore, hfsMetaCreateBlobCb, fs);
}

static void hfsMetaDoGetSuper(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_bs_get_super(fs->blobstore, hfsMetaGetSuperCb, fs);
}

int hfsMetaLoad(Hfs *fs) {
	fs->finished = false;

	if (!waiter(globalThread, hfsMetaDoGetSuper, fs, &fs->finished)) {
		return -1;
	}

	if (fs->metaBlobId == SPDK_BLOBID_INVALID) {
		errno = ENOENT;
		return -1;
	}

	SPDK_NOTICELOG("meta blobId: %"PRIu64"\n", fs->metaBlobId);
	return 0;
}

int hfsMetaFormat(Hfs *fs) {
	fs->finished = false;

	if (!waiter(globalThread, hfsMetaDoFormat, fs, &fs->finished)) {
		return -1;
	}

	if (fs->metaBlobId == SPDK_BLOBID_INVALID) {
		return -1;
	}

	SPDK_NOTICELOG("formatted, meta blobId: %"PRIu64"\n", fs->metaBlobId);
	return 0;
}

int hfsMetaInit(Hfs *fs) {
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

	if (hfsMetaFormat(fs) != 0) {
		free(fs->inodes);
		fs->inodes = NULL;
		return -1;
	}

	fs->inodeCount = 0;
	return hfsMetaSaveAll(fs);
}
