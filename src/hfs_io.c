#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

#include <spdk/blob.h>
#include <spdk/env.h>

#include "hfs_internal.h"
#include "hfs_io.h"
#include "hfs_log.h"

/* Keep the hot path bounded while covering the largest common database I/O. */
#define HFS_IO_POOL_COUNT	16U
#define HFS_IO_BUFFER_BYTES	(1U << 20)

/* One fixed-size DMA buffer leased by one request at a time. */
typedef struct HfsIoLease {
	struct hfs_io_pool *pool;
	uint32_t slot;
	uint8_t *buffer;
} HfsIoLease;

/* Bounded DMA storage shared by all request-local data I/O. */
struct hfs_io_pool {
	pthread_mutex_t lock;
	pthread_cond_t available;
	uint8_t *storage;
	size_t bufferBytes;
	uint32_t freeTop;
	uint32_t active;
	uint32_t freeSlots[HFS_IO_POOL_COUNT];
	bool closing;
};

/* Operation submitted by one request-local SPDK transfer context. */
typedef enum HfsIoOperation {
	HFS_IO_READ,
	HFS_IO_WRITE,
	HFS_IO_UNMAP,
	HFS_IO_WRITE_ZEROES
} HfsIoOperation;

/* Request state retained until the owner-thread callback completes. */
typedef struct HfsIoRequest {
	HfsFile *file;
	void *buffer;
	uint64_t offset;
	uint64_t units;
	HfsIoOperation operation;
	int operationRc;
	HfsCompletion completion;
} HfsIoRequest;

/* Capture an SPDK completion code before waking the synchronous caller. */
static void hfsIoDone(void *arg, int bserrno);

/* Convert an SPDK error into the errno convention used by the public API. */
static int hfsIoSetError(int rc) {
	int error = rc < 0 ? -rc : rc;

	errno = error == 0 ? EIO : error;
	return -1;
}

/* Return the number of complete I/O units one pool buffer can hold. */
static uint64_t hfsIoMaxUnits(const Hfs *fs) {
	return (uint64_t)(fs->ioPool->bufferBytes / (size_t)fs->unitSize);
}

/* Acquire one bounded DMA buffer, waiting until a request returns one. */
static int hfsIoAcquire(Hfs *fs, HfsIoLease *lease) {
	struct hfs_io_pool *pool = fs->ioPool;

	memset(lease, 0, sizeof(*lease));
	pthread_mutex_lock(&pool->lock);
	while (pool->freeTop == 0 && !pool->closing) {
		pthread_cond_wait(&pool->available, &pool->lock);
	}
	if (pool->closing) {
		pthread_mutex_unlock(&pool->lock);
		errno = ESHUTDOWN;
		return -1;
	}
	lease->pool = pool;
	lease->slot = pool->freeSlots[--pool->freeTop];
	lease->buffer = pool->storage +
		(size_t)lease->slot * pool->bufferBytes;
	pool->active++;
	pthread_mutex_unlock(&pool->lock);
	return 0;
}

/* Return one DMA buffer to the pool and wake the next waiting request. */
static void hfsIoRelease(HfsIoLease *lease) {
	struct hfs_io_pool *pool;

	if (lease == NULL || lease->pool == NULL) {
		return;
	}
	pool = lease->pool;
	pthread_mutex_lock(&pool->lock);
	pool->freeSlots[pool->freeTop++] = lease->slot;
	if (pool->active > 0) {
		pool->active--;
	}
	pthread_cond_broadcast(&pool->available);
	pthread_mutex_unlock(&pool->lock);
	memset(lease, 0, sizeof(*lease));
}

/* Start one SPDK blob operation on the dedicated owner thread. */
static void hfsIoStart(void *arg) {
	HfsIoRequest *request = arg;
	HfsFile *file = request->file;

	switch (request->operation) {
	case HFS_IO_READ:
		spdk_blob_io_read(file->blob, file->fs->channel, request->buffer,
				  request->offset, request->units,
				  hfsIoDone, request);
		break;
	case HFS_IO_WRITE:
		spdk_blob_io_write(file->blob, file->fs->channel, request->buffer,
				   request->offset, request->units,
				   hfsIoDone, request);
		break;
	case HFS_IO_UNMAP:
		spdk_blob_io_unmap(file->blob, file->fs->channel, request->offset,
				   request->units, hfsIoDone, request);
		break;
	case HFS_IO_WRITE_ZEROES:
		spdk_blob_io_write_zeroes(file->blob, file->fs->channel,
					 request->offset, request->units,
					 hfsIoDone, request);
		break;
	}
}

/* Capture an SPDK completion code before waking the synchronous caller. */
static void hfsIoDone(void *arg, int bserrno) {
	HfsIoRequest *request = arg;

	request->operationRc = bserrno;
	if (bserrno != 0) {
		HFS_LOG_ERROR("data I/O failed operation=%d offset=%"PRIu64
			      " units=%"PRIu64" bserrno=%d",
			      request->operation, request->offset, request->units,
			      bserrno);
	}
	hfsRuntimeCompletionComplete(&request->completion);
}

/* Start one request-local transfer and wait for its owner-thread callback. */
static int hfsIoSubmit(HfsFile *file, void *buffer, uint64_t offset,
		       uint64_t units, HfsIoOperation operation) {
	HfsIoRequest request = {
		.file = file,
		.buffer = buffer,
		.offset = offset,
		.units = units,
		.operation = operation,
		.operationRc = -EIO,
	};

	/* The callback owns only this request, so the HfsFile has no I/O scratch state. */
	hfsRuntimeCompletionReset(&request.completion);
	if (hfsRuntimeSubmit(hfsIoStart, &request) != 0) {
		HFS_LOG_ERROR("data I/O submission rejected operation=%d", operation);
		errno = ESHUTDOWN;
		return -1;
	}
	if (!hfsRuntimeCompletionWait(&request.completion,
				      HFS_RUNTIME_TIMEOUT_MS)) {
		HFS_LOG_ERROR("data I/O wait failed operation=%d errno=%d",
			      operation, errno);
		return -1;
	}
	if (request.operationRc != 0) {
		return hfsIoSetError(request.operationRc);
	}
	return 0;
}

/* Initialize the bounded DMA pool and its synchronization primitives. */
int hfsIoInit(Hfs *fs) {
	struct hfs_io_pool *pool;
	size_t bufferBytes;
	size_t storageBytes;
	int rc;
	bool ioLockReady = false;
	bool resizeLockReady = false;

	if (fs == NULL || fs->unitSize == 0 ||
	    (size_t)fs->unitSize != fs->unitSize) {
		errno = EINVAL;
		return -1;
	}
	pool = calloc(1, sizeof(*pool));
	if (pool == NULL) {
		errno = ENOMEM;
		return -1;
	}
	rc = pthread_mutex_init(&pool->lock, NULL);
	if (rc != 0) {
		errno = rc;
		goto fail_pool;
	}
	rc = pthread_cond_init(&pool->available, NULL);
	if (rc != 0) {
		errno = rc;
		goto fail_pool_mutex;
	}
	rc = pthread_rwlock_init(&fs->ioLock, NULL);
	if (rc != 0) {
		errno = rc;
		goto fail_pool_cond;
	}
	ioLockReady = true;
	rc = pthread_mutex_init(&fs->resizeLock, NULL);
	if (rc != 0) {
		errno = rc;
		goto fail_io_lock;
	}
	resizeLockReady = true;

	bufferBytes = HFS_IO_BUFFER_BYTES;
	if (bufferBytes < (size_t)fs->unitSize) {
		bufferBytes = (size_t)fs->unitSize;
	}
	bufferBytes -= bufferBytes % (size_t)fs->unitSize;
	if (bufferBytes == 0 || bufferBytes > SIZE_MAX / HFS_IO_POOL_COUNT) {
		errno = EOVERFLOW;
		goto fail_resize_lock;
	}
	storageBytes = bufferBytes * HFS_IO_POOL_COUNT;
	pool->storage = spdk_malloc(storageBytes, 0x1000, NULL,
				    SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	if (pool->storage == NULL) {
		errno = ENOMEM;
		goto fail_resize_lock;
	}
	pool->bufferBytes = bufferBytes;
	pool->freeTop = HFS_IO_POOL_COUNT;
	for (uint32_t i = 0; i < HFS_IO_POOL_COUNT; i++) {
		pool->freeSlots[i] = i;
	}
	fs->ioPool = pool;
	return 0;

fail_resize_lock:
	if (resizeLockReady) {
		pthread_mutex_destroy(&fs->resizeLock);
	}
fail_io_lock:
	if (ioLockReady) {
		pthread_rwlock_destroy(&fs->ioLock);
	}
fail_pool_cond:
	pthread_cond_destroy(&pool->available);
fail_pool_mutex:
	pthread_mutex_destroy(&pool->lock);
fail_pool:
	free(pool);
	return -1;
}

/* Drain active leases and release the DMA pool during filesystem teardown. */
void hfsIoFini(Hfs *fs) {
	struct hfs_io_pool *pool;

	if (fs == NULL || fs->ioPool == NULL) {
		return;
	}
	pool = fs->ioPool;
	pthread_mutex_lock(&pool->lock);
	pool->closing = true;
	pthread_cond_broadcast(&pool->available);
	while (pool->active != 0) {
		pthread_cond_wait(&pool->available, &pool->lock);
	}
	pthread_mutex_unlock(&pool->lock);
	spdk_free(pool->storage);
	pthread_cond_destroy(&pool->available);
	pthread_mutex_destroy(&pool->lock);
	free(pool);
	fs->ioPool = NULL;
	pthread_mutex_destroy(&fs->resizeLock);
	pthread_rwlock_destroy(&fs->ioLock);
}

/* Read one complete I/O unit into a leased DMA buffer. */
static int hfsIoReadUnit(HfsFile *file, HfsIoLease *lease,
			 uint64_t unitOffset) {
	return hfsIoSubmit(file, lease->buffer, unitOffset, 1, HFS_IO_READ);
}

/* Write one complete I/O unit from a leased DMA buffer. */
static int hfsIoWriteUnit(HfsFile *file, HfsIoLease *lease,
			  uint64_t unitOffset) {
	return hfsIoSubmit(file, lease->buffer, unitOffset, 1, HFS_IO_WRITE);
}

/* Preserve bytes outside an edge and replace only the requested subrange. */
static int hfsIoModifyPartial(HfsFile *file, uint64_t unitOffset,
			       size_t unitIndex, const uint8_t *source,
			       size_t length) {
	HfsIoLease lease;
	int rc;

	if (hfsIoAcquire(file->fs, &lease) != 0) {
		return -1;
	}
	rc = hfsIoReadUnit(file, &lease, unitOffset);
	if (rc == 0) {
		memcpy(lease.buffer + unitIndex, source, length);
		rc = hfsIoWriteUnit(file, &lease, unitOffset);
	}
	hfsIoRelease(&lease);
	return rc;
}

/* Zero an aligned range through pooled full-unit writes. */
static int hfsIoWriteZeroUnits(HfsFile *file, uint64_t offset, uint64_t units) {
	uint64_t maxUnits = hfsIoMaxUnits(file->fs);

	if (maxUnits == 0) {
		errno = EINVAL;
		return -1;
	}
	while (units != 0) {
		HfsIoLease lease;
		uint64_t chunkUnits = units > maxUnits ? maxUnits : units;
		int rc;

		if (hfsIoAcquire(file->fs, &lease) != 0) {
			return -1;
		}
		memset(lease.buffer, 0, (size_t)(chunkUnits * file->fs->unitSize));
		rc = hfsIoSubmit(file, lease.buffer, offset, chunkUnits, HFS_IO_WRITE);
		hfsIoRelease(&lease);
		if (rc != 0) {
			return -1;
		}
		offset += chunkUnits;
		units -= chunkUnits;
	}
	return 0;
}

/* Read a positioned range while the caller holds the filesystem read lock. */
static ssize_t hfsIoReadAtLocked(HfsFile *file, uint8_t *buffer,
				 size_t count, uint64_t offset) {
	uint64_t unitSize = file->fs->unitSize;
	uint64_t cursor = offset;
	size_t remaining = count;
	size_t done = 0;

	if (cursor % unitSize != 0) {
		uint64_t unitOffset = cursor / unitSize;
		size_t unitIndex = (size_t)(cursor % unitSize);
		size_t take = (size_t)unitSize - unitIndex;
		HfsIoLease lease;

		if (take > remaining) {
			take = remaining;
		}
		if (hfsIoAcquire(file->fs, &lease) != 0) {
			return -1;
		}
		if (hfsIoReadUnit(file, &lease, unitOffset) != 0) {
			hfsIoRelease(&lease);
			return -1;
		}
		memcpy(buffer, lease.buffer + unitIndex, take);
		hfsIoRelease(&lease);
		cursor += take;
		buffer += take;
		remaining -= take;
		done += take;
	}

	while (remaining >= (size_t)unitSize) {
		uint64_t fullUnits = remaining / (size_t)unitSize;
		uint64_t chunkUnits = fullUnits;
		HfsIoLease lease;

		if (chunkUnits > hfsIoMaxUnits(file->fs)) {
			chunkUnits = hfsIoMaxUnits(file->fs);
		}
		if (hfsIoAcquire(file->fs, &lease) != 0) {
			return -1;
		}
		if (hfsIoSubmit(file, lease.buffer, cursor / unitSize, chunkUnits,
				HFS_IO_READ) != 0) {
			hfsIoRelease(&lease);
			return -1;
		}
		memcpy(buffer, lease.buffer,
		       (size_t)(chunkUnits * unitSize));
		hfsIoRelease(&lease);
		cursor += chunkUnits * unitSize;
		buffer += (size_t)(chunkUnits * unitSize);
		remaining -= (size_t)(chunkUnits * unitSize);
		done += (size_t)(chunkUnits * unitSize);
	}

	if (remaining != 0) {
		HfsIoLease lease;

		if (hfsIoAcquire(file->fs, &lease) != 0) {
			return -1;
		}
		if (hfsIoReadUnit(file, &lease, cursor / unitSize) != 0) {
			hfsIoRelease(&lease);
			return -1;
		}
		memcpy(buffer, lease.buffer, remaining);
		hfsIoRelease(&lease);
		done += remaining;
	}
	return (ssize_t)done;
}

/* Write a positioned range while the caller holds the selected data lock. */
static ssize_t hfsIoWriteAtLocked(HfsFile *file, const uint8_t *buffer,
				  size_t count, uint64_t offset) {
	uint64_t unitSize = file->fs->unitSize;
	uint64_t cursor = offset;
	size_t remaining = count;
	size_t done = 0;

	if (hfsResizeTo(file, offset + count) != 0) {
		return -1;
	}
	if (cursor % unitSize != 0) {
		uint64_t unitOffset = cursor / unitSize;
		size_t unitIndex = (size_t)(cursor % unitSize);
		size_t take = (size_t)unitSize - unitIndex;

		if (take > remaining) {
			take = remaining;
		}
		if (hfsIoModifyPartial(file, unitOffset, unitIndex, buffer, take) != 0) {
			return -1;
		}
		cursor += take;
		buffer += take;
		remaining -= take;
		done += take;
	}

	while (remaining >= (size_t)unitSize) {
		uint64_t fullUnits = remaining / (size_t)unitSize;
		uint64_t chunkUnits = fullUnits;
		HfsIoLease lease;

		if (chunkUnits > hfsIoMaxUnits(file->fs)) {
			chunkUnits = hfsIoMaxUnits(file->fs);
		}
		if (hfsIoAcquire(file->fs, &lease) != 0) {
			return -1;
		}
		memcpy(lease.buffer, buffer, (size_t)(chunkUnits * unitSize));
		if (hfsIoSubmit(file, lease.buffer, cursor / unitSize, chunkUnits,
				HFS_IO_WRITE) != 0) {
			hfsIoRelease(&lease);
			return -1;
		}
		hfsIoRelease(&lease);
		cursor += chunkUnits * unitSize;
		buffer += (size_t)(chunkUnits * unitSize);
		remaining -= (size_t)(chunkUnits * unitSize);
		done += (size_t)(chunkUnits * unitSize);
	}

	if (remaining != 0 &&
	    hfsIoModifyPartial(file, cursor / unitSize, 0, buffer, remaining) != 0) {
		return -1;
	}
	return (ssize_t)(done + remaining);
}

/* Validate and read a positioned byte range through the bounded pool. */
ssize_t hfsIoReadAt(HfsFile *file, uint8_t *buffer, size_t count,
		    uint64_t offset) {
	ssize_t result;

	if (count == 0) {
		return 0;
	}
	if (file == NULL || file->fs == NULL || file->blob == NULL ||
	    file->fs->ioPool == NULL || file->fs->unitSize == 0 ||
	    buffer == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (count > (size_t)SSIZE_MAX || offset > UINT64_MAX - count) {
		errno = EOVERFLOW;
		return -1;
	}
	pthread_rwlock_rdlock(&file->fs->ioLock);
	result = hfsIoReadAtLocked(file, buffer, count, offset);
	pthread_rwlock_unlock(&file->fs->ioLock);
	return result;
}

/* Validate and write a positioned byte range with direct aligned writes. */
ssize_t hfsIoWriteAt(HfsFile *file, const uint8_t *buffer, size_t count,
		     uint64_t offset) {
	ssize_t result;
	bool aligned;

	if (count == 0) {
		return 0;
	}
	if (file == NULL || file->fs == NULL || file->blob == NULL ||
	    file->fs->ioPool == NULL || file->fs->unitSize == 0 ||
	    buffer == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (count > (size_t)SSIZE_MAX || offset > UINT64_MAX - count) {
		errno = EOVERFLOW;
		return -1;
	}
	aligned = (offset % file->fs->unitSize) == 0 &&
		(count % (size_t)file->fs->unitSize) == 0;
	if (aligned) {
		pthread_rwlock_rdlock(&file->fs->ioLock);
	} else {
		pthread_rwlock_wrlock(&file->fs->ioLock);
	}
	result = hfsIoWriteAtLocked(file, buffer, count, offset);
	pthread_rwlock_unlock(&file->fs->ioLock);
	return result;
}

/* Zero one partial unit without modifying the bytes outside the range. */
static int hfsIoZeroPartial(HfsFile *file, uint64_t unitOffset,
			    size_t unitIndex, size_t length) {
	HfsIoLease lease;
	int rc;

	if (hfsIoAcquire(file->fs, &lease) != 0) {
		return -1;
	}
	rc = hfsIoReadUnit(file, &lease, unitOffset);
	if (rc == 0) {
		memset(lease.buffer + unitIndex, 0, length);
		rc = hfsIoWriteUnit(file, &lease, unitOffset);
	}
	hfsIoRelease(&lease);
	return rc;
}

/* Unmap first, then guarantee zero-read semantics with zeroes or pooled writes. */
static int hfsIoUnmapUnits(HfsFile *file, uint64_t offset, uint64_t units) {
	int savedErrno;

	if (hfsIoSubmit(file, NULL, offset, units, HFS_IO_UNMAP) != 0) {
		savedErrno = errno;
		if (savedErrno != EOPNOTSUPP && savedErrno != ENOTSUP) {
			return -1;
		}
	}
	if (hfsIoSubmit(file, NULL, offset, units, HFS_IO_WRITE_ZEROES) == 0) {
		return 0;
	}
	savedErrno = errno;
	if (savedErrno != EOPNOTSUPP && savedErrno != ENOTSUP) {
		return -1;
	}
	return hfsIoWriteZeroUnits(file, offset, units);
}

/* Punch a range with edge-preserving zeroes and aligned unmap operations. */
int hfsIoPunchHole(HfsFile *file, uint64_t offset, uint64_t length) {
	uint64_t unitSize;
	uint64_t end;
	uint64_t cursor;
	uint64_t alignedEnd;

	if (length == 0) {
		return 0;
	}
	if (file == NULL || file->fs == NULL || file->blob == NULL ||
	    file->fs->ioPool == NULL || file->fs->unitSize == 0 ||
	    offset > UINT64_MAX - length) {
		errno = EINVAL;
		return -1;
	}
	unitSize = file->fs->unitSize;
	end = offset + length;
	pthread_rwlock_wrlock(&file->fs->ioLock);
	cursor = offset;
	if (cursor % unitSize != 0) {
		size_t unitIndex = (size_t)(cursor % unitSize);
		size_t take = (size_t)unitSize - unitIndex;

		if ((uint64_t)take > end - cursor) {
			take = (size_t)(end - cursor);
		}
		if (hfsIoZeroPartial(file, cursor / unitSize, unitIndex, take) != 0) {
			pthread_rwlock_unlock(&file->fs->ioLock);
			return -1;
		}
		cursor += take;
	}
	alignedEnd = (end / unitSize) * unitSize;
	if (cursor < alignedEnd) {
		if (hfsIoUnmapUnits(file, cursor / unitSize,
				    (alignedEnd - cursor) / unitSize) != 0) {
			pthread_rwlock_unlock(&file->fs->ioLock);
			return -1;
		}
		cursor = alignedEnd;
	}
	if (cursor < end &&
	    hfsIoZeroPartial(file, cursor / unitSize, 0,
			     (size_t)(end - cursor)) != 0) {
		pthread_rwlock_unlock(&file->fs->ioLock);
		return -1;
	}
	pthread_rwlock_unlock(&file->fs->ioLock);
	return 0;
}
