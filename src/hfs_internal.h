#ifndef HFS_INTERNAL_H
#define HFS_INTERNAL_H

#include <spdk/thread.h>

#include "hfs.h"

/*
 * Upper bound for waiter polling iterations (timeout guard). Kept
 * modest so a lost/starved SPDK message fails fast instead of
 * stalling a process exit for ~110s (MariaDB signal-thread hang).
 */
#define WAITER_MAX_TIME 5000000

/* Global SPDK thread that executes all blobstore operations. */
extern struct spdk_thread *globalThread;

/*
 * Synchronously run an async SPDK operation: post startFn to the SPDK
 * thread, poll until *finished is set, or timeout.
 */
bool waiter(struct spdk_thread *thread, spdk_msg_fn startFn, void *ctx, bool *finished);

#endif /* HFS_INTERNAL_H */