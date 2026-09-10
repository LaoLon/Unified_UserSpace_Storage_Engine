#ifndef HFS_H
#define HFS_H

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include <spdk/blob.h>

#include "hfs_range_lock.h"

struct hfs_inode;
struct hfs_io_pool;
struct hfs_inode_runtime;
struct hfs_meta_index;
struct spdk_bdev_desc;

/* Number of hashed range-lock buckets shared by all mounted inodes. */
#define HFS_RANGE_LOCK_COUNT 256U
/* Number of buckets used for the lazy per-inode runtime registry. */
#define HFS_INODE_RUNTIME_BUCKET_COUNT 256U

/* Completion state shared by one asynchronous SPDK request. */
typedef struct HfsCompletion {
	atomic_int state;
	/* Optional trace identity copied into wait and wake events. */
	uint64_t traceRequestId;
	uint64_t traceParentId;
	uint64_t traceInodeId;
	uint64_t traceOffset;
	uint64_t traceBytes;
	uint8_t traceOperation;
	uint8_t traceBufferKind;
	uint16_t traceReserved;
	uint32_t traceMetadataOperation;
	uint64_t traceStageStartNs;
	/* Runtime queue credit retained until the backend callback retires. */
	_Atomic bool runtimeCreditHeld;
	uint32_t runtimeReactorId;
} HfsCompletion;

/* Operations recorded by one request-private SPDK result object. */
typedef enum HfsRequestOperation {
	HFS_REQUEST_BLOB_CREATE,
	HFS_REQUEST_BLOB_OPEN,
	HFS_REQUEST_BLOB_CLOSE,
	HFS_REQUEST_BLOB_DELETE,
	HFS_REQUEST_BLOB_RESIZE,
	HFS_REQUEST_BLOB_READ,
	HFS_REQUEST_BLOB_WRITE,
	HFS_REQUEST_BLOB_UNMAP,
	HFS_REQUEST_BLOB_WRITE_ZEROES,
	HFS_REQUEST_BLOB_SYNC,
	HFS_REQUEST_BDEV_FLUSH,
	HFS_REQUEST_BDEV_FLUSH_POST_COMMIT,
	HFS_REQUEST_METADATA_OPEN,
	HFS_REQUEST_METADATA_READ,
	HFS_REQUEST_METADATA_WRITE,
	HFS_REQUEST_METADATA_FORMAT,
	HFS_REQUEST_METADATA_GET_SUPER,
	HFS_REQUEST_MOUNT,
	HFS_REQUEST_UMOUNT,
	HFS_REQUEST_CAPACITY,
} HfsRequestOperation;

/* Completion result owned by exactly one synchronous SPDK request. */
typedef struct HfsRequestResult {
	HfsCompletion completion;
	HfsRequestOperation operation;
	uint64_t requestId;
	uint64_t completedUnits;
	int submitRc;
	int completionRc;
	int bserrno;
} HfsRequestResult;

/* Maximum size of a single I/O buffer (bytes). */
#define BUFFER_SIZE		512
/* Maximum length of a file name including the trailing NUL. */
#define FILENAME_LENGTH		128

/*
 * Hfs: a mounted filesystem instance.
 * Wraps the SPDK blobstore plus metadata state (inode table) and the
 * I/O channel used for all blob operations.
 */
typedef struct Hfs {
	struct spdk_blob_store *blobstore;	/* underlying blobstore */
	struct spdk_bs_dev *bsDev;		/* underlying blobstore device */
	struct spdk_bdev_desc *bdevDesc;	/* descriptor used for durable flush */
	struct spdk_io_channel *bdevChannel;	/* bdev channel used for durable flush */
	uint64_t unitSize;			/* blobstore io unit size (bytes) */
	uint64_t clusterSize;			/* blobstore allocation cluster size (bytes) */
	uint64_t totalDataClusters;		/* immutable data-cluster capacity */
	_Atomic uint64_t freeClusterCount;	/* refreshed free data-cluster capacity */

	/* Metadata state. */
	spdk_blob_id metaBlobId;		/* blob id of the metadata blob */
	uint64_t rootInodeId;			/* inode id of the root directory */
	struct hfs_inode *inodes;		/* in-memory inode table */
	struct hfs_meta_index *metaIndex;		/* indexed path and directory state */
	uint32_t inodeCount;			/* number of used inodes */
	uint64_t nextInodeId;			/* next persistent inode id */
	uint64_t metaGeneration;			/* committed metadata generation */
	uint64_t metaDurableEpoch;			/* last generation durable on the metadata blob */
	bool metaNeedsMigration;			/* old metadata layout needs upgrade */
	bool metaDirty;			/* in-memory inode fields need commit */
	bool metaRecoveryRequired;		/* uncertain transaction requires remount */
	uint64_t *metaDirtyInodeIds;		/* data inodes waiting for a checkpoint */
	uint32_t metaDirtyInodeCount;		/* number of entries in metaDirtyInodeIds */
	struct spdk_blob *metaBlob;		/* resident metadata blob handle */

	/* Request-local range synchronization and DMA buffer pool. */
	HfsRangeLock rangeLocks[HFS_RANGE_LOCK_COUNT];
	pthread_mutex_t inodeRuntimeLock;
	struct hfs_inode_runtime *inodeRuntimeBuckets[HFS_INODE_RUNTIME_BUCKET_COUNT];
	struct hfs_inode_runtime *compatibilityRuntime;
	pthread_mutex_t compatibilityResizeLock;
	struct hfs_io_pool *ioPool;		/* bounded DMA buffer pool */


	struct spdk_io_channel *channel;	/* blobstore I/O channel */
	struct spdk_io_channel **dataChannels;	/* per-reactor data channels */
	uint32_t dataReactorCount;		/* configured data reactor count */
} Hfs;

/* Operation table for mount/umount (reserved for future use). */
typedef struct HfsOperation {
	int (*mount)(struct Hfs *fs);
	int (*umount)(struct Hfs *fs);
} HfsOperation;

/*
 * HfsFile: an open file handle.
 * 'offset' is the current read/write position for the sequential
 * hfsRead/hfsWrite calls. Positioned I/O uses request-local state.
 */
typedef struct HfsFile {
	uint8_t filename[FILENAME_LENGTH];	/* file name */
	spdk_blob_id blobId;			/* data blob id */
	struct spdk_blob *blob;			/* opened data blob */
	Hfs *fs;				/* owning filesystem */
	uint64_t inodeId;			/* persistent inode id, zero for standalone tests */
	struct hfs_inode_runtime *runtime;	/* per-inode runtime state */


	uint64_t offset;			/* sequential I/O position */

} HfsFile;

/* Operation table for file ops (reserved for future use). */
typedef struct HfsFileOperation {
	int (*open)(struct HfsFile *file);
	int (*read)(struct HfsFile *file);
	int (*write)(struct HfsFile *file);
	int (*close)(struct HfsFile *file);
} HfsFileOperation;

/* Initialize the SPDK environment (hugepages, subsystems, global thread). */
int hfsEnvSetup(void);

/* Mount the blobstore and allocate an I/O channel. */
int hfsMount(Hfs *fs);
/* Unload the blobstore and release the I/O channel. */
int hfsUmount(Hfs *fs);

/* Create a new blob and open it (creates a fresh file). */
int hfsOpen(HfsFile *file);
/* Open an existing blob by id (open-by-id path). */
int hfsOpenById(HfsFile *file);
/* Resize the blob so it covers at least 'bytes' (grow only). */
int hfsResizeTo(HfsFile *file, uint64_t bytes);
/* Resize the blob exactly to cover 'bytes' and release unused clusters. */
int hfsResizeExact(HfsFile *file, uint64_t bytes);
/* Sequential read at file->offset; advances the offset. */
int hfsRead(HfsFile *file, uint8_t *buffer, size_t count);
/* Sequential write at file->offset; advances the offset. */
int hfsWrite(HfsFile *file, const uint8_t *buffer, size_t count);
/* Read 'count' bytes at an explicit byte offset. */
ssize_t hfsReadAt(HfsFile *file, uint8_t *buffer, size_t count, uint64_t offset);
/* Write 'count' bytes at an explicit byte offset (auto-grow if needed). */
ssize_t hfsWriteAt(HfsFile *file, const uint8_t *buffer, size_t count, uint64_t offset);
/* Close the file handle; the blob is kept (unlink deletes it). */
int hfsClose(HfsFile *file);
/* Delete the data blob by id (used by unlink). */
int hfsDeleteBlobById(HfsFile *file);
/* Flush file metadata and the backing bdev for fsync semantics. */
int hfsSync(HfsFile *file);
/* Flush the final fsync barrier after an inode metadata transaction. */
int hfsSyncPostCommit(HfsFile *file);

#endif /* HFS_H */
