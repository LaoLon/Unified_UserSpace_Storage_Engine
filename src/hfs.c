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

/* Monotonic identifiers let diagnostics correlate one submit and completion. */
static _Atomic uint64_t g_hfsRequestId;

/* Return the stable test-injection label for one request operation. */
static const char *hfsRequestOperationName(HfsRequestOperation operation) {
	switch (operation) {
	case HFS_REQUEST_BLOB_CREATE:
		return "blob-create";
	case HFS_REQUEST_BLOB_OPEN:
		return "blob-open";
	case HFS_REQUEST_BLOB_CLOSE:
		return "blob-close";
	case HFS_REQUEST_BLOB_DELETE:
		return "blob-delete";
	case HFS_REQUEST_BLOB_RESIZE:
		return "blob-resize";
	case HFS_REQUEST_BLOB_READ:
		return "blob-read";
	case HFS_REQUEST_BLOB_WRITE:
		return "blob-write";
	case HFS_REQUEST_BLOB_UNMAP:
		return "blob-unmap";
	case HFS_REQUEST_BLOB_WRITE_ZEROES:
		return "blob-write-zeroes";
	case HFS_REQUEST_BLOB_SYNC:
		return "blob-sync";
	case HFS_REQUEST_BDEV_FLUSH:
		return "bdev-flush";
	case HFS_REQUEST_BDEV_FLUSH_POST_COMMIT:
		return "bdev-flush-post-commit";
	case HFS_REQUEST_METADATA_OPEN:
		return "metadata-open";
	case HFS_REQUEST_METADATA_READ:
		return "metadata-read";
	case HFS_REQUEST_METADATA_WRITE:
		return "metadata-write";
	case HFS_REQUEST_METADATA_FORMAT:
		return "metadata-format";
	case HFS_REQUEST_METADATA_GET_SUPER:
		return "metadata-get-super";
	case HFS_REQUEST_MOUNT:
		return "mount";
	case HFS_REQUEST_UMOUNT:
		return "umount";
	case HFS_REQUEST_CAPACITY:
		return "capacity";
	default:
		return "unknown";
	}
}

/* Enable deterministic pre-submit failures only when an explicit test env is set. */
static bool hfsRequestSubmitInjected(HfsRequestOperation operation) {
	const char *requested = getenv("HFS_TEST_FAIL_SUBMIT");

	return requested != NULL &&
		(strcmp(requested, "all") == 0 ||
			 strcmp(requested, hfsRequestOperationName(operation)) == 0);
}

/* Enable deterministic callback failures only when an explicit test env is set. */
bool hfsRequestCompletionInjected(HfsRequestOperation operation) {
	const char *requested = getenv("HFS_TEST_FAIL_COMPLETE");

	return requested != NULL &&
		(strcmp(requested, "all") == 0 ||
		 strcmp(requested, hfsRequestOperationName(operation)) == 0);
}

/* Convert an SPDK-style result into the public POSIX errno convention. */
static int hfsRequestSetError(int rc) {
	int error = rc < 0 ? -rc : rc;

	errno = error > 0 && error < 4096 ? error : EIO;
	return -1;
}

/* Initialize one result object before it is submitted to the owner thread. */
void hfsRequestInit(HfsRequestResult *result, HfsRequestOperation operation) {
	memset(result, 0, sizeof(*result));
	result->operation = operation;
	result->requestId = atomic_fetch_add_explicit(&g_hfsRequestId, 1,
						memory_order_relaxed) + 1;
	result->completionRc = -EIO;
	result->bserrno = -EIO;
}

/* Store the SPDK completion result before waking the waiting caller. */
void hfsRequestComplete(HfsRequestResult *result, int bserrno,
			uint64_t completedUnits) {
	result->bserrno = bserrno;
	result->completionRc = bserrno;
	result->completedUnits = completedUnits;
	hfsRuntimeCompletionComplete(&result->completion);
}

/* Submit one request-private operation and expose its exact completion error. */
int hfsRequestWait(HfsRequestResult *result, spdk_msg_fn startFn, void *ctx) {
	int rc;

	hfsRuntimeCompletionReset(&result->completion);
	if (hfsRequestSubmitInjected(result->operation)) {
		result->submitRc = -EIO;
		return hfsRequestSetError(result->submitRc);
	}
	rc = hfsRuntimeSubmit(startFn, ctx);
	result->submitRc = rc;
	if (rc != 0) {
		return hfsRequestSetError(rc);
	}
	if (!hfsRuntimeCompletionWait(&result->completion,
				      HFS_RUNTIME_TIMEOUT_MS)) {
		return -1;
	}
	if (result->completionRc != 0) {
		return hfsRequestSetError(result->completionRc);
	}
	return 0;
}

/* Submit a request through the owner-side batch dispatcher without waiting. */
int hfsRequestSubmitBatch(HfsRequestResult *result, spdk_msg_fn startFn,
			 void *ctx, HfsRuntimeBatchItem *batchItem) {
	int rc;

	hfsRuntimeCompletionReset(&result->completion);
	if (hfsRequestSubmitInjected(result->operation)) {
		result->submitRc = -EIO;
		return hfsRequestSetError(result->submitRc);
	}
	rc = hfsRuntimeSubmitBatch(batchItem, startFn, ctx);
	result->submitRc = rc;
	if (rc != 0) {
		return hfsRequestSetError(rc);
	}
	return 0;
}

/* Submit one data child to a selected reactor without waiting for completion. */
int hfsRequestSubmitData(HfsRequestResult *result, spdk_msg_fn startFn,
			 void *ctx, uint32_t reactorId) {
	int rc;

	hfsRuntimeCompletionReset(&result->completion);
	if (hfsRequestSubmitInjected(result->operation)) {
		result->submitRc = -EIO;
		return hfsRequestSetError(result->submitRc);
	}
	rc = hfsRuntimeSubmitData(reactorId, startFn, ctx);
	result->submitRc = rc;
	if (rc != 0) {
		return hfsRequestSetError(rc);
	}
	return 0;
}

/* Submit a data child through the selected reactor's coalescing queue. */
int hfsRequestSubmitDataBatch(HfsRequestResult *result, spdk_msg_fn startFn,
				      void *ctx, uint32_t reactorId,
				      HfsRuntimeBatchItem *batchItem) {
	int rc;

	hfsRuntimeCompletionReset(&result->completion);
	if (hfsRequestSubmitInjected(result->operation)) {
		result->submitRc = -EIO;
		return hfsRequestSetError(result->submitRc);
	}
	rc = hfsRuntimeSubmitDataBatch(reactorId, batchItem, startFn, ctx);
	result->submitRc = rc;
	if (rc != 0) {
		return hfsRequestSetError(rc);
	}
	return 0;
}

/* Wait for a submitted request and translate timeout or SPDK errors. */
int hfsRequestWaitResult(HfsRequestResult *result) {
	if (!hfsRuntimeCompletionWait(&result->completion,
				      HFS_RUNTIME_TIMEOUT_MS)) {
		return -1;
	}
	if (result->completionRc != 0) {
		return hfsRequestSetError(result->completionRc);
	}
	return 0;
}

/* Submit a data-path request through the owner-side batch dispatcher. */
int hfsRequestWaitBatch(HfsRequestResult *result, spdk_msg_fn startFn, void *ctx,
			HfsRuntimeBatchItem *batchItem) {
	int rc = hfsRequestSubmitBatch(result, startFn, ctx, batchItem);

	if (rc != 0) {
		return rc;
	}
	return hfsRequestWaitResult(result);
}

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
