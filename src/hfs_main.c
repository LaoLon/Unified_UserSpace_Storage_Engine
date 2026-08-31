#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spdk/log.h>

#include "hfs.h"
#include "hfs_internal.h"
#include "hfs_log.h"

int main(int argc, char *argv[]) {
	if (hfs_log_init() != 0) {
		fprintf(stderr, "hfs_log_init failed\n");
		return -1;
	}
	HFS_LOG_INFO("hfs_log initialized");

	if (hfsEnvSetup()) {
		HFS_LOG_ERROR("hfsEnvSetup failed");
		hfs_log_fini();
		return -1;
	}

	SPDK_NOTICELOG("hfsEnvSetup success\n");

	SPDK_NOTICELOG("\n\n hfs mount start \n\n");
	HFS_LOG_INFO("hfs mount start");
	Hfs *fs = calloc(1, sizeof(Hfs));
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

	SPDK_NOTICELOG("\n\n hfs read start \n\n");
	HFS_LOG_INFO("hfs read start");
	char rBuffer[BUFFER_SIZE] = {0};
	hfsRead(file, rBuffer, BUFFER_SIZE);
	SPDK_NOTICELOG("READ BUFFER: %s\n", rBuffer);
	HFS_LOG_INFO("READ BUFFER: %s", rBuffer);

	SPDK_NOTICELOG("\n\n hfs close start \n\n");
	HFS_LOG_INFO("hfs close start");
	hfsClose(file);

	free(file);

	SPDK_NOTICELOG("\n\n hfs umount start \n\n");
	HFS_LOG_INFO("hfs umount start");
	hfsUmount(fs);
	free(fs);

	HFS_LOG_INFO("hfs exiting");
	hfs_log_fini();
}