#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "hfs_meta_v5.h"

/* Fixed byte offsets inside the explicit little-endian superblock. */
#define V5_SB_MAGIC 0U
#define V5_SB_VERSION 4U
#define V5_SB_HEADER_BYTES 6U
#define V5_SB_FEATURES 8U
#define V5_SB_PAGE_SIZE 12U
#define V5_SB_GENERATION 16U
#define V5_SB_CHECKPOINT_LSN 24U
#define V5_SB_WAL_HEAD 32U
#define V5_SB_WAL_TAIL 40U
#define V5_SB_ROOT_PAGE 48U
#define V5_SB_INODE_COUNT 56U
#define V5_SB_PAGE_COUNT 60U
#define V5_SB_NEXT_INODE_ID 64U
#define V5_SB_WAL_OFFSET 72U
#define V5_SB_WAL_BYTES 80U
#define V5_SB_IMAGE_BYTES 88U
#define V5_SB_CHECKSUM 96U

/* Fixed byte offsets inside an explicit page header. */
#define V5_PAGE_MAGIC 0U
#define V5_PAGE_VERSION 4U
#define V5_PAGE_TYPE 6U
#define V5_PAGE_ID 8U
#define V5_PAGE_GENERATION 16U
#define V5_PAGE_PAYLOAD_LEN 24U
#define V5_PAGE_CHECKSUM 28U

/* Fixed byte offsets inside an explicit WAL header. */
#define V5_WAL_MAGIC 0U
#define V5_WAL_VERSION 4U
#define V5_WAL_TYPE 6U
#define V5_WAL_LENGTH 8U
#define V5_WAL_FLAGS 12U
#define V5_WAL_TXID 16U
#define V5_WAL_LSN 24U
#define V5_WAL_PAYLOAD_CHECKSUM 32U
#define V5_WAL_HEADER_CHECKSUM 36U

#define V5_INODE_ID 0U
#define V5_INODE_TYPE 8U
#define V5_INODE_PARENT_ID 16U
#define V5_INODE_BLOB_ID 24U
#define V5_INODE_MODE 32U
#define V5_INODE_UID 36U
#define V5_INODE_GID 40U
#define V5_INODE_SIZE 48U
#define V5_INODE_ATIME 56U
#define V5_INODE_MTIME 64U
#define V5_INODE_CTIME 72U
#define V5_INODE_NAME 80U

_Static_assert(FILENAME_LENGTH == 128, "v5 inode name width changed");
_Static_assert(V5_INODE_NAME + FILENAME_LENGTH == HFS_META_V5_INODE_SIZE,
	"v5 inode layout size changed");

/* Read and write unsigned little-endian fields without packed C structs. */
static uint16_t v5Get16(const uint8_t *p) {
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t v5Get32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t v5Get64(const uint8_t *p) {
	uint64_t value = 0;
	uint32_t i;

	for (i = 0; i < 8; i++) {
		value |= (uint64_t)p[i] << (8U * i);
	}
	return value;
}

static void v5Put16(uint8_t *p, uint16_t value) {
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static void v5Put32(uint8_t *p, uint32_t value) {
	uint32_t i;

	for (i = 0; i < 4; i++) {
		p[i] = (uint8_t)(value >> (8U * i));
	}
}

static void v5Put64(uint8_t *p, uint64_t value) {
	uint32_t i;

	for (i = 0; i < 8; i++) {
		p[i] = (uint8_t)(value >> (8U * i));
	}
}

/* Compute the same stable FNV-1a checksum on every host architecture. */
static uint32_t v5Checksum(const uint8_t *data, size_t len) {
	uint32_t checksum = 2166136261U;
	size_t i;

	for (i = 0; i < len; i++) {
		checksum ^= data[i];
		checksum *= 16777619U;
	}
	return checksum;
}

/* Checksum a superblock with its checksum field treated as zero. */
static uint32_t v5SuperChecksum(const uint8_t *super) {
	uint8_t copy[HFS_META_V5_SUPER_SIZE];

	memcpy(copy, super, sizeof(copy));
	v5Put32(copy + V5_SB_CHECKSUM, 0);
	return v5Checksum(copy, sizeof(copy));
}

/* Checksum a page header and payload with the page checksum cleared. */
static uint32_t v5PageChecksum(const uint8_t *page, uint32_t payloadLen) {
	uint8_t header[HFS_META_V5_PAGE_HEADER_SIZE];

	memcpy(header, page, sizeof(header));
	v5Put32(header + V5_PAGE_CHECKSUM, 0);
	return v5Checksum(header, sizeof(header)) +
	       v5Checksum(page + HFS_META_V5_PAGE_HEADER_SIZE, payloadLen);
}

/* Encode one inode into its fixed-size portable representation. */
static void v5EncodeInode(uint8_t *dst, const struct hfs_inode *inode) {
	memset(dst, 0, HFS_META_V5_INODE_SIZE);
	v5Put64(dst + V5_INODE_ID, inode->id);
	dst[V5_INODE_TYPE] = inode->type;
	v5Put64(dst + V5_INODE_PARENT_ID, inode->parentId);
	v5Put64(dst + V5_INODE_BLOB_ID, inode->blobId);
	v5Put32(dst + V5_INODE_MODE, inode->mode);
	v5Put32(dst + V5_INODE_UID, inode->uid);
	v5Put32(dst + V5_INODE_GID, inode->gid);
	v5Put64(dst + V5_INODE_SIZE, inode->size);
	v5Put64(dst + V5_INODE_ATIME, inode->atimeNs);
	v5Put64(dst + V5_INODE_MTIME, inode->mtimeNs);
	v5Put64(dst + V5_INODE_CTIME, inode->ctimeNs);
	memcpy(dst + V5_INODE_NAME, inode->name, FILENAME_LENGTH);
}

/* Decode one fixed-size inode representation after all bounds checks passed. */
static void v5DecodeInode(struct hfs_inode *inode, const uint8_t *src) {
	memset(inode, 0, sizeof(*inode));
	inode->id = v5Get64(src + V5_INODE_ID);
	inode->type = src[V5_INODE_TYPE];
	inode->parentId = v5Get64(src + V5_INODE_PARENT_ID);
	inode->blobId = v5Get64(src + V5_INODE_BLOB_ID);
	inode->mode = v5Get32(src + V5_INODE_MODE);
	inode->uid = v5Get32(src + V5_INODE_UID);
	inode->gid = v5Get32(src + V5_INODE_GID);
	inode->size = v5Get64(src + V5_INODE_SIZE);
	inode->atimeNs = v5Get64(src + V5_INODE_ATIME);
	inode->mtimeNs = v5Get64(src + V5_INODE_MTIME);
	inode->ctimeNs = v5Get64(src + V5_INODE_CTIME);
	memcpy(inode->name, src + V5_INODE_NAME, FILENAME_LENGTH);
	inode->name[FILENAME_LENGTH - 1] = '\0';
}

/* Validate and select the newest of the two superblock candidates. */
static int v5SelectSuper(const uint8_t *image, size_t imageLen,
	const uint8_t **selected, HfsMetaV5Info *info) {
	const uint8_t *a;
	const uint8_t *b;
	bool validA = false;
	bool validB = false;
	const uint8_t *best = NULL;
	uint64_t generationA = 0;
	uint64_t generationB = 0;

	if (image == NULL || imageLen < HFS_META_V5_DATA_OFFSET ||
		selected == NULL || info == NULL) {
		errno = EINVAL;
		return -1;
	}
	a = image + HFS_META_V5_SUPER_A_OFFSET;
	b = image + HFS_META_V5_SUPER_B_OFFSET;
	if (v5Get32(a + V5_SB_MAGIC) == HFS_META_V5_MAGIC &&
		v5Get16(a + V5_SB_VERSION) == HFS_META_V5_VERSION &&
		v5Get16(a + V5_SB_HEADER_BYTES) == HFS_META_V5_SUPER_SIZE &&
		v5Get32(a + V5_SB_PAGE_SIZE) == HFS_META_V5_PAGE_SIZE &&
		v5Get32(a + V5_SB_CHECKSUM) == v5SuperChecksum(a)) {
		validA = true;
		generationA = v5Get64(a + V5_SB_GENERATION);
	}
	if (v5Get32(b + V5_SB_MAGIC) == HFS_META_V5_MAGIC &&
		v5Get16(b + V5_SB_VERSION) == HFS_META_V5_VERSION &&
		v5Get16(b + V5_SB_HEADER_BYTES) == HFS_META_V5_SUPER_SIZE &&
		v5Get32(b + V5_SB_PAGE_SIZE) == HFS_META_V5_PAGE_SIZE &&
		v5Get32(b + V5_SB_CHECKSUM) == v5SuperChecksum(b)) {
		validB = true;
		generationB = v5Get64(b + V5_SB_GENERATION);
	}
	if (!validA && !validB) {
		errno = EIO;
		return -1;
	}
	best = validA && (!validB || generationA >= generationB) ? a : b;
	memset(info, 0, sizeof(*info));
	info->superAValid = validA;
	info->superBValid = validB;
	info->features = v5Get32(best + V5_SB_FEATURES);
	info->generation = v5Get64(best + V5_SB_GENERATION);
	info->checkpointLsn = v5Get64(best + V5_SB_CHECKPOINT_LSN);
	info->walHead = v5Get64(best + V5_SB_WAL_HEAD);
	info->walTail = v5Get64(best + V5_SB_WAL_TAIL);
	info->rootPage = v5Get64(best + V5_SB_ROOT_PAGE);
	info->inodeCount = v5Get32(best + V5_SB_INODE_COUNT);
	info->pageCount = v5Get32(best + V5_SB_PAGE_COUNT);
	info->nextInodeId = v5Get64(best + V5_SB_NEXT_INODE_ID);
	info->walOffset = v5Get64(best + V5_SB_WAL_OFFSET);
	info->imageBytes = v5Get64(best + V5_SB_IMAGE_BYTES);
	if (info->inodeCount > HFS_MAX_INODES ||
		info->pageCount != (info->inodeCount + HFS_META_V5_INODES_PER_PAGE - 1U) /
			HFS_META_V5_INODES_PER_PAGE ||
		info->walOffset < HFS_META_V5_DATA_OFFSET ||
		info->walHead < info->walOffset || info->walHead > info->walTail ||
		info->imageBytes < info->walTail || info->imageBytes > imageLen ||
		info->walTail > imageLen) {
		errno = EIO;
		return -1;
	}
	*selected = best;
	return 0;
}

/* Write one validated superblock copy from portable scalar fields. */
static void v5WriteSuper(uint8_t *dst, const HfsMetaV5Info *info) {
	memset(dst, 0, HFS_META_V5_PAGE_SIZE);
	v5Put32(dst + V5_SB_MAGIC, HFS_META_V5_MAGIC);
	v5Put16(dst + V5_SB_VERSION, HFS_META_V5_VERSION);
	v5Put16(dst + V5_SB_HEADER_BYTES, HFS_META_V5_SUPER_SIZE);
	v5Put32(dst + V5_SB_FEATURES, info->features);
	v5Put32(dst + V5_SB_PAGE_SIZE, HFS_META_V5_PAGE_SIZE);
	v5Put64(dst + V5_SB_GENERATION, info->generation);
	v5Put64(dst + V5_SB_CHECKPOINT_LSN, info->checkpointLsn);
	v5Put64(dst + V5_SB_WAL_HEAD, info->walHead);
	v5Put64(dst + V5_SB_WAL_TAIL, info->walTail);
	v5Put64(dst + V5_SB_ROOT_PAGE, info->rootPage);
	v5Put32(dst + V5_SB_INODE_COUNT, info->inodeCount);
	v5Put32(dst + V5_SB_PAGE_COUNT, info->pageCount);
	v5Put64(dst + V5_SB_NEXT_INODE_ID, info->nextInodeId);
	v5Put64(dst + V5_SB_WAL_OFFSET, info->walOffset);
	v5Put64(dst + V5_SB_WAL_BYTES, info->walTail - info->walHead);
	v5Put64(dst + V5_SB_IMAGE_BYTES, info->imageBytes);
	v5Put32(dst + V5_SB_CHECKSUM, v5SuperChecksum(dst));
}

/* Write one checkpoint page containing a contiguous inode range. */
static int v5WritePage(uint8_t *image, uint32_t pageId, uint64_t generation,
	const Hfs *fs, uint32_t firstInode, uint32_t count) {
	uint8_t *page;
	uint32_t i;

	if (count > HFS_META_V5_INODES_PER_PAGE ||
		firstInode > fs->inodeCount || count > fs->inodeCount - firstInode) {
		errno = EINVAL;
		return -1;
	}
	page = image + HFS_META_V5_DATA_OFFSET +
		(size_t)pageId * HFS_META_V5_PAGE_SIZE;
	memset(page, 0, HFS_META_V5_PAGE_SIZE);
	v5Put32(page + V5_PAGE_MAGIC, HFS_META_V5_MAGIC);
	v5Put16(page + V5_PAGE_VERSION, HFS_META_V5_VERSION);
	v5Put16(page + V5_PAGE_TYPE, HFS_META_V5_PAGE_INODES);
	v5Put64(page + V5_PAGE_ID, pageId);
	v5Put64(page + V5_PAGE_GENERATION, generation);
	v5Put32(page + V5_PAGE_PAYLOAD_LEN,
		count * HFS_META_V5_INODE_SIZE);
	for (i = 0; i < count; i++) {
		v5EncodeInode(page + HFS_META_V5_PAGE_HEADER_SIZE +
			(size_t)i * HFS_META_V5_INODE_SIZE,
			&fs->inodes[firstInode + i]);
	}
	v5Put32(page + V5_PAGE_CHECKSUM,
		v5PageChecksum(page, count * HFS_META_V5_INODE_SIZE));
	return 0;
}

/* Build a complete v5 checkpoint and copy the caller's WAL byte stream. */
int hfsMetaV5BuildImage(const Hfs *fs, uint64_t generation,
	uint64_t checkpointLsn, const uint8_t *wal, size_t walLen,
	uint8_t **outImage, size_t *outLen) {
	HfsMetaV5Info info;
	size_t pageBytes;
	size_t walOffset;
	size_t imageBytes;
	uint8_t *image;
	uint32_t page;

	if (fs == NULL || fs->inodes == NULL || outImage == NULL ||
		outLen == NULL || generation == 0 ||
		fs->inodeCount > HFS_MAX_INODES ||
		(walLen != 0 && wal == NULL) ||
		walLen > SIZE_MAX - HFS_META_V5_DATA_OFFSET) {
		errno = EINVAL;
		return -1;
	}
	pageBytes = ((size_t)fs->inodeCount + HFS_META_V5_INODES_PER_PAGE - 1U) /
		HFS_META_V5_INODES_PER_PAGE * HFS_META_V5_PAGE_SIZE;
	walOffset = HFS_META_V5_DATA_OFFSET + pageBytes;
	if (walOffset > SIZE_MAX - walLen) {
		errno = EOVERFLOW;
		return -1;
	}
	imageBytes = walOffset + walLen;
	if (imageBytes % HFS_META_V5_PAGE_SIZE != 0) {
		size_t padding = HFS_META_V5_PAGE_SIZE -
			imageBytes % HFS_META_V5_PAGE_SIZE;

		if (imageBytes > SIZE_MAX - padding) {
			errno = EOVERFLOW;
			return -1;
		}
		imageBytes += padding;
	}
	image = calloc(1, imageBytes == 0 ? 1 : imageBytes);
	if (image == NULL) {
		return -1;
	}
	memset(&info, 0, sizeof(info));
	info.generation = generation;
	info.checkpointLsn = checkpointLsn;
	info.walHead = walOffset;
	info.walTail = walOffset + walLen;
	info.rootPage = 0;
	info.nextInodeId = fs->nextInodeId;
	info.inodeCount = fs->inodeCount;
	info.pageCount = (uint32_t)(pageBytes / HFS_META_V5_PAGE_SIZE);
	info.walOffset = walOffset;
	info.imageBytes = imageBytes;
	for (page = 0; page < info.pageCount; page++) {
		uint32_t first = page * HFS_META_V5_INODES_PER_PAGE;
		uint32_t left = fs->inodeCount - first;
		uint32_t count = left > HFS_META_V5_INODES_PER_PAGE
			? HFS_META_V5_INODES_PER_PAGE : left;

		if (v5WritePage(image, page, generation, fs, first, count) != 0) {
			free(image);
			return -1;
		}
	}
	if (walLen != 0 && wal != NULL) {
		memcpy(image + walOffset, wal, walLen);
	}
	v5WriteSuper(image + HFS_META_V5_SUPER_A_OFFSET, &info);
	v5WriteSuper(image + HFS_META_V5_SUPER_B_OFFSET, &info);
	*outImage = image;
	*outLen = imageBytes;
	return 0;
}

/* Validate every checkpoint page after selecting a valid superblock. */
int hfsMetaV5Inspect(const uint8_t *image, size_t imageLen,
	HfsMetaV5Info *info) {
	const uint8_t *selected;
	size_t pageBytes;
	uint32_t page;

	if (v5SelectSuper(image, imageLen, &selected, info) != 0) {
		return -1;
	}
	(void)selected;
	pageBytes = (size_t)info->pageCount * HFS_META_V5_PAGE_SIZE;
	if ((info->pageCount != 0 && pageBytes / HFS_META_V5_PAGE_SIZE !=
		info->pageCount) ||
		pageBytes > imageLen - HFS_META_V5_DATA_OFFSET ||
		info->walOffset < HFS_META_V5_DATA_OFFSET + pageBytes ||
		info->walOffset > imageLen) {
		errno = EIO;
		return -1;
	}
	for (page = 0; page < info->pageCount; page++) {
		const uint8_t *data = image + HFS_META_V5_DATA_OFFSET +
			(size_t)page * HFS_META_V5_PAGE_SIZE;
		uint32_t payloadLen = v5Get32(data + V5_PAGE_PAYLOAD_LEN);

		if (v5Get32(data + V5_PAGE_MAGIC) != HFS_META_V5_MAGIC ||
			v5Get16(data + V5_PAGE_VERSION) != HFS_META_V5_VERSION ||
			v5Get16(data + V5_PAGE_TYPE) != HFS_META_V5_PAGE_INODES ||
			v5Get64(data + V5_PAGE_ID) != page ||
			payloadLen > HFS_META_V5_PAGE_PAYLOAD_SIZE ||
			payloadLen % HFS_META_V5_INODE_SIZE != 0 ||
			v5Get32(data + V5_PAGE_CHECKSUM) !=
				v5PageChecksum(data, payloadLen)) {
			errno = EIO;
			return -1;
		}
	}
	return 0;
}

/* Validate one WAL record and return its payload boundaries. */
static int v5WalRecord(const uint8_t *record, size_t remaining,
	uint16_t *type, uint64_t *txid, uint64_t *lsn,
	const uint8_t **payload, uint32_t *payloadLen) {
	uint32_t length;
	uint32_t headerChecksum;
	uint8_t header[HFS_META_V5_WAL_HEADER_SIZE];

	if (remaining < HFS_META_V5_WAL_HEADER_SIZE ||
		v5Get32(record + V5_WAL_MAGIC) != HFS_META_V5_WAL_MAGIC ||
		v5Get16(record + V5_WAL_VERSION) != HFS_META_V5_WAL_VERSION) {
		errno = EIO;
		return -1;
	}
	length = v5Get32(record + V5_WAL_LENGTH);
	if (length < HFS_META_V5_WAL_HEADER_SIZE || length > remaining ||
		length > INT_MAX || (length % 8U) != 0) {
		errno = EIO;
		return -1;
	}
	memcpy(header, record, sizeof(header));
	headerChecksum = v5Get32(header + V5_WAL_HEADER_CHECKSUM);
	v5Put32(header + V5_WAL_HEADER_CHECKSUM, 0);
	if (headerChecksum != v5Checksum(header, sizeof(header)) ||
		v5Get32(record + V5_WAL_PAYLOAD_CHECKSUM) !=
			v5Checksum(record + HFS_META_V5_WAL_HEADER_SIZE,
			length - HFS_META_V5_WAL_HEADER_SIZE)) {
		errno = EIO;
		return -1;
	}
	*type = v5Get16(record + V5_WAL_TYPE);
	*txid = v5Get64(record + V5_WAL_TXID);
	*lsn = v5Get64(record + V5_WAL_LSN);
	*payload = record + HFS_META_V5_WAL_HEADER_SIZE;
	*payloadLen = length - HFS_META_V5_WAL_HEADER_SIZE;
	return (int)length;
}

/* Require alignment bytes after a logical payload to remain zero. */
static bool v5PaddingIsZero(const uint8_t *payload, uint32_t payloadLen,
	uint32_t logicalLen) {
	uint32_t i;

	if (payloadLen < logicalLen || payloadLen - logicalLen >= 8U) {
		return false;
	}
	for (i = logicalLen; i < payloadLen; i++) {
		if (payload[i] != 0) {
			return false;
		}
	}
	return true;
}

/* Apply one committed inode record idempotently to the runtime table. */
static int v5ApplyInodeRecord(Hfs *fs, const uint8_t *payload,
	uint32_t payloadLen) {
	uint32_t index;

	if (payloadLen != 4U + HFS_META_V5_INODE_SIZE) {
		errno = EIO;
		return -1;
	}
	index = v5Get32(payload);
	if (index >= HFS_MAX_INODES) {
		errno = EIO;
		return -1;
	}
	v5DecodeInode(&fs->inodes[index], payload + 4);
	if (index >= fs->inodeCount) {
		fs->inodeCount = index + 1U;
	}
	return 0;
}

/* Load checkpoint pages and replay only complete, checksum-valid transactions. */
int hfsMetaV5Load(Hfs *fs, const uint8_t *image, size_t imageLen,
	uint64_t *lastAppliedLsn) {
	HfsMetaV5Info info;
	const uint8_t *selected;
	uint32_t page;
	uint32_t loadedCount = 0;
	uint32_t pendingCount = 0;
	struct PendingRecord {
		uint64_t txid;
		uint64_t lsn;
		const uint8_t *payload;
		uint32_t payloadLen;
	} pending[64];
	uint64_t applied = 0;

	if (fs == NULL || fs->inodes == NULL || lastAppliedLsn == NULL ||
		hfsMetaV5Inspect(image, imageLen, &info) != 0 ||
		v5SelectSuper(image, imageLen, &selected, &info) != 0) {
		return -1;
	}
	memset(fs->inodes, 0, (size_t)HFS_MAX_INODES * sizeof(*fs->inodes));
	for (page = 0; page < info.pageCount; page++) {
		const uint8_t *data = image + HFS_META_V5_DATA_OFFSET +
			(size_t)page * HFS_META_V5_PAGE_SIZE;
		uint32_t payloadLen = v5Get32(data + V5_PAGE_PAYLOAD_LEN);
		uint32_t i;

		for (i = 0; i < payloadLen / HFS_META_V5_INODE_SIZE; i++) {
			uint32_t index = page * HFS_META_V5_INODES_PER_PAGE + i;

			if (index >= HFS_MAX_INODES) {
				errno = EIO;
				return -1;
			}
			v5DecodeInode(&fs->inodes[index], data + HFS_META_V5_PAGE_HEADER_SIZE +
				(size_t)i * HFS_META_V5_INODE_SIZE);
			loadedCount++;
		}
	}
	if (loadedCount != info.inodeCount) {
		errno = EIO;
		return -1;
	}
	fs->inodeCount = info.inodeCount;
	fs->nextInodeId = info.nextInodeId;
	fs->metaGeneration = info.generation;
	atomic_store_explicit(&fs->metaDirty, false, memory_order_release);
	fs->metaNeedsMigration = false;
	fs->metaRecoveryRequired = false;
	if (info.walTail > info.walHead) {
		size_t offset = (size_t)info.walHead;
		size_t end = (size_t)info.walTail;

		while (offset < end) {
			uint16_t type;
			uint64_t txid;
			uint64_t lsn;
			const uint8_t *payload;
			uint32_t payloadLen;
			int length = v5WalRecord(image + offset, end - offset, &type,
				&txid, &lsn, &payload, &payloadLen);

			if (length < 0) {
				return -1;
			}
			if (type == HFS_META_V5_WAL_SET_INODE) {
				if (!v5PaddingIsZero(payload, payloadLen,
					4U + HFS_META_V5_INODE_SIZE)) {
					errno = EIO;
					return -1;
				}
				if (pendingCount == sizeof(pending) / sizeof(pending[0])) {
					errno = E2BIG;
					return -1;
				}
				pending[pendingCount++] = (struct PendingRecord){
					txid, lsn, payload, 4U + HFS_META_V5_INODE_SIZE};
			} else if (type == HFS_META_V5_WAL_COMMIT) {
				uint32_t i;
				uint32_t matched = 0;
				uint32_t kept = 0;
				uint32_t expected;

				if (!v5PaddingIsZero(payload, payloadLen, sizeof(uint32_t))) {
					errno = EIO;
					return -1;
				}
				for (i = 0; i < pendingCount; i++) {
					if (pending[i].txid == txid) {
						matched++;
					}
				}
				expected = v5Get32(payload);
				if (matched != expected) {
					errno = EIO;
					return -1;
				}
				for (i = 0; i < pendingCount; i++) {
					if (pending[i].txid == txid) {
						if (v5ApplyInodeRecord(fs, pending[i].payload,
							pending[i].payloadLen) != 0) {
							return -1;
						}
						if (pending[i].lsn > applied) {
							applied = pending[i].lsn;
						}
					} else {
						pending[kept++] = pending[i];
					}
				}
				if (lsn > applied) {
					applied = lsn;
				}
				pendingCount = kept;
			} else {
				errno = EIO;
				return -1;
			}
			offset += (size_t)length;
		}
	}
	*lastAppliedLsn = applied > info.checkpointLsn ? applied : info.checkpointLsn;
	return 0;
}

/* Encode one WAL record with checksummed header and payload. */
static int v5EncodeRecord(uint16_t type, uint64_t txid, uint64_t lsn,
	const uint8_t *payload, uint32_t payloadLen, uint8_t **outRecord,
	size_t *outLen) {
	uint32_t length;
	uint8_t *record;
	uint8_t header[HFS_META_V5_WAL_HEADER_SIZE];

	if (outRecord == NULL || outLen == NULL || payloadLen > UINT32_MAX -
		HFS_META_V5_WAL_HEADER_SIZE) {
		errno = EINVAL;
		return -1;
	}
	length = HFS_META_V5_WAL_HEADER_SIZE + payloadLen;
	if (length % 8U != 0) {
		length += 8U - length % 8U;
	}
	record = calloc(1, length);
	if (record == NULL) {
		return -1;
	}
	v5Put32(record + V5_WAL_MAGIC, HFS_META_V5_WAL_MAGIC);
	v5Put16(record + V5_WAL_VERSION, HFS_META_V5_WAL_VERSION);
	v5Put16(record + V5_WAL_TYPE, type);
	v5Put32(record + V5_WAL_LENGTH, length);
	v5Put64(record + V5_WAL_TXID, txid);
	v5Put64(record + V5_WAL_LSN, lsn);
	if (payloadLen != 0 && payload != NULL) {
		memcpy(record + HFS_META_V5_WAL_HEADER_SIZE, payload, payloadLen);
	}
	v5Put32(record + V5_WAL_PAYLOAD_CHECKSUM,
		v5Checksum(record + HFS_META_V5_WAL_HEADER_SIZE,
		length - HFS_META_V5_WAL_HEADER_SIZE));
	memcpy(header, record, sizeof(header));
	v5Put32(header + V5_WAL_HEADER_CHECKSUM, 0);
	v5Put32(record + V5_WAL_HEADER_CHECKSUM,
		v5Checksum(header, sizeof(header)));
	*outRecord = record;
	*outLen = length;
	return 0;
}

/* Encode a SET_INODE record with a fixed portable payload. */
int hfsMetaV5EncodeInodeRecord(uint64_t txid, uint64_t lsn, uint32_t index,
	const struct hfs_inode *inode, uint8_t **outRecord, size_t *outLen) {
	uint8_t payload[4U + HFS_META_V5_INODE_SIZE];

	if (inode == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(payload, 0, sizeof(payload));
	v5Put32(payload, index);
	v5EncodeInode(payload + 4, inode);
	return v5EncodeRecord(HFS_META_V5_WAL_SET_INODE, txid, lsn,
		payload, sizeof(payload), outRecord, outLen);
}

/* Encode a commit marker whose payload records the transaction size. */
int hfsMetaV5EncodeCommitRecord(uint64_t txid, uint64_t lsn,
	uint32_t recordCount, uint8_t **outRecord, size_t *outLen) {
	uint8_t payload[4];

	v5Put32(payload, recordCount);
	return v5EncodeRecord(HFS_META_V5_WAL_COMMIT, txid, lsn,
		payload, sizeof(payload), outRecord, outLen);
}
