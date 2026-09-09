#ifndef HFS_RANGE_LOCK_H
#define HFS_RANGE_LOCK_H

#include <stdbool.h>
#include <pthread.h>
#include <stdint.h>

/* Ownerless read/write lock used by synchronous and asynchronous I/O ranges. */
typedef struct HfsRangeLock {
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	uint32_t readers;
	uint32_t waitingWriters;
	/* Bound the number of readers admitted after a writer queues. */
	uint32_t readersAfterWriter;
	bool writer;
} HfsRangeLock;

/* Initialize one ownerless range lock. */
int hfsRangeLockInit(HfsRangeLock *lock);
/* Destroy one range lock after all users have stopped. */
void hfsRangeLockDestroy(HfsRangeLock *lock);
/* Acquire the shared side, allowing release from another thread. */
void hfsRangeLockReadLock(HfsRangeLock *lock);
/* Acquire the exclusive side, allowing release from another thread. */
void hfsRangeLockWriteLock(HfsRangeLock *lock);
/* Try to acquire the shared side without blocking. */
int hfsRangeLockTryReadLock(HfsRangeLock *lock);
/* Try to acquire the exclusive side without blocking. */
int hfsRangeLockTryWriteLock(HfsRangeLock *lock);
/* Release a shared or exclusive acquisition without owner checking. */
void hfsRangeLockUnlock(HfsRangeLock *lock, bool write);

#endif /* HFS_RANGE_LOCK_H */
