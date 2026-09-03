#include <errno.h>
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

/* SPDK thread that runs all async blobstore operations. */
struct spdk_thread *globalThread = NULL;

/*
 * Turn an async SPDK operation into a synchronous call.
 * Posts startFn to the SPDK thread, then polls the thread until the
 * callback sets *finished, or until WAITER_MAX_TIME is exceeded.
 * Returns false on timeout.
 */
bool waiter(struct spdk_thread *thread, spdk_msg_fn startFn, void *ctx, bool *finished) {
	spdk_thread_send_msg(thread, startFn, ctx);

	int waiterCount = 0;

	do {
		spdk_thread_poll(thread, 0, 0);
		waiterCount ++;
	} while(!(*finished) && waiterCount < WAITER_MAX_TIME);

	if (!(*finished) && waiterCount >= WAITER_MAX_TIME) {
		HFS_LOG_ERROR("waiter timed out (msg never completed)");
		return false; /* timeout */
	}

	return true;
}

/* Path of the JSON config describing the bdev (compile-time default). */
#ifdef HFS_DEFAULT_CONFIG
static const char *jsonFile = HFS_DEFAULT_CONFIG;
#else
static const char *jsonFile = "config/hfs.json";
#endif

/* Return the runtime config path or the build-time default. */
static const char *hfs_config_path(void) {
	const char *path = getenv("HFS_CONFIG_PATH");

	return (path != NULL && path[0] != '\0') ? path : jsonFile;
}

/* Completion state for asynchronous subsystem configuration loading. */
struct hfs_config_load_ctx {
	bool done;
	int rc;
};

/* Completion callback for subsystem (JSON config) loading. */
static void jsonAppLoadDone(int rc, void *ctx) {
	struct hfs_config_load_ctx *state = ctx;
	state->rc = rc;
	state->done = true;

	SPDK_NOTICELOG("jsonAppLoadDone\n");
}

/*
 * Called after spdk_subsystem_init completes: load the JSON config
 * (e.g. create the malloc bdev) through SPDK's JSON-RPC mechanism.
 */
static void hfsSubsystemInitDone(int rc, void *arg) {
	struct hfs_config_load_ctx *state = arg;
	void *json;
	size_t jsonSize;

	if (rc != 0) {
		jsonAppLoadDone(rc, state);
		return;
	}

	const char *configPath = hfs_config_path();

	json = spdk_posix_file_load_from_name(configPath, &jsonSize);
	if (json == NULL) {
		SPDK_ERRLOG("Could not read JSON config file: %s\n", configPath);
		jsonAppLoadDone(-EINVAL, state);
		return;
	}

	spdk_rpc_set_state(SPDK_RPC_RUNTIME);

	spdk_subsystem_load_config(json, (ssize_t)jsonSize,
				   jsonAppLoadDone, arg, true);
	free(json);
}

/* SPDK-thread task: initialize all SPDK subsystems. */
static void hfsJsonLoadFn(void *arg) {
	spdk_subsystem_init(hfsSubsystemInitDone, arg);
}

/*
 * Set up the SPDK environment: DPDK env init, log levels, the global
 * SPDK thread, and subsystem/JSON config loading.
 */
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

	struct hfs_config_load_ctx state = { .done = false, .rc = -EINVAL };

	if (!waiter(globalThread, hfsJsonLoadFn, &state, &state.done)) {
		return -1;
	}
	SPDK_NOTICELOG("jsonAppLoadDone complete\n");

	return state.rc == 0 ? 0 : -1;
}
