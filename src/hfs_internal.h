#ifndef HFS_INTERNAL_H
#define HFS_INTERNAL_H

#include <spdk/thread.h>

#include "hfs.h"
#include "hfs_runtime.h"

/* Global SPDK thread owned and polled by the runtime owner pthread. */
extern struct spdk_thread *globalThread;

/*
 * Submit an async SPDK operation to the owner thread and wait for its
 * request-level completion without polling from the caller thread.
 */
bool waiter(struct spdk_thread *thread, spdk_msg_fn startFn, void *ctx,
		HfsCompletion *completion);

/* Initialize, complete, and wait for a request-private SPDK result. */
void hfsRequestInit(HfsRequestResult *result, HfsRequestOperation operation);
void hfsRequestComplete(HfsRequestResult *result, int bserrno,
			uint64_t completedUnits);
int hfsRequestWait(HfsRequestResult *result, spdk_msg_fn startFn, void *ctx);
/* Submit a request without waiting so a parent can keep child I/O in flight. */
int hfsRequestSubmitBatch(HfsRequestResult *result, spdk_msg_fn startFn,
			 void *ctx, HfsRuntimeBatchItem *batchItem);
/* Wait for a previously submitted request and translate its completion error. */
int hfsRequestWaitResult(HfsRequestResult *result);
int hfsRequestWaitBatch(HfsRequestResult *result, spdk_msg_fn startFn, void *ctx,
			HfsRuntimeBatchItem *batchItem);
bool hfsRequestCompletionInjected(HfsRequestOperation operation);

#endif /* HFS_INTERNAL_H */
