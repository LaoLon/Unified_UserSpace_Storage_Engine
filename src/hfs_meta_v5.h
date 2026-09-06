#ifndef HFS_META_V5_H
#define HFS_META_V5_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hfs_meta.h"

/* Explicit v5 format constants; no C struct is persisted directly. */
#define HFS_META_V5_MAGIC 0x35534648U
#define HFS_META_V5_VERSION 5U
#define HFS_META_V5_PAGE_SIZE 4096U
#define HFS_META_V5_SUPER_SIZE 256U
#define HFS_META_V5_SUPER_A_OFFSET 0U
#define HFS_META_V5_SUPER_B_OFFSET HFS_META_V5_PAGE_SIZE
#define HFS_META_V5_DATA_OFFSET (2U * HFS_META_V5_PAGE_SIZE)
#define HFS_META_V5_PAGE_HEADER_SIZE 32U
#define HFS_META_V5_INODE_SIZE 208U
#define HFS_META_V5_PAGE_PAYLOAD_SIZE \
	(HFS_META_V5_PAGE_SIZE - HFS_META_V5_PAGE_HEADER_SIZE)
#define HFS_META_V5_INODES_PER_PAGE \
	(HFS_META_V5_PAGE_PAYLOAD_SIZE / HFS_META_V5_INODE_SIZE)
#define HFS_META_V5_WAL_HEADER_SIZE 40U
#define HFS_META_V5_WAL_MAGIC 0x4c574648U
#define HFS_META_V5_WAL_VERSION 1U

/* Page types are explicit values so future page families can coexist. */
enum HfsMetaV5PageType {
	HFS_META_V5_PAGE_INODES = 1,
};

/* WAL record types are replayed only after a matching commit marker. */
enum HfsMetaV5WalType {
	HFS_META_V5_WAL_SET_INODE = 1,
	HFS_META_V5_WAL_COMMIT = 0xffff,
};

/* Validated superblock state returned by the offline parser. */
typedef struct HfsMetaV5Info {
	uint32_t features;
	uint64_t generation;
	uint64_t checkpointLsn;
	uint64_t walHead;
	uint64_t walTail;
	uint64_t rootPage;
	uint64_t nextInodeId;
	uint64_t walOffset;
	uint64_t imageBytes;
	uint32_t inodeCount;
	uint32_t pageCount;
	bool superAValid;
	bool superBValid;
} HfsMetaV5Info;

/* Build a complete explicit v5 image with an optional WAL byte stream. */
int hfsMetaV5BuildImage(const Hfs *fs, uint64_t generation,
	uint64_t checkpointLsn, const uint8_t *wal, size_t walLen,
	uint8_t **outImage, size_t *outLen);

/* Validate both superblock candidates and all checkpoint pages. */
int hfsMetaV5Inspect(const uint8_t *image, size_t imageLen,
	HfsMetaV5Info *info);

/* Load a validated checkpoint and replay only committed WAL transactions. */
int hfsMetaV5Load(Hfs *fs, const uint8_t *image, size_t imageLen,
	uint64_t *lastAppliedLsn);

/* Encode a SET_INODE WAL record with an explicit fixed-size inode payload. */
int hfsMetaV5EncodeInodeRecord(uint64_t txid, uint64_t lsn, uint32_t index,
	const struct hfs_inode *inode, uint8_t **outRecord, size_t *outLen);

/* Encode a transaction commit marker. */
int hfsMetaV5EncodeCommitRecord(uint64_t txid, uint64_t lsn,
	uint32_t recordCount, uint8_t **outRecord, size_t *outLen);

#endif /* HFS_META_V5_H */
