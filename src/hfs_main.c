#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spdk/log.h>

#include "hfs.h"
#include "hfs_internal.h"
#include "hfs_lib.h"
#include "hfs_log.h"
#include "hfs_meta.h"

/*
 * Self-test program: exercises the full HFS lifecycle.
 * Runs mount -> open -> write -> offset read/write -> close -> umount,
 * plus metadata (super blob + inode table) persistence checks.
 */
int main(int argc, char *argv[]) {
	if (hfs_lib_init() != 0) {
		fprintf(stderr, "hfs_lib_init failed\n");
		return -1;
	}

	Hfs *fs = hfs_lib_get_fs();

	/*
	 * Reload the super blob id within the same process: verifies that
	 * set_super persisted (malloc bdev loses data across restarts).
	 */
	int loadRc = hfsMetaLoad(fs);
	SPDK_NOTICELOG("reload meta rc=%d\n", loadRc);
	HFS_LOG_INFO("reload meta rc=%d", loadRc);

	/*
	 * Inode table round-trip: add an inode -> saveAll -> reset the
	 * in-memory table -> loadAll should restore it from the blob.
	 */
	struct hfs_inode *in = hfsMetaAddInode(fs);
	if (in != NULL) {
		in->type = HFS_INODE_DIR;
		in->parentId = 0;
		in->blobId = SPDK_BLOBID_INVALID;
		snprintf(in->name, sizeof(in->name), "rootdir");
	}
	if (hfsMetaSaveAll(fs) != 0) {
		SPDK_NOTICELOG("saveAll failed\n");
		return -1;
	}
	fs->inodeCount = 0;
	if (hfsMetaLoadAll(fs) != 0) {
		SPDK_NOTICELOG("loadAll failed\n");
		return -1;
	}
	SPDK_NOTICELOG("inodeCount=%u first=%s id=%llu\n", fs->inodeCount,
		       fs->inodeCount > 0 ? fs->inodes[0].name : "?",
		       (unsigned long long)(fs->inodeCount > 0 ? fs->inodes[0].id : 0));
	HFS_LOG_INFO("inodeCount=%u first=%s id=%llu", fs->inodeCount,
		      fs->inodeCount > 0 ? fs->inodes[0].name : "?",
		      (unsigned long long)(fs->inodeCount > 0 ? fs->inodes[0].id : 0));

	SPDK_NOTICELOG("hfsEnvSetup success\n");

	SPDK_NOTICELOG("\n\n hfs mount start \n\n");
	HFS_LOG_INFO("hfs mount start");
	hfsMount(fs);

	SPDK_NOTICELOG("\n\n hfs open start \n\n");
	HFS_LOG_INFO("hfs open start");
	HfsFile *file = calloc(1, sizeof(HfsFile));
	file->fs = fs;
	hfsOpen(file);

	SPDK_NOTICELOG("\n\n hfs write start \n\n");
	HFS_LOG_INFO("hfs write start");
	char *buffer = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	hfsWrite(file, buffer, strlen(buffer));

	SPDK_NOTICELOG("\n\n hfs offset write/read start \n\n");
	HFS_LOG_INFO("hfs offset write/read start");
	hfsWriteAt(file, (uint8_t *)"BBBB", 4, 100);
	char oBuffer[16] = {0};
	ssize_t on = hfsReadAt(file, (uint8_t *)oBuffer, 4, 100);
	SPDK_NOTICELOG("OFFSET READ: %.*s (n=%zd)\n", (int)on, oBuffer, on);
	HFS_LOG_INFO("OFFSET READ: %.*s (n=%zd)", (int)on, oBuffer, on);

	SPDK_NOTICELOG("\n\n hfs read start \n\n");
	HFS_LOG_INFO("hfs read start");
	char rBuffer[BUFFER_SIZE] = {0};
	hfsReadAt(file, rBuffer, strlen(buffer), 0);
	SPDK_NOTICELOG("READ BUFFER: %s\n", rBuffer);
	HFS_LOG_INFO("READ BUFFER: %s", rBuffer);

	SPDK_NOTICELOG("\n\n hfs close start \n\n");
	HFS_LOG_INFO("hfs close start");
	hfsClose(file);

	free(file);

	SPDK_NOTICELOG("\n\n hfs umount start \n\n");
	HFS_LOG_INFO("hfs umount start");
	hfsUmount(fs);

	HFS_LOG_INFO("hfs exiting");
	hfs_lib_fini();
}