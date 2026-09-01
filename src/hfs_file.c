#include <string.h>

#include <spdk/blob.h>
#include <spdk/env.h>
#include <spdk/log.h>

#include "hfs_internal.h"

/* Completion callback for spdk_bs_delete_blob. */
static void hfsSpdkBlobDeleteCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	SPDK_NOTICELOG("blob deleted, id=%"PRIu64"\n", file->blobId);
	file->finished = true;
}

/* Completion callback for spdk_blob_close. */
static void hfsSpdkBlobCloseCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	SPDK_NOTICELOG("close complete\n");
	file->finished = true;
}

/* SPDK-thread task: close the data blob (handle only). */
static void hfsDoClose(void *arg) {
	HfsFile *file = (HfsFile *)arg;

	spdk_blob_close(file->blob, hfsSpdkBlobCloseCb, file);
}

/* Completion callback for spdk_bs_delete_blob (delete path). */
static void hfsSpdkBsDeleteDoneCb(void *arg, int bserrno) {
	hfsSpdkBlobDeleteCb(arg, bserrno);
}

/* SPDK-thread task: delete the data blob by id (unlink path). */
static void hfsDoDeleteBlob(void *arg) {
	HfsFile *file = (HfsFile *)arg;

	spdk_bs_delete_blob(file->fs->blobstore, file->blobId, hfsSpdkBsDeleteDoneCb, file);
}

/*
 * Close the file handle only; the blob is kept.
 * Blob deletion is the responsibility of unlink (hfsDeleteBlobById).
 */
int hfsClose(HfsFile *file) {
	file->finished = false;

	return waiter(globalThread, hfsDoClose, file, &file->finished);
}

/* Delete the data blob by id (used by unlink). */
int hfsDeleteBlobById(HfsFile *file) {
	file->finished = false;

	return waiter(globalThread, hfsDoDeleteBlob, file, &file->finished);
}

/* Completion callback for spdk_blob_io_read. */
static void hfsSpdkBlobReadCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	file->finished = true;
}

/* SPDK-thread task: read ioUnits starting at ioOffset into readBuffer. */
static void hfsDoRead(void *arg) {
	HfsFile *file = (HfsFile *)arg;

	spdk_blob_io_read(file->blob, file->fs->channel, file->readBuffer,
		file->ioOffset, file->ioUnits, hfsSpdkBlobReadCb, file);
}

/* Completion callback for spdk_blob_io_write. */
static void hfsSpdkBlobWriteCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	file->finished = true;
}

/* SPDK-thread task: write ioUnits at ioOffset from writeBuffer. */
static void hfsDoWrite(void *arg) {
	HfsFile *file = (HfsFile *)arg;

	spdk_blob_io_write(file->blob, file->fs->channel, file->writeBuffer,
		file->ioOffset, file->ioUnits, hfsSpdkBlobWriteCb, file);
}

/* Completion callback for spdk_blob_sync_md (open path). */
static void hfsSpdkBlobSyncCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	SPDK_NOTICELOG("open complete\n");

	file->finished = true;
}

/* Completion callback for spdk_blob_resize (open path): sync metadata. */
static void hfsSpdkBlobResizeCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	uint64_t total = spdk_blob_get_num_clusters(file->blob);
	file->fs->numCluster = total;

	SPDK_NOTICELOG("resize blob :%"PRIu64"\n", total);

	spdk_blob_sync_md(file->blob, hfsSpdkBlobSyncCb, file);
}

/*
 * Completion callback for spdk_bs_open_blob (open path):
 * grow the blob to all free clusters, then sync metadata.
 */
static void hfsSpdkBsOpenBlobCb(void *arg, struct spdk_blob *blb, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	file->blob = blb;

	uint64_t freeCluster = spdk_bs_free_cluster_count(file->fs->blobstore);
	SPDK_NOTICELOG("freeCluster : %"PRIu64"\n", freeCluster);
	file->fs->freeClusterCount = freeCluster;

	spdk_blob_resize(blb, freeCluster, hfsSpdkBlobResizeCb, file);
}

/* Completion callback for spdk_bs_create_blob (open path). */
static void hfsSpdkBsCreateBlobCb(void *arg, spdk_blob_id blobId, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	file->blobId = blobId;
	SPDK_NOTICELOG("blobId : %"PRIu64"\n", blobId);

	spdk_bs_open_blob(file->fs->blobstore, blobId, hfsSpdkBsOpenBlobCb, file);
}

/* SPDK-thread task: create a new blob (open creates a fresh file). */
static void hfsDoOpen(void *arg) {
	HfsFile *file = (HfsFile *)arg;

	spdk_bs_create_blob(file->fs->blobstore, hfsSpdkBsCreateBlobCb, file);
}

/* Open a fresh file: create blob -> open -> grow -> sync. */
int hfsOpen(HfsFile *file) {
	file->finished = false;

	return waiter(globalThread, hfsDoOpen, file, &file->finished);
}

/* Completion callback for spdk_bs_open_blob (open-by-id path). */
static void hfsSpdkBsOpenByIdCb(void *arg, struct spdk_blob *blb, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	file->blob = blb;
	file->finished = true;
}

/* SPDK-thread task: open an existing blob by id (no create). */
static void hfsDoOpenById(void *arg) {
	HfsFile *file = (HfsFile *)arg;

	spdk_bs_open_blob(file->fs->blobstore, file->blobId, hfsSpdkBsOpenByIdCb, file);
}

/* Open an existing blob by id; used by the library open path. */
int hfsOpenById(HfsFile *file) {
	file->finished = false;

	return waiter(globalThread, hfsDoOpenById, file, &file->finished);
}

/* Completion callback for spdk_blob_resize (write path). */
static void hfsSpdkBlobResizeDoneCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	file->finished = true;
}

/* SPDK-thread task: grow the blob to cover 'bytes'. */
static void hfsDoResize(void *arg) {
	HfsFile *file = (HfsFile *)arg;

	spdk_blob_resize(file->blob, file->resizeClusters, hfsSpdkBlobResizeDoneCb, file);
}

/* Grow the blob if needed so that offset+bytes fits; returns success. */
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

/* Resize the blob so it covers at least 'bytes' (grow only). */
int hfsResizeTo(HfsFile *file, uint64_t bytes) {
	uint64_t clusterSize = spdk_bs_get_cluster_size(file->fs->blobstore);
	uint64_t needClusters = (bytes + clusterSize - 1) / clusterSize;

	if (spdk_blob_get_num_clusters(file->blob) >= needClusters) {
		return 0;
	}

	file->resizeClusters = needClusters;
	file->finished = false;
	if (!waiter(globalThread, hfsDoResize, file, &file->finished)) {
		return -1;
	}
	return 0;
}

/*
 * Read 'count' bytes at an explicit byte offset.
 * Reads whole io units into a DMA buffer, then copies the requested
 * window back to the caller.
 */
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

/*
 * Write 'count' bytes at an explicit byte offset.
 * Uses read-modify-write on the covering io units so that
 * unaligned writes do not clobber surrounding data. Grows the blob
 * first if the write extends beyond the current size.
 */
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

	/* Read the covering units first (modify-in-place below). */
	file->readBuffer = tmp;
	file->writeBuffer = tmp;
	file->ioOffset = startUnit;
	file->ioUnits = units;
	file->finished = false;

	if (!waiter(globalThread, hfsDoRead, file, &file->finished)) {
		spdk_free(tmp);
		return -1;
	}

	/* Splice the user data into the buffer, then write it back. */
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

/* Sequential read at file->offset; advances the offset. */
int hfsRead(HfsFile *file, uint8_t *buffer, size_t count) {
	ssize_t n = hfsReadAt(file, buffer, count, file->offset);
	if (n > 0) {
		file->offset += (uint64_t)n;
	}
	return (int)n;
}

/* Sequential write at file->offset; advances the offset. */
int hfsWrite(HfsFile *file, const uint8_t *buffer, size_t count) {
	ssize_t n = hfsWriteAt(file, buffer, count, file->offset);
	if (n > 0) {
		file->offset += (uint64_t)n;
	}
	return (int)n;
}