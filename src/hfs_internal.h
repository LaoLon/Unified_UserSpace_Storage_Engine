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

#endif /* HFS_INTERNAL_H */
