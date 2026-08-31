#ifndef HFS_H
#define HFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <spdk/blob.h>

struct hfs_inode;

#define BUFFER_SIZE		512
#define FILENAME_LENGTH		128

typedef struct Hfs {
	struct spdk_blob_store *blobstore;
	uint64_t unitSize;
	uint64_t freeClusterCount;
	uint64_t numCluster;

	spdk_blob_id metaBlobId;
	uint64_t rootInodeId;
	struct hfs_inode *inodes;
	uint32_t inodeCount;
	struct spdk_blob *metaBlob;
	void *metaBuf;
	uint64_t metaUnits;
	uint64_t metaClusters;

	struct spdk_io_channel *channel;
	bool finished;
} Hfs;

typedef struct HfsOperation {
	int (*mount)(struct Hfs *fs);
	int (*umount)(struct Hfs *fs);
} HfsOperation;

typedef struct HfsFile {
	uint8_t filename[FILENAME_LENGTH];
	spdk_blob_id blobId;
	struct spdk_blob *blob;
	Hfs *fs;

	uint8_t *writeBuffer;
	uint8_t *readBuffer;

	uint64_t offset;
	uint64_t ioOffset;
	uint64_t ioUnits;
	uint64_t resizeClusters;

	bool finished;
} HfsFile;

typedef struct HfsFileOperation {
	int (*open)(struct HfsFile *file);
	int (*read)(struct HfsFile *file);
	int (*write)(struct HfsFile *file);
	int (*close)(struct HfsFile *file);
} HfsFileOperation;

int hfsEnvSetup(void);

int hfsMount(Hfs *fs);
int hfsUmount(Hfs *fs);

int hfsOpen(HfsFile *file);
int hfsRead(HfsFile *file, uint8_t *buffer, size_t count);
int hfsWrite(HfsFile *file, const uint8_t *buffer, size_t count);
ssize_t hfsReadAt(HfsFile *file, uint8_t *buffer, size_t count, uint64_t offset);
ssize_t hfsWriteAt(HfsFile *file, const uint8_t *buffer, size_t count, uint64_t offset);
int hfsClose(HfsFile *file);
int hfsDeleteBlobById(HfsFile *file);

#endif /* HFS_H */