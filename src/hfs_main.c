#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <spdk/log.h>

#include "hfs.h"
#include "hfs_internal.h"
#include "hfs_io.h"
#include "hfs_lib.h"
#include "hfs_log.h"
#include "hfs_meta.h"
#include "hfs_stats.h"

/*
 * Self-test program: exercises the full HFS lifecycle.
 * Runs mount -> open -> write -> offset read/write -> close -> umount,
 * plus metadata (super blob + inode table) persistence checks.
 */

/* Log a self-test assertion: INFO on pass, ERROR on failure. */
#define HFS_LOG_CHECK(fmt, ok, ...) do {		\
	if (ok) {					\
		HFS_LOG_INFO(fmt, __VA_ARGS__);		\
	} else {					\
		HFS_LOG_ERROR(fmt " [CHECK FAILED]",	\
			      __VA_ARGS__);		\
	}						\
} while (0)

typedef struct HfsInternalBenchStart {
	pthread_mutex_t lock;
	pthread_cond_t condition;
	uint64_t endNs;
	uint32_t readyWorkers;
	bool start;
	bool abort;
} HfsInternalBenchStart;

typedef struct HfsInternalBenchWorker {
	HfsFile *file;
	uint8_t *buffer;
	size_t bytes;
	uint64_t rangeBytes;
	uint64_t unitSize;
	uint64_t endNs;
	bool write;
	uint64_t seed;
	HfsInternalBenchStart *startState;
	_Atomic uint64_t *operations;
	_Atomic uint64_t *errors;
	_Atomic uint64_t *outstanding;
	_Atomic uint64_t *outstandingPeak;
} HfsInternalBenchWorker;

/* Return a monotonic nanosecond clock for the internal data-path benchmark. */
static uint64_t hfsInternalBenchNowNs(void) {
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
		return 0;
	}
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
		(uint64_t)ts.tv_nsec;
}

/* Advance a private pseudo-random state without taking a shared lock. */
static uint64_t hfsInternalBenchNext(HfsInternalBenchWorker *worker) {
	uint64_t value = worker->seed;

	value ^= value << 13;
	value ^= value >> 7;
	value ^= value << 17;
	worker->seed = value;
	return value;
}

/* Parse a positive benchmark setting while keeping the default deterministic. */
static uint64_t hfsInternalBenchEnv(const char *name, uint64_t fallback) {
	const char *value = getenv(name);
	char *end = NULL;
	unsigned long long parsed;

	if (value == NULL || value[0] == '\0') {
		return fallback;
	}
	parsed = strtoull(value, &end, 10);
	return end == value || *end != '\0' || parsed == 0 ? fallback : parsed;
}

/* Publish a worker's observed outstanding request peak without a lock. */
static void hfsInternalBenchRecordPeak(_Atomic uint64_t *peak,
						 uint64_t current) {
	uint64_t observed = atomic_load_explicit(peak, memory_order_relaxed);

	while (observed < current &&
	       !atomic_compare_exchange_weak_explicit(peak, &observed, current,
			memory_order_relaxed, memory_order_relaxed)) {
		/* Retry with the refreshed peak. */
	}
}

/* Run positioned I/O directly through HfsFile and the normal data engine. */
static void *hfsInternalBenchWorkerMain(void *arg) {
	HfsInternalBenchWorker *worker = arg;
	HfsInternalBenchStart *startState = worker->startState;

	pthread_mutex_lock(&startState->lock);
	startState->readyWorkers++;
	pthread_cond_broadcast(&startState->condition);
	while (!startState->start && !startState->abort) {
		pthread_cond_wait(&startState->condition, &startState->lock);
	}
	worker->endNs = startState->endNs;
	if (startState->abort) {
		pthread_mutex_unlock(&startState->lock);
		return NULL;
	}
	pthread_mutex_unlock(&startState->lock);

	while (hfsInternalBenchNowNs() < worker->endNs) {
		uint64_t maxUnit = (worker->rangeBytes - worker->bytes) /
			worker->unitSize;
		uint64_t offset = (maxUnit == 0 ? 0 :
			(hfsInternalBenchNext(worker) % (maxUnit + 1U))) *
			worker->unitSize;
		ssize_t result;
		uint64_t outstanding = atomic_fetch_add_explicit(worker->outstanding, 1,
			memory_order_relaxed) + 1U;

		hfsInternalBenchRecordPeak(worker->outstandingPeak, outstanding);
		result = worker->write ?
			hfsWriteAt(worker->file, worker->buffer, worker->bytes, offset) :
			hfsReadAt(worker->file, worker->buffer, worker->bytes, offset);
		atomic_fetch_sub_explicit(worker->outstanding, 1, memory_order_relaxed);

		if (result == (ssize_t)worker->bytes) {
			atomic_fetch_add_explicit(worker->operations, 1,
				memory_order_relaxed);
		} else {
			atomic_fetch_add_explicit(worker->errors, 1,
				memory_order_relaxed);
		}
	}
	return NULL;
}

/* Benchmark the internal HFS data path after mount and blob preparation. */
static int hfsInternalBenchmark(Hfs *fs) {
	HfsFile file = {0};
	pthread_t *threads = NULL;
	HfsInternalBenchWorker *workers = NULL;
	uint64_t operations;
	uint64_t errors;
	_Atomic uint64_t operationCount = 0;
	_Atomic uint64_t errorCount = 0;
	uint64_t bytes = hfsInternalBenchEnv("HFS_INTERNAL_BENCH_BS", 4096);
	uint64_t threadCount = hfsInternalBenchEnv("HFS_INTERNAL_BENCH_THREADS", 32);
	uint64_t duration = hfsInternalBenchEnv("HFS_INTERNAL_BENCH_SECONDS", 5);
	uint64_t rangeBytes = hfsInternalBenchEnv("HFS_INTERNAL_BENCH_SIZE", 256U << 20);
	const char *rw = getenv("HFS_INTERNAL_BENCH_RW");
	uint64_t startNs = 0;
	uint64_t endNs = 0;
	uint64_t finishNs = 0;
	uint64_t actualRuntimeNs;
	uint64_t i;
	uint64_t allocatedWorkers = 0;
	uint64_t startedWorkers = 0;
	_Atomic uint64_t outstanding = 0;
	_Atomic uint64_t outstandingPeak = 0;
	HfsInternalBenchStart startState;
	bool startLockInitialized = false;
	bool startConditionInitialized = false;
	int rc = -1;

	if (fs == NULL || bytes == 0 || threadCount > 256 || rangeBytes < bytes ||
		rangeBytes % fs->unitSize != 0 || bytes % fs->unitSize != 0) {
		fprintf(stderr, "invalid internal benchmark settings\n");
		return -1;
	}
	file.fs = fs;
	if (!hfsOpen(&file) || hfsResizeTo(&file, rangeBytes) != 0) {
		fprintf(stderr, "internal benchmark blob preparation failed\n");
		goto cleanup;
	}
	threads = calloc(threadCount, sizeof(*threads));
	workers = calloc(threadCount, sizeof(*workers));
	if (threads == NULL || workers == NULL) {
		goto cleanup;
	}
	if (pthread_mutex_init(&startState.lock, NULL) != 0) {
		goto cleanup;
	}
	startLockInitialized = true;
	if (pthread_cond_init(&startState.condition, NULL) != 0) {
		goto cleanup;
	}
	startConditionInitialized = true;
	startState.endNs = 0;
	startState.readyWorkers = 0;
	startState.start = false;
	startState.abort = false;
	for (i = 0; i < threadCount; i++) {
		workers[i].file = &file;
		workers[i].buffer = hfsIoDmaAlloc(bytes, 4096);
		workers[i].bytes = bytes;
		workers[i].rangeBytes = rangeBytes;
		workers[i].unitSize = fs->unitSize;
		workers[i].endNs = 0;
		workers[i].write = rw != NULL && strcasecmp(rw, "randwrite") == 0;
		workers[i].seed = UINT64_C(0x9e3779b97f4a7c15) + i * 17U;
		workers[i].startState = &startState;
		workers[i].operations = &operationCount;
		workers[i].errors = &errorCount;
		workers[i].outstanding = &outstanding;
		workers[i].outstandingPeak = &outstandingPeak;
		if (workers[i].buffer == NULL) {
			atomic_store_explicit(&errorCount, 1, memory_order_relaxed);
			break;
		}
		allocatedWorkers++;
		if (pthread_create(&threads[i], NULL, hfsInternalBenchWorkerMain,
			&workers[i]) != 0) {
			atomic_store_explicit(&errorCount, 1, memory_order_relaxed);
			break;
		}
		startedWorkers++;
	}
	if (startedWorkers != threadCount) {
		pthread_mutex_lock(&startState.lock);
		startState.abort = true;
		pthread_cond_broadcast(&startState.condition);
		pthread_mutex_unlock(&startState.lock);
	} else {
		pthread_mutex_lock(&startState.lock);
		while (startState.readyWorkers != startedWorkers) {
			pthread_cond_wait(&startState.condition, &startState.lock);
		}
		startNs = hfsInternalBenchNowNs();
		endNs = startNs + duration * UINT64_C(1000000000);
		startState.endNs = endNs;
		startState.start = true;
		pthread_cond_broadcast(&startState.condition);
		pthread_mutex_unlock(&startState.lock);
	}
	for (i = 0; i < startedWorkers; i++) {
		pthread_join(threads[i], NULL);
	}
	if (startedWorkers != threadCount) {
		startNs = hfsInternalBenchNowNs();
		endNs = startNs;
	}
	operations = atomic_load_explicit(&operationCount, memory_order_relaxed);
	errors = atomic_load_explicit(&errorCount, memory_order_relaxed);
	finishNs = hfsInternalBenchNowNs();
	actualRuntimeNs = finishNs > startNs ? finishNs - startNs : 1;
	if (startState.start && finishNs <= startNs) {
		actualRuntimeNs = 1;
	}
	printf("A3 internal bs=%"PRIu64" rw=%s workers=%"PRIu64
	       " actual_workers=%"PRIu64" actual_requests=%"PRIu64
	       " actual_spdk_submits=%"PRIuFAST64" outstanding_peak=%"PRIuFAST64
	       " runtime_ns=%"PRIu64" runtime_seconds=%.9f iops=%.2f errors=%"PRIu64"\n",
	       bytes, workers != NULL && workers[0].write ? "randwrite" : "randread",
	       startedWorkers,
	       startedWorkers, operations,
	       atomic_load_explicit(&hfsStatsSpdkSubmits, memory_order_relaxed),
	       atomic_load_explicit(&outstandingPeak, memory_order_relaxed),
	       actualRuntimeNs, (double)actualRuntimeNs / 1e9,
	       (double)operations * 1e9 / (double)actualRuntimeNs, errors);
	rc = errors == 0 ? 0 : -1;

cleanup:
	if (startConditionInitialized) {
		pthread_cond_destroy(&startState.condition);
	}
	if (startLockInitialized) {
		pthread_mutex_destroy(&startState.lock);
	}
	if (workers != NULL) {
		for (i = 0; i < allocatedWorkers; i++) {
			hfsIoDmaFree(workers[i].buffer);
		}
	}
	free(workers);
	free(threads);
	if (file.blob != NULL) {
		hfsClose(&file);
		hfsDeleteBlobById(&file);
	}
	return rc;
}

int main(int argc, char *argv[]) {
	if (hfs_lib_init() != 0) {
		fprintf(stderr, "hfs_lib_init failed\n");
		return -1;
	}

	Hfs *fs = hfs_lib_get_fs();
	if (getenv("HFS_INTERNAL_BENCH") != NULL) {
		int benchmarkRc = hfsInternalBenchmark(fs);
		hfs_lib_fini();
		return benchmarkRc;
	}

	/*
	 * Reload the super blob id within the same process: verifies that
	 * set_super persisted (malloc bdev loses data across restarts).
	 */
	int loadRc = hfsMetaLoad(fs);
	SPDK_NOTICELOG("reload meta rc=%d\n", loadRc);
	HFS_LOG_CHECK("reload meta rc=%d", loadRc == 0, loadRc);

	/*
	 * Inode table round-trip: the root inode created at format time
	 * must survive saveAll -> reset -> loadAll.
	 */
	if (hfsMetaSaveAll(fs) != 0) {
		HFS_LOG_ERROR("saveAll failed");
		SPDK_NOTICELOG("saveAll failed\n");
		return -1;
	}
	fs->inodeCount = 0;
	if (hfsMetaLoadAll(fs) != 0) {
		HFS_LOG_ERROR("loadAll failed");
		SPDK_NOTICELOG("loadAll failed\n");
		return -1;
	}
	SPDK_NOTICELOG("inodeCount=%u first=%s id=%llu\n", fs->inodeCount,
		       fs->inodeCount > 0 ? fs->inodes[0].name : "?",
		       (unsigned long long)(fs->inodeCount > 0 ? fs->inodes[0].id : 0));
	HFS_LOG_CHECK("inodeCount=%u first=%s id=%llu",
		      fs->inodeCount > 0,
		      fs->inodeCount, fs->inodeCount > 0 ? fs->inodes[0].name : "?",
		      (unsigned long long)(fs->inodeCount > 0 ? fs->inodes[0].id : 0));

	/* Path resolution: "/" resolves to the root inode. */
	struct hfs_inode *root = hfsMetaResolvePath(fs, "/");
	SPDK_NOTICELOG("resolve / -> %s id=%llu\n",
		       root ? root->name : "(null)",
		       (unsigned long long)(root ? root->id : 0));
	HFS_LOG_CHECK("resolve / -> %s id=%llu", root != NULL,
		      root ? root->name : "?", (unsigned long long)(root ? root->id : 0));

	/* A missing path must resolve to NULL with errno=ENOENT. */
	errno = 0;
	struct hfs_inode *missing = hfsMetaResolvePath(fs, "/nonexist");
	SPDK_NOTICELOG("resolve /nonexist -> %s errno=%d\n",
		       missing ? missing->name : "(null)", errno);
	HFS_LOG_CHECK("resolve /nonexist errno=%d",
		      missing == NULL && errno == ENOENT, errno);

	/* mkdir /d1 -> stat exists -> rmdir -> stat gone. */
	if (hfs_lib_mkdir("/d1", 0755) != 0) {
		HFS_LOG_ERROR("mkdir /d1 failed errno=%d", errno);
		SPDK_NOTICELOG("mkdir /d1 failed errno=%d\n", errno);
		return -1;
	}
	struct stat dstat;
	int sr = hfs_lib_stat("/d1", &dstat);
	SPDK_NOTICELOG("stat /d1 rc=%d mode=%o\n", sr,
		       (unsigned int)(sr == 0 ? dstat.st_mode : 0));
	HFS_LOG_CHECK("stat /d1 mode=%o", sr == 0,
		      (unsigned int)(sr == 0 ? dstat.st_mode : 0));

	/* Duplicate mkdir must fail with EEXIST. */
	errno = 0;
	int dupRc = hfs_lib_mkdir("/d1", 0755);
	SPDK_NOTICELOG("mkdir /d1 (dup) rc=%d errno=%d\n", dupRc, errno);
	HFS_LOG_CHECK("mkdir /d1 (dup) errno=%d", dupRc != 0 && errno == EEXIST, errno);

	/* rmdir on the (empty) directory. */
	errno = 0;
	int rmRc = hfs_lib_rmdir("/d1");
	SPDK_NOTICELOG("rmdir /d1 rc=%d errno=%d\n", rmRc, errno);
	HFS_LOG_CHECK("rmdir /d1 rc=%d", rmRc == 0, rmRc);

	/* After removal the path must be gone. */
	errno = 0;
	sr = hfs_lib_stat("/d1", &dstat);
	SPDK_NOTICELOG("stat /d1 after rmdir rc=%d errno=%d\n", sr, errno);
	HFS_LOG_CHECK("stat /d1 after rmdir errno=%d", sr != 0 && errno == ENOENT, errno);

	/* Create /a.txt with O_CREAT, stat size=0, reopen without O_CREAT. */
	errno = 0;
	int fd1 = hfs_lib_open("/a.txt", O_CREAT | O_RDWR, 0644);
	SPDK_NOTICELOG("open /a.txt (O_CREAT) fd=%d errno=%d\n", fd1, errno);
	HFS_LOG_CHECK("open /a.txt (O_CREAT) fd=%d", fd1 >= 0, fd1);

	errno = 0;
	sr = hfs_lib_stat("/a.txt", &dstat);
	SPDK_NOTICELOG("stat /a.txt rc=%d size=%lld mode=%o\n", sr,
		       (long long)(sr == 0 ? dstat.st_size : -1),
		       (unsigned int)(sr == 0 ? dstat.st_mode : 0));
	HFS_LOG_CHECK("stat /a.txt size=%lld mode=%o", sr == 0,
		      (long long)(sr == 0 ? dstat.st_size : -1),
		      (unsigned int)(sr == 0 ? dstat.st_mode : 0));

	errno = 0;
	int fd2 = hfs_lib_open("/a.txt", O_RDONLY, 0);
	SPDK_NOTICELOG("reopen /a.txt fd=%d errno=%d\n", fd2, errno);
	HFS_LOG_CHECK("reopen /a.txt fd=%d", fd2 >= 0, fd2);
	if (fd1 >= 0) {
		hfs_lib_close(fd1);
	}
	if (fd2 >= 0) {
		hfs_lib_close(fd2);
	}

	/* unlink: create /b.txt -> unlink -> stat ENOENT -> recreate ok. */
	int fd3 = hfs_lib_open("/b.txt", O_CREAT | O_RDWR, 0644);
	SPDK_NOTICELOG("open /b.txt (O_CREAT) fd=%d\n", fd3);
	HFS_LOG_CHECK("open /b.txt (O_CREAT) fd=%d", fd3 >= 0, fd3);
	if (fd3 >= 0) {
		hfs_lib_close(fd3);
	}

	errno = 0;
	int urc = hfs_lib_unlink("/b.txt");
	SPDK_NOTICELOG("unlink /b.txt rc=%d errno=%d\n", urc, errno);
	HFS_LOG_CHECK("unlink /b.txt rc=%d", urc == 0, urc);

	errno = 0;
	sr = hfs_lib_stat("/b.txt", &dstat);
	SPDK_NOTICELOG("stat /b.txt after unlink rc=%d errno=%d\n", sr, errno);
	HFS_LOG_CHECK("stat /b.txt after unlink errno=%d", sr != 0 && errno == ENOENT, errno);

	int fd4 = hfs_lib_open("/b.txt", O_CREAT | O_RDWR, 0644);
	SPDK_NOTICELOG("recreate /b.txt fd=%d\n", fd4);
	HFS_LOG_CHECK("recreate /b.txt fd=%d", fd4 >= 0, fd4);
	if (fd4 >= 0) {
		hfs_lib_close(fd4);
	}

	/* rename: /a.txt -> /c.txt (same dir), old path ENOENT. */
	errno = 0;
	int rrc = hfs_lib_rename("/a.txt", "/c.txt");
	SPDK_NOTICELOG("rename /a.txt -> /c.txt rc=%d errno=%d\n", rrc, errno);
	HFS_LOG_CHECK("rename /a.txt -> /c.txt rc=%d", rrc == 0, rrc);

	errno = 0;
	sr = hfs_lib_stat("/a.txt", &dstat);
	SPDK_NOTICELOG("stat /a.txt after rename rc=%d errno=%d\n", sr, errno);
	HFS_LOG_CHECK("stat /a.txt after rename errno=%d", sr != 0 && errno == ENOENT, errno);

	sr = hfs_lib_stat("/c.txt", &dstat);
	SPDK_NOTICELOG("stat /c.txt rc=%d size=%lld\n", sr,
		       (long long)(sr == 0 ? dstat.st_size : -1));
	HFS_LOG_CHECK("stat /c.txt size=%lld", sr == 0,
		      (long long)(sr == 0 ? dstat.st_size : -1));

	/* Cross-directory rename: mkdir /d2, move /c.txt into it. */
	if (hfs_lib_mkdir("/d2", 0755) != 0) {
		HFS_LOG_ERROR("mkdir /d2 failed errno=%d", errno);
		SPDK_NOTICELOG("mkdir /d2 failed errno=%d\n", errno);
		return -1;
	}
	rrc = hfs_lib_rename("/c.txt", "/d2/c.txt");
	SPDK_NOTICELOG("rename /c.txt -> /d2/c.txt rc=%d errno=%d\n", rrc, errno);
	HFS_LOG_CHECK("rename /c.txt -> /d2/c.txt rc=%d", rrc == 0, rrc);

	sr = hfs_lib_stat("/d2/c.txt", &dstat);
	SPDK_NOTICELOG("stat /d2/c.txt rc=%d\n", sr);
	HFS_LOG_CHECK("stat /d2/c.txt rc=%d", sr == 0, sr);

	/* rmdir /d2 must now fail with ENOTEMPTY (c.txt inside). */
	errno = 0;
	int rr = hfs_lib_rmdir("/d2");
	SPDK_NOTICELOG("rmdir /d2 (non-empty) rc=%d errno=%d\n", rr, errno);
	HFS_LOG_CHECK("rmdir /d2 (non-empty) errno=%d",
		      rr != 0 && errno == ENOTEMPTY, errno);

	/* ftruncate: open /e.txt, write 4 bytes, stat size=4, truncate 100. */
	int fd5 = hfs_lib_open("/e.txt", O_CREAT | O_RDWR, 0644);
	SPDK_NOTICELOG("open /e.txt fd=%d\n", fd5);
	HFS_LOG_CHECK("open /e.txt fd=%d", fd5 >= 0, fd5);
	if (fd5 >= 0) {
		hfs_lib_write(fd5, "data", 4);
		sr = hfs_lib_stat("/e.txt", &dstat);
		SPDK_NOTICELOG("stat /e.txt after write size=%lld\n",
			       (long long)(sr == 0 ? dstat.st_size : -1));
		HFS_LOG_CHECK("stat /e.txt after write size=%lld", sr == 0,
			      (long long)(sr == 0 ? dstat.st_size : -1));

		int trc = hfs_lib_ftruncate(fd5, 100);
		SPDK_NOTICELOG("ftruncate /e.txt 100 rc=%d errno=%d\n", trc, errno);
		HFS_LOG_CHECK("ftruncate /e.txt 100 rc=%d", trc == 0, trc);

		sr = hfs_lib_stat("/e.txt", &dstat);
		SPDK_NOTICELOG("stat /e.txt after truncate size=%lld\n",
			       (long long)(sr == 0 ? dstat.st_size : -1));
		HFS_LOG_CHECK("stat /e.txt after truncate size=%lld", sr == 0,
			      (long long)(sr == 0 ? dstat.st_size : -1));

		errno = 0;
		int frc = hfs_lib_fstat(fd5, &dstat);
		SPDK_NOTICELOG("fstat /e.txt rc=%d size=%lld errno=%d\n", frc,
			       (long long)(frc == 0 ? dstat.st_size : -1), errno);
		HFS_LOG_CHECK("fstat /e.txt size=%lld", frc == 0,
			      (long long)(frc == 0 ? dstat.st_size : -1));

		hfs_lib_close(fd5);
	}

	SPDK_NOTICELOG("hfsEnvSetup success\n");

	SPDK_NOTICELOG("\n\n hfs open start \n\n");
	HFS_LOG_INFO("hfs open start");
	HfsFile *file = calloc(1, sizeof(HfsFile));
	file->fs = fs;
	hfsOpen(file);

	SPDK_NOTICELOG("\n\n hfs write start \n\n");
	HFS_LOG_INFO("hfs write start");
	char *buffer = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	hfsWrite(file, buffer, strlen(buffer));

	SPDK_NOTICELOG("\n\n hfs offset write/read start \n\n");
	HFS_LOG_INFO("hfs offset write/read start");
	hfsWriteAt(file, (uint8_t *)"BBBB", 4, 100);
	char oBuffer[16] = {0};
	ssize_t on = hfsReadAt(file, (uint8_t *)oBuffer, 4, 100);
	SPDK_NOTICELOG("OFFSET READ: %.*s (n=%zd)\n", (int)on, oBuffer, on);
	HFS_LOG_INFO("OFFSET READ: %.*s (n=%zd)", (int)on, oBuffer, on);

	SPDK_NOTICELOG("\n\n hfs read start \n\n");
	HFS_LOG_INFO("hfs read start");
	char rBuffer[BUFFER_SIZE] = {0};
	hfsReadAt(file, rBuffer, strlen(buffer), 0);
	SPDK_NOTICELOG("READ BUFFER: %s\n", rBuffer);
	HFS_LOG_INFO("READ BUFFER: %s", rBuffer);

	SPDK_NOTICELOG("\n\n hfs close start \n\n");
	HFS_LOG_INFO("hfs close start");
	hfsClose(file);

	free(file);

	HFS_LOG_INFO("hfs exiting");
	hfs_lib_fini();
}
