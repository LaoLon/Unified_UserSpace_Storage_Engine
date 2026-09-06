#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hfs_meta_v5.h"

/* Compare the persistent inode fields covered by the v5 format. */
static int inodeEqual(const struct hfs_inode *left, const struct hfs_inode *right) {
	return left->id == right->id && left->type == right->type &&
		left->parentId == right->parentId && left->blobId == right->blobId &&
		left->mode == right->mode && left->uid == right->uid &&
		left->gid == right->gid && left->size == right->size &&
		left->atimeNs == right->atimeNs && left->mtimeNs == right->mtimeNs &&
		left->ctimeNs == right->ctimeNs &&
		memcmp(left->name, right->name, FILENAME_LENGTH) == 0;
}

/* Return a non-zero result when the explicit format contract fails. */
static int requireCondition(int condition, const char *message) {
	if (!condition) {
		fprintf(stderr, "metadata_v5_format_test: %s (errno=%d)\n", message,
			errno);
		return -1;
	}
	return 0;
}

/* Exercise checkpoint round-trip, committed WAL replay, torn WAL, and checksums. */
int main(void) {
	Hfs source = {0};
	Hfs restored = {0};
	Hfs replayed = {0};
	Hfs torn = {0};
	HfsMetaV5Info info;
	uint8_t *image = NULL;
	uint8_t *wal = NULL;
	uint8_t *inodeRecord = NULL;
	uint8_t *commitRecord = NULL;
	uint8_t *corrupt = NULL;
	size_t imageLen = 0;
	size_t inodeRecordLen = 0;
	size_t commitRecordLen = 0;
	size_t walLen;
	uint64_t appliedLsn = 0;
	int result = EXIT_FAILURE;

	source.inodes = calloc(HFS_MAX_INODES, sizeof(*source.inodes));
	restored.inodes = calloc(HFS_MAX_INODES, sizeof(*restored.inodes));
	replayed.inodes = calloc(HFS_MAX_INODES, sizeof(*replayed.inodes));
	torn.inodes = calloc(HFS_MAX_INODES, sizeof(*torn.inodes));
	if (requireCondition(source.inodes != NULL && restored.inodes != NULL &&
		replayed.inodes != NULL && torn.inodes != NULL, "allocate inode tables") != 0) {
		goto out;
	}
	source.inodeCount = 3;
	source.nextInodeId = 4;
	source.inodes[0] = (struct hfs_inode){
		.id = 1, .type = HFS_INODE_DIR, .mode = 0755, .uid = 1000,
		.gid = 1000, .size = 0, .atimeNs = 11, .mtimeNs = 12,
		.ctimeNs = 13, .name = "/"};
	source.inodes[1] = (struct hfs_inode){
		.id = 2, .type = HFS_INODE_FILE, .parentId = 1, .blobId = 77,
		.mode = 0640, .uid = 1000, .gid = 1001, .size = 8192,
		.atimeNs = 21, .mtimeNs = 22, .ctimeNs = 23, .name = "alpha"};
	source.inodes[2] = (struct hfs_inode){
		.id = 3, .type = HFS_INODE_FILE, .parentId = 1, .blobId = 78,
		.mode = 0600, .uid = 1002, .gid = 1002, .size = 4096,
		.atimeNs = 31, .mtimeNs = 32, .ctimeNs = 33, .name = "beta"};

	if (requireCondition(hfsMetaV5BuildImage(&source, 7, 10, NULL, 0,
		&image, &imageLen) == 0, "build checkpoint") != 0 ||
		requireCondition(hfsMetaV5Inspect(image, imageLen, &info) == 0 &&
		info.generation == 7 && info.checkpointLsn == 10 &&
		info.inodeCount == source.inodeCount && info.superAValid &&
		info.superBValid, "inspect checkpoint") != 0 ||
		requireCondition(hfsMetaV5Load(&restored, image, imageLen,
		&appliedLsn) == 0 && appliedLsn == 10 &&
		restored.inodeCount == source.inodeCount &&
		inodeEqual(&source.inodes[1], &restored.inodes[1]),
		"load checkpoint") != 0) {
		goto out;
	}

	source.inodes[1].size = 16384;
	source.inodes[1].mtimeNs = 99;
	if (requireCondition(hfsMetaV5EncodeInodeRecord(42, 11, 1,
		&source.inodes[1], &inodeRecord, &inodeRecordLen) == 0 &&
		hfsMetaV5EncodeCommitRecord(42, 12, 1, &commitRecord,
		&commitRecordLen) == 0, "encode WAL records") != 0) {
		goto out;
	}
	walLen = inodeRecordLen + commitRecordLen;
	wal = calloc(1, walLen);
	if (requireCondition(wal != NULL, "allocate WAL") != 0) {
		goto out;
	}
	memcpy(wal, inodeRecord, inodeRecordLen);
	memcpy(wal + inodeRecordLen, commitRecord, commitRecordLen);
	free(image);
	image = NULL;
	if (requireCondition(hfsMetaV5BuildImage(&restored, 8, 10, wal, walLen,
		&image, &imageLen) == 0 &&
		hfsMetaV5Load(&replayed, image, imageLen, &appliedLsn) == 0 &&
		appliedLsn == 12 && replayed.inodes[1].size == 16384 &&
		replayed.inodes[1].mtimeNs == 99, "replay committed WAL") != 0) {
		goto out;
	}

	free(image);
	image = NULL;
	if (requireCondition(hfsMetaV5BuildImage(&restored, 9, 10, inodeRecord,
		inodeRecordLen, &image, &imageLen) == 0 &&
		hfsMetaV5Load(&torn, image, imageLen, &appliedLsn) == 0 &&
		torn.inodes[1].size == restored.inodes[1].size && appliedLsn == 10,
		"ignore incomplete WAL") != 0) {
		goto out;
	}

	corrupt = malloc(imageLen);
	if (requireCondition(corrupt != NULL, "allocate corruption image") != 0) {
		goto out;
	}
	memcpy(corrupt, image, imageLen);
	corrupt[HFS_META_V5_DATA_OFFSET + HFS_META_V5_PAGE_HEADER_SIZE + 3] ^= 0x5a;
	if (requireCondition(hfsMetaV5Inspect(corrupt, imageLen, &info) != 0,
		"reject corrupt checkpoint page") != 0) {
		goto out;
	}
	memcpy(corrupt, image, imageLen);
	corrupt[HFS_META_V5_SUPER_A_OFFSET + 17] ^= 0x01;
	if (requireCondition(hfsMetaV5Inspect(corrupt, imageLen, &info) == 0 &&
		!info.superAValid && info.superBValid, "fall back to superblock B") != 0) {
		goto out;
	}
	result = EXIT_SUCCESS;
out:
	free(corrupt);
	free(commitRecord);
	free(inodeRecord);
	free(wal);
	free(image);
	free(source.inodes);
	free(restored.inodes);
	free(replayed.inodes);
	free(torn.inodes);
	return result;
}
