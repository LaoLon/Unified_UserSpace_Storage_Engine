#include <errno.h>

#include "hfs_range_lock.h"

/* Initialize the mutex/condition pair backing one ownerless range lock. */
int hfsRangeLockInit(HfsRangeLock *lock)
{
	int rc;

	if (lock == NULL) {
		return EINVAL;
	}
	rc = pthread_mutex_init(&lock->mutex, NULL);
	if (rc != 0) {
		return rc;
	}
	rc = pthread_cond_init(&lock->condition, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&lock->mutex);
		return rc;
	}
	lock->readers = 0;
	lock->waitingWriters = 0;
	lock->writer = false;
	return 0;
}

/* Destroy the lock primitives after the filesystem has quiesced. */
void hfsRangeLockDestroy(HfsRangeLock *lock)
{
	if (lock == NULL) {
		return;
	}
	pthread_cond_destroy(&lock->condition);
	pthread_mutex_destroy(&lock->mutex);
}

/* Acquire a shared range while writers have priority to avoid starvation. */
void hfsRangeLockReadLock(HfsRangeLock *lock)
{
	pthread_mutex_lock(&lock->mutex);
	while (lock->writer || lock->waitingWriters != 0) {
		pthread_cond_wait(&lock->condition, &lock->mutex);
	}
	lock->readers++;
	pthread_mutex_unlock(&lock->mutex);
}

/* Acquire an exclusive range while tracking queued writers for fairness. */
void hfsRangeLockWriteLock(HfsRangeLock *lock)
{
	pthread_mutex_lock(&lock->mutex);
	lock->waitingWriters++;
	while (lock->writer || lock->readers != 0) {
		pthread_cond_wait(&lock->condition, &lock->mutex);
	}
	lock->waitingWriters--;
	lock->writer = true;
	pthread_mutex_unlock(&lock->mutex);
}

/* Try to acquire a shared range without waiting on the worker thread. */
int hfsRangeLockTryReadLock(HfsRangeLock *lock)
{
	int rc = 0;

	pthread_mutex_lock(&lock->mutex);
	if (lock->writer || lock->waitingWriters != 0) {
		rc = EBUSY;
	} else {
		lock->readers++;
	}
	pthread_mutex_unlock(&lock->mutex);
	return rc;
}

/* Try to acquire an exclusive range without waiting on the worker thread. */
int hfsRangeLockTryWriteLock(HfsRangeLock *lock)
{
	int rc = 0;

	pthread_mutex_lock(&lock->mutex);
	if (lock->writer || lock->readers != 0) {
		rc = EBUSY;
	} else {
		lock->writer = true;
	}
	pthread_mutex_unlock(&lock->mutex);
	return rc;
}

/* Release a range without requiring the caller to be the acquiring thread. */
void hfsRangeLockUnlock(HfsRangeLock *lock, bool write)
{
	pthread_mutex_lock(&lock->mutex);
	if (write) {
		lock->writer = false;
	} else if (lock->readers != 0) {
		lock->readers--;
	}
	pthread_cond_broadcast(&lock->condition);
	pthread_mutex_unlock(&lock->mutex);
}
