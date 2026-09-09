#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <spdk/blob.h>
#include <spdk/env.h>
#include <spdk/log.h>

#include "hfs_internal.h"
#include "hfs_log.h"
#include "hfs_meta.h"
#include "hfs_meta_index.h"
#include "hfs_stats.h"

/* Versioned metadata header used to keep inode IDs stable across compaction. */
#define HFS_META_MAGIC		0x32465348U
#define HFS_META_VERSION	4U
#define HFS_META_VERSION_V3	3U
#define HFS_META_INTENT_MAGIC	0x544e4948U
#define HFS_META_INTENT_VERSION	2U
#define HFS_META_INTENT_VERSION_V1	1U
#define HFS_META_INTENT_ACTIVE	1U
#define HFS_META_INTENT_MAX_SLOTS	4U

/* Fixed on-disk header for the committed metadata image. */
typedef struct HfsMetaHeaderDisk {
	uint32_t magic;
	uint32_t version;
	uint64_t generation;
	uint64_t nextInodeId;
	uint32_t count;
	uint32_t checksum;
} HfsMetaHeaderDisk;

/* Inode layout used by the shard-30 metadata image. */
typedef struct HfsMetaInodeV3Disk {
	uint64_t id;
	uint8_t type;
	uint64_t parentId;
	uint64_t blobId;
	uint64_t size;
	uint64_t mtimeNs;
	uint64_t ctimeNs;
	char name[FILENAME_LENGTH];
} HfsMetaInodeV3Disk;

/* One after-image entry in the fixed-size metadata intent record. */
typedef struct HfsMetaIntentSlotDisk {
	uint32_t index;
	uint32_t reserved;
	struct hfs_inode inode;
} HfsMetaIntentSlotDisk;

/* Fixed-size intent record for namespace and inode-size transactions. */
typedef struct HfsMetaIntentDisk {
	uint32_t magic;
	uint32_t version;
	uint32_t state;
	uint32_t slotCount;
	uint64_t baseGeneration;
	uint64_t generation;
	uint64_t nextInodeId;
	uint32_t baseCount;
	uint32_t count;
	uint32_t checksum;
	uint32_t reserved;
	HfsMetaIntentSlotDisk slots[HFS_META_INTENT_MAX_SLOTS];
} HfsMetaIntentDisk;

/* Version-one intent layout kept for crash recovery of shard-30 images. */
typedef struct HfsMetaIntentSlotV1Disk {
	uint32_t index;
	uint32_t reserved;
	HfsMetaInodeV3Disk inode;
} HfsMetaIntentSlotV1Disk;

typedef struct HfsMetaIntentV1Disk {
	uint32_t magic;
	uint32_t version;
	uint32_t state;
	uint32_t slotCount;
	uint64_t baseGeneration;
	uint64_t generation;
	uint64_t nextInodeId;
	uint32_t baseCount;
	uint32_t count;
	uint32_t checksum;
	uint32_t reserved;
	HfsMetaIntentSlotV1Disk slots[HFS_META_INTENT_MAX_SLOTS];
} HfsMetaIntentV1Disk;

#define HFS_META_HEADER_SIZE	(sizeof(HfsMetaHeaderDisk))
#define HFS_META_INTENT_SIZE	(sizeof(HfsMetaIntentDisk))
#define HFS_META_INTENT_V1_SIZE	(sizeof(HfsMetaIntentV1Disk))
#define HFS_META_INODE_OFFSET	(HFS_META_HEADER_SIZE + HFS_META_INTENT_SIZE)
#define HFS_META_INODE_OFFSET_V3	(HFS_META_HEADER_SIZE + HFS_META_INTENT_V1_SIZE)

_Static_assert(sizeof(HfsMetaHeaderDisk) == 32,
	       "metadata header layout must remain fixed");

/* Continue an FNV-1a checksum over one byte range. */
static uint32_t hfsMetaChecksumUpdate(uint32_t checksum, const void *data,
					      size_t len) {
	const uint8_t *bytes = (const uint8_t *)data;
	size_t i;

	for (i = 0; i < len; i++) {
		checksum ^= bytes[i];
		checksum *= 16777619U;
	}
	return checksum;
}

/* Compute the checksum of a metadata image with the checksum field cleared. */
static uint32_t hfsMetaImageChecksum(const HfsMetaHeaderDisk *header,
					     const struct hfs_inode *inodes,
					     uint32_t count) {
	HfsMetaHeaderDisk copy = *header;
	uint32_t checksum = 2166136261U;

	copy.checksum = 0;
	checksum = hfsMetaChecksumUpdate(checksum, &copy, sizeof(copy));
	return hfsMetaChecksumUpdate(checksum, inodes,
					     (size_t)count * sizeof(*inodes));
}

/* Compute the checksum of a shard-30 metadata image. */
static uint32_t hfsMetaImageChecksumV3(const HfsMetaHeaderDisk *header,
					       const HfsMetaInodeV3Disk *inodes,
					       uint32_t count) {
	HfsMetaHeaderDisk copy = *header;
	uint32_t checksum = 2166136261U;

	copy.checksum = 0;
	checksum = hfsMetaChecksumUpdate(checksum, &copy, sizeof(copy));
	return hfsMetaChecksumUpdate(checksum, inodes,
					     (size_t)count * sizeof(*inodes));
}

/* Compute the checksum of a fixed-size intent record. */
static uint32_t hfsMetaIntentChecksum(const HfsMetaIntentDisk *intent) {
	HfsMetaIntentDisk copy = *intent;

	copy.checksum = 0;
	return hfsMetaChecksumUpdate(2166136261U, &copy, sizeof(copy));
}

/* Compute the checksum of a version-one metadata intent record. */
static uint32_t hfsMetaIntentChecksumV1(const HfsMetaIntentV1Disk *intent) {
	HfsMetaIntentV1Disk copy = *intent;

	copy.checksum = 0;
	return hfsMetaChecksumUpdate(2166136261U, &copy, sizeof(copy));
}

/* Stop at a requested persistence boundary to simulate an abrupt crash. */
static void hfsMetaCrashPoint(int point) {
	const char *step = getenv("HFS_META_CRASH_STEP");

	if (step != NULL && atoi(step) == point) {
		_exit(200 + point);
	}
}

/* Return the current realtime clock as nanoseconds since the epoch. */
static uint64_t hfsMetaNowNs(void) {
	struct timespec ts;

	if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
		return 0;
	}
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Initialize the minimal POSIX attributes for a newly created inode. */
static void hfsMetaInitAttributes(struct hfs_inode *inode, uint8_t type,
					  uint64_t nowNs) {
	inode->mode = type == HFS_INODE_DIR ? 0755U : 0644U;
	inode->uid = (uint32_t)getuid();
	inode->gid = (uint32_t)getgid();
	inode->atimeNs = nowNs;
	inode->mtimeNs = nowNs;
	inode->ctimeNs = nowNs;
}

/* Convert the pre-shard-32 inode layout and supply compatible defaults. */
static void hfsMetaConvertV3Inode(struct hfs_inode *dst,
					  const HfsMetaInodeV3Disk *src) {
	uint64_t nowNs = src->ctimeNs != 0 ? src->ctimeNs : hfsMetaNowNs();

	memset(dst, 0, sizeof(*dst));
	dst->id = src->id;
	dst->type = src->type;
	dst->parentId = src->parentId;
	dst->blobId = src->blobId;
	dst->size = src->size;
	dst->atimeNs = src->mtimeNs != 0 ? src->mtimeNs : nowNs;
	dst->mtimeNs = src->mtimeNs != 0 ? src->mtimeNs : nowNs;
	dst->ctimeNs = src->ctimeNs != 0 ? src->ctimeNs : nowNs;
	dst->mode = src->type == HFS_INODE_DIR ? 0755U : 0644U;
	dst->uid = (uint32_t)getuid();
	dst->gid = (uint32_t)getgid();
	memcpy(dst->name, src->name, sizeof(dst->name));
}

/* Return true only when the caller explicitly permits metadata formatting. */
static bool hfs_format_requested(void) {
	const char *format = getenv("HFS_FORMAT");

	return format != NULL && strcmp(format, "1") == 0;
}

/* Per-operation metadata I/O state; never stored in the mounted filesystem. */
typedef struct HfsMetaIoRequest {
	Hfs *fs;
	struct spdk_blob *blob;
	bool resident;
	void *buffer;
	uint64_t offset;
	uint64_t units;
	uint64_t clusters;
	HfsRequestResult result;
} HfsMetaIoRequest;

/* Owner-thread state used to reclaim blobs no longer named by metadata. */
typedef struct HfsMetaReclaimRequest {
	Hfs *fs;
	spdk_blob_id *blobIds;
	uint32_t count;
	uint32_t capacity;
	uint32_t deleteIndex;
	HfsRequestResult result;
} HfsMetaReclaimRequest;

/* Return whether a blob is still reachable from the committed inode table. */
static bool hfsMetaBlobReferenced(const Hfs *fs, spdk_blob_id blobId) {
	uint32_t i;

	if (fs == NULL || blobId == SPDK_BLOBID_INVALID) {
		return true;
	}
	if (blobId == fs->metaBlobId) {
		return true;
	}
	for (i = 0; i < fs->inodeCount; i++) {
		if (fs->inodes[i].type == HFS_INODE_FILE &&
		    fs->inodes[i].blobId == blobId) {
			return true;
		}
	}
	return false;
}

/* Append one unreferenced blob id while iterating on the owner thread. */
static int hfsMetaRememberUnreachableBlob(HfsMetaReclaimRequest *request,
					  spdk_blob_id blobId) {
	spdk_blob_id *ids;
	uint32_t capacity;

	if (request->count == request->capacity) {
		if (request->capacity > UINT32_MAX / 2U) {
			errno = ENOMEM;
			return -1;
		}
		capacity = request->capacity == 0 ? 16U : request->capacity * 2U;
		ids = realloc(request->blobIds, (size_t)capacity * sizeof(*ids));
		if (ids == NULL) {
			return -1;
		}
		request->blobIds = ids;
		request->capacity = capacity;
	}
	request->blobIds[request->count++] = blobId;
	return 0;
}

/* Submit the next unreachable blob deletion after enumeration is complete. */
static void hfsMetaReclaimDeleteNext(HfsMetaReclaimRequest *request);

/* Complete one unreachable blob deletion or continue the reclaim chain. */
static void hfsMetaReclaimDeleteCb(void *arg, int bserrno) {
	HfsMetaReclaimRequest *request = arg;

	if (bserrno != 0) {
		HFS_LOG_ERROR("mount reclaim delete failed blob=%"PRIu64,
			      request->blobIds[request->deleteIndex]);
		hfsRequestComplete(&request->result, bserrno, request->deleteIndex);
		return;
	}
	request->deleteIndex++;
	hfsMetaReclaimDeleteNext(request);
}

/* Delete all collected blobs and refresh free capacity only after success. */
static void hfsMetaReclaimDeleteNext(HfsMetaReclaimRequest *request) {
	if (request->deleteIndex == request->count) {
		atomic_store_explicit(&request->fs->freeClusterCount,
			spdk_bs_free_cluster_count(request->fs->blobstore),
			memory_order_relaxed);
		hfsRequestComplete(&request->result, 0, request->count);
		return;
	}
	spdk_bs_delete_blob(request->fs->blobstore,
			    request->blobIds[request->deleteIndex],
			    hfsMetaReclaimDeleteCb, request);
}

/* Collect unreachable blob ids before deleting so the iterator stays valid. */
static void hfsMetaReclaimIterCb(void *arg, struct spdk_blob *blob,
				 int bserrno) {
	HfsMetaReclaimRequest *request = arg;
	spdk_blob_id blobId;

	if (bserrno == -ENOENT) {
		hfsMetaReclaimDeleteNext(request);
		return;
	}
	if (bserrno != 0 || blob == NULL) {
		hfsRequestComplete(&request->result,
			bserrno != 0 ? bserrno : -EIO, request->count);
		return;
	}
	blobId = spdk_blob_get_id(blob);
	if (!hfsMetaBlobReferenced(request->fs, blobId) &&
	    hfsMetaRememberUnreachableBlob(request, blobId) != 0) {
		hfsRequestComplete(&request->result, -errno, request->count);
		return;
	}
	spdk_bs_iter_next(request->fs->blobstore, blob, hfsMetaReclaimIterCb,
			 request);
}

/* Start the blobstore enumeration from the SPDK owner thread. */
static void hfsMetaDoReclaimUnreachable(void *arg) {
	HfsMetaReclaimRequest *request = arg;

	if (hfsRequestCompletionInjected(request->result.operation)) {
		hfsRequestComplete(&request->result, -EIO, 0);
		return;
	}
	spdk_bs_iter_first(request->fs->blobstore, hfsMetaReclaimIterCb, request);
}

/* Reclaim blobs orphaned by an earlier committed namespace transaction. */
static int hfsMetaReclaimUnreachable(Hfs *fs) {
	HfsMetaReclaimRequest request;
	int rc;

	memset(&request, 0, sizeof(request));
	request.fs = fs;
	hfsRequestInit(&request.result, HFS_REQUEST_BLOB_DELETE);
	rc = hfsRequestWait(&request.result, hfsMetaDoReclaimUnreachable, &request);
	free(request.blobIds);
	return rc;
}

/* Complete one metadata I/O callback using its request-private result. */
static void hfsMetaOpCb(void *arg, int bserrno) {
	HfsMetaIoRequest *request = arg;

	hfsRequestComplete(&request->result, bserrno, request->units);
}

/* Store a successfully opened metadata blob before waking the caller. */
static void hfsMetaOpenBlobCb(void *arg, struct spdk_blob *blob, int bserrno) {
	HfsMetaIoRequest *request = arg;

	if (bserrno != 0 || blob == NULL) {
		hfsRequestComplete(&request->result,
				   bserrno != 0 ? bserrno : -EIO, 0);
		return;
	}
	request->blob = blob;
	hfsRequestComplete(&request->result, 0, 0);
}

/* SPDK-thread task: open the metadata blob. */
static void hfsMetaDoOpenBlob(void *arg) {
	HfsMetaIoRequest *request = arg;

	if (hfsRequestCompletionInjected(request->result.operation)) {
		hfsMetaOpenBlobCb(request, NULL, -EIO);
		return;
	}
	spdk_bs_open_blob(request->fs->blobstore, request->fs->metaBlobId,
			  hfsMetaOpenBlobCb, request);
}

/* SPDK-thread task: resize the metadata blob to the needed cluster count. */
static void hfsMetaDoResize(void *arg) {
	HfsMetaIoRequest *request = arg;

	if (hfsRequestCompletionInjected(request->result.operation)) {
		hfsMetaOpCb(request, -EIO);
		return;
	}
	spdk_blob_resize(request->blob, request->clusters, hfsMetaOpCb, request);
}

/* SPDK-thread task: write the serialized metadata into the blob. */
static void hfsMetaDoWrite(void *arg) {
	HfsMetaIoRequest *request = arg;

	if (hfsRequestCompletionInjected(request->result.operation)) {
		hfsMetaOpCb(request, -EIO);
		return;
	}
	spdk_blob_io_write(request->blob, request->fs->channel, request->buffer,
		request->offset / request->fs->unitSize, request->units,
		hfsMetaOpCb, request);
}

/* SPDK-thread task: read the metadata blob into the DMA buffer. */
static void hfsMetaDoRead(void *arg) {
	HfsMetaIoRequest *request = arg;

	if (hfsRequestCompletionInjected(request->result.operation)) {
		hfsMetaOpCb(request, -EIO);
		return;
	}
	spdk_blob_io_read(request->blob, request->fs->channel, request->buffer,
		request->offset / request->fs->unitSize, request->units,
		hfsMetaOpCb, request);
}

/* SPDK-thread task: flush blob metadata to the backing device. */
static void hfsMetaDoSync(void *arg) {
	HfsMetaIoRequest *request = arg;

	if (hfsRequestCompletionInjected(request->result.operation)) {
		hfsMetaOpCb(request, -EIO);
		return;
	}
	spdk_blob_sync_md(request->blob, hfsMetaOpCb, request);
}

/* Close one metadata blob and retire its request-local handle on success. */
static void hfsMetaCloseBlobCb(void *arg, int bserrno) {
	HfsMetaIoRequest *request = arg;

	if (bserrno == 0) {
		request->blob = NULL;
	}
	hfsRequestComplete(&request->result, bserrno, 0);
}

/* SPDK-thread task: close the metadata blob. */
static void hfsMetaDoClose(void *arg) {
	HfsMetaIoRequest *request = arg;

	if (hfsRequestCompletionInjected(request->result.operation)) {
		hfsMetaCloseBlobCb(request, -EIO);
		return;
	}
	spdk_blob_close(request->blob, hfsMetaCloseBlobCb, request);
}

/* Run one metadata I/O stage and preserve the exact SPDK completion status. */
static int hfsMetaWait(HfsMetaIoRequest *request, HfsRequestOperation operation,
			       spdk_msg_fn startFn) {
	hfsRequestInit(&request->result, operation);
	return hfsRequestWait(&request->result, startFn, request);
}

/* Reuse the mount-lifetime metadata handle, with a temporary fallback for setup. */
static int hfsMetaAcquireBlob(HfsMetaIoRequest *request) {
	if (request == NULL || request->fs == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (request->fs->metaBlob != NULL) {
		request->blob = request->fs->metaBlob;
		request->resident = true;
		return 0;
	}
	return hfsMetaWait(request, HFS_REQUEST_METADATA_OPEN,
			   hfsMetaDoOpenBlob);
}

/* Close a partially completed metadata request without replacing its failure. */
static void hfsMetaCloseBestEffort(HfsMetaIoRequest *request) {
	int savedErrno = errno;

	if (request->blob != NULL && !request->resident) {
		(void)hfsMetaWait(request, HFS_REQUEST_BLOB_CLOSE, hfsMetaDoClose);
	}
	errno = savedErrno;
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
	HfsMetaIoRequest request;
	void *tmp;
	int rc = -1;

	tmp = spdk_malloc(needUnits * ioUnitSize, 0x1000, NULL,
		SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (tmp == NULL) {
		errno = ENOMEM;
		return -1;
	}
	memset(tmp, 0, needUnits * ioUnitSize);
	memcpy(tmp, buf, len);

	memset(&request, 0, sizeof(request));
	request.fs = fs;
	request.buffer = tmp;
	request.units = needUnits;
	request.clusters = needClusters;

	if (hfsMetaAcquireBlob(&request) != 0) goto out;
	if (hfsMetaWait(&request, HFS_REQUEST_BLOB_RESIZE,
			hfsMetaDoResize) != 0) goto out;
	if (hfsMetaWait(&request, HFS_REQUEST_METADATA_WRITE,
			hfsMetaDoWrite) != 0) goto out;
	if (hfsMetaWait(&request, HFS_REQUEST_BLOB_SYNC,
			hfsMetaDoSync) != 0) goto out;
	hfsStatsRecordMetadataSyncFast();
	if (!request.resident &&
	    hfsMetaWait(&request, HFS_REQUEST_BLOB_CLOSE,
			hfsMetaDoClose) != 0) goto out;

	rc = 0;
	/* The v4 image is currently the physical page write; log bytes are tracked separately. */
	hfsStatsRecordMetadataWriteFast(0, len, needUnits * ioUnitSize);
out:
	if (rc != 0) {
		hfsMetaCloseBestEffort(&request);
	}
	spdk_free(tmp);
	return rc;
}

/* Flush a resident metadata handle after a group of range writes. */
static int hfsMetaBlobSync(Hfs *fs) {
	HfsMetaIoRequest request;

	if (fs == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(&request, 0, sizeof(request));
	request.fs = fs;
	if (hfsMetaAcquireBlob(&request) != 0) {
		return -1;
	}
	if (hfsMetaWait(&request, HFS_REQUEST_BLOB_SYNC,
			 hfsMetaDoSync) != 0) {
		hfsMetaCloseBestEffort(&request);
		return -1;
	}
	hfsStatsRecordMetadataSyncFast();
	if (!request.resident &&
		hfsMetaWait(&request, HFS_REQUEST_BLOB_CLOSE,
			     hfsMetaDoClose) != 0) {
		return -1;
	}
	return 0;
}

/* Update a byte range with edge read-modify-write and one optional sync. */
static int hfsMetaBlobWriteRange(Hfs *fs, const void *buf, size_t len,
				 uint64_t offset, bool sync) {
	uint64_t ioUnitSize;
	uint64_t end;
	uint64_t alignedOffset;
	uint64_t alignedEnd;
	uint64_t alignedLen;
	uint64_t needClusters;
	uint64_t clusterSize;
	uint64_t currentClusters;
	HfsMetaIoRequest request;
	uint8_t *tmp;
	size_t copyOffset;
	int rc;

	if (fs == NULL || buf == NULL || len == 0 || fs->unitSize == 0 ||
		offset > UINT64_MAX - len) {
		errno = EINVAL;
		return -1;
	}
	ioUnitSize = fs->unitSize;
	end = offset + len;
	if (end > UINT64_MAX - (ioUnitSize - 1)) {
		errno = EOVERFLOW;
		return -1;
	}
	alignedOffset = offset - offset % ioUnitSize;
	alignedEnd = (end + ioUnitSize - 1) / ioUnitSize * ioUnitSize;
	if (alignedEnd < end || alignedEnd - alignedOffset > SIZE_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	alignedLen = alignedEnd - alignedOffset;
	clusterSize = spdk_bs_get_cluster_size(fs->blobstore);
	if (clusterSize == 0) {
		errno = EIO;
		return -1;
	}
	if (alignedEnd > UINT64_MAX - (clusterSize - 1)) {
		errno = EOVERFLOW;
		return -1;
	}
	needClusters = (alignedEnd + clusterSize - 1) / clusterSize;
	if (needClusters == 0) {
		errno = EOVERFLOW;
		return -1;
	}
	tmp = spdk_malloc((size_t)alignedLen, 0x1000, NULL,
			  SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (tmp == NULL) {
		errno = ENOMEM;
		return -1;
	}
	memset(tmp, 0, (size_t)alignedLen);
	memset(&request, 0, sizeof(request));
	request.fs = fs;
	request.buffer = tmp;
	request.offset = alignedOffset;
	request.units = alignedLen / ioUnitSize;
	currentClusters = fs->metaBlob != NULL
		? spdk_blob_get_num_clusters(fs->metaBlob) : 0;
	if (currentClusters < needClusters) {
		request.clusters = needClusters;
		if (hfsMetaAcquireBlob(&request) != 0 ||
			hfsMetaWait(&request, HFS_REQUEST_BLOB_RESIZE,
				    hfsMetaDoResize) != 0) {
			hfsMetaCloseBestEffort(&request);
			spdk_free(tmp);
			return -1;
		}
	} else if (hfsMetaAcquireBlob(&request) != 0) {
		spdk_free(tmp);
		return -1;
	}
	if (alignedOffset != offset || alignedEnd != end) {
		if (hfsMetaWait(&request, HFS_REQUEST_METADATA_READ,
				 hfsMetaDoRead) != 0) {
			hfsMetaCloseBestEffort(&request);
			spdk_free(tmp);
			return -1;
		}
	}
	copyOffset = (size_t)(offset - alignedOffset);
	memcpy(tmp + copyOffset, buf, len);
	rc = hfsMetaWait(&request, HFS_REQUEST_METADATA_WRITE,
			 hfsMetaDoWrite);
	if (rc == 0 && sync) {
		rc = hfsMetaWait(&request, HFS_REQUEST_BLOB_SYNC,
				 hfsMetaDoSync);
		if (rc == 0) {
			hfsStatsRecordMetadataSyncFast();
		}
	}
	if (!request.resident &&
		hfsMetaWait(&request, HFS_REQUEST_BLOB_CLOSE,
			     hfsMetaDoClose) != 0 && rc == 0) {
		rc = -1;
	}
	if (rc == 0) {
		hfsStatsRecordMetadataWriteFast(len, 0, alignedLen);
	}
	spdk_free(tmp);
	return rc;
}

/* Read a byte range with aligned DMA I/O and no metadata image allocation. */
static int hfsMetaBlobReadRange(Hfs *fs, void *buf, size_t len,
				uint64_t offset) {
	uint64_t ioUnitSize;
	uint64_t end;
	uint64_t alignedOffset;
	uint64_t alignedEnd;
	uint64_t alignedLen;
	uint64_t clusterSize;
	uint64_t clusters;
	HfsMetaIoRequest request;
	uint8_t *tmp;
	size_t copyOffset;
	int rc = -1;

	if (fs == NULL || buf == NULL || len == 0 || fs->unitSize == 0 ||
		offset > UINT64_MAX - len) {
		errno = EINVAL;
		return -1;
	}
	ioUnitSize = fs->unitSize;
	end = offset + len;
	if (end > UINT64_MAX - (ioUnitSize - 1)) {
		errno = EOVERFLOW;
		return -1;
	}
	alignedOffset = offset - offset % ioUnitSize;
	alignedEnd = (end + ioUnitSize - 1) / ioUnitSize * ioUnitSize;
	if (alignedEnd < end || alignedEnd - alignedOffset > SIZE_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	alignedLen = alignedEnd - alignedOffset;
	clusterSize = spdk_bs_get_cluster_size(fs->blobstore);
	if (clusterSize == 0) {
		errno = EIO;
		return -1;
	}
	memset(&request, 0, sizeof(request));
	request.fs = fs;
	if (hfsMetaAcquireBlob(&request) != 0) {
		return -1;
	}
	clusters = spdk_blob_get_num_clusters(request.blob);
	if (clusters == 0 || clusters > UINT64_MAX / clusterSize ||
		alignedEnd > clusters * clusterSize) {
		errno = EIO;
		hfsMetaCloseBestEffort(&request);
		return -1;
	}
	tmp = spdk_malloc((size_t)alignedLen, 0x1000, NULL,
			  SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (tmp == NULL) {
		errno = ENOMEM;
		hfsMetaCloseBestEffort(&request);
		return -1;
	}
	request.buffer = tmp;
	request.offset = alignedOffset;
	request.units = alignedLen / ioUnitSize;
	if (hfsMetaWait(&request, HFS_REQUEST_METADATA_READ,
			hfsMetaDoRead) == 0) {
		copyOffset = (size_t)(offset - alignedOffset);
		memcpy(buf, tmp + copyOffset, len);
		rc = 0;
	}
	if (!request.resident &&
		hfsMetaWait(&request, HFS_REQUEST_BLOB_CLOSE,
			     hfsMetaDoClose) != 0 && rc == 0) {
		rc = -1;
	}
	if (rc != 0) {
		hfsMetaCloseBestEffort(&request);
	}
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
	HfsMetaIoRequest request;
	void *tmp;
	int rc = -1;

	if (outBuf == NULL || outLen == NULL) {
		errno = EINVAL;
		return -1;
	}
	*outBuf = NULL;
	*outLen = 0;
	memset(&request, 0, sizeof(request));
	request.fs = fs;
	if (hfsMetaAcquireBlob(&request) != 0) {
		return -1;
	}

	/* Derive the read size from the current blob size. */
	clusters = spdk_blob_get_num_clusters(request.blob);
	units = clusters * clusterSize / ioUnitSize;
	if (units == 0) {
		errno = EIO;
		hfsMetaCloseBestEffort(&request);
		return -1;
	}

	tmp = spdk_malloc(units * ioUnitSize, 0x1000, NULL,
		SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (tmp == NULL) {
		errno = ENOMEM;
		hfsMetaCloseBestEffort(&request);
		return -1;
	}

	request.buffer = tmp;
	request.units = units;
	if (hfsMetaWait(&request, HFS_REQUEST_METADATA_READ,
			hfsMetaDoRead) != 0) goto out;
	if (!request.resident &&
	    hfsMetaWait(&request, HFS_REQUEST_BLOB_CLOSE,
			hfsMetaDoClose) != 0) goto out;

	*outBuf = tmp;
	*outLen = units * ioUnitSize;
	return 0;
out:
	hfsMetaCloseBestEffort(&request);
	spdk_free(tmp);
	return rc;
}

/* Build a stable metadata image for one generation. */
static int hfsMetaBuildStableImage(Hfs *fs, uint64_t generation,
					   uint8_t **outBuf, size_t *outLen) {
	HfsMetaHeaderDisk header;
	size_t inodeBytes;
	size_t len;
	uint8_t *buf;

	if (fs->nextInodeId == 0 || generation == 0 ||
	    fs->inodeCount > HFS_MAX_INODES) {
		errno = EIO;
		return -1;
	}
	inodeBytes = (size_t)fs->inodeCount * sizeof(struct hfs_inode);
	if (inodeBytes > SIZE_MAX - HFS_META_INODE_OFFSET) {
		errno = EOVERFLOW;
		return -1;
	}
	len = HFS_META_INODE_OFFSET + inodeBytes;
	buf = calloc(1, len);
	if (buf == NULL) {
		return -1;
	}
	memset(&header, 0, sizeof(header));
	header.magic = HFS_META_MAGIC;
	header.version = HFS_META_VERSION;
	header.generation = generation;
	header.nextInodeId = fs->nextInodeId;
	header.count = fs->inodeCount;
	header.checksum = hfsMetaImageChecksum(&header, fs->inodes,
						fs->inodeCount);
	memcpy(buf, &header, sizeof(header));
	memcpy(buf + HFS_META_INODE_OFFSET, fs->inodes, inodeBytes);
	*outBuf = buf;
	*outLen = len;
	return 0;
}

/* Reject another mutation once a failed transaction must be recovered by remount. */
static int hfsMetaEnsureWritable(const Hfs *fs) {
	if (fs == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (fs->metaRecoveryRequired) {
		errno = EIO;
		return -1;
	}
	return 0;
}

/* Return true when one inode id is already waiting for a checkpoint. */
static bool hfsMetaDirtyContains(const Hfs *fs, uint64_t inodeId) {
	uint32_t i;

	if (fs == NULL || fs->metaDirtyInodeIds == NULL) {
		return false;
	}
	for (i = 0; i < fs->metaDirtyInodeCount; i++) {
		if (fs->metaDirtyInodeIds[i] == inodeId) {
			return true;
		}
	}
	return false;
}

/* Expose dirty-set membership needed by fdatasync's data-only decision. */
bool hfsMetaInodeDirty(const Hfs *fs, uint64_t inodeId) {
	return hfsMetaDirtyContains(fs, inodeId);
}

/* Clear the in-memory dirty set after a stable metadata epoch is durable. */
static void hfsMetaDirtyClear(Hfs *fs) {
	if (fs == NULL) {
		return;
	}
	fs->metaDirtyInodeCount = 0;
	fs->metaDirty = false;
}

/* Mark one data inode for the next metadata group commit. */
void hfsMetaMarkInodeDirty(Hfs *fs, uint64_t inodeId) {
	uint64_t *ids;

	if (fs == NULL || inodeId == 0) {
		return;
	}
	fs->metaDirty = true;
	if (hfsMetaDirtyContains(fs, inodeId)) {
		return;
	}
	if (fs->metaDirtyInodeCount >= HFS_MAX_INODES) {
		return;
	}
	if (fs->metaDirtyInodeIds == NULL) {
		fs->metaDirtyInodeIds = calloc(HFS_MAX_INODES,
					       sizeof(*fs->metaDirtyInodeIds));
		if (fs->metaDirtyInodeIds == NULL) {
			return;
		}
	}
	ids = fs->metaDirtyInodeIds;
	ids[fs->metaDirtyInodeCount++] = inodeId;
}

/* Append one inode table index exactly once to a bounded v4 transaction. */
static bool hfsMetaAppendCommitIndex(uint32_t *indices, uint32_t *count,
					     uint32_t index) {
	uint32_t i;

	for (i = 0; i < *count; i++) {
		if (indices[i] == index) {
			return true;
		}
	}
	if (*count >= HFS_META_INTENT_MAX_SLOTS) {
		return false;
	}
	indices[(*count)++] = index;
	return true;
}

/* Merge explicit namespace changes with dirty data inodes for group commit. */
static bool hfsMetaCollectCommitIndices(Hfs *fs, const uint32_t *indices,
					uint32_t indexCount,
					uint32_t *selected,
					uint32_t *selectedCount) {
	uint32_t i;

	*selectedCount = 0;
	for (i = 0; i < indexCount; i++) {
		if (indices[i] >= fs->inodeCount ||
		    !hfsMetaAppendCommitIndex(selected, selectedCount, indices[i])) {
			return false;
		}
	}
	for (i = 0; i < fs->metaDirtyInodeCount; i++) {
		struct hfs_inode *inode = hfsMetaFindById(fs,
						 fs->metaDirtyInodeIds[i]);
		uint32_t index;

		if (inode == NULL) {
			continue;
		}
		index = (uint32_t)(inode - fs->inodes);
		if (!hfsMetaAppendCommitIndex(selected, selectedCount, index)) {
			return false;
		}
	}
	return true;
}

/* Build the fixed-size intent after-image for a metadata transaction. */
static void hfsMetaBuildIntent(Hfs *fs, uint64_t baseGeneration,
				       uint64_t generation, uint32_t baseCount,
				       const uint32_t *indices, uint32_t indexCount,
				       HfsMetaIntentDisk *intent) {
	uint32_t i;

	memset(intent, 0, sizeof(*intent));
	intent->magic = HFS_META_INTENT_MAGIC;
	intent->version = HFS_META_INTENT_VERSION;
	intent->state = HFS_META_INTENT_ACTIVE;
	intent->baseGeneration = baseGeneration;
	intent->generation = generation;
	intent->nextInodeId = fs->nextInodeId;
	intent->baseCount = baseCount;
	intent->count = fs->inodeCount;
	for (i = 0; i < indexCount && intent->slotCount < HFS_META_INTENT_MAX_SLOTS;
	     i++) {
		uint32_t index = indices[i];

		if (index >= fs->inodeCount) {
			continue;
		}
		intent->slots[intent->slotCount].index = index;
		intent->slots[intent->slotCount].inode = fs->inodes[index];
		intent->slotCount++;
	}
	intent->checksum = hfsMetaIntentChecksum(intent);
}

/*
 * Commit a metadata image with intent-before-data ordering.
 *
 * A data-write checkpoint is different from a namespace mutation: the write
 * already returned success, so a failure before the intent becomes durable
 * must retain its dirty in-memory inode for a later fsync retry.
 */
static int hfsMetaCommit(Hfs *fs, const uint32_t *indices,
				 uint32_t indexCount, bool retainRuntimeMutation) {
	HfsMetaHeaderDisk oldHeader;
	HfsMetaHeaderDisk newHeader;
	HfsMetaIntentDisk intent;
	uint32_t selectedIndices[HFS_META_INTENT_MAX_SLOTS];
	uint32_t selectedCount;
	uint32_t fallbackBatch;
	uint64_t generation;
	int rc = -1;
	int savedErrno;
	uint32_t i;
	bool intentPublished = false;

	if (hfsMetaEnsureWritable(fs) != 0) {
		return -1;
	}
	if (indexCount > HFS_META_INTENT_MAX_SLOTS) {
		errno = E2BIG;
		return -1;
	}
	/* Allocation failure while recording a dirty id must not drop that write. */
	if (fs->metaDirty && fs->metaDirtyInodeCount == 0) {
		if (hfsMetaSaveAll(fs) == 0) {
			return 0;
		}
		fs->metaRecoveryRequired = true;
		return -1;
	}
	/*
	 * The v4 intent has four after-image slots. If concurrent dirty writes
	 * exceed that bound, use the established full-image path rather than
	 * publishing a header checksum for inode ranges that are not protected by
	 * the intent record.
	 */
	if (!hfsMetaCollectCommitIndices(fs, indices, indexCount,
					 selectedIndices, &selectedCount)) {
		fallbackBatch = fs->metaDirtyInodeCount + indexCount;
		if (hfsMetaSaveAll(fs) == 0) {
			hfsStatsRecordMetadataTransactionFast(fallbackBatch != 0 ?
							      fallbackBatch : 1);
			return 0;
		}
		fs->metaRecoveryRequired = true;
		return -1;
	}
	indices = selectedIndices;
	indexCount = selectedCount;
	if (fs->metaNeedsMigration && hfsMetaSaveAll(fs) != 0) {
		fs->metaRecoveryRequired = true;
		return -1;
	}
	if (fs->metaGeneration == UINT64_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	generation = fs->metaGeneration == 0 ? 1 : fs->metaGeneration + 1;

	if (hfsMetaBlobReadRange(fs, &oldHeader, sizeof(oldHeader), 0) != 0) {
		fs->metaRecoveryRequired = true;
		goto out;
	}
	if (oldHeader.magic != HFS_META_MAGIC ||
	    oldHeader.version != HFS_META_VERSION) {
		errno = EINVAL;
		goto out;
	}
	memset(&newHeader, 0, sizeof(newHeader));
	newHeader.magic = HFS_META_MAGIC;
	newHeader.version = HFS_META_VERSION;
	newHeader.generation = generation;
	newHeader.nextInodeId = fs->nextInodeId;
	newHeader.count = fs->inodeCount;
	newHeader.checksum = hfsMetaImageChecksum(&newHeader, fs->inodes,
						 fs->inodeCount);
	hfsMetaBuildIntent(fs, oldHeader.generation, generation,
				   oldHeader.count, indices, indexCount, &intent);
	if (hfsMetaBlobWriteRange(fs, &intent, sizeof(intent),
					 HFS_META_HEADER_SIZE, true) != 0) {
		goto out;
	}
	intentPublished = true;
	hfsMetaCrashPoint(1);

	if (hfsMetaBlobWriteRange(fs, &newHeader, sizeof(newHeader), 0, false) != 0) {
		goto out;
	}
	for (i = 0; i < indexCount; i++) {
		uint32_t index = indices[i];

		if (index >= fs->inodeCount) {
			continue;
		}
		if (hfsMetaBlobWriteRange(fs, &fs->inodes[index],
					 sizeof(fs->inodes[index]),
					 HFS_META_INODE_OFFSET +
					 (uint64_t)index * sizeof(fs->inodes[index]), false) != 0) {
			goto out;
		}
	}
	if (hfsMetaBlobSync(fs) != 0) {
		goto out;
	}
	hfsMetaCrashPoint(2);

	memset(&intent, 0, sizeof(intent));
	hfsMetaCrashPoint(3);
	if (hfsMetaBlobWriteRange(fs, &intent, sizeof(intent),
					 HFS_META_HEADER_SIZE, true) != 0) {
		goto out;
	}
	fs->metaGeneration = generation;
	fs->metaDurableEpoch = generation;
	fs->metaNeedsMigration = false;
	hfsMetaDirtyClear(fs);
	hfsStatsRecordMetadataTransactionFast(indexCount == 0 ? 1 : indexCount);
	if (!hfsMetaIndexReady(fs) && hfsMetaIndexRebuild(fs) != 0) {
		HFS_LOG_ERROR("metadata index rebuild deferred: %s", strerror(errno));
	}
	rc = 0;
out:
	savedErrno = errno;
	if (rc != 0) {
		/*
		 * A published intent can replay after a restart, so it must fail
		 * closed. Before publication, namespace mutations roll memory back;
		 * a dirty data checkpoint instead remains visible and retryable.
		 */
		if (retainRuntimeMutation && !intentPublished) {
			fs->metaDirty = true;
		} else if (intentPublished) {
			fs->metaRecoveryRequired = true;
		} else if (hfsMetaLoadAll(fs) != 0) {
			fs->metaRecoveryRequired = true;
		}
		errno = savedErrno;
	}
	return rc;
}

/* Serialize a stable versioned image and clear any intent record. */
int hfsMetaSaveAll(Hfs *fs) {
	uint8_t *buf;
	size_t len;
	uint64_t generation = fs->metaGeneration == 0 ? 1 : fs->metaGeneration;
	int rc;

	if (hfsMetaEnsureWritable(fs) != 0) {
		return -1;
	}
	if (hfsMetaBuildStableImage(fs, generation, &buf, &len) != 0) {
		return -1;
	}
	rc = hfsMetaBlobWrite(fs, buf, len);
	free(buf);
	if (rc == 0) {
		fs->metaGeneration = generation;
		fs->metaDurableEpoch = generation;
		fs->metaNeedsMigration = false;
		hfsMetaDirtyClear(fs);
	}
	return rc;
}

/* Validate one fixed-size intent record before applying its after-images. */
static bool hfsMetaIntentValid(const HfsMetaIntentDisk *intent) {
	uint32_t i;

	if (intent->magic != HFS_META_INTENT_MAGIC ||
	    intent->version != HFS_META_INTENT_VERSION ||
	    intent->state != HFS_META_INTENT_ACTIVE ||
	    intent->slotCount > HFS_META_INTENT_MAX_SLOTS ||
	    intent->baseCount > HFS_MAX_INODES || intent->count > HFS_MAX_INODES ||
	    intent->nextInodeId == 0 ||
	    intent->generation <= intent->baseGeneration ||
	    intent->checksum != hfsMetaIntentChecksum(intent)) {
		return false;
	}
	for (i = 0; i < intent->slotCount; i++) {
		if (intent->slots[i].index >= intent->count) {
			return false;
		}
	}
	return true;
}

/* Validate a shard-30 intent before applying its legacy after-images. */
static bool hfsMetaIntentV1Valid(const HfsMetaIntentV1Disk *intent) {
	uint32_t i;

	if (intent->magic != HFS_META_INTENT_MAGIC ||
	    intent->version != HFS_META_INTENT_VERSION_V1 ||
	    intent->state != HFS_META_INTENT_ACTIVE ||
	    intent->slotCount > HFS_META_INTENT_MAX_SLOTS ||
	    intent->baseCount > HFS_MAX_INODES || intent->count > HFS_MAX_INODES ||
	    intent->nextInodeId == 0 ||
	    intent->generation <= intent->baseGeneration ||
	    intent->checksum != hfsMetaIntentChecksumV1(intent)) {
		return false;
	}
	for (i = 0; i < intent->slotCount; i++) {
		if (intent->slots[i].index >= intent->count) {
			return false;
		}
	}
	return true;
}

/* Rebuild the in-memory inode table and replay a valid intent if present. */
int hfsMetaLoadAll(Hfs *fs) {
	const size_t legacyHeaderSize = sizeof(uint32_t);
	const size_t oldHeaderSize = sizeof(uint32_t) * 2 + sizeof(uint64_t);
	HfsMetaHeaderDisk header;
	HfsMetaIntentDisk intent;
	HfsMetaIntentV1Disk intentV1;
	uint8_t *buf;
	size_t len;
	size_t inodeOffset;
	size_t inodeSize;
	uint32_t first;
	uint32_t version = 0;
	uint32_t count;
	uint64_t nextInodeId = 0;
	uint64_t maxInodeId = 0;
	uint32_t i;
	bool currentFormat = false;
	bool v3Format = false;

	if (hfsMetaBlobRead(fs, (void **)&buf, &len) != 0) {
		return -1;
	}
	if (len < legacyHeaderSize) {
		spdk_free(buf);
		errno = EIO;
		return -1;
	}
	memcpy(&first, buf, sizeof(first));
	inodeOffset = legacyHeaderSize;
	if (first == HFS_META_MAGIC) {
		if (len < sizeof(uint32_t) * 2) {
			spdk_free(buf);
			errno = EIO;
			return -1;
		}
		memcpy(&version, buf + sizeof(first), sizeof(version));
		if (version == HFS_META_VERSION) {
			if (len < HFS_META_INODE_OFFSET) {
				spdk_free(buf);
				errno = EIO;
				return -1;
			}
			memcpy(&header, buf, sizeof(header));
			count = header.count;
			inodeOffset = HFS_META_INODE_OFFSET;
			currentFormat = true;
		} else if (version == HFS_META_VERSION_V3) {
			if (len < HFS_META_INODE_OFFSET_V3) {
				spdk_free(buf);
				errno = EIO;
				return -1;
			}
			memcpy(&header, buf, sizeof(header));
			count = header.count;
			inodeOffset = HFS_META_INODE_OFFSET_V3;
			v3Format = true;
		} else if (version == 2U) {
			if (len < oldHeaderSize) {
				spdk_free(buf);
				errno = EIO;
				return -1;
			}
			memcpy(&nextInodeId, buf + sizeof(first) + sizeof(version),
			       sizeof(nextInodeId));
			memcpy(&count, buf + oldHeaderSize - sizeof(count),
			       sizeof(count));
			inodeOffset = oldHeaderSize;
		} else {
			spdk_free(buf);
			errno = EINVAL;
			return -1;
		}
	} else {
		count = first;
	}
	inodeSize = currentFormat ? sizeof(struct hfs_inode) :
				    sizeof(HfsMetaInodeV3Disk);
	if (count > HFS_MAX_INODES || inodeOffset > len ||
	    (size_t)count > (len - inodeOffset) / inodeSize) {
		spdk_free(buf);
		errno = EIO;
		return -1;
	}
	if (currentFormat &&
	    (header.generation == 0 ||
	     header.checksum != hfsMetaImageChecksum(&header,
						(const struct hfs_inode *)(buf + inodeOffset), count))) {
		spdk_free(buf);
		errno = EIO;
		return -1;
	}
	if (v3Format &&
	    (header.generation == 0 ||
	     header.checksum != hfsMetaImageChecksumV3(&header,
					(const HfsMetaInodeV3Disk *)(buf + inodeOffset), count))) {
		spdk_free(buf);
		errno = EIO;
		return -1;
	}

	if (currentFormat) {
		memcpy(fs->inodes, buf + inodeOffset,
		       (size_t)count * sizeof(struct hfs_inode));
	} else {
		for (i = 0; i < count; i++) {
			const HfsMetaInodeV3Disk *oldInode =
				(const HfsMetaInodeV3Disk *)(buf + inodeOffset) + i;

			hfsMetaConvertV3Inode(&fs->inodes[i], oldInode);
		}
	}
	fs->inodeCount = count;
	for (i = 0; i < count; i++) {
		if (fs->inodes[i].id > maxInodeId) {
			maxInodeId = fs->inodes[i].id;
		}
	}
	if (currentFormat) {
		nextInodeId = header.nextInodeId;
		fs->metaGeneration = header.generation;
		fs->metaNeedsMigration = false;
	} else if (nextInodeId == 0) {
		nextInodeId = maxInodeId == UINT64_MAX ? 0 : maxInodeId + 1;
		fs->metaGeneration = 1;
		fs->metaNeedsMigration = true;
	} else {
		fs->metaGeneration = 1;
		fs->metaNeedsMigration = true;
	}
	if (nextInodeId == 0 || nextInodeId <= maxInodeId) {
		nextInodeId = maxInodeId == UINT64_MAX ? 0 : maxInodeId + 1;
		fs->metaNeedsMigration = true;
	}
	fs->nextInodeId = nextInodeId;
	fs->metaDurableEpoch = fs->metaGeneration;
	hfsMetaDirtyClear(fs);

	if (currentFormat && len >= HFS_META_INODE_OFFSET + sizeof(intent)) {
		bool matchesPendingImage;
		bool matchesCommittedImage;

		memcpy(&intent, buf + HFS_META_HEADER_SIZE, sizeof(intent));
		matchesPendingImage = intent.baseGeneration == header.generation &&
				      intent.baseCount == header.count;
		matchesCommittedImage = intent.generation == header.generation &&
					intent.count == header.count;
		if (hfsMetaIntentValid(&intent) &&
		    (matchesPendingImage || matchesCommittedImage)) {
			for (i = 0; i < intent.slotCount; i++) {
				fs->inodes[intent.slots[i].index] =
					intent.slots[i].inode;
			}
			fs->inodeCount = intent.count;
			fs->nextInodeId = intent.nextInodeId;
			fs->metaGeneration = intent.generation;
			spdk_free(buf);
			if (hfsMetaIndexRebuild(fs) != 0) {
				return -1;
			}
			return hfsMetaSaveAll(fs);
		}
	}
	if (v3Format && len >= HFS_META_INODE_OFFSET_V3 + sizeof(intentV1)) {
		bool matchesPendingImage;
		bool matchesCommittedImage;

		memcpy(&intentV1, buf + HFS_META_HEADER_SIZE, sizeof(intentV1));
		matchesPendingImage = intentV1.baseGeneration == header.generation &&
					      intentV1.baseCount == header.count;
		matchesCommittedImage = intentV1.generation == header.generation &&
						intentV1.count == header.count;
		if (hfsMetaIntentV1Valid(&intentV1) &&
		    (matchesPendingImage || matchesCommittedImage)) {
			for (i = 0; i < intentV1.slotCount; i++) {
				HfsMetaInodeV3Disk *oldInode = &intentV1.slots[i].inode;

				hfsMetaConvertV3Inode(
					&fs->inodes[intentV1.slots[i].index], oldInode);
			}
			fs->inodeCount = intentV1.count;
			fs->nextInodeId = intentV1.nextInodeId;
			fs->metaGeneration = intentV1.generation;
			spdk_free(buf);
			if (hfsMetaIndexRebuild(fs) != 0) {
				return -1;
			}
			return hfsMetaSaveAll(fs);
		}
	}
	spdk_free(buf);
	if (fs->nextInodeId == 0) {
		errno = ENOSPC;
		return -1;
	}
	return hfsMetaIndexRebuild(fs);
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
	hfsMetaIndexMarkDirty(fs);
	return inode;
}

/* Linear fallback for a dentry lookup while an index is being rebuilt. */
static struct hfs_inode *hfsMetaFindByNameLinear(Hfs *fs, uint64_t parentId,
							const char *name) {
	uint32_t i;

	for (i = 0; i < fs->inodeCount; i++) {
		if (fs->inodes[i].parentId == parentId &&
		    strcmp(fs->inodes[i].name, name) == 0) {
			return &fs->inodes[i];
		}
	}
	return NULL;
}

/* Look up an inode by (parentId, name), using the dentry index when valid. */
struct hfs_inode *hfsMetaFindByName(Hfs *fs, uint64_t parentId, const char *name) {
	struct hfs_inode *inode;

	inode = hfsMetaIndexFindByName(fs, parentId, name);
	if (inode != NULL || hfsMetaIndexReady(fs)) {
		return inode;
	}
	return hfsMetaFindByNameLinear(fs, parentId, name);
}

/* Linear fallback for an inode-id lookup while an index is being rebuilt. */
static struct hfs_inode *hfsMetaFindByIdLinear(Hfs *fs, uint64_t id) {
	uint32_t i;

	for (i = 0; i < fs->inodeCount; i++) {
		if (fs->inodes[i].id == id) {
			return &fs->inodes[i];
		}
	}
	return NULL;
}

/* Look up an inode by id, using the id index when valid. */
struct hfs_inode *hfsMetaFindById(Hfs *fs, uint64_t id) {
	struct hfs_inode *inode;

	inode = hfsMetaIndexFindById(fs, id);
	if (inode != NULL || hfsMetaIndexReady(fs)) {
		return inode;
	}
	return hfsMetaFindByIdLinear(fs, id);
}

/* Return one child in indexed name order, with linear fallback. */
struct hfs_inode *hfsMetaChildAt(Hfs *fs, uint64_t parentId,
				 uint32_t ordinal) {
	struct hfs_inode *inode;
	uint32_t i;
	uint32_t seen = 0;

	inode = hfsMetaIndexChildAt(fs, parentId, ordinal);
	if (inode != NULL || hfsMetaIndexReady(fs)) {
		return inode;
	}
	for (i = 0; i < fs->inodeCount; i++) {
		if (fs->inodes[i].parentId == parentId) {
			if (seen++ == ordinal) {
				return &fs->inodes[i];
			}
		}
	}
	return NULL;
}

/* Count children through the directory index, with linear fallback. */
uint32_t hfsMetaChildCount(Hfs *fs, uint64_t parentId) {
	uint32_t count;
	uint32_t i;

	count = hfsMetaIndexChildCount(fs, parentId);
	if (hfsMetaIndexReady(fs)) {
		return count;
	}
	count = 0;
	for (i = 0; i < fs->inodeCount; i++) {
		if (fs->inodes[i].parentId == parentId) {
			count++;
		}
	}
	return count;
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

	if (path == NULL || path[0] == '\0') {
		errno = ENOENT;
		return NULL;
	}
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
int hfsMetaMkdir(Hfs *fs, struct hfs_inode *parent, const char *name,
			 uint32_t mode) {
	struct hfs_inode *dir;
	struct timespec ts;
	uint32_t index;

	if (hfsMetaEnsureWritable(fs) != 0) {
		return -1;
	}
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
	hfsMetaInitAttributes(dir, HFS_INODE_DIR,
			       (uint64_t)ts.tv_sec * 1000000000ULL +
			       (uint64_t)ts.tv_nsec);
	dir->mode = mode & 07777U;
	snprintf(dir->name, sizeof(dir->name), "%s", name);
	index = fs->inodeCount - 1;

	return hfsMetaCommit(fs, &index, 1, false);
}

/*
 * Remove an empty directory resolved from 'path'.
 * Fails with ENOTDIR if it is a file, ENOTEMPTY if it has children.
 */
int hfsMetaRmdir(Hfs *fs, const char *path) {
	struct hfs_inode *dir;
	uint32_t removedIndex = 0;
	uint32_t i;

	if (hfsMetaEnsureWritable(fs) != 0) {
		return -1;
	}
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
			removedIndex = i;
			fs->inodes[i] = fs->inodes[fs->inodeCount - 1];
			fs->inodeCount--;
			hfsMetaIndexMarkDirty(fs);
			break;
		}
	}

	return removedIndex < fs->inodeCount
		? hfsMetaCommit(fs, &removedIndex, 1, false)
		: hfsMetaCommit(fs, NULL, 0, false);
}

/* Async context for hfsMetaCreateFile (blob creation waiter). */
struct hfs_meta_create_ctx {
	Hfs *fs;
	HfsRequestResult result;
	spdk_blob_id blobId;
};

/* Completion callback for spdk_bs_create_blob (file data blob). */
static void hfsMetaCreateFileCb(void *arg, spdk_blob_id blobId, int bserrno) {
	struct hfs_meta_create_ctx *ctx = (struct hfs_meta_create_ctx *)arg;

	if (bserrno != 0 || blobId == SPDK_BLOBID_INVALID) {
		hfsRequestComplete(&ctx->result, bserrno != 0 ? bserrno : -EIO, 0);
		return;
	}
	ctx->blobId = blobId;
	hfsRequestComplete(&ctx->result, 0, 0);
}

/* SPDK-thread task: allocate a new data blob for a file. */
static void hfsMetaDoCreateFileBlob(void *arg) {
	struct hfs_meta_create_ctx *ctx = (struct hfs_meta_create_ctx *)arg;

	if (hfsRequestCompletionInjected(ctx->result.operation)) {
		hfsMetaCreateFileCb(ctx, SPDK_BLOBID_INVALID, -EIO);
		return;
	}
	spdk_bs_create_blob(ctx->fs->blobstore, hfsMetaCreateFileCb, ctx);
}

/*
 * Create a new file: allocate a data blob, add a FILE inode under
 * 'parent' and persist the table. Fails with EEXIST if the name is taken.
 */
int hfsMetaCreateFile(Hfs *fs, struct hfs_inode *parent, const char *name,
			      uint32_t mode) {
	struct hfs_meta_create_ctx ctx;
	struct hfs_inode *in;
	struct timespec ts;
	uint32_t index;
	int savedErrno;
	int rc;

	if (hfsMetaEnsureWritable(fs) != 0) {
		return -1;
	}
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

	ctx.blobId = SPDK_BLOBID_INVALID;
	ctx.fs = fs;
	hfsRequestInit(&ctx.result, HFS_REQUEST_BLOB_CREATE);

	if (hfsRequestWait(&ctx.result, hfsMetaDoCreateFileBlob, &ctx) != 0) {
		return -1;
	}

	in = hfsMetaAddInode(fs);
	if (in == NULL) {
		HfsFile file = {
			.fs = fs,
			.blobId = ctx.blobId,
		};

		errno = ENOSPC;
		(void)hfsDeleteBlobById(&file);
		errno = ENOSPC;
		return -1;
	}

	clock_gettime(CLOCK_REALTIME, &ts);
	in->type = HFS_INODE_FILE;
	in->parentId = parent->id;
	in->blobId = ctx.blobId;
	in->size = 0;
	hfsMetaInitAttributes(in, HFS_INODE_FILE,
			       (uint64_t)ts.tv_sec * 1000000000ULL +
			       (uint64_t)ts.tv_nsec);
	in->mode = mode & 07777U;
	snprintf(in->name, sizeof(in->name), "%s", name);
	index = fs->inodeCount - 1;

	rc = hfsMetaCommit(fs, &index, 1, false);
	if (rc == 0 || fs->metaRecoveryRequired) {
		return rc;
	}
	/* The failed transaction was never published, so release its new blob. */
	savedErrno = errno;
	{
		HfsFile file = {
			.fs = fs,
			.blobId = ctx.blobId,
		};
		(void)hfsDeleteBlobById(&file);
	}
	errno = savedErrno;
	return -1;
}

/*
 * Remove a file inode from the table; the caller deletes its blob.
 * Fails with EISDIR for a directory; returns the blob id to delete.
 */
int hfsMetaRemoveFile(Hfs *fs, struct hfs_inode *parent, const char *name,
		      uint64_t *blobId) {
	struct hfs_inode *in;
	uint32_t removedIndex = 0;
	uint32_t i;

	if (hfsMetaEnsureWritable(fs) != 0) {
		return -1;
	}
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
			removedIndex = i;
			fs->inodes[i] = fs->inodes[fs->inodeCount - 1];
			fs->inodeCount--;
			hfsMetaIndexMarkDirty(fs);
			break;
		}
	}

	return removedIndex < fs->inodeCount
		? hfsMetaCommit(fs, &removedIndex, 1, false)
		: hfsMetaCommit(fs, NULL, 0, false);
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
	uint32_t changedIndices[2];
	uint32_t changedCount = 0;
	uint32_t dstIndex = 0;
	uint64_t srcId;
	uint64_t newParentId;
	const char *newLeaf;
	uint32_t i;

	if (hfsMetaEnsureWritable(fs) != 0) {
		return -1;
	}
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
		dstIndex = (uint32_t)(dst - fs->inodes);
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
				hfsMetaIndexMarkDirty(fs);
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
	hfsMetaIndexMarkDirty(fs);
	/* Record final positions after compaction, never pre-delete offsets. */
	changedIndices[changedCount++] = (uint32_t)(src - fs->inodes);
	if (dst != NULL && dstIndex < fs->inodeCount &&
	    dstIndex != changedIndices[0]) {
		changedIndices[changedCount++] = dstIndex;
	}

	return hfsMetaCommit(fs, changedIndices, changedCount, false);
}

/* Update an inode's size and mtime, then persist the table. */
int hfsMetaSetSize(Hfs *fs, struct hfs_inode *in, uint64_t size) {
	struct timespec ts;
	uint32_t index = (uint32_t)(in - fs->inodes);

	if (hfsMetaEnsureWritable(fs) != 0) {
		return -1;
	}
	clock_gettime(CLOCK_REALTIME, &ts);
	in->size = size;
	in->mtimeNs = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
	in->ctimeNs = in->mtimeNs;

	return hfsMetaCommit(fs, &index, 1, false);
}

/* Persist a permission-bit change and refresh ctime. */
int hfsMetaSetMode(Hfs *fs, struct hfs_inode *in, uint32_t mode) {
	uint32_t index = (uint32_t)(in - fs->inodes);

	if (hfsMetaEnsureWritable(fs) != 0) {
		return -1;
	}
	in->mode = mode & 07777U;
	in->ctimeNs = hfsMetaNowNs();
	return hfsMetaCommit(fs, &index, 1, false);
}

/* Persist one or both owner-id changes and refresh ctime. */
int hfsMetaSetOwner(Hfs *fs, struct hfs_inode *in, uint32_t uid,
			    uint32_t gid, bool setUid, bool setGid) {
	uint32_t index = (uint32_t)(in - fs->inodes);

	if (hfsMetaEnsureWritable(fs) != 0) {
		return -1;
	}
	if (!setUid && !setGid) {
		return 0;
	}
	if (setUid) {
		in->uid = uid;
	}
	if (setGid) {
		in->gid = gid;
	}
	in->ctimeNs = hfsMetaNowNs();
	return hfsMetaCommit(fs, &index, 1, false);
}

/* Persist selected timestamps and refresh ctime for a metadata change. */
int hfsMetaSetTimes(Hfs *fs, struct hfs_inode *in, uint64_t atimeNs,
			    uint64_t mtimeNs, bool setAtime, bool setMtime) {
	uint32_t index = (uint32_t)(in - fs->inodes);

	if (hfsMetaEnsureWritable(fs) != 0) {
		return -1;
	}
	if (!setAtime && !setMtime) {
		return 0;
	}
	if (setAtime) {
		in->atimeNs = atimeNs;
	}
	if (setMtime) {
		in->mtimeNs = mtimeNs;
	}
	in->ctimeNs = hfsMetaNowNs();
	return hfsMetaCommit(fs, &index, 1, false);
}

/* Commit a successful data-write checkpoint for an inode selected by id. */
int hfsMetaCommitInode(Hfs *fs, uint64_t inodeId) {
	struct hfs_inode *in = hfsMetaFindById(fs, inodeId);
	uint32_t index;

	if (hfsMetaEnsureWritable(fs) != 0) {
		return -1;
	}
	if (in == NULL) {
		errno = ENOENT;
		return -1;
	}
	index = (uint32_t)(in - fs->inodes);
	hfsMetaMarkInodeDirty(fs, inodeId);
	return hfsMetaCommit(fs, &index, 1, true);
}

/* Commit all dirty data inodes, allowing one fsync to serve its peers. */
int hfsMetaCommitDirty(Hfs *fs) {
	if (hfsMetaEnsureWritable(fs) != 0) {
		return -1;
	}
	if (!fs->metaDirty) {
		return 0;
	}
	return hfsMetaCommit(fs, NULL, 0, true);
}

/* Return the last metadata generation known durable to this mount. */
uint64_t hfsMetaDurableEpoch(const Hfs *fs) {
	return fs == NULL ? 0 : fs->metaDurableEpoch;
}

/* Request state used while loading or creating the blobstore super blob. */
typedef struct HfsMetaSuperRequest {
	Hfs *fs;
	spdk_blob_id blobId;
	HfsRequestResult result;
	int primaryRc;
} HfsMetaSuperRequest;

/* Complete one super-blob lookup and publish only a successful id. */
static void hfsMetaGetSuperCb(void *arg, spdk_blob_id blobId, int bserrno) {
	HfsMetaSuperRequest *request = arg;

	if (bserrno != 0) {
		request->fs->metaBlobId = SPDK_BLOBID_INVALID;
		hfsRequestComplete(&request->result, bserrno, 0);
		return;
	}
	request->fs->metaBlobId = blobId;
	hfsRequestComplete(&request->result, 0, 0);
}

/* Finish failed format cleanup without masking the original set-super error. */
static void hfsMetaFormatDeleteCb(void *arg, int bserrno) {
	HfsMetaSuperRequest *request = arg;

	if (bserrno != 0) {
		HFS_LOG_ERROR("metadata format cleanup failed blob=%"PRIu64" bserrno=%d",
			      request->blobId, bserrno);
	}
	hfsRequestComplete(&request->result, request->primaryRc, 0);
}

/* Publish the metadata blob id only after it becomes the blobstore super blob. */
static void hfsMetaSetSuperCb(void *arg, int bserrno) {
	HfsMetaSuperRequest *request = arg;

	if (bserrno == 0) {
		request->fs->metaBlobId = request->blobId;
		hfsRequestComplete(&request->result, 0, 0);
		return;
	}
	request->primaryRc = bserrno;
	spdk_bs_delete_blob(request->fs->blobstore, request->blobId,
			    hfsMetaFormatDeleteCb, request);
}

/* Create one metadata blob and register it as the blobstore super blob. */
static void hfsMetaCreateBlobCb(void *arg, spdk_blob_id blobId, int bserrno) {
	HfsMetaSuperRequest *request = arg;

	if (bserrno != 0 || blobId == SPDK_BLOBID_INVALID) {
		hfsRequestComplete(&request->result,
				   bserrno != 0 ? bserrno : -EIO, 0);
		return;
	}
	request->blobId = blobId;
	spdk_bs_set_super(request->fs->blobstore, blobId, hfsMetaSetSuperCb,
			  request);
}

/* SPDK-thread task: create the metadata blob (first-time format). */
static void hfsMetaDoFormat(void *arg) {
	HfsMetaSuperRequest *request = arg;

	if (hfsRequestCompletionInjected(request->result.operation)) {
		hfsMetaCreateBlobCb(request, SPDK_BLOBID_INVALID, -EIO);
		return;
	}
	spdk_bs_create_blob(request->fs->blobstore, hfsMetaCreateBlobCb, request);
}

/* SPDK-thread task: fetch the blobstore super blob id. */
static void hfsMetaDoGetSuper(void *arg) {
	HfsMetaSuperRequest *request = arg;

	if (hfsRequestCompletionInjected(request->result.operation)) {
		hfsMetaGetSuperCb(request, SPDK_BLOBID_INVALID, -EIO);
		return;
	}
	spdk_bs_get_super(request->fs->blobstore, hfsMetaGetSuperCb, request);
}

/* Open the metadata blob once and keep the owner-thread handle for the mount. */
static int hfsMetaOpenResident(Hfs *fs) {
	HfsMetaIoRequest request;

	if (fs == NULL || fs->metaBlobId == SPDK_BLOBID_INVALID) {
		errno = EINVAL;
		return -1;
	}
	if (fs->metaBlob != NULL) {
		return 0;
	}
	memset(&request, 0, sizeof(request));
	request.fs = fs;
	if (hfsMetaWait(&request, HFS_REQUEST_METADATA_OPEN,
			hfsMetaDoOpenBlob) != 0) {
		return -1;
	}
	fs->metaBlob = request.blob;
	request.blob = NULL;
	return 0;
}

/* Close the resident metadata handle before the blobstore is unloaded. */
static int hfsMetaCloseResident(Hfs *fs) {
	HfsMetaIoRequest request;
	int rc;

	if (fs == NULL || fs->metaBlob == NULL) {
		return 0;
	}
	memset(&request, 0, sizeof(request));
	request.fs = fs;
	request.blob = fs->metaBlob;
	rc = hfsMetaWait(&request, HFS_REQUEST_BLOB_CLOSE, hfsMetaDoClose);
	if (rc == 0) {
		fs->metaBlob = NULL;
	}
	return rc;
}

/*
 * Load the super blob id from the blobstore.
 * Returns -1 with errno=ENOENT if the storage has never been formatted.
 */
int hfsMetaLoad(Hfs *fs) {
	HfsMetaSuperRequest request;

	if (fs == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(&request, 0, sizeof(request));
	request.fs = fs;
	fs->metaBlobId = SPDK_BLOBID_INVALID;
	hfsRequestInit(&request.result, HFS_REQUEST_METADATA_GET_SUPER);
	if (hfsRequestWait(&request.result, hfsMetaDoGetSuper, &request) != 0) {
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
	HfsMetaSuperRequest request;

	if (fs == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(&request, 0, sizeof(request));
	request.fs = fs;
	request.blobId = SPDK_BLOBID_INVALID;
	fs->metaBlobId = SPDK_BLOBID_INVALID;
	hfsRequestInit(&request.result, HFS_REQUEST_METADATA_FORMAT);
	if (hfsRequestWait(&request.result, hfsMetaDoFormat, &request) != 0) {
		return -1;
	}

	if (fs->metaBlobId == SPDK_BLOBID_INVALID) {
		return -1;
	}

	SPDK_NOTICELOG("formatted, meta blobId: %"PRIu64"\n", fs->metaBlobId);
	return 0;
}

/* Release resident metadata resources after all callers have stopped. */
void hfsMetaFini(Hfs *fs) {
	int savedErrno;

	if (fs == NULL) {
		return;
	}
	savedErrno = errno;
	if (hfsMetaCloseResident(fs) != 0) {
		HFS_LOG_ERROR("metadata blob close failed during fini");
	}
	hfsMetaIndexFini(fs);
	free(fs->inodes);
	fs->inodes = NULL;
	free(fs->metaDirtyInodeIds);
	fs->metaDirtyInodeIds = NULL;
	fs->metaDirtyInodeCount = 0;
	fs->inodeCount = 0;
	errno = savedErrno;
}

/*
 * Entry point for metadata handling.
 * Allocates the inode table, then either loads an existing table or
 * formats a fresh one (with an empty table persisted).
 */
int hfsMetaInit(Hfs *fs) {
	struct hfs_inode *root;
	int rc;

	fs->inodes = calloc(HFS_MAX_INODES, sizeof(struct hfs_inode));
	if (fs->inodes == NULL) {
		return -1;
	}
	if (hfsMetaIndexInit(fs) != 0) {
		hfsMetaFini(fs);
		return -1;
	}

	if (hfsMetaLoad(fs) == 0) {
		if (hfsMetaOpenResident(fs) != 0) {
			hfsMetaFini(fs);
			return -1;
		}
		if (hfsMetaLoadAll(fs) != 0) {
			hfsMetaFini(fs);
			return -1;
		}
		/* A prior committed unlink/rename may have left a retryable blob. */
		if (hfsMetaReclaimUnreachable(fs) != 0) {
			HFS_LOG_ERROR("mount reclaim deferred: %s", strerror(errno));
		}
		return 0;
	}
	if (errno != ENOENT) {
		hfsMetaFini(fs);
		return -1;
	}
	if (!hfs_format_requested()) {
		hfsMetaFini(fs);
		return -1;
	}

	if (hfsMetaFormat(fs) != 0) {
		hfsMetaFini(fs);
		return -1;
	}
	if (hfsMetaOpenResident(fs) != 0) {
		hfsMetaFini(fs);
		return -1;
	}

	/* Create the root directory inode and persist the table. */
	fs->inodeCount = 0;
	fs->nextInodeId = 1;
	root = hfsMetaAddInode(fs);
	if (root == NULL) {
		hfsMetaFini(fs);
		return -1;
	}
	root->type = HFS_INODE_DIR;
	root->parentId = 0;
	root->blobId = SPDK_BLOBID_INVALID;
	hfsMetaInitAttributes(root, HFS_INODE_DIR, hfsMetaNowNs());
	snprintf(root->name, sizeof(root->name), "/");
	fs->rootInodeId = root->id;
	rc = hfsMetaSaveAll(fs);
	if (rc == 0) {
		rc = hfsMetaIndexRebuild(fs);
	}
	if (rc != 0) {
		hfsMetaFini(fs);
	}
	return rc;
}
