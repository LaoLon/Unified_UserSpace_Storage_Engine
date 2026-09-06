#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <time.h>
#include <unistd.h>

#include <spdk/env.h>
#include <spdk/init.h>
#include <spdk/log.h>
#include <spdk/thread.h>

#include "hfs_log.h"
#include "hfs_runtime.h"
#include "hfs_stats.h"

/* Completion states are intentionally simple for one-shot requests. */
#define HFS_COMPLETION_PENDING 0
#define HFS_COMPLETION_DONE 1
#define HFS_COMPLETION_TIMED_OUT 2

/* Runtime state owned by the process-wide HFS library instance. */
struct hfs_runtime_state {
	pthread_mutex_t lock;
	pthread_cond_t stateChanged;
	pthread_t owner;
	struct spdk_thread *spdkThread;
	bool threadCreated;
	bool ready;
	bool accepting;
	bool stopSubmitted;
	bool subsystemsReady;
	int initRc;
	_Atomic pid_t ownerPid;
	_Atomic uint64_t queued;
	_Atomic uint64_t inflight;
};

static struct hfs_runtime_state g_runtime = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.stateChanged = PTHREAD_COND_INITIALIZER,
};
static _Atomic bool g_ownerViolationLogged;

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
#define pthread_mutex_lock(lock) hfsRuntimeMutexLock(lock)
#define pthread_mutex_unlock(lock) hfsRuntimeMutexUnlock(lock)

/* Optional wrapper retained only while statistics are enabled. */
typedef struct HfsRuntimeMessage {
	spdk_msg_fn fn;
	void *ctx;
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

/* Start the asynchronous subsystem finalizer before exiting the owner thread. */
static void hfsRuntimeSubsystemFiniDone(void *arg) {
	struct hfs_runtime_state *runtime = arg;

	spdk_thread_exit(runtime->spdkThread);
}

/* Stop the SPDK thread after all subsystem resources have been finalized. */
static void hfsRuntimeStopMessage(void *arg) {
	struct hfs_runtime_state *runtime = arg;

	if (runtime->subsystemsReady) {
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
	ownerStartNs = hfsStatsNowNs();
	message->fn(message->ctx);
	ownerNs = hfsStatsNowNs() - ownerStartNs;
	depth = atomic_fetch_sub_explicit(&g_runtime.inflight, 1,
		memory_order_relaxed);
	hfsStatsRecordRuntimeDispatchFast(queueWaitNs, ownerNs, depth);
	free(message);
}

/* Publish owner initialization state to hfsRuntimeStart. */
static void hfsRuntimePublishReady(int rc) {
	pthread_mutex_lock(&g_runtime.lock);
	g_runtime.initRc = rc;
	g_runtime.ready = true;
	g_runtime.accepting = rc == 0;
	pthread_cond_broadcast(&g_runtime.stateChanged);
	pthread_mutex_unlock(&g_runtime.lock);
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

	pthread_mutex_lock(&g_runtime.lock);
	g_runtime.spdkThread = thread;
	globalThread = thread;
	g_runtime.threadCreated = true;
	atomic_store_explicit(&g_runtime.ownerPid, getpid(), memory_order_release);
	pthread_mutex_unlock(&g_runtime.lock);
	spdk_set_thread(thread);
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

	pthread_mutex_lock(&g_runtime.lock);
	globalThread = NULL;
	g_runtime.spdkThread = NULL;
	g_runtime.accepting = false;
	atomic_store_explicit(&g_runtime.ownerPid, 0, memory_order_release);
	pthread_cond_broadcast(&g_runtime.stateChanged);
	pthread_mutex_unlock(&g_runtime.lock);
	return NULL;
}

/* Start the owner pthread once and wait for SPDK initialization. */
int hfsRuntimeStart(void) {
	int rc;

	pthread_mutex_lock(&g_runtime.lock);
	if (g_runtime.threadCreated && g_runtime.accepting) {
		pthread_mutex_unlock(&g_runtime.lock);
		return 0;
	}
	if (g_runtime.threadCreated || g_runtime.ready) {
		pthread_mutex_unlock(&g_runtime.lock);
		return -ESHUTDOWN;
	}

	rc = pthread_create(&g_runtime.owner, NULL, hfsRuntimeOwnerMain, NULL);
	if (rc != 0) {
		pthread_mutex_unlock(&g_runtime.lock);
		return -rc;
	}
	while (!g_runtime.ready) {
		pthread_cond_wait(&g_runtime.stateChanged, &g_runtime.lock);
	}
	rc = g_runtime.initRc;
	pthread_mutex_unlock(&g_runtime.lock);

	if (rc != 0) {
		pthread_join(g_runtime.owner, NULL);
	}
	return rc;
}

/* Submit a message while preventing a concurrent runtime stop race. */
int hfsRuntimeSubmit(spdk_msg_fn fn, void *ctx) {
	HfsRuntimeMessage *message = NULL;
	uint64_t queueDepth;
	int rc = 0;

	/* A fork child must never enqueue a pointer into the parent's SPDK queue. */
	if (hfsRuntimeIsForkChild()) {
		HFS_STATS_SPDK_REJECT();
		return -ECHILD;
	}
	if (hfsStatsTimingFastEnabled()) {
		message = calloc(1, sizeof(*message));
		if (message == NULL) {
			HFS_STATS_ERROR();
			return -ENOMEM;
		}
		message->fn = fn;
		message->ctx = ctx;
		message->enqueueNs = hfsStatsNowNs();
	}
	pthread_mutex_lock(&g_runtime.lock);
	if (hfsRuntimeIsForkChild()) {
		rc = -ECHILD;
	} else if (!g_runtime.accepting || g_runtime.spdkThread == NULL) {
		rc = -ESHUTDOWN;
	} else {
		HFS_STATS_SPDK_SUBMIT();
		if (message != NULL) {
			queueDepth = atomic_fetch_add_explicit(&g_runtime.queued, 1,
				memory_order_relaxed) + 1U;
			hfsStatsRecordRuntimeSubmitFast(queueDepth);
			spdk_thread_send_msg(g_runtime.spdkThread,
				hfsRuntimeDispatchMessage, message);
			message = NULL;
		} else {
			spdk_thread_send_msg(g_runtime.spdkThread, fn, ctx);
		}
	}
	if (rc != 0) {
		HFS_STATS_SPDK_REJECT();
	}
	pthread_mutex_unlock(&g_runtime.lock);
	free(message);
	return rc;
}

/* Mark subsystem setup complete so shutdown can run the matching finalizer. */
void hfsRuntimeMarkSubsystemReady(void) {
	pthread_mutex_lock(&g_runtime.lock);
	g_runtime.subsystemsReady = true;
	pthread_mutex_unlock(&g_runtime.lock);
}

/* Return true only for the dedicated SPDK owner pthread. */
bool hfsRuntimeIsOwnerThread(void) {
	bool owner;

	if (hfsRuntimeIsForkChild()) {
		return false;
	}
	pthread_mutex_lock(&g_runtime.lock);
	owner = g_runtime.threadCreated && pthread_equal(pthread_self(), g_runtime.owner);
	pthread_mutex_unlock(&g_runtime.lock);
	return owner;
}

/* Return true when this process inherited the runtime from a different PID. */
bool hfsRuntimeIsForkChild(void) {
	pid_t ownerPid;

	ownerPid = atomic_load_explicit(&g_runtime.ownerPid, memory_order_acquire);
	return ownerPid != 0 && ownerPid != getpid();
}

/* Stop the runtime after callers have released all HFS resources. */
int hfsRuntimeStop(void) {
	if (hfsRuntimeIsForkChild()) {
		return -ECHILD;
	}
	pthread_mutex_lock(&g_runtime.lock);
	if (!g_runtime.threadCreated) {
		pthread_mutex_unlock(&g_runtime.lock);
		return 0;
	}
	if (!g_runtime.stopSubmitted && g_runtime.spdkThread != NULL) {
		g_runtime.stopSubmitted = true;
		g_runtime.accepting = false;
		spdk_thread_send_msg(g_runtime.spdkThread, hfsRuntimeStopMessage, &g_runtime);
	}
	pthread_mutex_unlock(&g_runtime.lock);

	pthread_join(g_runtime.owner, NULL);
	pthread_mutex_lock(&g_runtime.lock);
	g_runtime.threadCreated = false;
	g_runtime.ready = false;
	g_runtime.stopSubmitted = false;
	g_runtime.subsystemsReady = false;
	g_runtime.initRc = 0;
	pthread_mutex_unlock(&g_runtime.lock);
	return 0;
}

/* Reset one request before its SPDK message is submitted. */
void hfsRuntimeCompletionReset(HfsCompletion *completion) {
	atomic_store_explicit(&completion->state, HFS_COMPLETION_PENDING,
				      memory_order_relaxed);
}

/* Complete one request and wake any blocked business thread. */
void hfsRuntimeCompletionComplete(HfsCompletion *completion) {
	HFS_STATS_SPDK_COMPLETE();
	if (!hfsRuntimeIsOwnerThread() &&
	    !atomic_exchange_explicit(&g_ownerViolationLogged, true, memory_order_relaxed)) {
		HFS_LOG_ERROR("SPDK completion arrived outside owner thread");
	}
	atomic_store_explicit(&completion->state, HFS_COMPLETION_DONE, memory_order_release);
	hfsRuntimeFutexWake(completion);
}

/* Wait for one request without consuming CPU in the calling thread. */
bool hfsRuntimeCompletionWait(HfsCompletion *completion, uint32_t timeoutMs) {
	struct timespec deadline;
	struct timespec remaining;
	int expected;
	int rc;

	clock_gettime(CLOCK_MONOTONIC, &deadline);
	hfsRuntimeAddMs(&deadline, timeoutMs);
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
					syscall(SYS_futex, (int *)&completion->state,
						FUTEX_WAIT_PRIVATE, HFS_COMPLETION_TIMED_OUT,
						NULL, NULL, 0);
				}
				return false;
			}
			if (expected == HFS_COMPLETION_DONE) {
				return true;
			}
			continue;
		}
		rc = syscall(SYS_futex, (int *)&completion->state, FUTEX_WAIT_PRIVATE,
			     HFS_COMPLETION_PENDING, &remaining, NULL, 0);
		if (rc != 0 && errno != EAGAIN && errno != EINTR) {
			return false;
		}
	}
	return true;
}
