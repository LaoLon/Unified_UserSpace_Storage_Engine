#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include <spdk/bdev.h>
#include <spdk/blob.h>
#include <spdk/env.h>
#include <spdk/log.h>

#include "hfs_internal.h"
#include "hfs_inode_runtime.h"
#include "hfs_io.h"
#include "hfs_log.h"
#include "hfs_stats.h"
#include "hfs_trace.h"

/* Request state used by blob lifecycle and fsync operation chains. */
typedef struct HfsFileRequest {
	HfsFile *file;
	Hfs *fs;
	HfsRequestResult result;
	struct spdk_bs_dev_cb_args flushArgs;
	HfsRequestOperation flushOperation;
	uint64_t traceFlushStartNs;
	int primaryRc;
	bool createdBlob;
} HfsFileRequest;

/* Refresh the cached free capacity while running on the SPDK owner thread. */
static void hfsRefreshFreeClusterCount(Hfs *fs) {
	if (fs == NULL || fs->blobstore == NULL) {
		return;
	}
	atomic_store_explicit(&fs->freeClusterCount,
			      spdk_bs_free_cluster_count(fs->blobstore),
			      memory_order_relaxed);
}

/* Complete a regular data-blob delete request. */
static void hfsSpdkBlobDeleteCb(void *arg, int bserrno) {
	HfsFileRequest *request = arg;
	HfsFile *file = request->file;

	if (bserrno != 0) {
		HFS_LOG_ERROR("delete blob %"PRIu64" failed bserrno=%d",
			      file->blobId, bserrno);
	} else {
		hfsRefreshFreeClusterCount(file->fs);
	}
	hfsRequestComplete(&request->result, bserrno, 0);
}

/* Complete a close request and retire the local blob handle only on success. */
static void hfsSpdkBlobCloseCb(void *arg, int bserrno) {
	HfsFileRequest *request = arg;
	HfsFile *file = request->file;

	if (bserrno != 0) {
		HFS_LOG_ERROR("close blob %"PRIu64" failed bserrno=%d",
			      file->blobId, bserrno);
	} else {
		file->blob = NULL;
	}
	hfsRequestComplete(&request->result, bserrno, 0);
}

/* SPDK-thread task: close the data blob handle. */
static void hfsDoClose(void *arg) {
	HfsFileRequest *request = arg;

	if (hfsRequestCompletionInjected(request->result.operation)) {
		hfsSpdkBlobCloseCb(request, -EIO);
		return;
	}
	spdk_blob_close(request->file->blob, hfsSpdkBlobCloseCb, request);
}

/* SPDK-thread task: delete a data blob by its stable id. */
static void hfsDoDeleteBlob(void *arg) {
	HfsFileRequest *request = arg;
	HfsFile *file = request->file;

	if (hfsRequestCompletionInjected(request->result.operation)) {
		hfsSpdkBlobDeleteCb(request, -EIO);
		return;
	}
	spdk_bs_delete_blob(file->fs->blobstore, file->blobId,
			    hfsSpdkBlobDeleteCb, request);
}

/* Complete the device flush used by file fsync. */
static void hfsSpdkFsyncFlushCb(struct spdk_io_channel *channel, void *arg,
				 int bserrno) {
	HfsFileRequest *request = arg;

	if (bserrno != 0) {
		HFS_LOG_ERROR("bdev flush failed bserrno=%d", bserrno);
	}
	hfsRequestComplete(&request->result, bserrno, 0);
}

/* Complete a direct bdev flush and release the SPDK I/O object. */
static void hfsSpdkBdevFlushCb(struct spdk_bdev_io *bdevIo, bool success, void *arg) {
	HfsFileRequest *request = arg;

	if (!success) {
		HFS_LOG_ERROR("bdev flush failed");
	}
	spdk_bdev_free_io(bdevIo);
	hfsRequestComplete(&request->result, success ? 0 : -EIO, 0);
}

/* Submit a direct bdev flush, with a blobstore-device fallback for custom devices. */
static void hfsSubmitFsyncFlush(HfsFileRequest *request) {
	HfsFile *file = request->file;
	Hfs *fs = file != NULL ? file->fs : request->fs;
	int rc;

	/* Count both real and injected barriers so failure ratios remain visible. */
	hfsStatsRecordFlushFast();
	request->traceFlushStartNs = hfsTraceNowNs();
	request->result.completion.traceStageStartNs = request->traceFlushStartNs;
	hfsTraceEmitDetail(HFS_TRACE_FLUSH_BEGIN,
		HFS_TRACE_OPERATION_FLUSH, HFS_TRACE_BUFFER_NONE,
		request->result.requestId, request->result.completion.traceParentId,
		request->result.completion.traceInodeId, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, HFS_TRACE_META_OP_FLUSH, 0, 0);
	if (hfsRequestCompletionInjected(request->flushOperation)) {
		hfsRequestComplete(&request->result, -EIO, 0);
		return;
	}
	if (fs->bdevDesc != NULL && fs->bdevChannel != NULL) {
		rc = spdk_bdev_flush(fs->bdevDesc, fs->bdevChannel, 0, 0,
				      hfsSpdkBdevFlushCb, request);
		if (rc != 0) {
			hfsRequestComplete(&request->result, rc, 0);
		}
		return;
	}
	if (fs->bsDev == NULL || fs->bsDev->flush == NULL) {
		hfsRequestComplete(&request->result, -EOPNOTSUPP, 0);
		return;
	}
	memset(&request->flushArgs, 0, sizeof(request->flushArgs));
	request->flushArgs.cb_fn = hfsSpdkFsyncFlushCb;
	request->flushArgs.channel = fs->channel;
	request->flushArgs.cb_arg = request;
	fs->bsDev->flush(fs->bsDev, fs->channel,
				      &request->flushArgs);
}

/* Flush blob metadata before flushing the backing device. */
static void hfsSpdkFsyncBlobCb(void *arg, int bserrno) {
	HfsFileRequest *request = arg;

	if (bserrno != 0) {
		hfsRequestComplete(&request->result, bserrno, 0);
		return;
	}
	hfsSubmitFsyncFlush(request);
}

/* SPDK-thread task: start the file metadata and device flush chain. */
static void hfsDoSync(void *arg) {
	HfsFileRequest *request = arg;
	HfsFile *file = request->file;

	if (hfsRequestCompletionInjected(request->result.operation)) {
		hfsSpdkFsyncBlobCb(request, -EIO);
		return;
	}
	if (file == NULL || file->blob == NULL) {
		hfsSubmitFsyncFlush(request);
		return;
	}
	spdk_blob_sync_md(file->blob, hfsSpdkFsyncBlobCb, request);
}

/* Complete a blob-only metadata sync without starting the flush chain. */
static void hfsSpdkSyncDataCb(void *arg, int bserrno) {
	HfsFileRequest *request = arg;

	hfsRequestComplete(&request->result, bserrno, 0);
}

/* SPDK-thread task: synchronize blob metadata without a device barrier. */
static void hfsDoSyncData(void *arg) {
	HfsFileRequest *request = arg;
	HfsFile *file = request->file;

	if (hfsRequestCompletionInjected(request->result.operation)) {
		hfsRequestComplete(&request->result, -EIO, 0);
		return;
	}
	if (file == NULL || file->blob == NULL) {
		hfsRequestComplete(&request->result, 0, 0);
		return;
	}
	spdk_blob_sync_md(file->blob, hfsSpdkSyncDataCb, request);
}

/* Preserve the first open failure while the cleanup chain releases its blob. */
static void hfsOpenCleanupDeleteCb(void *arg, int bserrno) {
	HfsFileRequest *request = arg;

	if (bserrno != 0) {
		HFS_LOG_ERROR("open cleanup delete failed blob=%"PRIu64" bserrno=%d",
			      request->file->blobId, bserrno);
	} else {
		hfsRefreshFreeClusterCount(request->file->fs);
	}
	request->file->blob = NULL;
	hfsRequestComplete(&request->result, request->primaryRc, 0);
}

/* Close a just-opened blob before deleting it after a failed open chain. */
static void hfsOpenCleanupCloseCb(void *arg, int bserrno) {
	HfsFileRequest *request = arg;

	if (bserrno != 0) {
		HFS_LOG_ERROR("open cleanup close failed blob=%"PRIu64" bserrno=%d",
			      request->file->blobId, bserrno);
	}
	request->file->blob = NULL;
	spdk_bs_delete_blob(request->file->fs->blobstore, request->file->blobId,
			    hfsOpenCleanupDeleteCb, request);
}

/* Start cleanup without replacing the operation error observed by the caller. */
static void hfsOpenFail(HfsFileRequest *request, int bserrno) {
	request->primaryRc = bserrno != 0 ? bserrno : -EIO;
	if (request->file->blob != NULL) {
		spdk_blob_close(request->file->blob, hfsOpenCleanupCloseCb, request);
		return;
	}
	if (request->createdBlob) {
		spdk_bs_delete_blob(request->file->fs->blobstore,
				    request->file->blobId, hfsOpenCleanupDeleteCb, request);
		return;
	}
	hfsRequestComplete(&request->result, request->primaryRc, 0);
}

/* Complete the metadata sync for a newly created file blob. */
static void hfsSpdkBlobSyncCb(void *arg, int bserrno) {
	HfsFileRequest *request = arg;

	if (bserrno != 0) {
		HFS_LOG_ERROR("blob sync failed bserrno=%d", bserrno);
		hfsOpenFail(request, bserrno);
		return;
	}
	hfsRequestComplete(&request->result, 0, 0);
}

/* Open a created blob and continue the fresh-blob metadata sync chain. */
static void hfsSpdkBsOpenBlobCb(void *arg, struct spdk_blob *blob, int bserrno) {
	HfsFileRequest *request = arg;

	if (bserrno != 0 || blob == NULL) {
		HFS_LOG_ERROR("open created blob failed bserrno=%d", bserrno);
		hfsOpenFail(request, bserrno);
		return;
	}
	request->file->blob = blob;
	spdk_blob_sync_md(blob, hfsSpdkBlobSyncCb, request);
}

/* Capture a newly allocated blob id before opening it. */
static void hfsSpdkBsCreateBlobCb(void *arg, spdk_blob_id blobId, int bserrno) {
	HfsFileRequest *request = arg;

	if (bserrno != 0 || blobId == SPDK_BLOBID_INVALID) {
		HFS_LOG_ERROR("create blob failed bserrno=%d", bserrno);
		hfsRequestComplete(&request->result, bserrno != 0 ? bserrno : -EIO, 0);
		return;
	}
	request->file->blobId = blobId;
	request->createdBlob = true;
	spdk_bs_open_blob(request->file->fs->blobstore, blobId,
			  hfsSpdkBsOpenBlobCb, request);
}

/* SPDK-thread task: create a new blob and begin the fresh-file chain. */
static void hfsDoOpen(void *arg) {
	HfsFileRequest *request = arg;

	if (hfsRequestCompletionInjected(request->result.operation)) {
		hfsSpdkBsCreateBlobCb(request, SPDK_BLOBID_INVALID, -EIO);
		return;
	}
	spdk_bs_create_blob(request->file->fs->blobstore, hfsSpdkBsCreateBlobCb,
			    request);
}

/* Open a fresh file: create blob -> open -> sync, with cleanup on failure. */
int hfsOpen(HfsFile *file) {
	HfsFileRequest request;
	int rc;

	if (file == NULL || file->fs == NULL) {
		errno = EINVAL;
		return 0;
	}
	memset(&request, 0, sizeof(request));
	request.file = file;
	hfsRequestInit(&request.result, HFS_REQUEST_BLOB_CREATE);
	rc = hfsRequestWait(&request.result, hfsDoOpen, &request);
	if (rc == 0 && file->blob != NULL) {
		file->runtime = hfsInodeRuntimeForFile(file);
		if (file->runtime != NULL) {
			atomic_store_explicit(&file->runtime->allocatedClusters,
					      spdk_blob_get_num_clusters(file->blob),
					      memory_order_release);
		}
	}
	return rc == 0;
}

/* Complete an open-by-id request without publishing a failed blob handle. */
static void hfsSpdkBsOpenByIdCb(void *arg, struct spdk_blob *blob, int bserrno) {
	HfsFileRequest *request = arg;

	if (bserrno != 0 || blob == NULL) {
		HFS_LOG_ERROR("open blob %"PRIu64" failed bserrno=%d",
			      request->file->blobId, bserrno);
		hfsRequestComplete(&request->result, bserrno != 0 ? bserrno : -EIO, 0);
		return;
	}
	request->file->blob = blob;
	hfsRequestComplete(&request->result, 0, 0);
}

/* SPDK-thread task: open an existing blob by id. */
static void hfsDoOpenById(void *arg) {
	HfsFileRequest *request = arg;

	if (hfsRequestCompletionInjected(request->result.operation)) {
		hfsSpdkBsOpenByIdCb(request, NULL, -EIO);
		return;
	}
	spdk_bs_open_blob(request->file->fs->blobstore, request->file->blobId,
			  hfsSpdkBsOpenByIdCb, request);
}

/* Open an existing blob by id; used by the library open path. */
int hfsOpenById(HfsFile *file) {
	HfsFileRequest request;

	if (file == NULL || file->fs == NULL) {
		errno = EINVAL;
		return 0;
	}
	memset(&request, 0, sizeof(request));
	request.file = file;
	hfsRequestInit(&request.result, HFS_REQUEST_BLOB_OPEN);
	return hfsRequestWait(&request.result, hfsDoOpenById, &request) == 0;
}

/* Close the file handle only; the blob is kept for a later unlink. */
int hfsClose(HfsFile *file) {
	HfsFileRequest request;

	if (file == NULL || file->fs == NULL) {
		errno = EINVAL;
		return 0;
	}
	if (file->blob == NULL) {
		return 1;
	}
	memset(&request, 0, sizeof(request));
	request.file = file;
	hfsRequestInit(&request.result, HFS_REQUEST_BLOB_CLOSE);
	return hfsRequestWait(&request.result, hfsDoClose, &request) == 0;
}

/* Delete the data blob by id after its directory entry has been removed. */
int hfsDeleteBlobById(HfsFile *file) {
	HfsFileRequest request;

	if (file == NULL || file->fs == NULL) {
		errno = EINVAL;
		return 0;
	}
	memset(&request, 0, sizeof(request));
	request.file = file;
	hfsRequestInit(&request.result, HFS_REQUEST_BLOB_DELETE);
	return hfsRequestWait(&request.result, hfsDoDeleteBlob, &request) == 0;
}

/* Execute a blob sync followed by one named device-flush boundary. */
static int hfsSyncWithFlushOperation(HfsFile *file,
				     HfsRequestOperation flushOperation) {
	HfsFileRequest request;

	if (file == NULL || file->fs == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(&request, 0, sizeof(request));
	request.file = file;
	request.flushOperation = flushOperation;
	hfsRequestInit(&request.result, HFS_REQUEST_BLOB_SYNC);
	/* Attach the file identity so flush and completion wake events correlate. */
	request.result.completion.traceInodeId = file->inodeId;
	return hfsRequestWait(&request.result, hfsDoSync, &request);
}

/* Flush one open file and its backing bdev for the first fsync barrier. */
int hfsSync(HfsFile *file) {
	return hfsSyncWithFlushOperation(file, HFS_REQUEST_BDEV_FLUSH);
}

/* Synchronize one blob while leaving the shared device barrier to the batch. */
int hfsSyncData(HfsFile *file) {
	HfsFileRequest request;

	if (file == NULL || file->fs == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(&request, 0, sizeof(request));
	request.file = file;
	hfsRequestInit(&request.result, HFS_REQUEST_BLOB_SYNC);
	request.result.completion.traceInodeId = file->inodeId;
	return hfsRequestWait(&request.result, hfsDoSyncData, &request);
}

/* Issue one durable device barrier without requiring a file handle. */
int hfsDeviceFlush(Hfs *fs) {
	HfsFileRequest request;

	if (fs == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(&request, 0, sizeof(request));
	request.fs = fs;
	request.flushOperation = HFS_REQUEST_BDEV_FLUSH;
	hfsRequestInit(&request.result, HFS_REQUEST_BDEV_FLUSH);
	return hfsRequestWait(&request.result, hfsDoSync, &request);
}

/* Flush the device boundary that follows a completed metadata transaction. */
int hfsSyncPostCommit(HfsFile *file) {
	return hfsSyncWithFlushOperation(file,
					HFS_REQUEST_BDEV_FLUSH_POST_COMMIT);
}

/* Per-request state for a blob resize operation. */
typedef struct HfsResizeRequest {
	HfsFile *file;
	uint64_t clusters;
	HfsRequestResult result;
} HfsResizeRequest;

/* Complete one request-local blob resize. */
static void hfsSpdkBlobResizeDoneCb(void *arg, int bserrno) {
	HfsResizeRequest *request = arg;

	if (bserrno == 0) {
		hfsRefreshFreeClusterCount(request->file->fs);
	}
	hfsRequestComplete(&request->result, bserrno, request->clusters);
}

/* SPDK-thread task: grow the blob to the request's cluster count. */
static void hfsDoResize(void *arg) {
	HfsResizeRequest *request = arg;

	if (hfsRequestCompletionInjected(request->result.operation)) {
		hfsSpdkBlobResizeDoneCb(request, -EIO);
		return;
	}
	spdk_blob_resize(request->file->blob, request->clusters,
			 hfsSpdkBlobResizeDoneCb, request);
}

/* Resize one blob to a target cluster count, optionally preserving growth-only semantics. */
static int hfsResizeClusters(HfsFile *file, uint64_t needClusters,
				  bool growOnly) {
	HfsResizeRequest request;
	uint64_t currentClusters;

	currentClusters = spdk_blob_get_num_clusters(file->blob);
	if (currentClusters == needClusters ||
	    (growOnly && currentClusters >= needClusters)) {
		return 0;
	}

	memset(&request, 0, sizeof(request));
	request.file = file;
	request.clusters = needClusters;
	hfsRequestInit(&request.result, HFS_REQUEST_BLOB_RESIZE);
	if (hfsRequestWait(&request.result, hfsDoResize, &request) != 0) {
		HFS_LOG_ERROR("blob resize failed clusters=%"PRIu64
			      " submitRc=%d completionRc=%d bserrno=%d",
			      needClusters, request.result.submitRc,
			      request.result.completionRc, request.result.bserrno);
		return -1;
	}
	return 0;
}

/* Convert a byte length into a blob cluster count. */
static int hfsBytesToClusters(HfsFile *file, uint64_t bytes,
			      uint64_t *clustersOut) {
	uint64_t clusterSize;
	uint64_t needClusters;

	if (file == NULL || file->fs == NULL || file->blob == NULL ||
	    clustersOut == NULL) {
		errno = EINVAL;
		return -1;
	}
	clusterSize = spdk_bs_get_cluster_size(file->fs->blobstore);
	if (clusterSize == 0) {
		errno = EINVAL;
		return -1;
	}
	needClusters = bytes == 0 ? 0 : (bytes - 1) / clusterSize + 1;
	*clustersOut = needClusters;
	return 0;
}

/* Resize the blob so it covers at least 'bytes' (grow only). */
int hfsResizeTo(HfsFile *file, uint64_t bytes) {
	uint64_t needClusters;
	HfsInodeRuntime *runtime;
	int rc;

	if (hfsBytesToClusters(file, bytes, &needClusters) != 0) {
		return -1;
	}

	runtime = hfsInodeRuntimeForFile(file);
	if (runtime == NULL && file->inodeId != 0) {
		return -1;
	}
	if (runtime != NULL &&
	    atomic_load_explicit(&runtime->allocatedClusters,
				 memory_order_acquire) >= needClusters) {
		hfsStatsRecordResizeFast(true);
		return 0;
	}
	if (runtime == NULL) {
		errno = EIO;
		return -1;
	}
	pthread_mutex_lock(&runtime->resizeLock);
	rc = hfsResizeClusters(file, needClusters, true);
	hfsStatsRecordResizeFast(false);
	if (rc == 0 && runtime != NULL) {
		atomic_store_explicit(&runtime->allocatedClusters, needClusters,
				      memory_order_release);
	}
	pthread_mutex_unlock(&runtime->resizeLock);
	return rc;
}

/* Resize the blob exactly to the clusters required by a logical file size. */
int hfsResizeExact(HfsFile *file, uint64_t bytes) {
	uint64_t needClusters;
	HfsInodeRuntime *runtime;
	int rc;

	if (hfsBytesToClusters(file, bytes, &needClusters) != 0) {
		return -1;
	}

	runtime = hfsInodeRuntimeForFile(file);
	if (runtime == NULL && file->inodeId != 0) {
		return -1;
	}
	if (runtime == NULL) {
		errno = EIO;
		return -1;
	}
	pthread_mutex_lock(&runtime->resizeLock);
	rc = hfsResizeClusters(file, needClusters, false);
	if (rc == 0 && runtime != NULL) {
		atomic_store_explicit(&runtime->allocatedClusters, needClusters,
				      memory_order_release);
	}
	pthread_mutex_unlock(&runtime->resizeLock);
	return rc;
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
