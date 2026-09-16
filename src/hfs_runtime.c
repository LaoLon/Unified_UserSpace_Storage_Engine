#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <time.h>
#include <unistd.h>

#include <spdk/env.h>
#include <spdk/init.h>
#include <spdk/log.h>
#include <spdk/thread.h>

#include "hfs_log.h"
#include "hfs_op.h"
#include "hfs_profile.h"
#include "hfs_runtime.h"
#include "hfs_stats.h"
#include "hfs_trace.h"

/* Completion states are intentionally simple for one-shot requests. */
#define HFS_COMPLETION_PENDING 0
#define HFS_COMPLETION_DONE 1
#define HFS_COMPLETION_TIMED_OUT 2
#define HFS_RUNTIME_MAX_DATA_REACTORS 8U
#define HFS_RUNTIME_DEFAULT_QUEUE_DEPTH 32U
#define HFS_RUNTIME_MIN_QUEUE_DEPTH 4U
#define HFS_RUNTIME_MAX_QUEUE_DEPTH 256U
#define HFS_RUNTIME_DEFAULT_STRIPE_KIB 1024UL
#define HFS_RUNTIME_MIN_STRIPE_KIB 64UL
#define HFS_RUNTIME_MAX_STRIPE_KIB 16384UL

/* One optional SPDK data reactor; reactor zero remains the metadata owner. */
typedef struct HfsRuntimeDataReactor {
	pthread_t owner;
	_Atomic(struct spdk_thread *) spdkThread;
	pthread_mutex_t lock;
	pthread_cond_t stateChanged;
	pthread_mutex_t batchLock;
	pthread_cond_t queueAvailable;
	HfsRuntimeBatchItem *batchHead;
	HfsRuntimeBatchItem *batchTail;
	bool batchScheduled;
	bool queueClosed;
	uint32_t queueDepth;
	uint32_t inflight;
	uint32_t maxDepth;
	unsigned int index;
	int cpu;
	uint32_t idleBackoffUs;
	_Atomic uint64_t lastPollExitNs;
	bool threadCreated;
	bool ready;
	int initRc;
} HfsRuntimeDataReactor;

/* Runtime state owned by the process-wide HFS library instance. */
struct hfs_runtime_state {
	pthread_mutex_t lock;
	pthread_cond_t stateChanged;
	pthread_mutex_t batchLock;
	HfsRuntimeBatchItem *batchHead;
	HfsRuntimeBatchItem *batchTail;
	bool batchScheduled;
	pthread_t owner;
	_Atomic(struct spdk_thread *) spdkThread;
	bool threadCreated;
	bool ready;
	bool batchEnabled;
	bool posixQueueEnabled;
	uint32_t queueDepthLimit;
	/* Serialize multi-producer SPDK ring admission to avoid tail reservation stalls. */
	pthread_mutex_t messageSubmitLock;
	_Atomic bool accepting;
	bool stopSubmitted;
	_Atomic bool subsystemsReady;
	int initRc;
	_Atomic pid_t ownerPid;
	_Atomic uint64_t queued;
	_Atomic uint64_t inflight;
	_Atomic uint32_t submitters;
	HfsRuntimeDataReactor *dataReactors;
	uint32_t dataReactorCount;
	uint64_t dataStripeBytes;
	bool waitModeAdaptive;
	uint64_t waitSpinNs;
};

static struct hfs_runtime_state g_runtime = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.stateChanged = PTHREAD_COND_INITIALIZER,
	.batchLock = PTHREAD_MUTEX_INITIALIZER,
	.messageSubmitLock = PTHREAD_MUTEX_INITIALIZER,
};
static _Atomic bool g_ownerViolationLogged;

/* Owner identity is immutable for a runtime generation and needs no mutex lookup. */
static _Thread_local bool g_hfsRuntimeOwnerThread;
static _Thread_local bool g_hfsRuntimeDataThread;

/* Parse a boolean environment switch with a safe enabled default. */
static bool hfsRuntimeBoolEnv(const char *name, bool defaultValue) {
	const char *value = getenv(name);

	if (value == NULL || value[0] == '\0') {
		return defaultValue;
	}
	return strcmp(value, "0") != 0 && strcasecmp(value, "false") != 0 &&
		strcasecmp(value, "off") != 0 && strcasecmp(value, "no") != 0;
}

/* Serialize producers before entering SPDK's lockless message ring. */
static void hfsRuntimeSendMessage(struct spdk_thread *thread,
					spdk_msg_fn fn, void *ctx, bool dataMessage) {
	if (thread == NULL || fn == NULL) {
		return;
	}
	hfsStatsMessageSubmitLockBefore();
	pthread_mutex_lock(&g_runtime.messageSubmitLock);
	hfsStatsMessageSubmitLockAfter();
	hfsStatsRecordMessage(dataMessage);
	spdk_thread_send_msg(thread, fn, ctx);
	hfsStatsMessageSubmitLockRelease();
	pthread_mutex_unlock(&g_runtime.messageSubmitLock);
}

/* Instrument the runtime state mutex while keeping the fast path lock-free in stats-off mode. */
static int hfsRuntimeMutexLock(pthread_mutex_t *lock) {
	int rc;

	hfsStatsLockBeforeFast(HFS_STATS_LOCK_RUNTIME);
	rc = pthread_mutex_lock(lock);
	hfsStatsLockAfterFast(HFS_STATS_LOCK_RUNTIME);
	return rc;
}

/* Release the runtime state mutex and account for its hold duration. */
static int hfsRuntimeMutexUnlock(pthread_mutex_t *lock) {
	hfsStatsLockReleaseFast(HFS_STATS_LOCK_RUNTIME);
	return pthread_mutex_unlock(lock);
}

/* Keep every runtime state mutex operation paired with its timing sample. */
#define HFS_RUNTIME_STATE_LOCK(lock) hfsRuntimeMutexLock(lock)
#define HFS_RUNTIME_STATE_UNLOCK(lock) hfsRuntimeMutexUnlock(lock)

/* Optional wrapper retained only while statistics are enabled. */
typedef struct HfsRuntimeMessage {
	spdk_msg_fn fn;
	void *ctx;
	void *profileChild;
	uint64_t enqueueNs;
} HfsRuntimeMessage;

/* SPDK thread that receives every HFS operation message. */
struct spdk_thread *globalThread;

/* Add milliseconds to a monotonic timestamp. */
static void hfsRuntimeAddMs(struct timespec *ts, uint32_t timeoutMs) {
	ts->tv_sec += timeoutMs / 1000;
	ts->tv_nsec += (long)(timeoutMs % 1000) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec++;
		ts->tv_nsec -= 1000000000L;
	}
}

/* Return the remaining relative time before a monotonic deadline. */
static bool hfsRuntimeRemaining(const struct timespec *deadline,
				struct timespec *remaining) {
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	if (now.tv_sec > deadline->tv_sec ||
	    (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)) {
		return false;
	}

	remaining->tv_sec = deadline->tv_sec - now.tv_sec;
	remaining->tv_nsec = deadline->tv_nsec - now.tv_nsec;
	if (remaining->tv_nsec < 0) {
		remaining->tv_sec--;
		remaining->tv_nsec += 1000000000L;
	}
	return true;
}

/* Wake threads waiting on one completion state. */
static void hfsRuntimeFutexWake(HfsCompletion *completion) {
	syscall(SYS_futex, (int *)&completion->state, FUTEX_WAKE_PRIVATE,
		INT_MAX, NULL, NULL, 0);
}

/* Issue a short architecture-specific pause during a gated adaptive wait. */
static inline void hfsRuntimeCpuRelax(void) {
#if defined(__x86_64__) || defined(__i386__)
	__asm__ volatile("pause" ::: "memory");
#else
	sched_yield();
#endif
}

/* Start the asynchronous subsystem finalizer before exiting the owner thread. */
static void hfsRuntimeSubsystemFiniDone(void *arg) {
	struct hfs_runtime_state *runtime = arg;

	spdk_thread_exit(runtime->spdkThread);
}

/* Stop the SPDK thread after all subsystem resources have been finalized. */
static void hfsRuntimeStopMessage(void *arg) {
	struct hfs_runtime_state *runtime = arg;

	if (atomic_load_explicit(&runtime->subsystemsReady, memory_order_acquire)) {
		spdk_subsystem_fini(hfsRuntimeSubsystemFiniDone, runtime);
	} else {
		spdk_thread_exit(runtime->spdkThread);
	}
}

/* Dispatch a statistics-wrapped message on the SPDK owner thread. */
static void hfsRuntimeDispatchMessage(void *arg) {
	HfsRuntimeMessage *message = arg;
	uint64_t queueWaitNs;
	uint64_t ownerStartNs;
	uint64_t ownerNs;
	uint64_t depth;

	depth = atomic_fetch_sub_explicit(&g_runtime.queued, 1,
		memory_order_relaxed) - 1U;
	atomic_fetch_add_explicit(&g_runtime.inflight, 1, memory_order_relaxed);
	queueWaitNs = hfsStatsNowNs() - message->enqueueNs;
	hfsProfileChildDequeue(message->profileChild);
	ownerStartNs = hfsStatsNowNs();
	message->fn(message->ctx);
	ownerNs = hfsStatsNowNs() - ownerStartNs;
	depth = atomic_fetch_sub_explicit(&g_runtime.inflight, 1,
		memory_order_relaxed);
	hfsStatsRecordRuntimeDispatchFast(queueWaitNs, ownerNs, depth);
	free(message);
}

/* Drain all requests currently waiting behind one owner-thread notification. */
static void hfsRuntimeDispatchBatch(void *arg) {
	HfsRuntimeBatchItem *item;
	HfsRuntimeBatchItem *next;
	uint64_t queueWaitNs;
	uint64_t ownerStartNs;
	uint64_t ownerNs;
	uint64_t depth;
	bool timing;

	(void)arg;
	timing = hfsStatsTimingFastEnabled();
	pthread_mutex_lock(&g_runtime.batchLock);
	item = g_runtime.batchHead;
	g_runtime.batchHead = NULL;
	g_runtime.batchTail = NULL;
	/* Clear the gate before running callbacks so a concurrent submit schedules the next batch. */
	g_runtime.batchScheduled = false;
	pthread_mutex_unlock(&g_runtime.batchLock);

	while (item != NULL) {
		next = item->next;
		if (hfsTraceEnabledFast()) {
			uint64_t nowNs = hfsTraceNowNs();
			uint64_t waitNs = item->enqueueNs != 0 && nowNs >= item->enqueueNs ?
					  nowNs - item->enqueueNs : 0;
			hfsTraceEmit(HFS_TRACE_RUNTIME_DEQUEUE,
				     HFS_TRACE_OPERATION_NONE,
				     HFS_TRACE_BUFFER_NONE,
				     0, item->traceParentId, 0, 0, 0, 0,
				     (uint32_t)atomic_load_explicit(&g_runtime.queued,
								    memory_order_relaxed),
				     0, 0, 0, waitNs, 0);
		}
		hfsProfileChildDequeue(item->profileChild);
		if (item->op != NULL) {
			(void)hfsOpMarkSubmitted(item->op);
		}
		if (timing) {
			depth = atomic_fetch_sub_explicit(&g_runtime.queued, 1,
				memory_order_relaxed) - 1U;
			atomic_fetch_add_explicit(&g_runtime.inflight, 1,
				memory_order_relaxed);
			queueWaitNs = hfsStatsNowNs() - item->enqueueNs;
			ownerStartNs = hfsStatsNowNs();
			item->fn(item->ctx);
			ownerNs = hfsStatsNowNs() - ownerStartNs;
			depth = atomic_fetch_sub_explicit(&g_runtime.inflight, 1,
				memory_order_relaxed);
			hfsStatsRecordRuntimeDispatchFast(queueWaitNs, ownerNs, depth);
		} else {
			item->fn(item->ctx);
		}
		item = next;
	}
}

/* Return one bounded credit to a reactor and wake blocked producers. */
static void hfsRuntimeDataCreditReleaseReactor(HfsRuntimeDataReactor *reactor) {
	if (reactor == NULL) {
		return;
	}
	pthread_mutex_lock(&reactor->batchLock);
	if (reactor->inflight != 0) {
		reactor->inflight--;
	}
	pthread_cond_broadcast(&reactor->queueAvailable);
	pthread_mutex_unlock(&reactor->batchLock);
}

/* Release one reactor credit after the backend callback has retired. */
static void hfsRuntimeDataCreditRelease(HfsCompletion *completion) {
	HfsRuntimeDataReactor *reactor;
	uint32_t reactorId;

	if (completion == NULL ||
	    !atomic_exchange_explicit(&completion->runtimeCreditHeld, false,
					   memory_order_acq_rel)) {
		return;
	}
	reactorId = completion->runtimeReactorId;
	if (reactorId == 0 || g_runtime.dataReactors == NULL ||
	    reactorId >= g_runtime.dataReactorCount) {
		return;
	}
	reactor = &g_runtime.dataReactors[reactorId - 1U];
	hfsRuntimeDataCreditReleaseReactor(reactor);
}

/* Drain one data-reactor batch while retaining a bounded backend credit. */
static void hfsRuntimeDispatchDataBatch(void *arg) {
	HfsRuntimeDataReactor *reactor = arg;
	HfsRuntimeBatchItem *item;
	HfsRuntimeBatchItem *next;
	bool timing = hfsStatsTimingFastEnabled();

	/* Keep the notification armed while draining newly appended items. */
	for (;;) {
		uint64_t batchSize = 0;

		pthread_mutex_lock(&reactor->batchLock);
		item = reactor->batchHead;
		reactor->batchHead = NULL;
		reactor->batchTail = NULL;
		if (item == NULL) {
			reactor->batchScheduled = false;
			pthread_mutex_unlock(&reactor->batchLock);
			break;
		}
		pthread_mutex_unlock(&reactor->batchLock);
		for (HfsRuntimeBatchItem *cursor = item; cursor != NULL;
		     cursor = cursor->next) {
			batchSize++;
		}
		hfsStatsRecordQueueDrainFast(reactor->index, batchSize);
		while (item != NULL) {
			uint64_t queueWaitNs = 0;
			uint64_t ownerStartNs = 0;
			uint64_t depth = 0;
			uint64_t nowNs;
			uint32_t currentQueueDepth;

			next = item->next;
			pthread_mutex_lock(&reactor->batchLock);
			if (reactor->queueDepth != 0) {
				reactor->queueDepth--;
			}
			reactor->inflight++;
			currentQueueDepth = reactor->queueDepth;
			pthread_mutex_unlock(&reactor->batchLock);
			if (hfsTraceEnabledFast()) {
				nowNs = hfsTraceNowNs();
				queueWaitNs = item->enqueueNs != 0 && nowNs >= item->enqueueNs ?
					nowNs - item->enqueueNs : 0;
				hfsTraceEmit(HFS_TRACE_RUNTIME_DEQUEUE,
					     HFS_TRACE_OPERATION_NONE,
					     HFS_TRACE_BUFFER_NONE,
					     0, item->traceParentId, 0, 0, 0, 0,
					     currentQueueDepth, reactor->index, 0, 0,
					     queueWaitNs, 0);
			}
			hfsProfileChildDequeue(item->profileChild);
			if (item->op != NULL) {
				(void)hfsOpMarkSubmitted(item->op);
			}
			if (timing) {
				depth = atomic_fetch_sub_explicit(&g_runtime.queued, 1,
						memory_order_relaxed) - 1U;
				atomic_fetch_add_explicit(&g_runtime.inflight, 1,
						memory_order_relaxed);
				ownerStartNs = hfsStatsNowNs();
			}
			item->fn(item->ctx);
			if (timing) {
				uint64_t ownerNs = hfsStatsNowNs() - ownerStartNs;
				depth = atomic_fetch_sub_explicit(&g_runtime.inflight, 1,
						memory_order_relaxed);
				hfsStatsRecordRuntimeDispatchFast(queueWaitNs, ownerNs, depth);
			}
			/* Non-I/O users do not attach a completion and release immediately. */
			if (item->completion == NULL) {
				hfsRuntimeDataCreditReleaseReactor(reactor);
			}
			item = next;
		}
	}
}

/* Publish owner initialization state to hfsRuntimeStart. */
static void hfsRuntimePublishReady(int rc) {
	HFS_RUNTIME_STATE_LOCK(&g_runtime.lock);
	g_runtime.initRc = rc;
	g_runtime.ready = true;
	atomic_store_explicit(&g_runtime.accepting, rc == 0,
		memory_order_release);
	hfsTraceThreadWake(HFS_TRACE_OPERATION_THREAD, HFS_TRACE_WAKE_RUNTIME,
		HFS_TRACE_META_OP_NONE, 0, 0, 0, 0, 0,
		(uint32_t)atomic_load_explicit(&g_runtime.queued, memory_order_relaxed),
		0, 0, rc);
	pthread_cond_broadcast(&g_runtime.stateChanged);
	HFS_RUNTIME_STATE_UNLOCK(&g_runtime.lock);
}

/* Block signals reserved for the host process signal-management thread. */
static int hfsRuntimeBlockManagedSignals(sigset_t *previousMask) {
	sigset_t managedSignals;

	sigemptyset(&managedSignals);
	sigaddset(&managedSignals, SIGHUP);
	sigaddset(&managedSignals, SIGQUIT);
	sigaddset(&managedSignals, SIGALRM);
	sigaddset(&managedSignals, SIGTERM);
	sigaddset(&managedSignals, SIGTSTP);
	return pthread_sigmask(SIG_BLOCK, &managedSignals, previousMask);
}

/* Parse the optional CPU used by the metadata owner reactor. */
static int hfsRuntimeOwnerCpuEnv(void) {
	const char *value = getenv("HFS_OWNER_REACTOR_CPU");
	char *end = NULL;
	unsigned long parsed;

	if (value == NULL || value[0] == '\0') {
		return hfsRuntimeBoolEnv("HFS_REACTOR_PIN", false) ? 0 : -1;
	}
	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed > INT_MAX) {
		return -1;
	}
	return (int)parsed;
}

/* Initialize SPDK and poll one SPDK thread until the runtime is stopped. */
static void *hfsRuntimeOwnerMain(void *arg) {
	struct spdk_env_opts opts = {
		.opts_size = sizeof(struct spdk_env_opts),
	};
	struct spdk_thread *thread;
	sigset_t previousSignalMask;
	bool signalMaskSaved = false;
	int rc;

	(void)arg;
	rc = hfsRuntimeBlockManagedSignals(&previousSignalMask);
	if (rc != 0) {
		HFS_LOG_ERROR("failed to block managed signals rc=%d", rc);
		hfsRuntimePublishReady(-rc);
		return NULL;
	}
	signalMaskSaved = true;
	{
		int ownerCpu = hfsRuntimeOwnerCpuEnv();

		if (ownerCpu >= 0) {
			cpu_set_t cpuSet;

			CPU_ZERO(&cpuSet);
			CPU_SET((unsigned int)ownerCpu, &cpuSet);
			(void)pthread_setaffinity_np(pthread_self(), sizeof(cpuSet), &cpuSet);
		}
	}
	spdk_env_opts_init(&opts);
	rc = spdk_env_init(&opts);
	if (rc != 0) {
		pthread_sigmask(SIG_SETMASK, &previousSignalMask, NULL);
		hfsRuntimePublishReady(rc);
		return NULL;
	}

	spdk_log_set_print_level(SPDK_LOG_NOTICE);
	spdk_log_set_level(SPDK_LOG_NOTICE);
	spdk_log_open(NULL);

	rc = spdk_thread_lib_init(NULL, 0);
	if (rc != 0) {
		spdk_env_fini();
		if (signalMaskSaved) {
			pthread_sigmask(SIG_SETMASK, &previousSignalMask, NULL);
		}
		hfsRuntimePublishReady(rc);
		return NULL;
	}

	thread = spdk_thread_create("hfs-owner", NULL);
	if (thread == NULL) {
		spdk_thread_lib_fini();
		spdk_env_fini();
		if (signalMaskSaved) {
			pthread_sigmask(SIG_SETMASK, &previousSignalMask, NULL);
		}
		hfsRuntimePublishReady(-ENOMEM);
		return NULL;
	}

	HFS_RUNTIME_STATE_LOCK(&g_runtime.lock);
	atomic_store_explicit(&g_runtime.spdkThread, thread, memory_order_release);
	globalThread = thread;
	g_runtime.threadCreated = true;
	atomic_store_explicit(&g_runtime.ownerPid, getpid(), memory_order_release);
	HFS_RUNTIME_STATE_UNLOCK(&g_runtime.lock);
	spdk_set_thread(thread);
	g_hfsRuntimeOwnerThread = true;
	hfsRuntimePublishReady(0);

	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}

	spdk_set_thread(NULL);
	spdk_thread_destroy(thread);
	spdk_thread_lib_fini();
	spdk_env_fini();
	if (signalMaskSaved) {
		pthread_sigmask(SIG_SETMASK, &previousSignalMask, NULL);
	}

	HFS_RUNTIME_STATE_LOCK(&g_runtime.lock);
	globalThread = NULL;
	atomic_store_explicit(&g_runtime.spdkThread, NULL, memory_order_release);
	atomic_store_explicit(&g_runtime.accepting, false, memory_order_release);
	atomic_store_explicit(&g_runtime.ownerPid, 0, memory_order_release);
	hfsTraceThreadWake(HFS_TRACE_OPERATION_THREAD, HFS_TRACE_WAKE_RUNTIME,
		HFS_TRACE_META_OP_NONE, 0, 0, 0, 0, 0,
		(uint32_t)atomic_load_explicit(&g_runtime.queued, memory_order_relaxed),
		0, 0, 0);
	pthread_cond_broadcast(&g_runtime.stateChanged);
	HFS_RUNTIME_STATE_UNLOCK(&g_runtime.lock);
	g_hfsRuntimeOwnerThread = false;
	return NULL;
}

/* Return the number of online logical CPUs, with a safe single-CPU fallback. */
static uint32_t hfsRuntimeOnlineCpuCount(void) {
	long cpuCount = sysconf(_SC_NPROCESSORS_ONLN);

	if (cpuCount <= 0) {
		return 1U;
	}
	if ((unsigned long)cpuCount > UINT32_MAX) {
		return UINT32_MAX;
	}
	return (uint32_t)cpuCount;
}

/* Select a conservative automatic total: owner plus roughly one data reactor per four CPUs. */
static uint32_t hfsRuntimeAutoReactorCount(void) {
	uint32_t cpuCount = hfsRuntimeOnlineCpuCount();
	uint32_t dataCount;

	/* A single logical CPU cannot host a separate polling thread without oversubscription. */
	if (cpuCount <= 1U) {
		return 1U;
	}
	dataCount = cpuCount / 4U;
	if (dataCount == 0U) {
		dataCount = 1U;
	}
	if (dataCount > 4U) {
		dataCount = 4U;
	}
	return dataCount + 1U;
}

/* Parse a bounded total reactor count; reactor zero remains the metadata owner. */
static uint32_t hfsRuntimeDataReactorCountEnv(void) {
	const char *value = getenv("HFS_DATA_REACTORS");
	char *end = NULL;
	unsigned long parsed;

	if (value == NULL || value[0] == '\0') {
		return hfsRuntimeAutoReactorCount();
	}
	parsed = strtoul(value, &end, 10);
	if (end == value || *end != '\0' || parsed == 0 ||
	    parsed > HFS_RUNTIME_MAX_DATA_REACTORS) {
		return 1;
	}
	return (uint32_t)parsed;
}

/* Parse a bounded per-reactor queue depth for POSIX backpressure. */
static uint32_t hfsRuntimeQueueDepthEnv(void) {
	const char *value = getenv("HFS_POSIX_QUEUE_DEPTH");
	char *end = NULL;
	unsigned long parsed;

	if (value == NULL || value[0] == '\0') {
		return HFS_RUNTIME_DEFAULT_QUEUE_DEPTH;
	}
	parsed = strtoul(value, &end, 10);
	if (end == value || *end != '\0' || parsed < HFS_RUNTIME_MIN_QUEUE_DEPTH ||
	    parsed > HFS_RUNTIME_MAX_QUEUE_DEPTH) {
		return HFS_RUNTIME_DEFAULT_QUEUE_DEPTH;
	}
	return (uint32_t)parsed;
}

/* Parse the configured logical stripe size used for data-reactor routing. */
static uint64_t hfsRuntimeDataStripeBytesEnv(void) {
	const char *value = getenv("HFS_DATA_STRIPE_KIB");
	char *end = NULL;
	unsigned long parsed;

	if (value == NULL || value[0] == '\0') {
		return HFS_RUNTIME_DEFAULT_STRIPE_KIB << 10;
	}
	parsed = strtoul(value, &end, 10);
	if (end == value || *end != '\0' ||
	    parsed < HFS_RUNTIME_MIN_STRIPE_KIB ||
	    parsed > HFS_RUNTIME_MAX_STRIPE_KIB) {
		return HFS_RUNTIME_DEFAULT_STRIPE_KIB << 10;
	}
	return (uint64_t)parsed << 10;
}

/* Parse the bounded idle poll backoff in microseconds; zero preserves busy-polling. */
static uint32_t hfsRuntimeIdleBackoffUsEnv(void) {
	const char *value = getenv("HFS_REACTOR_IDLE_BACKOFF_US");
	char *end = NULL;
	unsigned long parsed;

	if (value == NULL || value[0] == '\0') {
		return 0;
	}
	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed > 1000000UL) {
		return 0;
	}
	return (uint32_t)parsed;
}

/* Parse the optional adaptive wait window, expressed in microseconds. */
static uint64_t hfsRuntimeWaitSpinNsEnv(void) {
	const char *value = getenv("HFS_WAIT_SPIN_US");
	char *end = NULL;
	double parsed;

	if (value == NULL || value[0] == '\0') {
		value = getenv("HFS_ADAPTIVE_SPIN_US");
	}
	if (value == NULL || value[0] == '\0') {
		return 0;
	}
	errno = 0;
	parsed = strtod(value, &end);
	if (errno != 0 || end == value || *end != '\0' || parsed != parsed ||
	    parsed < 0.0 || parsed > 5.0) {
		return 0;
	}
	return (uint64_t)(parsed * 1000.0 + 0.5);
}

/* Return the optional CPU affinity entry for one data reactor. */
static int hfsRuntimeDataCpuEnv(unsigned int index) {
	const char *value = getenv("HFS_DATA_REACTOR_CPUS");
	const char *cursor;
	char *end = NULL;
	unsigned int current = 0;
	unsigned long parsed;

	if (value == NULL || value[0] == '\0') {
		return -1;
	}
	cursor = value;
	while (*cursor != '\0') {
		while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') {
			cursor++;
		}
		if (*cursor == '\0') {
			break;
		}
		parsed = strtoul(cursor, &end, 10);
		if (end == cursor) {
			return -1;
		}
		if (current == index && parsed <= INT_MAX) {
			return (int)parsed;
		}
		current++;
		cursor = end;
		while (*cursor == ' ' || *cursor == '\t') {
			cursor++;
		}
		if (*cursor != ',' && *cursor != '\0') {
			return -1;
		}
	}
	/* Missing entries fall back to owner+index only for the explicit pin mode. */
	return hfsRuntimeBoolEnv("HFS_REACTOR_PIN", false) ? (int)index + 1 : -1;
}

/* Publish one data reactor's SPDK thread and initialization result. */
static void hfsRuntimeDataPublishReady(HfsRuntimeDataReactor *reactor,
					       int rc) {
	pthread_mutex_lock(&reactor->lock);
	reactor->initRc = rc;
	reactor->ready = true;
	hfsTraceThreadWake(HFS_TRACE_OPERATION_THREAD, HFS_TRACE_WAKE_RUNTIME,
		HFS_TRACE_META_OP_NONE, 0, 0, 0, 0, 0, 0, reactor->index, 0, rc);
	pthread_cond_broadcast(&reactor->stateChanged);
	pthread_mutex_unlock(&reactor->lock);
}

/* Poll one independently owned SPDK data reactor until shutdown. */
static void *hfsRuntimeDataMain(void *arg) {
	HfsRuntimeDataReactor *reactor = arg;
	struct spdk_thread *thread;
	char name[32];
	sigset_t previousSignalMask;
	uint64_t lastPollNs = 0;
	unsigned int idlePolls = 0;
	bool signalMaskSaved = false;
	int rc;

	rc = hfsRuntimeBlockManagedSignals(&previousSignalMask);
	if (rc != 0) {
		hfsRuntimeDataPublishReady(reactor, -rc);
		return NULL;
	}
	signalMaskSaved = true;
	if (reactor->cpu >= 0) {
		cpu_set_t cpuSet;

		CPU_ZERO(&cpuSet);
		CPU_SET((unsigned int)reactor->cpu, &cpuSet);
		(void)pthread_setaffinity_np(pthread_self(), sizeof(cpuSet), &cpuSet);
	}
	snprintf(name, sizeof(name), "hfs-data-%u", reactor->index);
	thread = spdk_thread_create(name, NULL);
	if (thread == NULL) {
		if (signalMaskSaved) {
			pthread_sigmask(SIG_SETMASK, &previousSignalMask, NULL);
		}
		hfsRuntimeDataPublishReady(reactor, -ENOMEM);
		return NULL;
	}
	atomic_store_explicit(&reactor->spdkThread, thread, memory_order_release);
	pthread_mutex_lock(&reactor->lock);
	reactor->threadCreated = true;
	pthread_mutex_unlock(&reactor->lock);
	hfsRuntimeDataPublishReady(reactor, 0);
	spdk_set_thread(thread);
	g_hfsRuntimeDataThread = true;
	while (!spdk_thread_is_exited(thread)) {
		int pollResult;

		pollResult = spdk_thread_poll(thread, 0, 0);

		/* Poll telemetry is enabled only for the explicit timing profile. */
		if (hfsStatsTimingFastEnabled()) {
			uint64_t nowNs = hfsStatsNowNs();
			uint64_t gapNs = lastPollNs != 0 && nowNs >= lastPollNs ?
				nowNs - lastPollNs : 0;
			atomic_store_explicit(&reactor->lastPollExitNs, nowNs,
				memory_order_release);
			hfsStatsRecordReactorPoll(reactor->index, pollResult > 0, gapNs);
			lastPollNs = nowNs;
		}
		if (reactor->idleBackoffUs != 0 && pollResult == 0) {
			bool idle;

			pthread_mutex_lock(&reactor->batchLock);
			idle = reactor->queueDepth == 0 && reactor->inflight == 0 &&
				!reactor->queueClosed;
			pthread_mutex_unlock(&reactor->batchLock);
			if (idle) {
				idlePolls++;
				if (idlePolls >= 8U) {
					struct timespec delay = {
						.tv_sec = reactor->idleBackoffUs / 1000000U,
						.tv_nsec = (long)(reactor->idleBackoffUs % 1000000U) * 1000L,
					};

					(void)nanosleep(&delay, NULL);
					idlePolls = 0;
				}
			} else {
				idlePolls = 0;
			}
		} else if (pollResult != 0) {
			/* A message or completion immediately returns to busy polling. */
			idlePolls = 0;
		}
	}
	g_hfsRuntimeDataThread = false;
	spdk_set_thread(NULL);
	spdk_thread_destroy(thread);
	atomic_store_explicit(&reactor->spdkThread, NULL, memory_order_release);
	if (signalMaskSaved) {
		pthread_sigmask(SIG_SETMASK, &previousSignalMask, NULL);
	}
	return NULL;
}

/* Request one data reactor to exit after its queued messages drain. */
static void hfsRuntimeDataStopMessage(void *arg) {
	HfsRuntimeDataReactor *reactor = arg;
	struct spdk_thread *thread =
		atomic_load_explicit(&reactor->spdkThread, memory_order_acquire);

	if (thread != NULL) {
		spdk_thread_exit(thread);
	}
}

/* Start optional data reactors after the global SPDK thread library is ready. */
static int hfsRuntimeStartDataReactors(void) {
	uint32_t count = hfsRuntimeDataReactorCountEnv();
	uint32_t i;
	uint32_t initialized = 0;
	uint32_t created = 0;
	int rc;
	const char *configured = getenv("HFS_DATA_REACTORS");

	/* Make the automatic decision visible without touching the I/O hot path. */
	HFS_LOG_INFO("reactor topology: online_cpus=%u total=%u data=%u source=%s",
		hfsRuntimeOnlineCpuCount(), count, count > 0U ? count - 1U : 0U,
		configured != NULL && configured[0] != '\0' ? "env" : "auto");

	g_runtime.dataReactorCount = count;
	if (count <= 1) {
		return 0;
	}
	g_runtime.dataReactors = calloc(count - 1U,
				       sizeof(*g_runtime.dataReactors));
	if (g_runtime.dataReactors == NULL) {
		g_runtime.dataReactorCount = 0;
		return -ENOMEM;
	}
	for (i = 1; i < count; i++) {
		HfsRuntimeDataReactor *reactor = &g_runtime.dataReactors[i - 1U];

		reactor->index = i;
		reactor->cpu = hfsRuntimeDataCpuEnv(i - 1U);
		reactor->idleBackoffUs = hfsRuntimeIdleBackoffUsEnv();
		pthread_mutex_init(&reactor->lock, NULL);
		pthread_cond_init(&reactor->stateChanged, NULL);
		pthread_mutex_init(&reactor->batchLock, NULL);
		pthread_cond_init(&reactor->queueAvailable, NULL);
		reactor->maxDepth = g_runtime.queueDepthLimit;
		reactor->queueClosed = false;
		initialized = i;
		rc = pthread_create(&reactor->owner, NULL, hfsRuntimeDataMain,
				    reactor);
		if (rc != 0) {
			break;
		}
		created = i;
		pthread_mutex_lock(&reactor->lock);
		while (!reactor->ready) {
			HfsTraceWaitToken traceWait = {0};
			hfsTraceThreadWaitBegin(&traceWait, HFS_TRACE_OPERATION_THREAD,
				HFS_TRACE_WAKE_RUNTIME, HFS_TRACE_META_OP_NONE, 0, 0, 0, 0,
				0, 0, reactor->index, 0);
			{
				int waitRc = pthread_cond_wait(&reactor->stateChanged,
					&reactor->lock);
				hfsTraceThreadWaitEnd(&traceWait, waitRc);
			}
		}
		rc = reactor->initRc;
		pthread_mutex_unlock(&reactor->lock);
		if (rc != 0) {
			break;
		}
	}
	if (i != count || rc != 0) {
		for (uint32_t j = 1; j <= created; j++) {
			HfsRuntimeDataReactor *reactor =
				&g_runtime.dataReactors[j - 1U];
			struct spdk_thread *thread = atomic_load_explicit(
				&reactor->spdkThread, memory_order_acquire);

			if (thread != NULL) {
				hfsRuntimeSendMessage(thread, hfsRuntimeDataStopMessage,
						 reactor, true);
			}
		}
		for (uint32_t j = 1; j <= created; j++) {
			HfsRuntimeDataReactor *reactor =
				&g_runtime.dataReactors[j - 1U];
			pthread_join(reactor->owner, NULL);
		}
		for (uint32_t j = 1; j <= initialized; j++) {
			HfsRuntimeDataReactor *reactor =
				&g_runtime.dataReactors[j - 1U];
			pthread_cond_destroy(&reactor->stateChanged);
			pthread_cond_destroy(&reactor->queueAvailable);
			pthread_mutex_destroy(&reactor->batchLock);
			pthread_mutex_destroy(&reactor->lock);
		}
		free(g_runtime.dataReactors);
		g_runtime.dataReactors = NULL;
		g_runtime.dataReactorCount = 0;
		return rc != 0 ? rc : -EIO;
	}
	return 0;
}

/* Stop and join all optional data reactors before the global SPDK thread. */
static void hfsRuntimeStopDataReactors(void) {
	uint32_t i;

	if (g_runtime.dataReactors == NULL) {
		g_runtime.dataReactorCount = 0;
		return;
	}
	for (i = 1; i < g_runtime.dataReactorCount; i++) {
		HfsRuntimeDataReactor *reactor = &g_runtime.dataReactors[i - 1U];
		struct spdk_thread *thread = atomic_load_explicit(
			&reactor->spdkThread, memory_order_acquire);

		if (thread != NULL) {
		hfsRuntimeSendMessage(thread, hfsRuntimeDataStopMessage, reactor, true);
		}
	}
	for (i = 1; i < g_runtime.dataReactorCount; i++) {
		HfsRuntimeDataReactor *reactor = &g_runtime.dataReactors[i - 1U];

		pthread_join(reactor->owner, NULL);
		pthread_cond_destroy(&reactor->stateChanged);
		pthread_cond_destroy(&reactor->queueAvailable);
		pthread_mutex_destroy(&reactor->batchLock);
		pthread_mutex_destroy(&reactor->lock);
	}
	free(g_runtime.dataReactors);
	g_runtime.dataReactors = NULL;
	g_runtime.dataReactorCount = 0;
}

/* Start the owner pthread once and wait for SPDK initialization. */
int hfsRuntimeStart(void) {
	const char *batchEnv;
	const char *waitMode;
	int rc;

	HFS_RUNTIME_STATE_LOCK(&g_runtime.lock);
	if (g_runtime.threadCreated &&
	    atomic_load_explicit(&g_runtime.accepting, memory_order_acquire)) {
		HFS_RUNTIME_STATE_UNLOCK(&g_runtime.lock);
		return 0;
	}
	if (g_runtime.threadCreated || g_runtime.ready) {
		HFS_RUNTIME_STATE_UNLOCK(&g_runtime.lock);
		return -ESHUTDOWN;
	}
	batchEnv = getenv("HFS_RUNTIME_BATCH");
	g_runtime.posixQueueEnabled = hfsRuntimeBoolEnv("HFS_POSIX_QUEUE", true);
	g_runtime.queueDepthLimit = hfsRuntimeQueueDepthEnv();
	g_runtime.batchEnabled = (batchEnv == NULL || strcmp(batchEnv, "0") != 0) &&
		g_runtime.posixQueueEnabled;
	g_runtime.dataStripeBytes = hfsRuntimeDataStripeBytesEnv();
	waitMode = getenv("HFS_WAIT_MODE");
	g_runtime.waitModeAdaptive = waitMode != NULL &&
		!strcasecmp(waitMode, "adaptive");
	g_runtime.waitSpinNs = g_runtime.waitModeAdaptive ?
		hfsRuntimeWaitSpinNsEnv() : 0;

	rc = pthread_create(&g_runtime.owner, NULL, hfsRuntimeOwnerMain, NULL);
	if (rc != 0) {
		HFS_RUNTIME_STATE_UNLOCK(&g_runtime.lock);
		return -rc;
	}
	while (!g_runtime.ready) {
		HfsTraceWaitToken traceWait = {0};
		hfsTraceThreadWaitBegin(&traceWait, HFS_TRACE_OPERATION_THREAD,
			HFS_TRACE_WAKE_RUNTIME, HFS_TRACE_META_OP_NONE, 0, 0, 0, 0,
			0, 0, 0, 0);
		{
			int waitRc = pthread_cond_wait(&g_runtime.stateChanged,
				&g_runtime.lock);
			hfsTraceThreadWaitEnd(&traceWait, waitRc);
		}
	}
	rc = g_runtime.initRc;
	HFS_RUNTIME_STATE_UNLOCK(&g_runtime.lock);

	if (rc != 0) {
		pthread_join(g_runtime.owner, NULL);
		return rc;
	}
	rc = hfsRuntimeStartDataReactors();
	if (rc != 0) {
		(void)hfsRuntimeStop();
	}
	return rc;
}

/* Submit a message while preventing a concurrent runtime stop race. */
int hfsRuntimeSubmit(spdk_msg_fn fn, void *ctx) {
	HfsRuntimeMessage *message = NULL;
	struct spdk_thread *thread;
	uint64_t queueDepth;
	int rc = 0;

	/* A fork child must never enqueue a pointer into the parent's SPDK queue. */
	if (hfsRuntimeIsForkChild()) {
		HFS_STATS_SPDK_REJECT();
		return -ECHILD;
	}
	/* Admission count closes the stop-vs-submit race without taking state.lock. */
	atomic_fetch_add_explicit(&g_runtime.submitters, 1,
		memory_order_acquire);
	if (!atomic_load_explicit(&g_runtime.accepting, memory_order_acquire)) {
		rc = -ESHUTDOWN;
		goto submit_done;
	}
	thread = atomic_load_explicit(&g_runtime.spdkThread, memory_order_acquire);
	if (thread == NULL) {
		rc = -ESHUTDOWN;
		goto submit_done;
	}
	if (hfsStatsTimingFastEnabled()) {
		message = calloc(1, sizeof(*message));
		if (message == NULL) {
			HFS_STATS_ERROR();
			rc = -ENOMEM;
			goto submit_done;
		}
		message->fn = fn;
		message->ctx = ctx;
		message->enqueueNs = hfsStatsNowNs();
	}
	/* Recheck after allocation so stop cannot admit a late request. */
	if (hfsRuntimeIsForkChild()) {
		rc = -ECHILD;
	} else if (!atomic_load_explicit(&g_runtime.accepting, memory_order_acquire)) {
		rc = -ESHUTDOWN;
	} else {
		HFS_STATS_SPDK_SUBMIT();
		if (message != NULL) {
			queueDepth = atomic_fetch_add_explicit(&g_runtime.queued, 1,
				memory_order_relaxed) + 1U;
			hfsStatsRecordRuntimeSubmitFast(queueDepth);
			hfsRuntimeSendMessage(thread, hfsRuntimeDispatchMessage, message,
						 false);
			message = NULL;
		} else {
			hfsRuntimeSendMessage(thread, fn, ctx, false);
		}
	}

submit_done:
	if (rc != 0) {
		HFS_STATS_SPDK_REJECT();
	}
	free(message);
	atomic_fetch_sub_explicit(&g_runtime.submitters, 1,
		memory_order_release);
	return rc;
}

/* Submit one message to a non-global data reactor. */
/* Submit a direct data message and retain the optional profiling child. */
static int hfsRuntimeSubmitDataWithProfile(uint32_t reactorId,
						 spdk_msg_fn fn, void *ctx,
						 void *profileChild) {
	HfsRuntimeMessage *message = NULL;
	HfsRuntimeDataReactor *reactor;
	struct spdk_thread *thread;
	uint64_t queueDepth;
	int rc = 0;

	if (fn == NULL) {
		return -EINVAL;
	}
	if (reactorId == 0) {
		return hfsRuntimeSubmit(fn, ctx);
	}
	if (hfsRuntimeIsForkChild()) {
		HFS_STATS_SPDK_REJECT();
		return -ECHILD;
	}
	if (g_runtime.dataReactors == NULL ||
	    reactorId >= g_runtime.dataReactorCount) {
		HFS_STATS_SPDK_REJECT();
		return -ESHUTDOWN;
	}
	reactor = &g_runtime.dataReactors[reactorId - 1U];
	atomic_fetch_add_explicit(&g_runtime.submitters, 1,
					memory_order_acquire);
	if (!atomic_load_explicit(&g_runtime.accepting, memory_order_acquire)) {
		rc = -ESHUTDOWN;
		goto data_submit_done;
	}
	thread = atomic_load_explicit(&reactor->spdkThread, memory_order_acquire);
	if (thread == NULL) {
		rc = -ESHUTDOWN;
		goto data_submit_done;
	}
	if (hfsStatsTimingFastEnabled()) {
		message = calloc(1, sizeof(*message));
		if (message == NULL) {
			HFS_STATS_ERROR();
			rc = -ENOMEM;
			goto data_submit_done;
		}
		message->fn = fn;
		message->ctx = ctx;
	message->profileChild = profileChild;
		message->enqueueNs = hfsStatsNowNs();
	}
	if (hfsRuntimeIsForkChild() ||
	    !atomic_load_explicit(&g_runtime.accepting, memory_order_acquire)) {
		rc = hfsRuntimeIsForkChild() ? -ECHILD : -ESHUTDOWN;
		goto data_submit_done;
	}
	HFS_STATS_SPDK_SUBMIT();
	if (message != NULL) {
		hfsProfileChildEnqueue(profileChild, 0);
		queueDepth = atomic_fetch_add_explicit(&g_runtime.queued, 1,
						memory_order_relaxed) + 1U;
		hfsStatsRecordRuntimeSubmitFast(queueDepth);
		hfsRuntimeSendMessage(thread, hfsRuntimeDispatchMessage, message,
					 true);
		message = NULL;
	} else {
		hfsRuntimeSendMessage(thread, fn, ctx, true);
	}

data_submit_done:
	if (rc != 0) {
		HFS_STATS_SPDK_REJECT();
	}
	free(message);
	atomic_fetch_sub_explicit(&g_runtime.submitters, 1,
					memory_order_release);
	return rc;
}

/* Submit one direct data message without a profile child. */
int hfsRuntimeSubmitData(uint32_t reactorId, spdk_msg_fn fn, void *ctx) {
	return hfsRuntimeSubmitDataWithProfile(reactorId, fn, ctx, NULL);
}

/* Queue one request on a data reactor and coalesce concurrent submissions. */
int hfsRuntimeSubmitDataBatch(uint32_t reactorId, HfsRuntimeBatchItem *item,
				      spdk_msg_fn fn, void *ctx) {
	HfsRuntimeDataReactor *reactor;
	struct spdk_thread *thread;
	bool notify = false;
	bool timing;
	bool creditWaited = false;
	bool directFallback = false;
	uint64_t creditWaitStartNs = 0;
	uint64_t queueDepth = 0;
	int rc = 0;

	if (reactorId == 0) {
		return hfsRuntimeSubmitBatch(item, fn, ctx);
	}
	if (item == NULL || fn == NULL || g_runtime.dataReactors == NULL ||
	    reactorId >= g_runtime.dataReactorCount) {
		HFS_STATS_SPDK_REJECT();
		return -EINVAL;
	}
	if (!g_runtime.batchEnabled || !g_runtime.posixQueueEnabled) {
		return hfsRuntimeSubmitData(reactorId, fn, ctx);
	}
	if (hfsRuntimeIsForkChild()) {
		HFS_STATS_SPDK_REJECT();
		return -ECHILD;
	}
	reactor = &g_runtime.dataReactors[reactorId - 1U];
	atomic_fetch_add_explicit(&g_runtime.submitters, 1, memory_order_acquire);
	if (!atomic_load_explicit(&g_runtime.accepting, memory_order_acquire)) {
		rc = -ESHUTDOWN;
		goto data_batch_done;
	}
	thread = atomic_load_explicit(&reactor->spdkThread, memory_order_acquire);
	if (thread == NULL) {
		rc = -ESHUTDOWN;
		goto data_batch_done;
	}
	timing = hfsStatsTimingFastEnabled();
	pthread_mutex_lock(&reactor->batchLock);
	while (!reactor->queueClosed &&
	       atomic_load_explicit(&g_runtime.accepting, memory_order_acquire) &&
	       reactor->queueDepth + reactor->inflight >= reactor->maxDepth) {
		HfsTraceWaitToken traceWait = {0};

		if (hfsRuntimeIsOwnerThread()) {
			rc = -EAGAIN;
			break;
		}
		/*
		 * POSIX range locks are held by the caller until this request completes.
		 * Blocking here can invert that lock order: a full queue waits for an
		 * earlier request while retaining a range lock needed by that request.
		 * Fall back to the legacy single-message submission instead of waiting.
		 */
		if (timing && !creditWaited) {
			creditWaited = true;
			creditWaitStartNs = hfsStatsNowNs();
		}
		hfsTraceThreadWaitBegin(&traceWait, HFS_TRACE_OPERATION_THREAD,
			HFS_TRACE_WAKE_RUNTIME, HFS_TRACE_META_OP_NONE, 0, 0, 0, 0,
			reactor->queueDepth + reactor->inflight, reactorId, 0, 0);
		hfsTraceThreadWaitEnd(&traceWait, EAGAIN);
		pthread_mutex_unlock(&reactor->batchLock);
		if (creditWaited) {
			hfsStatsRecordQueueCreditWaitFast(reactorId,
				hfsStatsNowNs() >= creditWaitStartNs ?
				hfsStatsNowNs() - creditWaitStartNs : 0);
		}
		directFallback = true;
		rc = hfsRuntimeSubmitDataWithProfile(reactorId, fn, ctx,
			item->profileChild);
		goto data_batch_done;
	}
	if (creditWaited) {
		hfsStatsRecordQueueCreditWaitFast(reactorId,
			hfsStatsNowNs() >= creditWaitStartNs ?
			hfsStatsNowNs() - creditWaitStartNs : 0);
	}
	if (rc != 0 || reactor->queueClosed ||
	    !atomic_load_explicit(&g_runtime.accepting, memory_order_acquire)) {
		if (rc == 0) {
			rc = -ESHUTDOWN;
		}
		pthread_mutex_unlock(&reactor->batchLock);
		goto data_batch_done;
	}
	item->fn = fn;
	item->ctx = ctx;
	item->enqueueNs = timing ? hfsStatsNowNs() : 0;
	item->next = NULL;
	queueDepth = ++reactor->queueDepth;
	if (item->completion != NULL) {
		item->completion->runtimeReactorId = reactorId;
		atomic_store_explicit(&item->completion->runtimeCreditHeld, true,
					      memory_order_release);
	}
	if (reactor->batchTail != NULL) {
		reactor->batchTail->next = item;
	} else {
		reactor->batchHead = item;
	}
	hfsProfileChildEnqueue(item->profileChild, 0);
	reactor->batchTail = item;
	if (!reactor->batchScheduled) {
		reactor->batchScheduled = true;
		notify = true;
	}
	pthread_mutex_unlock(&reactor->batchLock);
	if (timing) {
		uint64_t depth = atomic_fetch_add_explicit(&g_runtime.queued, 1,
				memory_order_relaxed) + 1U;
		hfsStatsRecordRuntimeSubmitFast(depth);
	}
	hfsStatsRecordQueueDepthFast(reactorId, queueDepth);
	hfsTraceEmit(HFS_TRACE_RUNTIME_ENQUEUE,
		     HFS_TRACE_OPERATION_NONE, HFS_TRACE_BUFFER_NONE, 0,
		     item->traceParentId, 0, 0, 0, 0,
		     (uint32_t)queueDepth, reactorId, 0, 0, 0, 0);
	HFS_STATS_SPDK_SUBMIT();
	if (notify) {
		hfsRuntimeSendMessage(thread, hfsRuntimeDispatchDataBatch, reactor, true);
	}

data_batch_done:
	if (rc != 0 && !directFallback) {
		HFS_STATS_SPDK_REJECT();
	}
	atomic_fetch_sub_explicit(&g_runtime.submitters, 1, memory_order_release);
	return rc;
}

/* Queue one request and send only one SPDK message for a concurrent burst. */
int hfsRuntimeSubmitBatch(HfsRuntimeBatchItem *item, spdk_msg_fn fn, void *ctx) {
	struct spdk_thread *thread;
	uint64_t queueDepth;
	bool notify = false;
	bool timing;
	int rc = 0;

	if (item == NULL || fn == NULL) {
		HFS_STATS_SPDK_REJECT();
		return -EINVAL;
	}
	/* Keep an explicit escape hatch for A/B tests and emergency rollback. */
	if (!g_runtime.batchEnabled) {
		return hfsRuntimeSubmit(fn, ctx);
	}
	/* A fork child must never enqueue a pointer into the parent's SPDK queue. */
	if (hfsRuntimeIsForkChild()) {
		HFS_STATS_SPDK_REJECT();
		return -ECHILD;
	}
	atomic_fetch_add_explicit(&g_runtime.submitters, 1,
		memory_order_acquire);
	if (!atomic_load_explicit(&g_runtime.accepting, memory_order_acquire)) {
		rc = -ESHUTDOWN;
		goto submit_done;
	}
	thread = atomic_load_explicit(&g_runtime.spdkThread, memory_order_acquire);
	if (thread == NULL) {
		rc = -ESHUTDOWN;
		goto submit_done;
	}
	/* Recheck admission immediately before linking the caller-owned item. */
	if (hfsRuntimeIsForkChild()) {
		rc = -ECHILD;
		goto submit_done;
	}
	if (!atomic_load_explicit(&g_runtime.accepting, memory_order_acquire)) {
		rc = -ESHUTDOWN;
		goto submit_done;
	}
	timing = hfsStatsTimingFastEnabled();
	item->fn = fn;
	item->ctx = ctx;
	item->enqueueNs = timing ? hfsStatsNowNs() : 0;
	item->next = NULL;
	/* Account before linking so the owner cannot retire an uncounted item. */
	if (timing) {
		queueDepth = atomic_fetch_add_explicit(&g_runtime.queued, 1,
			memory_order_relaxed) + 1U;
		hfsStatsRecordRuntimeSubmitFast(queueDepth);
	}
	hfsTraceEmit(HFS_TRACE_RUNTIME_ENQUEUE,
		     HFS_TRACE_OPERATION_NONE, HFS_TRACE_BUFFER_NONE, 0,
		     item->traceParentId, 0, 0, 0, 0,
		     (uint32_t)atomic_load_explicit(&g_runtime.queued,
						    memory_order_relaxed),
		     0, 0, 0, 0, 0);
	pthread_mutex_lock(&g_runtime.batchLock);
	if (g_runtime.batchTail != NULL) {
		g_runtime.batchTail->next = item;
	} else {
		g_runtime.batchHead = item;
	}
	hfsProfileChildEnqueue(item->profileChild, 0);
	g_runtime.batchTail = item;
	if (!g_runtime.batchScheduled) {
		g_runtime.batchScheduled = true;
		notify = true;
	}
	pthread_mutex_unlock(&g_runtime.batchLock);
	HFS_STATS_SPDK_SUBMIT();
	if (notify) {
		hfsRuntimeSendMessage(thread, hfsRuntimeDispatchBatch, NULL, false);
	}

submit_done:
	if (rc != 0) {
		HFS_STATS_SPDK_REJECT();
	}
	atomic_fetch_sub_explicit(&g_runtime.submitters, 1,
		memory_order_release);
	return rc;
}

/* Request context for allocating or releasing one reactor-local channel. */
typedef struct HfsRuntimeChannelOp {
	Hfs *fs;
	struct spdk_io_channel **slot;
	bool release;
	HfsCompletion completion;
	int rc;
} HfsRuntimeChannelOp;

/* Allocate or release a blobstore channel on its owning reactor thread. */
static void hfsRuntimeChannelOp(void *arg) {
	HfsRuntimeChannelOp *operation = arg;

	if (operation->release) {
		if (operation->slot != NULL && *operation->slot != NULL) {
			spdk_bs_free_io_channel(*operation->slot);
			*operation->slot = NULL;
		}
		operation->rc = 0;
	} else {
		*operation->slot = spdk_bs_alloc_io_channel(operation->fs->blobstore);
		operation->rc = *operation->slot == NULL ? -ENOMEM : 0;
	}
	hfsRuntimeCompletionComplete(&operation->completion);
}

/* Allocate per-reactor channels after the blobstore has been mounted. */
int hfsRuntimeDataInit(Hfs *fs) {
	uint32_t count;
	uint32_t i;

	if (fs == NULL || fs->blobstore == NULL || fs->channel == NULL) {
		return -EINVAL;
	}
	if (fs->dataChannels != NULL) {
		return 0;
	}
	count = g_runtime.dataReactorCount == 0 ? 1 : g_runtime.dataReactorCount;
	fs->dataChannels = calloc(count, sizeof(*fs->dataChannels));
	if (fs->dataChannels == NULL) {
		return -ENOMEM;
	}
	fs->dataReactorCount = count;
	fs->dataChannels[0] = fs->channel;
	for (i = 1; i < count; i++) {
		HfsRuntimeChannelOp operation;

		memset(&operation, 0, sizeof(operation));
		operation.fs = fs;
		operation.slot = &fs->dataChannels[i];
		hfsRuntimeCompletionReset(&operation.completion);
		if (hfsRuntimeSubmitData(i, hfsRuntimeChannelOp, &operation) != 0 ||
		    !hfsRuntimeCompletionWait(&operation.completion,
					       HFS_RUNTIME_TIMEOUT_MS) ||
		    operation.rc != 0) {
			hfsRuntimeDataFini(fs);
			return operation.rc != 0 ? operation.rc : -EIO;
		}
	}
	return 0;
}

/* Release per-reactor channels before the blobstore itself is unloaded. */
int hfsRuntimeDataFini(Hfs *fs) {
	uint32_t i;
	int firstError = 0;

	if (fs == NULL || fs->dataChannels == NULL) {
		return 0;
	}
	for (i = 1; i < fs->dataReactorCount; i++) {
		HfsRuntimeChannelOp operation;

		if (fs->dataChannels[i] == NULL) {
			continue;
		}
		memset(&operation, 0, sizeof(operation));
		operation.fs = fs;
		operation.slot = &fs->dataChannels[i];
		operation.release = true;
		hfsRuntimeCompletionReset(&operation.completion);
		if (hfsRuntimeSubmitData(i, hfsRuntimeChannelOp, &operation) != 0 ||
		    !hfsRuntimeCompletionWait(&operation.completion,
					       HFS_RUNTIME_TIMEOUT_MS) ||
		    operation.rc != 0) {
			if (firstError == 0) {
				firstError = operation.rc != 0 ? operation.rc : -EIO;
			}
		}
	}
	if (firstError != 0) {
		return firstError;
	}
	free(fs->dataChannels);
	fs->dataChannels = NULL;
	fs->dataReactorCount = 0;
	return 0;
}

/* Select a stable reactor by blob stripe while keeping reactor zero as fallback. */
uint32_t hfsRuntimeDataReactorFor(const Hfs *fs, uint64_t blobId,
					 uint64_t logicalUnit) {
	uint64_t hash;
	uint64_t stripeUnits;
	uint64_t stripe;

	if (fs == NULL || fs->dataReactorCount == 0) {
		return 0;
	}
	if (fs->unitSize == 0 || g_runtime.dataStripeBytes == 0) {
		return 0;
	}
	stripeUnits = (g_runtime.dataStripeBytes + fs->unitSize - 1U) /
		fs->unitSize;
	if (stripeUnits == 0) {
		stripeUnits = 1;
	}
	stripe = logicalUnit / stripeUnits;
	/* Mix blob ids and stripes so adjacent stripes spread across reactors. */
	hash = blobId ^ (stripe * UINT64_C(0x9e3779b97f4a7c15));
	hash ^= hash >> 30;
	hash *= UINT64_C(0xbf58476d1ce4e5b9);
	hash ^= hash >> 27;
	hash *= UINT64_C(0x94d049bb133111eb);
	hash ^= hash >> 31;
	/* Reactor zero remains the metadata/compatibility owner. */
	if (fs->dataReactorCount <= 1) {
		return 0;
	}
	return 1U + (uint32_t)(hash % (fs->dataReactorCount - 1U));
}

/* Return the channel owned by one selected data reactor. */
struct spdk_io_channel *hfsRuntimeDataChannel(const Hfs *fs,
							 uint32_t reactorId) {
	if (fs == NULL || fs->dataChannels == NULL ||
	    reactorId >= fs->dataReactorCount) {
		return NULL;
	}
	return fs->dataChannels[reactorId];
}

/* Return the current backend credit count for one data reactor. */
uint32_t hfsRuntimeDataInFlight(uint32_t reactorId) {
	HfsRuntimeDataReactor *reactor;
	uint32_t value;

	if (reactorId == 0 || g_runtime.dataReactors == NULL ||
	    reactorId >= g_runtime.dataReactorCount) {
		return 0;
	}
	reactor = &g_runtime.dataReactors[reactorId - 1U];
	pthread_mutex_lock(&reactor->batchLock);
	value = reactor->inflight;
	pthread_mutex_unlock(&reactor->batchLock);
	return value;
}

/* Return the queued item count for one data reactor. */
uint32_t hfsRuntimeDataQueueDepth(uint32_t reactorId) {
	HfsRuntimeDataReactor *reactor;
	uint32_t value;

	if (reactorId == 0 || g_runtime.dataReactors == NULL ||
	    reactorId >= g_runtime.dataReactorCount) {
		return 0;
	}
	reactor = &g_runtime.dataReactors[reactorId - 1U];
	pthread_mutex_lock(&reactor->batchLock);
	value = reactor->queueDepth;
	pthread_mutex_unlock(&reactor->batchLock);
	return value;
}

/* Estimate scheduler or poll delay using the previous poll completion timestamp. */
uint64_t hfsRuntimeDataPollGapNs(uint32_t reactorId) {
	HfsRuntimeDataReactor *reactor;
	uint64_t lastPollExitNs;
	uint64_t nowNs;

	if (!hfsStatsTimingFastEnabled() || reactorId == 0 ||
	    g_runtime.dataReactors == NULL ||
	    reactorId >= g_runtime.dataReactorCount) {
		return 0;
	}
	reactor = &g_runtime.dataReactors[reactorId - 1U];
	lastPollExitNs = atomic_load_explicit(&reactor->lastPollExitNs,
		memory_order_acquire);
	nowNs = hfsStatsNowNs();
	return lastPollExitNs != 0 && nowNs >= lastPollExitNs ?
		nowNs - lastPollExitNs : 0;
}

/* Mark subsystem setup complete so shutdown can run the matching finalizer. */
void hfsRuntimeMarkSubsystemReady(void) {
	atomic_store_explicit(&g_runtime.subsystemsReady, true,
		memory_order_release);
}

/* Return true only for the dedicated SPDK owner pthread. */
bool hfsRuntimeIsOwnerThread(void) {
	if (hfsRuntimeIsForkChild()) {
		return false;
	}
	return g_hfsRuntimeOwnerThread || g_hfsRuntimeDataThread;
}

/* Return true when this process inherited the runtime from a different PID. */
bool hfsRuntimeIsForkChild(void) {
	pid_t ownerPid;

	ownerPid = atomic_load_explicit(&g_runtime.ownerPid, memory_order_acquire);
	return ownerPid != 0 && ownerPid != getpid();
}

/* Close every data queue and wake producers blocked on bounded credits. */
static void hfsRuntimeCloseDataQueues(void) {
	uint32_t i;

	if (g_runtime.dataReactors == NULL) {
		return;
	}
	for (i = 1; i < g_runtime.dataReactorCount; i++) {
		HfsRuntimeDataReactor *reactor = &g_runtime.dataReactors[i - 1U];

		pthread_mutex_lock(&reactor->batchLock);
		reactor->queueClosed = true;
		pthread_cond_broadcast(&reactor->queueAvailable);
		pthread_mutex_unlock(&reactor->batchLock);
	}
}

/* Wait for queued and backend-owned tickets before destroying reactor threads. */
static void hfsRuntimeDrainDataQueues(void) {
	uint64_t deadline = hfsStatsNowNs() +
		(uint64_t)HFS_RUNTIME_TIMEOUT_MS * 1000000ULL;
	bool busy;
	uint32_t i;

	if (g_runtime.dataReactors == NULL) {
		return;
	}
	do {
		busy = false;
		for (i = 1; i < g_runtime.dataReactorCount; i++) {
			HfsRuntimeDataReactor *reactor =
				&g_runtime.dataReactors[i - 1U];

			pthread_mutex_lock(&reactor->batchLock);
			if (reactor->queueDepth != 0 || reactor->inflight != 0) {
				busy = true;
			}
			pthread_mutex_unlock(&reactor->batchLock);
		}
		if (!busy || hfsStatsNowNs() >= deadline) {
			break;
		}
		sched_yield();
	} while (true);
	if (busy) {
		HFS_LOG_ERROR("data reactor drain timed out; forcing runtime stop");
	}
}

/* Stop the runtime after callers have released all HFS resources. */
int hfsRuntimeStop(void) {
	struct spdk_thread *thread;
	bool submitStop = false;
	uint64_t queued;
	uint64_t inflight;

	if (hfsRuntimeIsForkChild()) {
		return -ECHILD;
	}
	HFS_RUNTIME_STATE_LOCK(&g_runtime.lock);
	if (!g_runtime.threadCreated) {
		HFS_RUNTIME_STATE_UNLOCK(&g_runtime.lock);
		return 0;
	}
	if (g_runtime.stopSubmitted) {
		/* A concurrent stopper waits for the first stopper to finish joining. */
		while (g_runtime.threadCreated) {
			HfsTraceWaitToken traceWait = {0};
			hfsTraceThreadWaitBegin(&traceWait, HFS_TRACE_OPERATION_THREAD,
				HFS_TRACE_WAKE_RUNTIME, HFS_TRACE_META_OP_NONE, 0, 0, 0, 0,
				0, 0, 0, 0);
			{
				int waitRc = pthread_cond_wait(&g_runtime.stateChanged,
					&g_runtime.lock);
				hfsTraceThreadWaitEnd(&traceWait, waitRc);
			}
		}
		HFS_RUNTIME_STATE_UNLOCK(&g_runtime.lock);
		return 0;
	}
	thread = atomic_load_explicit(&g_runtime.spdkThread, memory_order_acquire);
	if (!g_runtime.stopSubmitted) {
		g_runtime.stopSubmitted = true;
		atomic_store_explicit(&g_runtime.accepting, false, memory_order_release);
		hfsRuntimeCloseDataQueues();
		submitStop = thread != NULL;
	}
	HFS_RUNTIME_STATE_UNLOCK(&g_runtime.lock);

	if (submitStop) {
		/* Drain submitters before placing stop behind every admitted message. */
		while (atomic_load_explicit(&g_runtime.submitters,
				memory_order_acquire) != 0) {
			sched_yield();
		}
		/* Stop data reactors before the global SPDK environment is finalized. */
		hfsRuntimeDrainDataQueues();
		hfsRuntimeStopDataReactors();
		/* Detailed timing mode also waits for wrapped callbacks to retire. */
		do {
			queued = atomic_load_explicit(&g_runtime.queued,
				memory_order_acquire);
			inflight = atomic_load_explicit(&g_runtime.inflight,
				memory_order_acquire);
			if (!hfsStatsTimingFastEnabled() ||
			    (queued == 0 && inflight == 0)) {
				break;
			}
			sched_yield();
		} while (true);
		hfsRuntimeSendMessage(thread, hfsRuntimeStopMessage, &g_runtime, false);
	}

	pthread_join(g_runtime.owner, NULL);
	HFS_RUNTIME_STATE_LOCK(&g_runtime.lock);
	g_runtime.threadCreated = false;
	g_runtime.ready = false;
	g_runtime.stopSubmitted = false;
	atomic_store_explicit(&g_runtime.subsystemsReady, false,
		memory_order_relaxed);
	g_runtime.initRc = 0;
	hfsTraceThreadWake(HFS_TRACE_OPERATION_THREAD, HFS_TRACE_WAKE_RUNTIME,
		HFS_TRACE_META_OP_NONE, 0, 0, 0, 0, 0,
		(uint32_t)atomic_load_explicit(&g_runtime.queued, memory_order_relaxed),
		0, 0, 0);
	pthread_cond_broadcast(&g_runtime.stateChanged);
	HFS_RUNTIME_STATE_UNLOCK(&g_runtime.lock);
	return 0;
}

/* Reset one request before its SPDK message is submitted. */
void hfsRuntimeCompletionReset(HfsCompletion *completion) {
	atomic_store_explicit(&completion->runtimeCreditHeld, false,
				      memory_order_relaxed);
	completion->runtimeReactorId = 0;
	atomic_store_explicit(&completion->state, HFS_COMPLETION_PENDING,
				      memory_order_relaxed);
}

/* Complete one request and wake any blocked business thread. */
void hfsRuntimeCompletionComplete(HfsCompletion *completion) {
	bool diagnostics;

	if (completion == NULL) {
		return;
	}
	diagnostics = hfsStatsEnabledFast() || hfsStatsTimingFastEnabled() ||
		hfsTraceEnabledFast();
	if (hfsStatsEnabledFast()) {
		HFS_STATS_SPDK_COMPLETE();
	}
	hfsRuntimeDataCreditRelease(completion);
	if (diagnostics && !hfsRuntimeIsOwnerThread() &&
	    !atomic_exchange_explicit(&g_ownerViolationLogged, true, memory_order_relaxed)) {
		HFS_LOG_ERROR("SPDK completion arrived outside owner thread");
	}
	atomic_store_explicit(&completion->state, HFS_COMPLETION_DONE, memory_order_release);
	if (diagnostics) {
		hfsProfileChildWake(completion->profileChild);
		/* Record the wake immediately before futex wake for diagnostics. */
		hfsTraceEmitDetail(HFS_TRACE_COMPLETION_WAKE,
			(HfsTraceOperation)completion->traceOperation,
			(HfsTraceBufferKind)completion->traceBufferKind,
			completion->traceRequestId, completion->traceParentId,
			completion->traceInodeId, completion->traceOffset,
			completion->traceBytes, 0,
			(uint32_t)atomic_load_explicit(&g_runtime.queued, memory_order_relaxed),
			0, 0, 0, 0, 0, HFS_TRACE_WAKE_COMPLETION,
			completion->traceMetadataOperation, 0, 0);
	}
	hfsRuntimeFutexWake(completion);
}

/* Wait for one request with an optional bounded adaptive spin before futex sleep. */
bool hfsRuntimeCompletionWait(HfsCompletion *completion, uint32_t timeoutMs) {
	struct timespec deadline;
	struct timespec remaining;
	uint64_t traceStartNs = hfsTraceNowNs();
	uint64_t spinStartNs;
	int expected;
	int rc;
	bool completed = true;

	hfsTraceEmitDetail(HFS_TRACE_COMPLETION_WAIT_BEGIN,
		(HfsTraceOperation)completion->traceOperation,
		(HfsTraceBufferKind)completion->traceBufferKind,
		completion->traceRequestId, completion->traceParentId,
		completion->traceInodeId, completion->traceOffset,
		completion->traceBytes, 0,
		(uint32_t)atomic_load_explicit(&g_runtime.queued, memory_order_relaxed),
		0, 0, 0, 0, 0, 0,
		completion->traceMetadataOperation, 0, 0);

	clock_gettime(CLOCK_MONOTONIC, &deadline);
	hfsRuntimeAddMs(&deadline, timeoutMs);
	if (g_runtime.waitModeAdaptive && g_runtime.waitSpinNs != 0 &&
	    atomic_load_explicit(&completion->state, memory_order_acquire) ==
		HFS_COMPLETION_PENDING) {
		spinStartNs = hfsStatsNowNs();
		while (atomic_load_explicit(&completion->state, memory_order_acquire) ==
		       HFS_COMPLETION_PENDING) {
			uint64_t nowNs = hfsStatsNowNs();

			if (spinStartNs == 0 || nowNs < spinStartNs ||
			    nowNs - spinStartNs >= g_runtime.waitSpinNs) {
				break;
			}
			hfsRuntimeCpuRelax();
		}
	}
	while (atomic_load_explicit(&completion->state, memory_order_acquire) !=
	       HFS_COMPLETION_DONE) {
		if (!hfsRuntimeRemaining(&deadline, &remaining)) {
			/* Mark timeout only if the callback has not won the race. */
			expected = HFS_COMPLETION_PENDING;
			if (atomic_compare_exchange_strong_explicit(
					&completion->state, &expected, HFS_COMPLETION_TIMED_OUT,
					memory_order_acq_rel, memory_order_acquire)) {
				/* Keep the caller's context alive until the owner callback retires. */
				errno = ETIMEDOUT;
					while (atomic_load_explicit(&completion->state,
						memory_order_acquire) != HFS_COMPLETION_DONE) {
						hfsStatsRecordFutexAttempt();
						syscall(SYS_futex, (int *)&completion->state,
							FUTEX_WAIT_PRIVATE, HFS_COMPLETION_TIMED_OUT,
							NULL, NULL, 0);
					}
					completed = false;
					break;
				}
			if (expected == HFS_COMPLETION_DONE) {
				break;
			}
			continue;
		}
		{
			uint64_t sleepStart = hfsStatsNowNs();

			hfsStatsRecordFutexAttempt();
		rc = syscall(SYS_futex, (int *)&completion->state, FUTEX_WAIT_PRIVATE,
			     HFS_COMPLETION_PENDING, &remaining, NULL, 0);
			if (rc == 0) {
				hfsStatsRecordFutexSuccess(hfsStatsNowNs() >= sleepStart ?
					hfsStatsNowNs() - sleepStart : 0);
			} else if (errno == EAGAIN) {
				hfsStatsRecordFutexEagain();
			} else if (errno == EINTR) {
				hfsStatsRecordFutexEintr();
			}
		}
		if (rc != 0 && errno != EAGAIN && errno != EINTR) {
			completed = false;
			break;
		}
	}
	hfsProfileChildWaitReturn(completion->profileChild);
	hfsTraceEmitDetail(HFS_TRACE_COMPLETION_WAIT_END,
		(HfsTraceOperation)completion->traceOperation,
		(HfsTraceBufferKind)completion->traceBufferKind,
		completion->traceRequestId, completion->traceParentId,
		completion->traceInodeId, completion->traceOffset,
		completion->traceBytes, 0,
		(uint32_t)atomic_load_explicit(&g_runtime.queued, memory_order_relaxed),
		0, 0, 0, 0, 0, 0,
		completion->traceMetadataOperation,
		traceStartNs != 0 && hfsTraceNowNs() >= traceStartNs
			? hfsTraceNowNs() - traceStartNs : 0,
		completed ? 0 : -ETIMEDOUT);
	return completed;
}
