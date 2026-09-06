#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hfs_async.h"
#include "hfs_lib.h"
#include "hfs_stats.h"

#define HFS_ASYNC_DEFAULT_WORKERS 4U
#define HFS_ASYNC_MAX_WORKERS 32U
#define HFS_ASYNC_DEFAULT_QUEUE 256U
#define HFS_ASYNC_MAX_QUEUE 4096U

typedef enum HfsAsyncOperation {
	HFS_ASYNC_PREAD,
	HFS_ASYNC_PWRITE,
	HFS_ASYNC_PREADV,
	HFS_ASYNC_PWRITEV,
	HFS_ASYNC_FSYNC,
} HfsAsyncOperation;

/* One request owns all operation arguments until its worker retires. */
struct HfsAsyncRequest {
	pthread_mutex_t lock;
	pthread_cond_t done;
	_Atomic uint32_t refs;
	HfsAsyncOperation operation;
	int fd;
	void *buffer;
	const void *constBuffer;
	const struct iovec *iov;
	int iovcnt;
	size_t count;
	off_t offset;
	HfsAsyncCallback callback;
	void *callbackContext;
	struct HfsAsyncRequest *next;
	bool cancelRequested;
	bool completed;
	ssize_t result;
	int error;
};

/* Bounded process-local executor used by the first async API stage. */
typedef struct HfsAsyncRuntime {
	pthread_mutex_t lock;
	pthread_cond_t available;
	pthread_t *workers;
	HfsAsyncRequest *head;
	HfsAsyncRequest *tail;
	size_t queueDepth;
	size_t queueLimit;
	unsigned int workerCount;
	bool initialized;
	bool stopping;
} HfsAsyncRuntime;

static HfsAsyncRuntime g_async = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.available = PTHREAD_COND_INITIALIZER,
};

/* Parse a bounded positive environment setting or use its default. */
static unsigned int hfsAsyncEnvUnsigned(const char *name,
					 unsigned int fallback, unsigned int maximum) {
	const char *value = getenv(name);
	char *end = NULL;
	unsigned long parsed;

	if (value == NULL || value[0] == '\0') {
		return fallback;
	}
	parsed = strtoul(value, &end, 10);
	if (*value == '\0' || end == value || *end != '\0' || parsed == 0 ||
	    parsed > maximum) {
		return fallback;
	}
	return (unsigned int)parsed;
}

/* Drop one reference and destroy a request only after all users retire. */
static void hfsAsyncRequestReleaseInternal(HfsAsyncRequest *request) {
	if (atomic_fetch_sub_explicit(&request->refs, 1U,
					 memory_order_acq_rel) == 1U) {
		pthread_cond_destroy(&request->done);
		pthread_mutex_destroy(&request->lock);
		free(request);
	}
}

/* Publish a completion and optionally invoke the user callback. */
static void hfsAsyncComplete(HfsAsyncRequest *request, ssize_t result,
				     int error) {
	HfsAsyncCallback callback;
	void *context;
	bool deliver;

	pthread_mutex_lock(&request->lock);
	request->result = result;
	request->error = error;
	request->completed = true;
	callback = request->callback;
	context = request->callbackContext;
	deliver = callback != NULL && !request->cancelRequested;
	pthread_cond_broadcast(&request->done);
	pthread_mutex_unlock(&request->lock);
	if (deliver) {
		callback(request, result, error, context);
	}
	hfsStatsRecordAsyncCompleteFast();
	hfsAsyncRequestReleaseInternal(request);
}

/* Execute one request on a worker without holding executor locks. */
static void hfsAsyncExecute(HfsAsyncRequest *request) {
	ssize_t result = -1;
	int error = 0;
	bool cancelled;

	pthread_mutex_lock(&request->lock);
	cancelled = request->cancelRequested;
	pthread_mutex_unlock(&request->lock);
	if (cancelled) {
		hfsAsyncComplete(request, -1, ECANCELED);
		return;
	}
	switch (request->operation) {
	case HFS_ASYNC_PREAD:
		result = hfs_lib_pread(request->fd, request->buffer,
					 request->count, request->offset);
		break;
	case HFS_ASYNC_PWRITE:
		result = hfs_lib_pwrite(request->fd, request->constBuffer,
					 request->count, request->offset);
		break;
	case HFS_ASYNC_PREADV:
		result = hfs_lib_preadv(request->fd, request->iov,
					 request->iovcnt, request->offset);
		break;
	case HFS_ASYNC_PWRITEV:
		result = hfs_lib_pwritev(request->fd, request->iov,
					  request->iovcnt, request->offset);
		break;
	case HFS_ASYNC_FSYNC:
		result = hfs_lib_fsync(request->fd);
		break;
	}
	if (result < 0) {
		error = errno != 0 ? errno : EIO;
	}
	hfsAsyncComplete(request, result, error);
}

/* Worker loop that drains queued requests until executor shutdown. */
static void *hfsAsyncWorkerMain(void *arg) {
	(void)arg;
	for (;;) {
		HfsAsyncRequest *request;

		pthread_mutex_lock(&g_async.lock);
		while (g_async.head == NULL && !g_async.stopping) {
			pthread_cond_wait(&g_async.available, &g_async.lock);
		}
		if (g_async.head == NULL && g_async.stopping) {
			pthread_mutex_unlock(&g_async.lock);
			return NULL;
		}
		request = g_async.head;
		g_async.head = request->next;
		if (g_async.head == NULL) {
			g_async.tail = NULL;
		}
		g_async.queueDepth--;
		pthread_mutex_unlock(&g_async.lock);
		hfsAsyncExecute(request);
	}
}

/* Initialize the worker pool used by libhfs async requests. */
int hfsAsyncInit(void) {
	unsigned int workerCount;
	unsigned int queueLimit;
	unsigned int i;
	int rc;

	pthread_mutex_lock(&g_async.lock);
	if (g_async.initialized) {
		pthread_mutex_unlock(&g_async.lock);
		return 0;
	}
	workerCount = hfsAsyncEnvUnsigned("HFS_ASYNC_WORKERS",
		HFS_ASYNC_DEFAULT_WORKERS, HFS_ASYNC_MAX_WORKERS);
	queueLimit = hfsAsyncEnvUnsigned("HFS_ASYNC_QUEUE_DEPTH",
		HFS_ASYNC_DEFAULT_QUEUE, HFS_ASYNC_MAX_QUEUE);
	g_async.workers = calloc(workerCount, sizeof(*g_async.workers));
	if (g_async.workers == NULL) {
		pthread_mutex_unlock(&g_async.lock);
		errno = ENOMEM;
		return -1;
	}
	g_async.workerCount = workerCount;
	g_async.queueLimit = queueLimit;
	g_async.stopping = false;
	for (i = 0; i < workerCount; i++) {
		rc = pthread_create(&g_async.workers[i], NULL,
					hfsAsyncWorkerMain, NULL);
		if (rc != 0) {
			g_async.stopping = true;
			pthread_cond_broadcast(&g_async.available);
			pthread_mutex_unlock(&g_async.lock);
			while (i-- > 0) {
				pthread_join(g_async.workers[i], NULL);
			}
			free(g_async.workers);
			g_async.workers = NULL;
			g_async.workerCount = 0;
			errno = rc;
			return -1;
		}
	}
	g_async.initialized = true;
	pthread_mutex_unlock(&g_async.lock);
	return 0;
}

/* Stop all workers after queued requests have completed or been cancelled. */
void hfsAsyncFini(void) {
	HfsAsyncRequest *request;
	unsigned int i;

	pthread_mutex_lock(&g_async.lock);
	if (!g_async.initialized) {
		pthread_mutex_unlock(&g_async.lock);
		return;
	}
	g_async.stopping = true;
	pthread_cond_broadcast(&g_async.available);
	pthread_mutex_unlock(&g_async.lock);
	for (i = 0; i < g_async.workerCount; i++) {
		pthread_join(g_async.workers[i], NULL);
	}
	free(g_async.workers);
	pthread_mutex_lock(&g_async.lock);
	request = g_async.head;
	g_async.head = NULL;
	g_async.tail = NULL;
	g_async.queueDepth = 0;
	g_async.workerCount = 0;
	g_async.workers = NULL;
	g_async.initialized = false;
	g_async.stopping = false;
	pthread_mutex_unlock(&g_async.lock);
	while (request != NULL) {
		HfsAsyncRequest *next = request->next;

		hfsAsyncComplete(request, -1, ECANCELED);
		request = next;
	}
}

/* Allocate and queue one request while retaining caller and worker references. */
static int hfsAsyncSubmit(HfsAsyncOperation operation, int fd, void *buffer,
				  const void *constBuffer, const struct iovec *iov,
				  int iovcnt, size_t count, off_t offset,
				  HfsAsyncCallback callback, void *context,
				  HfsAsyncRequest **requestOut) {
	HfsAsyncRequest *request;

	if (requestOut == NULL) {
		errno = EINVAL;
		return -1;
	}
	*requestOut = NULL;
	request = calloc(1, sizeof(*request));
	if (request == NULL) {
		errno = ENOMEM;
		return -1;
	}
	if (pthread_mutex_init(&request->lock, NULL) != 0) {
		free(request);
		errno = ENOMEM;
		return -1;
	}
	if (pthread_cond_init(&request->done, NULL) != 0) {
		pthread_mutex_destroy(&request->lock);
		free(request);
		errno = ENOMEM;
		return -1;
	}
	atomic_init(&request->refs, 2U);
	request->operation = operation;
	request->fd = fd;
	request->buffer = buffer;
	request->constBuffer = constBuffer;
	request->iov = iov;
	request->iovcnt = iovcnt;
	request->count = count;
	request->offset = offset;
	request->callback = callback;
	request->callbackContext = context;
	pthread_mutex_lock(&g_async.lock);
	if (!g_async.initialized || g_async.stopping) {
		pthread_mutex_unlock(&g_async.lock);
		hfsAsyncRequestReleaseInternal(request);
		hfsAsyncRequestReleaseInternal(request);
		errno = ENODEV;
		return -1;
	}
	if (g_async.queueDepth >= g_async.queueLimit) {
		pthread_mutex_unlock(&g_async.lock);
		hfsAsyncRequestReleaseInternal(request);
		hfsAsyncRequestReleaseInternal(request);
		errno = EAGAIN;
		return -1;
	}
	request->next = NULL;
	if (g_async.tail != NULL) {
		g_async.tail->next = request;
	} else {
		g_async.head = request;
	}
	g_async.tail = request;
	g_async.queueDepth++;
	hfsStatsRecordAsyncSubmitFast(g_async.queueDepth);
	pthread_cond_signal(&g_async.available);
	*requestOut = request;
	pthread_mutex_unlock(&g_async.lock);
	return 0;
}

/* Submit a positioned read request. */
int hfsAsyncPread(int fd, void *buffer, size_t count, off_t offset,
			  HfsAsyncCallback callback, void *context,
			  HfsAsyncRequest **requestOut) {
	if (offset < 0 || (count != 0 && buffer == NULL)) {
		errno = EINVAL;
		return -1;
	}
	return hfsAsyncSubmit(HFS_ASYNC_PREAD, fd, buffer, NULL, NULL, 0,
				      count, offset, callback, context, requestOut);
}

/* Submit a positioned write request. */
int hfsAsyncPwrite(int fd, const void *buffer, size_t count, off_t offset,
			   HfsAsyncCallback callback, void *context,
			   HfsAsyncRequest **requestOut) {
	if (offset < 0 || (count != 0 && buffer == NULL)) {
		errno = EINVAL;
		return -1;
	}
	return hfsAsyncSubmit(HFS_ASYNC_PWRITE, fd, NULL, buffer, NULL, 0,
				      count, offset, callback, context, requestOut);
}

/* Submit a positioned vector read request. */
int hfsAsyncPreadv(int fd, const struct iovec *iov, int iovcnt, off_t offset,
			   HfsAsyncCallback callback, void *context,
			   HfsAsyncRequest **requestOut) {
	if (offset < 0 || iovcnt < 0 || (iovcnt != 0 && iov == NULL)) {
		errno = EINVAL;
		return -1;
	}
	return hfsAsyncSubmit(HFS_ASYNC_PREADV, fd, NULL, NULL, iov, iovcnt,
				      0, offset, callback, context, requestOut);
}

/* Submit a positioned vector write request. */
int hfsAsyncPwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset,
			    HfsAsyncCallback callback, void *context,
			    HfsAsyncRequest **requestOut) {
	if (offset < 0 || iovcnt < 0 || (iovcnt != 0 && iov == NULL)) {
		errno = EINVAL;
		return -1;
	}
	return hfsAsyncSubmit(HFS_ASYNC_PWRITEV, fd, NULL, NULL, iov, iovcnt,
				      0, offset, callback, context, requestOut);
}

/* Submit an fsync request without blocking the caller. */
int hfsAsyncFsync(int fd, HfsAsyncCallback callback, void *context,
			 HfsAsyncRequest **requestOut) {
	return hfsAsyncSubmit(HFS_ASYNC_FSYNC, fd, NULL, NULL, NULL, 0, 0, 0,
				      callback, context, requestOut);
}

/* Mark one request so its callback is not delivered after cancellation. */
int hfsAsyncCancel(HfsAsyncRequest *request) {
	if (request == NULL) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&request->lock);
	if (request->completed) {
		pthread_mutex_unlock(&request->lock);
		errno = EALREADY;
		return -1;
	}
	request->cancelRequested = true;
	pthread_mutex_unlock(&request->lock);
	hfsStatsRecordAsyncCancelFast();
	return 0;
}

/* Wait for one request while retaining the caller-owned reference. */
int hfsAsyncWait(HfsAsyncRequest *request, uint32_t timeoutMs) {
	struct timespec deadline;
	int rc = 0;

	if (request == NULL) {
		errno = EINVAL;
		return -1;
	}
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += timeoutMs / 1000U;
	deadline.tv_nsec += (long)(timeoutMs % 1000U) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&request->lock);
	while (!request->completed && rc == 0) {
		rc = pthread_cond_timedwait(&request->done, &request->lock,
					    &deadline);
	}
	if (!request->completed && rc == ETIMEDOUT) {
		pthread_mutex_unlock(&request->lock);
		errno = ETIMEDOUT;
		return -1;
	}
	pthread_mutex_unlock(&request->lock);
	return rc == 0 ? 0 : -1;
}

/* Read a completed result without changing request ownership. */
int hfsAsyncResult(HfsAsyncRequest *request, ssize_t *resultOut,
			   int *errorOut) {
	if (request == NULL || resultOut == NULL || errorOut == NULL) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&request->lock);
	if (!request->completed) {
		pthread_mutex_unlock(&request->lock);
		errno = EAGAIN;
		return -1;
	}
	*resultOut = request->result;
	*errorOut = request->error;
	pthread_mutex_unlock(&request->lock);
	return 0;
}

/* Release the request reference owned by the caller. */
void hfsAsyncRelease(HfsAsyncRequest *request) {
	if (request != NULL) {
		hfsAsyncRequestReleaseInternal(request);
	}
}
