#include <errno.h>
#include <string.h>

#include <spdk/bdev.h>
#include <spdk/blob.h>
#include <spdk/env.h>
#include <spdk/log.h>

#include "hfs_internal.h"
#include "hfs_log.h"

/* Completion callback for spdk_bs_delete_blob. */
static void hfsSpdkBlobDeleteCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	if (bserrno != 0) {
		HFS_LOG_ERROR("delete blob %"PRIu64" failed bserrno=%d",
			      file->blobId, bserrno);
	}
	SPDK_NOTICELOG("blob deleted, id=%"PRIu64"\n", file->blobId);
	hfsRuntimeCompletionComplete(&file->completion);
}

/* Completion callback for spdk_blob_close. */
static void hfsSpdkBlobCloseCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	if (bserrno != 0) {
		HFS_LOG_ERROR("close blob %"PRIu64" failed bserrno=%d",
			      file->blobId, bserrno);
	}
	SPDK_NOTICELOG("close complete\n");
	hfsRuntimeCompletionComplete(&file->completion);
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

/* Complete the device flush used by file fsync. */
static void hfsSpdkFsyncFlushCb(struct spdk_io_channel *channel, void *arg,
					int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	file->operationRc = bserrno;
	if (bserrno != 0) {
		HFS_LOG_ERROR("bdev flush failed bserrno=%d", bserrno);
	}
	hfsRuntimeCompletionComplete(&file->completion);
}

/* Complete a direct bdev flush and release the SPDK I/O object. */
static void hfsSpdkBdevFlushCb(struct spdk_bdev_io *bdevIo, bool success, void *arg) {
	HfsFile *file = (HfsFile *)arg;

	file->operationRc = success ? 0 : -EIO;
	if (!success) {
		HFS_LOG_ERROR("bdev flush failed");
	}
	spdk_bdev_free_io(bdevIo);
	hfsRuntimeCompletionComplete(&file->completion);
}

/* Submit a direct bdev flush, with a blobstore-device fallback for custom devices. */
static void hfsSubmitFsyncFlush(HfsFile *file) {
	int rc;

	if (file->fs->bdevDesc != NULL && file->fs->bdevChannel != NULL) {
		rc = spdk_bdev_flush(file->fs->bdevDesc, file->fs->bdevChannel, 0, 0,
				      hfsSpdkBdevFlushCb, file);
		if (rc != 0) {
			file->operationRc = rc;
			hfsRuntimeCompletionComplete(&file->completion);
		}
		return;
	}
	if (file->fs->bsDev == NULL || file->fs->bsDev->flush == NULL) {
		file->operationRc = -EOPNOTSUPP;
		hfsRuntimeCompletionComplete(&file->completion);
		return;
	}
	memset(&file->flushArgs, 0, sizeof(file->flushArgs));
	file->flushArgs.cb_fn = hfsSpdkFsyncFlushCb;
	file->flushArgs.channel = file->fs->channel;
	file->flushArgs.cb_arg = file;
	file->fs->bsDev->flush(file->fs->bsDev, file->fs->channel,
				      &file->flushArgs);
}

/* Flush blob metadata before flushing the backing device. */
static void hfsSpdkFsyncBlobCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	if (bserrno != 0) {
		file->operationRc = bserrno;
		hfsRuntimeCompletionComplete(&file->completion);
		return;
	}
	hfsSubmitFsyncFlush(file);
}

/* SPDK-thread task: start the file metadata and device flush chain. */
static void hfsDoSync(void *arg) {
	HfsFile *file = (HfsFile *)arg;

	if (file->blob == NULL) {
		hfsSubmitFsyncFlush(file);
		return;
	}
	spdk_blob_sync_md(file->blob, hfsSpdkFsyncBlobCb, file);
}

/*
 * Close the file handle only; the blob is kept.
 * Blob deletion is the responsibility of unlink (hfsDeleteBlobById).
 */
int hfsClose(HfsFile *file) {
	return waiter(globalThread, hfsDoClose, file, &file->completion);
}

/* Delete the data blob by id (used by unlink). */
int hfsDeleteBlobById(HfsFile *file) {
	return waiter(globalThread, hfsDoDeleteBlob, file, &file->completion);
}

/* Flush one open file and its backing bdev for POSIX fsync semantics. */
int hfsSync(HfsFile *file) {
	if (file == NULL || file->fs == NULL) {
		errno = EINVAL;
		return -1;
	}
	file->operationRc = 0;
	if (!waiter(globalThread, hfsDoSync, file, &file->completion)) {
		errno = ETIMEDOUT;
		return -1;
	}
	if (file->operationRc != 0) {
		errno = -file->operationRc;
		return -1;
	}
	return 0;
}

/* Completion callback for spdk_blob_io_read. */
static void hfsSpdkBlobReadCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	if (bserrno != 0) {
		HFS_LOG_ERROR("blob read failed unit=%"PRIu64" n=%"PRIu64
			      " bserrno=%d", file->ioOffset, file->ioUnits, bserrno);
	}
	hfsRuntimeCompletionComplete(&file->completion);
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

	if (bserrno != 0) {
		HFS_LOG_ERROR("blob write failed unit=%"PRIu64" n=%"PRIu64
			      " bserrno=%d", file->ioOffset, file->ioUnits, bserrno);
	}
	hfsRuntimeCompletionComplete(&file->completion);
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

	if (bserrno != 0) {
		HFS_LOG_ERROR("blob sync failed bserrno=%d", bserrno);
	}
	SPDK_NOTICELOG("open complete\n");

	hfsRuntimeCompletionComplete(&file->completion);
}

/* Completion callback for spdk_blob_resize (open path): sync metadata. */
static void hfsSpdkBlobResizeCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	if (bserrno != 0) {
		HFS_LOG_ERROR("blob resize failed bserrno=%d", bserrno);
	}
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

	if (bserrno != 0) {
		HFS_LOG_ERROR("open blob failed bserrno=%d", bserrno);
		hfsRuntimeCompletionComplete(&file->completion);
		return;
	}
	file->blob = blb;

	uint64_t freeCluster = spdk_bs_free_cluster_count(file->fs->blobstore);
	SPDK_NOTICELOG("freeCluster : %"PRIu64"\n", freeCluster);
	file->fs->freeClusterCount = freeCluster;

	spdk_blob_resize(blb, freeCluster, hfsSpdkBlobResizeCb, file);
}

/* Completion callback for spdk_bs_create_blob (open path). */
static void hfsSpdkBsCreateBlobCb(void *arg, spdk_blob_id blobId, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	if (bserrno != 0) {
		HFS_LOG_ERROR("create blob failed bserrno=%d", bserrno);
		hfsRuntimeCompletionComplete(&file->completion);
		return;
	}
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
	return waiter(globalThread, hfsDoOpen, file, &file->completion);
}

/* Completion callback for spdk_bs_open_blob (open-by-id path). */
static void hfsSpdkBsOpenByIdCb(void *arg, struct spdk_blob *blb, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	if (bserrno != 0) {
		HFS_LOG_ERROR("open blob %"PRIu64" failed bserrno=%d",
			      file->blobId, bserrno);
	}
	file->blob = blb;
	hfsRuntimeCompletionComplete(&file->completion);
}

/* SPDK-thread task: open an existing blob by id (no create). */
static void hfsDoOpenById(void *arg) {
	HfsFile *file = (HfsFile *)arg;

	spdk_bs_open_blob(file->fs->blobstore, file->blobId, hfsSpdkBsOpenByIdCb, file);
}

/* Open an existing blob by id; used by the library open path. */
int hfsOpenById(HfsFile *file) {
	return waiter(globalThread, hfsDoOpenById, file, &file->completion);
}

/* Completion callback for spdk_blob_resize (write path). */
static void hfsSpdkBlobResizeDoneCb(void *arg, int bserrno) {
	HfsFile *file = (HfsFile *)arg;

	if (bserrno != 0) {
		HFS_LOG_ERROR("blob resize (write path) failed bserrno=%d", bserrno);
	}
	hfsRuntimeCompletionComplete(&file->completion);
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
	return waiter(globalThread, hfsDoResize, file, &file->completion);
}

/* Resize the blob so it covers at least 'bytes' (grow only). */
int hfsResizeTo(HfsFile *file, uint64_t bytes) {
	uint64_t clusterSize = spdk_bs_get_cluster_size(file->fs->blobstore);
	uint64_t needClusters = (bytes + clusterSize - 1) / clusterSize;

	if (spdk_blob_get_num_clusters(file->blob) >= needClusters) {
		return 0;
	}

	file->resizeClusters = needClusters;
	if (!waiter(globalThread, hfsDoResize, file, &file->completion)) {
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
	if (!waiter(globalThread, hfsDoRead, file, &file->completion)) {
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
	if (!waiter(globalThread, hfsDoRead, file, &file->completion)) {
		spdk_free(tmp);
		return -1;
	}

	/* Splice the user data into the buffer, then write it back. */
	memcpy(tmp + skip, buffer, count);

	if (!waiter(globalThread, hfsDoWrite, file, &file->completion)) {
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
