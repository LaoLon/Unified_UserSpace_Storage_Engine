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
#include "hfs_meta.h"

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

/* Generic completion callback: marks the waiter as done. */
static void hfsMetaOpCb(void *arg, int bserrno) {
	Hfs *fs = (Hfs *)arg;

	hfsRuntimeCompletionComplete(&fs->completion);
}

/* Completion callback for spdk_bs_open_blob: stores the opened blob. */
static void hfsMetaOpenBlobCb(void *arg, struct spdk_blob *blob, int bserrno) {
	Hfs *fs = (Hfs *)arg;

	fs->metaBlob = blob;
	hfsRuntimeCompletionComplete(&fs->completion);
}

/* SPDK-thread task: open the metadata blob. */
static void hfsMetaDoOpenBlob(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_bs_open_blob(fs->blobstore, fs->metaBlobId, hfsMetaOpenBlobCb, fs);
}

/* SPDK-thread task: resize the metadata blob to the needed cluster count. */
static void hfsMetaDoResize(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_blob_resize(fs->metaBlob, fs->metaClusters, hfsMetaOpCb, fs);
}

/* SPDK-thread task: write the serialized metadata into the blob. */
static void hfsMetaDoWrite(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_blob_io_write(fs->metaBlob, fs->channel, fs->metaBuf,
		0, fs->metaUnits, hfsMetaOpCb, fs);
}

/* SPDK-thread task: read the metadata blob into the DMA buffer. */
static void hfsMetaDoRead(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_blob_io_read(fs->metaBlob, fs->channel, fs->metaBuf,
		0, fs->metaUnits, hfsMetaOpCb, fs);
}

/* SPDK-thread task: flush blob metadata to the backing device. */
static void hfsMetaDoSync(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_blob_sync_md(fs->metaBlob, hfsMetaOpCb, fs);
}

/* SPDK-thread task: close the metadata blob. */
static void hfsMetaDoClose(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_blob_close(fs->metaBlob, hfsMetaOpCb, fs);
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
	void *tmp;
	int rc = -1;

	tmp = spdk_malloc(needUnits * ioUnitSize, 0x1000, NULL,
		SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (tmp == NULL) {
		return -1;
	}
	memset(tmp, 0, needUnits * ioUnitSize);
	memcpy(tmp, buf, len);

	fs->metaBuf = tmp;
	fs->metaUnits = needUnits;
	fs->metaClusters = needClusters;

	if (!waiter(globalThread, hfsMetaDoOpenBlob, fs, &fs->completion)) goto out;
	if (!waiter(globalThread, hfsMetaDoResize, fs, &fs->completion)) goto out;
	if (!waiter(globalThread, hfsMetaDoWrite, fs, &fs->completion)) goto out;
	if (!waiter(globalThread, hfsMetaDoSync, fs, &fs->completion)) goto out;
	if (!waiter(globalThread, hfsMetaDoClose, fs, &fs->completion)) goto out;

	rc = 0;
out:
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
	void *tmp;
	int rc = -1;

	if (!waiter(globalThread, hfsMetaDoOpenBlob, fs, &fs->completion)) {
		return -1;
	}

	/* Derive the read size from the current blob size. */
	clusters = spdk_blob_get_num_clusters(fs->metaBlob);
	units = clusters * clusterSize / ioUnitSize;
	if (units == 0) {
		waiter(globalThread, hfsMetaDoClose, fs, &fs->completion);
		return -1;
	}

	tmp = spdk_malloc(units * ioUnitSize, 0x1000, NULL,
		SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (tmp == NULL) {
		waiter(globalThread, hfsMetaDoClose, fs, &fs->completion);
		return -1;
	}

	fs->metaBuf = tmp;
	fs->metaUnits = units;
	if (!waiter(globalThread, hfsMetaDoRead, fs, &fs->completion)) goto out;
	if (!waiter(globalThread, hfsMetaDoClose, fs, &fs->completion)) goto out;

	*outBuf = tmp;
	*outLen = units * ioUnitSize;
	return 0;
out:
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

/* Commit a metadata image with intent-before-data ordering. */
static int hfsMetaCommit(Hfs *fs, const uint32_t *indices,
				 uint32_t indexCount) {
	HfsMetaHeaderDisk oldHeader;
	HfsMetaIntentDisk intent;
	uint8_t *oldBuf = NULL;
	uint8_t *pendingBuf = NULL;
	uint8_t *committedBuf = NULL;
	uint8_t *stableBuf = NULL;
	size_t oldLen;
	size_t newLen;
	size_t pendingLen;
	uint64_t generation;
	int rc = -1;

	if (indexCount > HFS_META_INTENT_MAX_SLOTS) {
		errno = E2BIG;
		return -1;
	}
	if (fs->metaNeedsMigration && hfsMetaSaveAll(fs) != 0) {
		return -1;
	}
	if (fs->metaGeneration == UINT64_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	generation = fs->metaGeneration == 0 ? 1 : fs->metaGeneration + 1;

	if (hfsMetaBlobRead(fs, (void **)&oldBuf, &oldLen) != 0 ||
	    oldLen < HFS_META_INODE_OFFSET) {
		errno = EIO;
		goto out;
	}
	memcpy(&oldHeader, oldBuf, sizeof(oldHeader));
	if (oldHeader.magic != HFS_META_MAGIC ||
	    oldHeader.version != HFS_META_VERSION) {
		errno = EINVAL;
		goto out;
	}
	if (hfsMetaBuildStableImage(fs, generation, &stableBuf, &newLen) != 0) {
		goto out;
	}
	pendingLen = oldLen > newLen ? oldLen : newLen;
	pendingBuf = calloc(1, pendingLen);
	if (pendingBuf == NULL) {
		goto out;
	}
	memcpy(pendingBuf, oldBuf, oldLen);
	hfsMetaBuildIntent(fs, oldHeader.generation, generation,
				   oldHeader.count, indices, indexCount, &intent);
	memcpy(pendingBuf + HFS_META_HEADER_SIZE, &intent, sizeof(intent));
	if (hfsMetaBlobWrite(fs, pendingBuf, pendingLen) != 0) {
		goto out;
	}
	hfsMetaCrashPoint(1);

	memcpy(stableBuf + HFS_META_HEADER_SIZE, &intent, sizeof(intent));
	committedBuf = stableBuf;
	if (hfsMetaBlobWrite(fs, committedBuf, newLen) != 0) {
		goto out;
	}
	hfsMetaCrashPoint(2);

	memset(stableBuf + HFS_META_HEADER_SIZE, 0, sizeof(intent));
	hfsMetaCrashPoint(3);
	if (hfsMetaBlobWrite(fs, stableBuf, newLen) != 0) {
		goto out;
	}
	fs->metaGeneration = generation;
	fs->metaNeedsMigration = false;
	fs->metaDirty = false;
	rc = 0;
out:
	if (oldBuf != NULL) {
		spdk_free(oldBuf);
	}
	free(pendingBuf);
	free(stableBuf);
	return rc;
}

/* Serialize a stable versioned image and clear any intent record. */
int hfsMetaSaveAll(Hfs *fs) {
	uint8_t *buf;
	size_t len;
	uint64_t generation = fs->metaGeneration == 0 ? 1 : fs->metaGeneration;
	int rc;

	if (hfsMetaBuildStableImage(fs, generation, &buf, &len) != 0) {
		return -1;
	}
	rc = hfsMetaBlobWrite(fs, buf, len);
	free(buf);
	if (rc == 0) {
		fs->metaGeneration = generation;
		fs->metaNeedsMigration = false;
		fs->metaDirty = false;
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
	fs->metaDirty = false;

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
			return hfsMetaSaveAll(fs);
		}
	}
	spdk_free(buf);
	if (fs->nextInodeId == 0) {
		errno = ENOSPC;
		return -1;
	}
	return 0;
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
	return inode;
}

/* Linear search for an inode by (parentId, name). */
struct hfs_inode *hfsMetaFindByName(Hfs *fs, uint64_t parentId, const char *name) {
	uint32_t i;

	for (i = 0; i < fs->inodeCount; i++) {
		if (fs->inodes[i].parentId == parentId &&
		    strcmp(fs->inodes[i].name, name) == 0) {
			return &fs->inodes[i];
		}
	}
	return NULL;
}

/* Linear search for an inode by id. */
struct hfs_inode *hfsMetaFindById(Hfs *fs, uint64_t id) {
	uint32_t i;

	for (i = 0; i < fs->inodeCount; i++) {
		if (fs->inodes[i].id == id) {
			return &fs->inodes[i];
		}
	}
	return NULL;
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

	return hfsMetaCommit(fs, &index, 1);
}

/*
 * Remove an empty directory resolved from 'path'.
 * Fails with ENOTDIR if it is a file, ENOTEMPTY if it has children.
 */
int hfsMetaRmdir(Hfs *fs, const char *path) {
	struct hfs_inode *dir;
	uint32_t removedIndex = 0;
	uint32_t i;

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
			break;
		}
	}

	return removedIndex < fs->inodeCount
		? hfsMetaCommit(fs, &removedIndex, 1)
		: hfsMetaCommit(fs, NULL, 0);
}

/* Async context for hfsMetaCreateFile (blob creation waiter). */
struct hfs_meta_create_ctx {
	Hfs *fs;
	HfsCompletion completion;
	int rc;
	spdk_blob_id blobId;
};

/* Completion callback for spdk_bs_create_blob (file data blob). */
static void hfsMetaCreateFileCb(void *arg, spdk_blob_id blobId, int bserrno) {
	struct hfs_meta_create_ctx *ctx = (struct hfs_meta_create_ctx *)arg;

	ctx->blobId = blobId;
	ctx->rc = bserrno;
	hfsRuntimeCompletionComplete(&ctx->completion);
}

/* SPDK-thread task: allocate a new data blob for a file. */
static void hfsMetaDoCreateFileBlob(void *arg) {
	struct hfs_meta_create_ctx *ctx = (struct hfs_meta_create_ctx *)arg;

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

	ctx.rc = 0;
	ctx.blobId = SPDK_BLOBID_INVALID;
	ctx.fs = fs;

	if (!waiter(globalThread, hfsMetaDoCreateFileBlob, &ctx, &ctx.completion)) {
		errno = ETIMEDOUT;
		return -1;
	}
	if (ctx.rc != 0) {
		errno = -ctx.rc;
		return -1;
	}

	in = hfsMetaAddInode(fs);
	if (in == NULL) {
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

	return hfsMetaCommit(fs, &index, 1);
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
			break;
		}
	}

	return removedIndex < fs->inodeCount
		? hfsMetaCommit(fs, &removedIndex, 1)
		: hfsMetaCommit(fs, NULL, 0);
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
	/* Record final positions after compaction, never pre-delete offsets. */
	changedIndices[changedCount++] = (uint32_t)(src - fs->inodes);
	if (dst != NULL && dstIndex < fs->inodeCount &&
	    dstIndex != changedIndices[0]) {
		changedIndices[changedCount++] = dstIndex;
	}

	return hfsMetaCommit(fs, changedIndices, changedCount);
}

/* Update an inode's size and mtime, then persist the table. */
int hfsMetaSetSize(Hfs *fs, struct hfs_inode *in, uint64_t size) {
	struct timespec ts;
	uint32_t index = (uint32_t)(in - fs->inodes);

	clock_gettime(CLOCK_REALTIME, &ts);
	in->size = size;
	in->mtimeNs = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
	in->ctimeNs = in->mtimeNs;

	return hfsMetaCommit(fs, &index, 1);
}

/* Persist a permission-bit change and refresh ctime. */
int hfsMetaSetMode(Hfs *fs, struct hfs_inode *in, uint32_t mode) {
	uint32_t index = (uint32_t)(in - fs->inodes);

	in->mode = mode & 07777U;
	in->ctimeNs = hfsMetaNowNs();
	return hfsMetaCommit(fs, &index, 1);
}

/* Persist one or both owner-id changes and refresh ctime. */
int hfsMetaSetOwner(Hfs *fs, struct hfs_inode *in, uint32_t uid,
			    uint32_t gid, bool setUid, bool setGid) {
	uint32_t index = (uint32_t)(in - fs->inodes);

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
	return hfsMetaCommit(fs, &index, 1);
}

/* Persist selected timestamps and refresh ctime for a metadata change. */
int hfsMetaSetTimes(Hfs *fs, struct hfs_inode *in, uint64_t atimeNs,
			    uint64_t mtimeNs, bool setAtime, bool setMtime) {
	uint32_t index = (uint32_t)(in - fs->inodes);

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
	return hfsMetaCommit(fs, &index, 1);
}

/* Commit metadata changes for an inode selected by its stable id. */
int hfsMetaCommitInode(Hfs *fs, uint64_t inodeId) {
	struct hfs_inode *in = hfsMetaFindById(fs, inodeId);
	uint32_t index;

	if (in == NULL) {
		errno = ENOENT;
		return -1;
	}
	index = (uint32_t)(in - fs->inodes);
	return hfsMetaCommit(fs, &index, 1);
}

/* Completion callback for spdk_bs_get_super: records the super blob id. */
static void hfsMetaGetSuperCb(void *arg, spdk_blob_id blobId, int bserrno) {
	Hfs *fs = (Hfs *)arg;

	if (bserrno != 0) {
		fs->metaBlobId = SPDK_BLOBID_INVALID;
		hfsRuntimeCompletionComplete(&fs->completion);
		return;
	}

	fs->metaBlobId = blobId;
	hfsRuntimeCompletionComplete(&fs->completion);
}

/* Completion callback for spdk_bs_set_super. */
static void hfsMetaSetSuperCb(void *arg, int bserrno) {
	Hfs *fs = (Hfs *)arg;

	hfsRuntimeCompletionComplete(&fs->completion);
}

/* Completion callback for spdk_bs_create_blob: registers the new blob
 * as the blobstore super blob. */
static void hfsMetaCreateBlobCb(void *arg, spdk_blob_id blobId, int bserrno) {
	Hfs *fs = (Hfs *)arg;

	if (bserrno != 0) {
		hfsRuntimeCompletionComplete(&fs->completion);
		return;
	}

	fs->metaBlobId = blobId;
	spdk_bs_set_super(fs->blobstore, blobId, hfsMetaSetSuperCb, fs);
}

/* SPDK-thread task: create the metadata blob (first-time format). */
static void hfsMetaDoFormat(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_bs_create_blob(fs->blobstore, hfsMetaCreateBlobCb, fs);
}

/* SPDK-thread task: fetch the blobstore super blob id. */
static void hfsMetaDoGetSuper(void *arg) {
	Hfs *fs = (Hfs *)arg;

	spdk_bs_get_super(fs->blobstore, hfsMetaGetSuperCb, fs);
}

/*
 * Load the super blob id from the blobstore.
 * Returns -1 with errno=ENOENT if the storage has never been formatted.
 */
int hfsMetaLoad(Hfs *fs) {
	if (!waiter(globalThread, hfsMetaDoGetSuper, fs, &fs->completion)) {
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
	if (!waiter(globalThread, hfsMetaDoFormat, fs, &fs->completion)) {
		return -1;
	}

	if (fs->metaBlobId == SPDK_BLOBID_INVALID) {
		return -1;
	}

	SPDK_NOTICELOG("formatted, meta blobId: %"PRIu64"\n", fs->metaBlobId);
	return 0;
}

/*
 * Entry point for metadata handling.
 * Allocates the inode table, then either loads an existing table or
 * formats a fresh one (with an empty table persisted).
 */
int hfsMetaInit(Hfs *fs) {
	struct hfs_inode *root;

	fs->inodes = calloc(HFS_MAX_INODES, sizeof(struct hfs_inode));
	if (fs->inodes == NULL) {
		return -1;
	}

	if (hfsMetaLoad(fs) == 0) {
		return hfsMetaLoadAll(fs);
	}
	if (errno != ENOENT) {
		free(fs->inodes);
		fs->inodes = NULL;
		return -1;
	}
	if (!hfs_format_requested()) {
		free(fs->inodes);
		fs->inodes = NULL;
		return -1;
	}

	if (hfsMetaFormat(fs) != 0) {
		free(fs->inodes);
		fs->inodes = NULL;
		return -1;
	}

	/* Create the root directory inode and persist the table. */
	fs->inodeCount = 0;
	fs->nextInodeId = 1;
	root = hfsMetaAddInode(fs);
	if (root == NULL) {
		free(fs->inodes);
		fs->inodes = NULL;
		return -1;
	}
	root->type = HFS_INODE_DIR;
	root->parentId = 0;
	root->blobId = SPDK_BLOBID_INVALID;
	hfsMetaInitAttributes(root, HFS_INODE_DIR, hfsMetaNowNs());
	snprintf(root->name, sizeof(root->name), "/");
	fs->rootInodeId = root->id;
	return hfsMetaSaveAll(fs);
}
