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

/*
 * Turn an async SPDK operation into a synchronous call.
 * Posts startFn to the dedicated owner thread, then waits on the request
 * completion without polling SPDK from the calling thread.
 */

bool waiter(struct spdk_thread *thread, spdk_msg_fn startFn, void *ctx,
		HfsCompletion *completion) {
	int rc;

	(void)thread;
	hfsRuntimeCompletionReset(completion);
	rc = hfsRuntimeSubmit(startFn, ctx);
	if (rc != 0) {
		errno = -rc;
		return false;
	}

	if (!hfsRuntimeCompletionWait(completion, HFS_RUNTIME_TIMEOUT_MS)) {
		HFS_LOG_ERROR("waiter timed out or runtime stopped");
		return false;
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
	HfsCompletion completion;
	int rc;
};

/* Completion callback for subsystem (JSON config) loading. */
static void jsonAppLoadDone(int rc, void *ctx) {
	struct hfs_config_load_ctx *state = ctx;
	state->rc = rc;

	SPDK_NOTICELOG("jsonAppLoadDone\n");
	hfsRuntimeCompletionComplete(&state->completion);
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
 * Set up the SPDK owner runtime and load the subsystem/JSON configuration.
 */
int hfsEnvSetup(void) {
	struct hfs_config_load_ctx state = { .rc = -EINVAL };

	if (hfsRuntimeStart() != 0) {
		return -1;
	}
	if (!waiter(globalThread, hfsJsonLoadFn, &state, &state.completion)) {
		return -1;
	}
	SPDK_NOTICELOG("jsonAppLoadDone complete\n");

	if (state.rc != 0) {
		return -1;
	}
	hfsRuntimeMarkSubsystemReady();
	return 0;
}
