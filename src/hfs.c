#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spdk/env.h>
#include <spdk/event.h>
#include <spdk/file.h>
#include <spdk/init.h>
#include <spdk/log.h>
#include <spdk/rpc.h>
#include <spdk/thread.h>

#include "hfs_internal.h"
#include "hfs_log.h"

struct spdk_thread *globalThread = NULL;

bool waiter(struct spdk_thread *thread, spdk_msg_fn startFn, void *ctx, bool *finished) {
	spdk_thread_send_msg(thread, startFn, ctx);

	int waiterCount = 0;

	do {
		spdk_thread_poll(thread, 0, 0);
		waiterCount ++;
	} while(!(*finished) && waiterCount < WAITER_MAX_TIME);

	if (!(*finished) && waiterCount >= WAITER_MAX_TIME) {
		return false; // timeout
	}

	return true;
}

// zvfs.json

#ifdef HFS_DEFAULT_CONFIG
static const char *jsonFile = HFS_DEFAULT_CONFIG;
#else
static const char *jsonFile = "config/hfs.json";
#endif

static void jsonAppLoadDone(int rc, void *ctx) {
	bool *done = ctx;
	*done = true;

	SPDK_NOTICELOG("jsonAppLoadDone\n");
}

static void hfsSubsystemInitDone(int rc, void *arg) {
	void *json;
	size_t jsonSize;

	if (rc != 0) {
		jsonAppLoadDone(rc, arg);
		return;
	}

	json = spdk_posix_file_load_from_name(jsonFile, &jsonSize);
	if (json == NULL) {
		SPDK_ERRLOG("Could not read JSON config file: %s\n", jsonFile);
		jsonAppLoadDone(-EINVAL, arg);
		return;
	}

	spdk_rpc_set_state(SPDK_RPC_RUNTIME);

	spdk_subsystem_load_config(json, (ssize_t)jsonSize,
				   jsonAppLoadDone, arg, true);
	free(json);
}

static void hfsJsonLoadFn(void *arg) {
	spdk_subsystem_init(hfsSubsystemInitDone, arg);
}

int hfsEnvSetup(void) {
	struct spdk_env_opts opts;
	spdk_env_opts_init(&opts);

	if (0 != spdk_env_init(&opts)) {
		return -1;
	}

	spdk_log_set_print_level(SPDK_LOG_NOTICE);
	spdk_log_set_level(SPDK_LOG_NOTICE);
	spdk_log_open(NULL);

	spdk_thread_lib_init(NULL, 0);
	globalThread = spdk_thread_create("global", NULL);
	spdk_set_thread(globalThread);

	bool done = false;

	waiter(globalThread, hfsJsonLoadFn, &done, &done);
	SPDK_NOTICELOG("jsonAppLoadDone complete\n");

	return 0;
}

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