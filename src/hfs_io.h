#ifndef HFS_IO_H
#define HFS_IO_H

#include <sys/uio.h>

#include "hfs.h"

/* Completion callback for a non-blocking positioned data operation. */
typedef void (*HfsIoAsyncCallback)(void *context, ssize_t result, int error);

/* Initialize the bounded DMA buffer pool and data synchronization state. */
int hfsIoInit(Hfs *fs);
/* Stop new pool waiters and release all DMA buffers after I/O has drained. */
void hfsIoFini(Hfs *fs);

/* Read a byte range through request-local DMA buffers. */
ssize_t hfsIoReadAt(HfsFile *file, uint8_t *buffer, size_t count,
		    uint64_t offset);
/* Write a byte range with aligned direct writes and edge read-modify-write. */
ssize_t hfsIoWriteAt(HfsFile *file, const uint8_t *buffer, size_t count,
			     uint64_t offset);
/* Attempt one native SPDK vector read; EAGAIN requests the compatibility fallback. */
ssize_t hfsIoReadvAt(HfsFile *file, const struct iovec *iov, int iovcnt,
			     size_t total, uint64_t offset);
/* Attempt one native SPDK vector write; EAGAIN requests the compatibility fallback. */
ssize_t hfsIoWritevAt(HfsFile *file, const struct iovec *iov, int iovcnt,
			      size_t total, uint64_t offset);
/* Submit an aligned positioned read without occupying an executor worker. */
int hfsIoReadAtAsync(HfsFile *file, uint8_t *buffer, size_t count,
		     uint64_t offset, HfsIoAsyncCallback callback, void *context);
/* Submit an aligned positioned write without occupying an executor worker. */
int hfsIoWriteAtAsync(HfsFile *file, const uint8_t *buffer, size_t count,
		      uint64_t offset, HfsIoAsyncCallback callback, void *context);
/* Punch a byte range, using unmap for aligned units and zeroing edge units. */
int hfsIoPunchHole(HfsFile *file, uint64_t offset, uint64_t length);
/* Punch/zero a range while the caller already holds the inode sync write lock. */
int hfsIoPunchHoleLocked(HfsFile *file, uint64_t offset, uint64_t length);
/* Clear a byte range so a later file-size increase can never expose stale data. */
int hfsIoZeroRange(HfsFile *file, uint64_t offset, uint64_t length);
/* Zero a range while the caller already holds the inode sync write lock. */
int hfsIoZeroRangeLocked(HfsFile *file, uint64_t offset, uint64_t length);

#endif /* HFS_IO_H */
