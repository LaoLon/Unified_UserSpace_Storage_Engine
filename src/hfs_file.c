#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include <spdk/bdev.h>
#include <spdk/blob.h>
#include <spdk/env.h>
#include <spdk/log.h>

#include "hfs_internal.h"
#include "hfs_io.h"
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

/* Per-request state for a blob resize operation. */
typedef struct HfsResizeRequest {
	HfsFile *file;
	uint64_t clusters;
	int operationRc;
	HfsCompletion completion;
} HfsResizeRequest;

/* Complete one request-local blob resize. */
static void hfsSpdkBlobResizeDoneCb(void *arg, int bserrno) {
	HfsResizeRequest *request = arg;

	request->operationRc = bserrno;
	if (bserrno == 0) {
		request->file->fs->numCluster =
			spdk_blob_get_num_clusters(request->file->blob);
	}
	hfsRuntimeCompletionComplete(&request->completion);
}

/* SPDK-thread task: grow the blob to the request's cluster count. */
static void hfsDoResize(void *arg) {
	HfsResizeRequest *request = arg;

	spdk_blob_resize(request->file->blob, request->clusters,
			 hfsSpdkBlobResizeDoneCb, request);
}

/* Convert an SPDK result into a public errno and return failure. */
static int hfsResizeError(int rc) {
	errno = rc < 0 ? -rc : (rc == 0 ? EIO : rc);
	return -1;
}

/* Resize the blob so it covers at least 'bytes' (grow only). */
int hfsResizeTo(HfsFile *file, uint64_t bytes) {
	uint64_t clusterSize;
	uint64_t needClusters;
	HfsResizeRequest request;
	int rc;

	if (file == NULL || file->fs == NULL || file->blob == NULL) {
		errno = EINVAL;
		return -1;
	}
	clusterSize = spdk_bs_get_cluster_size(file->fs->blobstore);
	if (clusterSize == 0) {
		errno = EINVAL;
		return -1;
	}
	needClusters = bytes == 0 ? 0 : (bytes - 1) / clusterSize + 1;

	pthread_mutex_lock(&file->fs->resizeLock);
	if (spdk_blob_get_num_clusters(file->blob) >= needClusters) {
		pthread_mutex_unlock(&file->fs->resizeLock);
		return 0;
	}

	memset(&request, 0, sizeof(request));
	request.file = file;
	request.clusters = needClusters;
	request.operationRc = -EIO;
	if (!waiter(globalThread, hfsDoResize, &request, &request.completion)) {
		pthread_mutex_unlock(&file->fs->resizeLock);
		return -1;
	}
	rc = request.operationRc;
	pthread_mutex_unlock(&file->fs->resizeLock);
	if (rc != 0) {
		HFS_LOG_ERROR("blob resize failed clusters=%"PRIu64" bserrno=%d",
			      needClusters, rc);
		return hfsResizeError(rc);
	}
	return 0;
}

/* Read a byte range at an explicit offset through the request-local I/O path. */
ssize_t hfsReadAt(HfsFile *file, uint8_t *buffer, size_t count, uint64_t offset) {
	return hfsIoReadAt(file, buffer, count, offset);
}

/* Write a byte range at an explicit offset through the request-local I/O path. */
ssize_t hfsWriteAt(HfsFile *file, const uint8_t *buffer, size_t count, uint64_t offset) {
	return hfsIoWriteAt(file, buffer, count, offset);
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
