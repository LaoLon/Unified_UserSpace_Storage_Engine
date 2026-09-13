#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "hfs.h"
#include "hfs_durability.h"
#include "hfs_stats.h"

#define HFS_DURABILITY_DEFAULT_WINDOW_US 200U
#define HFS_DURABILITY_DEFAULT_BATCH 64U
#define HFS_DURABILITY_MIN_WINDOW_US 10U
#define HFS_DURABILITY_MAX_WINDOW_US 5000U
#define HFS_DURABILITY_MIN_BATCH 1U
#define HFS_DURABILITY_MAX_BATCH 256U

#define HFS_DURABILITY_TRACE_MAX_EVENTS 512U
#define HFS_DURABILITY_TRACE_PHASE_SIZE 32U

/* One optional trace event emitted only by the synchronous durability path. */
typedef struct HfsDurabilityTraceEvent {
	uint64_t sequence;
	uint64_t epochId;
	uint64_t inodeId;
	int result;
	char phase[HFS_DURABILITY_TRACE_PHASE_SIZE];
} HfsDurabilityTraceEvent;

static pthread_mutex_t g_hfsDurabilityTraceLock = PTHREAD_MUTEX_INITIALIZER;
static HfsDurabilityTraceEvent
	g_hfsDurabilityTrace[HFS_DURABILITY_TRACE_MAX_EVENTS];
static char g_hfsDurabilityTracePath[PATH_MAX];
static uint32_t g_hfsDurabilityTraceCount;
static uint64_t g_hfsDurabilityTraceSequence;
static bool g_hfsDurabilityTraceInitialized;
static bool g_hfsDurabilityTraceEnabled;

/* Keep host-managed termination signals on the host's signal thread. */
static void hfsDurabilityBlockManagedSignals(void) {
	sigset_t managedSignals;

	sigemptyset(&managedSignals);
	sigaddset(&managedSignals, SIGHUP);
	sigaddset(&managedSignals, SIGQUIT);
	sigaddset(&managedSignals, SIGALRM);
	sigaddset(&managedSignals, SIGTERM);
	sigaddset(&managedSignals, SIGTSTP);
	(void)pthread_sigmask(SIG_BLOCK, &managedSignals, NULL);
}

/* Mount-level request queue that coalesces data and metadata barriers. */
struct HfsDurabilityCoordinator {
	struct Hfs *fs;
	HfsDurabilityBatchProcessor processor;
	pthread_mutex_t lock;
	pthread_cond_t changed;
	pthread_t worker;
	HfsDurabilityRequest *head;
	HfsDurabilityRequest *tail;
	size_t depth;
	uint32_t windowUs;
	uint32_t maxBatch;
	uint64_t nextEpoch;
	bool stopping;
	bool started;
};

static uint32_t hfsDurabilityEnvUint(const char *name, uint32_t fallback,
		uint32_t minimum, uint32_t maximum) {
	const char *value = getenv(name);
	char *end = NULL;
	unsigned long parsed;

	if (value == NULL || value[0] == '\0') {
		return fallback;
	}
	parsed = strtoul(value, &end, 10);
	if (end == value || *end != '\0' || parsed < minimum ||
		parsed > maximum) {
		return fallback;
	}
	return (uint32_t)parsed;
}

/* Return a realtime deadline matching the default pthread condition clock. */
static void hfsDurabilityDeadline(uint32_t windowUs, struct timespec *deadline) {
	clock_gettime(CLOCK_REALTIME, deadline);
	deadline->tv_sec += windowUs / 1000000U;
	deadline->tv_nsec += (long)(windowUs % 1000000U) * 1000L;
	if (deadline->tv_nsec >= 1000000000L) {
		deadline->tv_sec++;
		deadline->tv_nsec -= 1000000000L;
	}
}

/* Publish one result and wake every caller that joined the same batch. */
static void hfsDurabilityComplete(HfsDurabilityRequest *request, int result,
		int error) {
	pthread_mutex_lock(&request->lock);
	request->result = result;
	request->error = error;
	request->completed = true;
	pthread_cond_signal(&request->done);
	pthread_mutex_unlock(&request->lock);
}

/* Process queued requests in bounded batches until the mount is stopped. */
static void *hfsDurabilityWorker(void *arg) {
	HfsDurabilityCoordinator *coordinator = arg;

	hfsDurabilityBlockManagedSignals();

	for (;;) {
		HfsDurabilityRequest *batchHead;
		HfsDurabilityRequest *batchTail = NULL;
		HfsDurabilityRequest *request;
		HfsDurabilityRequest **requests;
		size_t count = 0;
		size_t index;
		int batchRc;

		pthread_mutex_lock(&coordinator->lock);
		while (coordinator->head == NULL && !coordinator->stopping) {
			pthread_cond_wait(&coordinator->changed, &coordinator->lock);
		}
		if (coordinator->head == NULL && coordinator->stopping) {
			pthread_mutex_unlock(&coordinator->lock);
			break;
		}
		batchHead = coordinator->head;
		if (coordinator->depth < coordinator->maxBatch) {
			struct timespec deadline;

			hfsDurabilityDeadline(coordinator->windowUs, &deadline);
			while (coordinator->depth < coordinator->maxBatch &&
				coordinator->tail != NULL &&
				pthread_cond_timedwait(&coordinator->changed,
					&coordinator->lock, &deadline) == 0) {
				/* New requests extend the batch until the bounded deadline. */
			}
		}
		coordinator->head = NULL;
		coordinator->tail = NULL;
		coordinator->depth = 0;
		pthread_mutex_unlock(&coordinator->lock);
		/* One bounded request batch is one durable epoch. */
		coordinator->nextEpoch++;
		hfsStatsRecordDurabilityEpochFast();

		request = batchHead;
		while (request != NULL && count < coordinator->maxBatch) {
			batchTail = request;
			request = request->next;
			count++;
		}
		if (batchTail != NULL) {
			batchTail->next = NULL;
		}
		/* Keep an unexpectedly long queue for the next worker iteration. */
		if (request != NULL) {
			HfsDurabilityRequest *remainder = request;
			pthread_mutex_lock(&coordinator->lock);
			coordinator->head = remainder;
			coordinator->tail = remainder;
			coordinator->depth = 1;
			while (coordinator->tail->next != NULL) {
				coordinator->tail = coordinator->tail->next;
				coordinator->depth++;
			}
			pthread_cond_signal(&coordinator->changed);
			pthread_mutex_unlock(&coordinator->lock);
		}

		requests = calloc(count, sizeof(*requests));
		if (requests == NULL) {
			for (request = batchHead; request != NULL; request = request->next) {
				hfsDurabilityComplete(request, -1, ENOMEM);
			}
			continue;
		}
		request = batchHead;
		for (index = 0; index < count; index++) {
			requests[index] = request;
			request->epochId = coordinator->nextEpoch;
			request = request->next;
		}
		hfsStatsRecordDurabilityEpochWaiterFast(count > 1 ? count - 1U : 0U);
		hfsStatsRecordDurabilityLeaderFast();
		for (index = 1; index < count; index++) {
			hfsStatsRecordDurabilityWaiterFast();
		}
		batchRc = coordinator->processor == NULL ? -EOPNOTSUPP :
			coordinator->processor(coordinator->fs, requests, count);
		for (index = 0; index < count; index++) {
			if (!requests[index]->completed) {
				int requestResult = batchRc == 0 ? requests[index]->result : -1;
				int requestError = batchRc == 0 ? requests[index]->error :
					(batchRc < 0 ? -batchRc : EIO);

				hfsDurabilityComplete(requests[index], requestResult,
					requestError);
			}
		}
		free(requests);
	}
	return NULL;
}

/* Start the worker and parse optional bounded coalescing parameters. */
int hfsDurabilityCoordinatorInit(struct Hfs *fs,
		HfsDurabilityBatchProcessor processor) {
	HfsDurabilityCoordinator *coordinator;

	if (fs == NULL || processor == NULL) {
		errno = EINVAL;
		return -1;
	}
	coordinator = calloc(1, sizeof(*coordinator));
	if (coordinator == NULL) {
		errno = ENOMEM;
		return -1;
	}
	coordinator->fs = fs;
	coordinator->processor = processor;
	coordinator->windowUs = hfsDurabilityEnvUint(
		"HFS_DURABILITY_GROUP_WINDOW_US", HFS_DURABILITY_DEFAULT_WINDOW_US,
		HFS_DURABILITY_MIN_WINDOW_US, HFS_DURABILITY_MAX_WINDOW_US);
	coordinator->maxBatch = hfsDurabilityEnvUint(
		"HFS_DURABILITY_GROUP_MAX_BATCH", HFS_DURABILITY_DEFAULT_BATCH,
		HFS_DURABILITY_MIN_BATCH, HFS_DURABILITY_MAX_BATCH);
	if (pthread_mutex_init(&coordinator->lock, NULL) != 0) {
		free(coordinator);
		errno = EAGAIN;
		return -1;
	}
	if (pthread_cond_init(&coordinator->changed, NULL) != 0) {
		pthread_mutex_destroy(&coordinator->lock);
		free(coordinator);
		errno = EAGAIN;
		return -1;
	}
	if (pthread_create(&coordinator->worker, NULL, hfsDurabilityWorker,
			coordinator) != 0) {
		pthread_cond_destroy(&coordinator->changed);
		pthread_mutex_destroy(&coordinator->lock);
		free(coordinator);
		errno = EAGAIN;
		return -1;
	}
	coordinator->started = true;
	fs->durability = coordinator;
	return 0;
}

/* Stop the worker after all queued requests have reached a terminal result. */
void hfsDurabilityCoordinatorStop(struct Hfs *fs) {
	HfsDurabilityCoordinator *coordinator;
	HfsDurabilityRequest *request;

	if (fs == NULL || fs->durability == NULL) {
		return;
	}
	coordinator = fs->durability;
	pthread_mutex_lock(&coordinator->lock);
	coordinator->stopping = true;
	request = coordinator->head;
	coordinator->head = NULL;
	coordinator->tail = NULL;
	coordinator->depth = 0;
	pthread_cond_signal(&coordinator->changed);
	pthread_mutex_unlock(&coordinator->lock);
	while (request != NULL) {
		HfsDurabilityRequest *next = request->next;

		hfsDurabilityComplete(request, -1, ESHUTDOWN);
		request = next;
	}
	if (coordinator->started) {
		pthread_join(coordinator->worker, NULL);
	}
	pthread_cond_destroy(&coordinator->changed);
	pthread_mutex_destroy(&coordinator->lock);
	free(coordinator);
	fs->durability = NULL;
}

/* Initialize a caller-owned request before it enters the coordinator queue. */
int hfsDurabilityRequestInit(HfsDurabilityRequest *request,
		struct HfsOpenFile *openFile, bool dataOnly) {
	if (request == NULL || openFile == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(request, 0, sizeof(*request));
	request->openFile = openFile;
	request->dataOnly = dataOnly;
	if (pthread_mutex_init(&request->lock, NULL) != 0) {
		errno = EAGAIN;
		return -1;
	}
	if (pthread_cond_init(&request->done, NULL) != 0) {
		pthread_mutex_destroy(&request->lock);
		errno = EAGAIN;
		return -1;
	}
	return 0;
}

/* Queue one request and wait until its batch processor publishes the result. */
int hfsDurabilityRequestSubmit(struct Hfs *fs,
		HfsDurabilityRequest *request) {
	HfsDurabilityCoordinator *coordinator;

	if (fs == NULL || request == NULL || fs->durability == NULL) {
		errno = ESHUTDOWN;
		return -1;
	}
	coordinator = fs->durability;
	pthread_mutex_lock(&coordinator->lock);
	if (coordinator->stopping) {
		pthread_mutex_unlock(&coordinator->lock);
		errno = ESHUTDOWN;
		return -1;
	}
	request->next = NULL;
	if (coordinator->tail == NULL) {
		coordinator->head = request;
	} else {
		coordinator->tail->next = request;
	}
	coordinator->tail = request;
	coordinator->depth++;
	pthread_cond_signal(&coordinator->changed);
	pthread_mutex_unlock(&coordinator->lock);

	pthread_mutex_lock(&request->lock);
	while (!request->completed) {
		pthread_cond_wait(&request->done, &request->lock);
	}
	pthread_mutex_unlock(&request->lock);
	if (request->result != 0) {
		errno = request->error != 0 ? request->error : EIO;
	}
	return request->result;
}

/* Destroy request synchronization state after the worker has completed it. */
void hfsDurabilityRequestFini(HfsDurabilityRequest *request) {
	if (request == NULL) {
		return;
	}
	pthread_cond_destroy(&request->done);
	pthread_mutex_destroy(&request->lock);
}

/* Initialize a disabled-by-default trace from HFS_DURABILITY_TRACE_PATH. */
void hfsDurabilityTraceInit(void) {
	const char *path;

	if (g_hfsDurabilityTraceInitialized) {
		return;
	}
	path = getenv("HFS_DURABILITY_TRACE_PATH");
	g_hfsDurabilityTracePath[0] = '\0';
	g_hfsDurabilityTraceEnabled = false;
	if (path != NULL && path[0] != '\0' &&
	    snprintf(g_hfsDurabilityTracePath,
		     sizeof(g_hfsDurabilityTracePath), "%s", path) <
		(int)sizeof(g_hfsDurabilityTracePath)) {
		g_hfsDurabilityTraceEnabled = true;
	}
	g_hfsDurabilityTraceCount = 0;
	g_hfsDurabilityTraceSequence = 0;
	g_hfsDurabilityTraceInitialized = true;
}

/* Append a bounded in-memory event while preserving the public errno value. */
void hfsDurabilityTraceRecordEpoch(uint64_t epochId, uint64_t inodeId,
		const char *phase, int result) {
	HfsDurabilityTraceEvent *event;
	int savedErrno = errno;

	if (!g_hfsDurabilityTraceEnabled || phase == NULL) {
		return;
	}
	pthread_mutex_lock(&g_hfsDurabilityTraceLock);
	if (g_hfsDurabilityTraceCount < HFS_DURABILITY_TRACE_MAX_EVENTS) {
		event = &g_hfsDurabilityTrace[g_hfsDurabilityTraceCount++];
		event->sequence = ++g_hfsDurabilityTraceSequence;
		event->epochId = epochId;
		event->inodeId = inodeId;
		event->result = result;
		snprintf(event->phase, sizeof(event->phase), "%s", phase);
	}
	pthread_mutex_unlock(&g_hfsDurabilityTraceLock);
	errno = savedErrno;
}

/* Preserve the legacy trace API for synchronous fallback callers. */
void hfsDurabilityTraceRecord(uint64_t inodeId, const char *phase, int result) {
	hfsDurabilityTraceRecordEpoch(0, inodeId, phase, result);
}

/* Write trace events through direct syscalls to avoid LD_PRELOAD recursion. */
void hfsDurabilityTraceFini(void) {
	char line[160];
	uint32_t i;
	int fd;
	int length;
	int savedErrno = errno;

	if (!g_hfsDurabilityTraceInitialized || !g_hfsDurabilityTraceEnabled) {
		return;
	}
	pthread_mutex_lock(&g_hfsDurabilityTraceLock);
	fd = (int)syscall(SYS_openat, AT_FDCWD, g_hfsDurabilityTracePath,
		O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
	if (fd >= 0) {
		for (i = 0; i < g_hfsDurabilityTraceCount; i++) {
			length = snprintf(line, sizeof(line),
				"sequence=%"PRIu64" epoch=%"PRIu64" inode=%"PRIu64" phase=%s result=%d\n",
				g_hfsDurabilityTrace[i].sequence,
				g_hfsDurabilityTrace[i].epochId,
				g_hfsDurabilityTrace[i].inodeId,
				g_hfsDurabilityTrace[i].phase,
				g_hfsDurabilityTrace[i].result);
			if (length > 0 && length < (int)sizeof(line)) {
				(void)syscall(SYS_write, fd, line, (size_t)length);
			}
		}
		(void)syscall(SYS_close, fd);
	}
	g_hfsDurabilityTraceEnabled = false;
	pthread_mutex_unlock(&g_hfsDurabilityTraceLock);
	errno = savedErrno;
}
