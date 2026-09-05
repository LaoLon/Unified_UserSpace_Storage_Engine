#ifndef HFS_H
#define HFS_H

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include <spdk/blob.h>

struct hfs_inode;
struct hfs_io_pool;
struct spdk_bdev_desc;

/* Completion state shared by one asynchronous SPDK request. */
typedef struct HfsCompletion {
	atomic_int state;
} HfsCompletion;

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
	uint32_t inodeCount;			/* number of used inodes */
	uint64_t nextInodeId;			/* next persistent inode id */
	uint64_t metaGeneration;			/* committed metadata generation */
	bool metaNeedsMigration;			/* old metadata layout needs upgrade */
	bool metaDirty;			/* in-memory inode fields need commit */

	/* Request-local data I/O synchronization and DMA buffer pool. */
	pthread_rwlock_t ioLock;		/* shared aligned I/O/RMW lock */
	pthread_mutex_t resizeLock;		/* serializes blob growth decisions */
	struct hfs_io_pool *ioPool;		/* bounded DMA buffer pool */

	/* Scratch state for metadata blob I/O (waiter chain). */
	struct spdk_blob *metaBlob;		/* opened metadata blob */
	void *metaBuf;				/* DMA buffer for read/write */
	uint64_t metaUnits;			/* io units to transfer */
	uint64_t metaClusters;			/* target cluster count */

	struct spdk_io_channel *channel;	/* blobstore I/O channel */
	HfsCompletion completion;			/* mount/metadata request completion */
	int operationRc;			/* result of the current mount operation */
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


	uint64_t offset;			/* sequential I/O position */

	HfsCompletion completion;			/* open/close/flush request completion */
	struct spdk_bs_dev_cb_args flushArgs;	/* fallback blobstore flush arguments */
	int operationRc;			/* result of the current file operation */
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

#endif /* HFS_H */
