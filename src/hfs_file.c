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

	SPDK_NOTICELOG("READ BUFFER : %s\n", file->readBuffer);

	file->finished = true;
}

static void hfsDoRead(void *arg) {
	HfsFile *file = (HfsFile *)arg;

	memset(file->readBuffer, 0x0, BUFFER_SIZE);

	spdk_blob_io_read(file->blob, file->fs->channel, file->readBuffer,
		0, 1, hfsSpdkBlobReadCb, file);
}

static void hfsSpdkBlobWriteCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;
	SPDK_NOTICELOG("write complete\n");

	file->finished = true;
}

static void hfsDoWrite(void *arg) {
	HfsFile *file = (HfsFile *)arg;

	spdk_blob_io_write(file->blob, file->fs->channel, file->writeBuffer,
		0, 1, hfsSpdkBlobWriteCb, file);
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

// read
int hfsRead(HfsFile *file, uint8_t *buffer, size_t count) {
	file->finished = false;
	waiter(globalThread, hfsDoRead, file, &file->finished);

	int len = strlen(file->readBuffer);
	memcpy(buffer, file->readBuffer, len);

	return len;
}

// write
int hfsWrite(HfsFile *file, const uint8_t *buffer, size_t count) {
	file->finished = false;

	memcpy(file->writeBuffer, buffer, count);

	return waiter(globalThread, hfsDoWrite, file, &file->finished);
}

// close
int hfsClose(HfsFile *file) {
	file->finished = false;

	return waiter(globalThread, hfsDoClose, file, &file->finished);
}