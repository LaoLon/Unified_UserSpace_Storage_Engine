#ifndef HFS_META_H
#define HFS_META_H

#include <stdint.h>

#include "hfs.h"

#define HFS_SUPER_MAGIC	0x48465342U	/* 'HFSB' */
#define HFS_SUPER_VERSION	1U
#define HFS_MAX_INODES	65536U

#define HFS_INODE_DIR	1
#define HFS_INODE_FILE	2

struct hfs_super_block {
	uint64_t magic;
	uint32_t version;
	uint64_t metaBlobId;
	uint64_t rootInodeId;
	uint32_t checksum;
};

struct hfs_inode {
	uint64_t id;
	uint8_t type;
	uint64_t parentId;
	uint64_t blobId;
	uint64_t size;
	uint64_t mtimeNs;
	uint64_t ctimeNs;
	char name[FILENAME_LENGTH];
};

int hfsMetaInit(Hfs *fs);
int hfsMetaLoad(Hfs *fs);
int hfsMetaFormat(Hfs *fs);
int hfsMetaSave(Hfs *fs);

#endif /* HFS_META_H */