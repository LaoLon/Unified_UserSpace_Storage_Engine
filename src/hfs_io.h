#ifndef HFS_IO_H
#define HFS_IO_H

#include <sys/uio.h>

#include "hfs.h"

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
/* Punch a byte range, using unmap for aligned units and zeroing edge units. */
int hfsIoPunchHole(HfsFile *file, uint64_t offset, uint64_t length);
/* Punch/zero a range while the caller already holds the inode sync write lock. */
int hfsIoPunchHoleLocked(HfsFile *file, uint64_t offset, uint64_t length);
/* Clear a byte range so a later file-size increase can never expose stale data. */
int hfsIoZeroRange(HfsFile *file, uint64_t offset, uint64_t length);
/* Zero a range while the caller already holds the inode sync write lock. */
int hfsIoZeroRangeLocked(HfsFile *file, uint64_t offset, uint64_t length);

#endif /* HFS_IO_H */
