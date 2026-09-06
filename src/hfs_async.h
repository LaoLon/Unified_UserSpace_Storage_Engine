#ifndef HFS_ASYNC_H
#define HFS_ASYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>

/* Opaque request context retained until the caller releases it. */
typedef struct HfsAsyncRequest HfsAsyncRequest;

/* Completion callback delivered by an HFS async worker. */
typedef void (*HfsAsyncCallback)(HfsAsyncRequest *request, ssize_t result,
					int error, void *context);

/* Start and stop the bounded async request executor. */
int hfsAsyncInit(void);
void hfsAsyncFini(void);

/* Submit positioned data operations without blocking the caller. */
int hfsAsyncPread(int fd, void *buffer, size_t count, off_t offset,
			  HfsAsyncCallback callback, void *context,
			  HfsAsyncRequest **requestOut);
int hfsAsyncPwrite(int fd, const void *buffer, size_t count, off_t offset,
			   HfsAsyncCallback callback, void *context,
			   HfsAsyncRequest **requestOut);
int hfsAsyncPreadv(int fd, const struct iovec *iov, int iovcnt, off_t offset,
			   HfsAsyncCallback callback, void *context,
			   HfsAsyncRequest **requestOut);
int hfsAsyncPwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset,
			    HfsAsyncCallback callback, void *context,
			    HfsAsyncRequest **requestOut);
int hfsAsyncFsync(int fd, HfsAsyncCallback callback, void *context,
			 HfsAsyncRequest **requestOut);

/* Cancel callback delivery; an already-running operation is allowed to finish. */
int hfsAsyncCancel(HfsAsyncRequest *request);
/* Wait for completion without consuming the request's caller reference. */
int hfsAsyncWait(HfsAsyncRequest *request, uint32_t timeoutMs);
/* Read the completed operation result after hfsAsyncWait succeeds. */
int hfsAsyncResult(HfsAsyncRequest *request, ssize_t *resultOut,
			   int *errorOut);
/* Release the caller-owned request reference. */
void hfsAsyncRelease(HfsAsyncRequest *request);

/* Create a worker-free request object for the checked libhfs direct path. */
int hfsAsyncCreateDirect(HfsAsyncCallback callback, void *context,
			 HfsAsyncRequest **requestOut);
/* Account a direct request only after its low-level parent was accepted. */
void hfsAsyncRecordDirectSubmit(HfsAsyncRequest *request);
/* Complete a worker-free request from an SPDK child-parent callback. */
void hfsAsyncCompleteDirect(HfsAsyncRequest *request, ssize_t result,
			    int error);
/* Discard a direct request when the low-level path elects executor fallback. */
void hfsAsyncDiscardDirect(HfsAsyncRequest *request);

#endif /* HFS_ASYNC_H */
