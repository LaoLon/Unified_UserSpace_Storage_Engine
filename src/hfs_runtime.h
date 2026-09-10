#ifndef HFS_RUNTIME_H
#define HFS_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include <spdk/thread.h>

#include "hfs.h"

/* Maximum time a synchronous API waits for one SPDK request. */
#define HFS_RUNTIME_TIMEOUT_MS 5000

/* Start the dedicated SPDK owner pthread and its SPDK thread. */
int hfsRuntimeStart(void);
/* Submit one message to the dedicated SPDK owner thread. */
int hfsRuntimeSubmit(spdk_msg_fn fn, void *ctx);
/* Submit a data-path message to a selected per-reactor SPDK thread. */
int hfsRuntimeSubmitData(uint32_t reactorId, spdk_msg_fn fn, void *ctx);
/* One request queued into the owner-side batch dispatcher. */
typedef struct HfsRuntimeBatchItem {
	spdk_msg_fn fn;
	void *ctx;
	uint64_t enqueueNs;
	uint64_t traceParentId;
	HfsCompletion *completion;
	struct HfsRuntimeBatchItem *next;
} HfsRuntimeBatchItem;
/* Queue one request and coalesce concurrent submissions into one owner message. */
int hfsRuntimeSubmitBatch(HfsRuntimeBatchItem *item, spdk_msg_fn fn, void *ctx);
/* Queue one request and coalesce concurrent submissions on a data reactor. */
int hfsRuntimeSubmitDataBatch(uint32_t reactorId, HfsRuntimeBatchItem *item,
				      spdk_msg_fn fn, void *ctx);
/* Allocate and release per-reactor blobstore channels after mount. */
int hfsRuntimeDataInit(Hfs *fs);
int hfsRuntimeDataFini(Hfs *fs);
/* Select the stable reactor/channel for one blob stripe and logical unit. */
uint32_t hfsRuntimeDataReactorFor(const Hfs *fs, uint64_t blobId,
					 uint64_t logicalUnit);
struct spdk_io_channel *hfsRuntimeDataChannel(const Hfs *fs,
						 uint32_t reactorId);
/* Stop accepting work, drain SPDK state, and join the owner pthread. */
int hfsRuntimeStop(void);
/* Mark subsystem configuration complete so runtime teardown can finalize it. */
void hfsRuntimeMarkSubsystemReady(void);
/* Check whether the caller is the dedicated SPDK owner pthread. */
bool hfsRuntimeIsOwnerThread(void);
/* Return true when the caller is a fork child of the runtime owner process. */
bool hfsRuntimeIsForkChild(void);

/* Reset a request completion before submitting a new operation. */
void hfsRuntimeCompletionReset(HfsCompletion *completion);
/* Complete a request and wake all futex waiters. */
void hfsRuntimeCompletionComplete(HfsCompletion *completion);
/* Wait with a monotonic deadline while retaining the request context on timeout. */
bool hfsRuntimeCompletionWait(HfsCompletion *completion, uint32_t timeoutMs);

#endif /* HFS_RUNTIME_H */
