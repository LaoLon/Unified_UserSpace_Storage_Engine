#include <string.h>

#include <spdk/blob.h>
#include <spdk/env.h>
#include <spdk/log.h>

#include "hfs_internal.h"

static void hfsSpdkBlobDeleteCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	spdk_free(file->writeBuffer);
	spdk_free(file->readBuffer);

	SPDK_NOTICELOG("close complete\n");
	file->finished = true;
}

static void hfsSpdkBlobCloseCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	spdk_bs_delete_blob(file->fs->blobstore, file->blobId, hfsSpdkBlobDeleteCb, file);
}

static void hfsDoClose(void *arg) {
	HfsFile *file = (HfsFile *)arg;

	spdk_blob_close(file->blob, hfsSpdkBlobCloseCb, file);
}

static void hfsSpdkBlobReadCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	file->finished = true;
}

static void hfsDoRead(void *arg) {
	HfsFile *file = (HfsFile *)arg;

	spdk_blob_io_read(file->blob, file->fs->channel, file->readBuffer,
		file->ioOffset, file->ioUnits, hfsSpdkBlobReadCb, file);
}

static void hfsSpdkBlobWriteCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	file->finished = true;
}

static void hfsDoWrite(void *arg) {
	HfsFile *file = (HfsFile *)arg;

	spdk_blob_io_write(file->blob, file->fs->channel, file->writeBuffer,
		file->ioOffset, file->ioUnits, hfsSpdkBlobWriteCb, file);
}

static void hfsSpdkBlobSyncCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;
	file->writeBuffer = spdk_malloc(BUFFER_SIZE, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (file->writeBuffer == NULL) {
		return ;
	}

	file->readBuffer = spdk_malloc(BUFFER_SIZE, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (file->readBuffer == NULL) {
		return ;
	}

	SPDK_NOTICELOG("open complete\n");

	file->finished = true;
}

static void hfsSpdkBlobResizeCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	uint64_t total = spdk_blob_get_num_clusters(file->blob);
	file->fs->numCluster = total;

	SPDK_NOTICELOG("resize blob :%"PRIu64"\n", total);

	spdk_blob_sync_md(file->blob, hfsSpdkBlobSyncCb, file);
}

static void hfsSpdkBsOpenBlobCb(void *arg, struct spdk_blob *blb, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	file->blob = blb;

	uint64_t freeCluster = spdk_bs_free_cluster_count(file->fs->blobstore);
	SPDK_NOTICELOG("freeCluster : %"PRIu64"\n", freeCluster);
	file->fs->freeClusterCount = freeCluster;

	spdk_blob_resize(blb, freeCluster, hfsSpdkBlobResizeCb, file);
}

static void hfsSpdkBsCreateBlobCb(void *arg, spdk_blob_id blobId, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	file->blobId = blobId;
	SPDK_NOTICELOG("blobId : %"PRIu64"\n", blobId);

	spdk_bs_open_blob(file->fs->blobstore, blobId, hfsSpdkBsOpenBlobCb, file);
}

static void hfsDoOpen(void *arg) {
	HfsFile *file = (HfsFile *)arg;

	spdk_bs_create_blob(file->fs->blobstore, hfsSpdkBsCreateBlobCb, file);
}

// open
int hfsOpen(HfsFile *file) {
	file->finished = false;

	return waiter(globalThread, hfsDoOpen, file, &file->finished);
}

static void hfsSpdkBlobResizeDoneCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	file->finished = true;
}

static void hfsDoResize(void *arg) {
	HfsFile *file = (HfsFile *)arg;

	spdk_blob_resize(file->blob, file->resizeClusters, hfsSpdkBlobResizeDoneCb, file);
}

static bool hfsEnsureSize(HfsFile *file, uint64_t bytes) {
	uint64_t clusterSize = spdk_bs_get_cluster_size(file->fs->blobstore);
	uint64_t needClusters = (bytes + clusterSize - 1) / clusterSize;

	if (spdk_blob_get_num_clusters(file->blob) >= needClusters) {
		return true;
	}

	file->resizeClusters = needClusters;
	file->finished = false;
	return waiter(globalThread, hfsDoResize, file, &file->finished);
}

// read at offset
ssize_t hfsReadAt(HfsFile *file, uint8_t *buffer, size_t count, uint64_t offset) {
	uint64_t unitSize = file->fs->unitSize;
	uint64_t startUnit = offset / unitSize;
	uint64_t endByte = offset + count;
	uint64_t endUnit = (endByte + unitSize - 1) / unitSize;
	uint64_t units = endUnit - startUnit;
	size_t bufSize = (size_t)(units * unitSize);
	size_t skip = (size_t)(offset - startUnit * unitSize);
	size_t toCopy;
	uint8_t *tmp;

	tmp = spdk_malloc(bufSize, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (tmp == NULL) {
		return -1;
	}

	file->readBuffer = tmp;
	file->ioOffset = startUnit;
	file->ioUnits = units;
	file->finished = false;

	if (!waiter(globalThread, hfsDoRead, file, &file->finished)) {
		spdk_free(tmp);
		return -1;
	}

	toCopy = (bufSize > skip && count > bufSize - skip) ? (bufSize - skip) : count;
	memcpy(buffer, tmp + skip, toCopy);

	spdk_free(tmp);
	file->readBuffer = NULL;
	return (ssize_t)toCopy;
}

// write at offset
ssize_t hfsWriteAt(HfsFile *file, const uint8_t *buffer, size_t count, uint64_t offset) {
	uint64_t unitSize = file->fs->unitSize;
	uint64_t startUnit = offset / unitSize;
	uint64_t endByte = offset + count;
	uint64_t endUnit = (endByte + unitSize - 1) / unitSize;
	uint64_t units = endUnit - startUnit;
	size_t bufSize = (size_t)(units * unitSize);
	size_t skip = (size_t)(offset - startUnit * unitSize);
	uint8_t *tmp;

	if (!hfsEnsureSize(file, endByte)) {
		return -1;
	}

	tmp = spdk_malloc(bufSize, 0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (tmp == NULL) {
		return -1;
	}

	file->readBuffer = tmp;
	file->writeBuffer = tmp;
	file->ioOffset = startUnit;
	file->ioUnits = units;
	file->finished = false;

	if (!waiter(globalThread, hfsDoRead, file, &file->finished)) {
		spdk_free(tmp);
		return -1;
	}

	memcpy(tmp + skip, buffer, count);

	file->finished = false;
	if (!waiter(globalThread, hfsDoWrite, file, &file->finished)) {
		spdk_free(tmp);
		return -1;
	}

	spdk_free(tmp);
	file->readBuffer = NULL;
	file->writeBuffer = NULL;
	return (ssize_t)count;
}

// read
int hfsRead(HfsFile *file, uint8_t *buffer, size_t count) {
	ssize_t n = hfsReadAt(file, buffer, count, file->offset);
	if (n > 0) {
		file->offset += (uint64_t)n;
	}
	return (int)n;
}

// write
int hfsWrite(HfsFile *file, const uint8_t *buffer, size_t count) {
	ssize_t n = hfsWriteAt(file, buffer, count, file->offset);
	if (n > 0) {
		file->offset += (uint64_t)n;
	}
	return (int)n;
}

// close
int hfsClose(HfsFile *file) {
	file->finished = false;

	return waiter(globalThread, hfsDoClose, file, &file->finished);
}