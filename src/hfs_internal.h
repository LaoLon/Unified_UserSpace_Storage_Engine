#ifndef HFS_INTERNAL_H
#define HFS_INTERNAL_H

#include <spdk/thread.h>

#include "hfs.h"

#define WAITER_MAX_TIME 2000000000

extern struct spdk_thread *globalThread;

bool waiter(struct spdk_thread *thread, spdk_msg_fn startFn, void *ctx, bool *finished);

#endif /* HFS_INTERNAL_H */