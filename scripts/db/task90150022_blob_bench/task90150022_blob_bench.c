#include "spdk/stdinc.h"

#include <inttypes.h>
#include <time.h>

#include "spdk/blob.h"
#include "spdk/blob_bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"

typedef struct BlobBench BlobBench;

typedef struct BlobBenchOp {
	BlobBench *bench;
	void *buffer;
	uint64_t startTick;
	uint32_t channelIndex;
} BlobBenchOp;

struct BlobBench {
	struct spdk_blob_store *bs;
	struct spdk_blob *blob;
	struct spdk_io_channel **channels;
	uint32_t channelCount;
	spdk_blob_id blobId;
	const char *bdevName;
	uint32_t blockBytes;
	uint32_t ioUnits;
	uint32_t queueDepth;
	uint64_t durationSeconds;
	uint64_t ioUnitSize;
	uint64_t clusterSize;
	uint64_t rangeUnits;
	uint64_t deadlineTick;
	uint64_t ticksHz;
	uint64_t randomState;
	uint64_t submitted;
	uint64_t completed;
	uint64_t errors;
	uint64_t outstanding;
	uint64_t latencyBuckets[64];
	uint64_t totalLatencyTicks;
	uint32_t finishedOps;
	int rc;
	bool stopping;
	BlobBenchOp *ops;
};

static void blobBenchIoDone(void *arg, int bserrno);

/* Ignore unsupported bdev events during the bounded standalone benchmark. */
static void blobBenchBdevEvent(enum spdk_bdev_event_type type,
		struct spdk_bdev *bdev, void *eventCtx) {
	(void)type;
	(void)bdev;
	(void)eventCtx;
}

/* Return a positive integer environment setting or a deterministic default. */
static uint64_t blobBenchEnv(const char *name, uint64_t fallback) {
	const char *value = getenv(name);
	char *end = NULL;
	unsigned long long parsed;

	if (value == NULL || value[0] == '\0') {
		return fallback;
	}
	parsed = strtoull(value, &end, 10);
	return end == value || *end != '\0' || parsed == 0 ? fallback : parsed;
}

/* Advance the private random offset generator on the SPDK thread. */
static uint64_t blobBenchNextRandom(BlobBench *bench) {
	uint64_t value = bench->randomState;

	value ^= value << 13;
	value ^= value >> 7;
	value ^= value << 17;
	bench->randomState = value;
	return value;
}

/* Record a completed blob I/O in a logarithmic latency histogram. */
static void blobBenchRecordLatency(BlobBench *bench, uint64_t ticks) {
	unsigned int bucket = ticks == 0 ? 0 : 63U - (unsigned int)__builtin_clzll(ticks);

	if (bucket >= 64U) {
		bucket = 63U;
	}
	bench->latencyBuckets[bucket]++;
	bench->totalLatencyTicks += ticks;
}

/* Convert one histogram percentile to microseconds using SPDK ticks. */
static double blobBenchQuantileUs(const BlobBench *bench, unsigned int percentile) {
	uint64_t target = (bench->completed * percentile + 99U) / 100U;
	uint64_t seen = 0;
	unsigned int bucket;

	if (target == 0) {
		target = 1;
	}
	for (bucket = 0; bucket < 64U; bucket++) {
		seen += bench->latencyBuckets[bucket];
		if (seen >= target) {
			return (double)(UINT64_C(1) << bucket) * 1e6 /
				(double)bench->ticksHz;
		}
	}
	return 0.0;
}

/* Stop the SPDK application after blobstore teardown completes. */
static void blobBenchUnloadDone(void *arg, int bserrno) {
	BlobBench *bench = arg;

	if (bserrno != 0 && bench->rc == 0) {
		bench->rc = bserrno;
	}
	spdk_app_stop(bench->rc);
}

/* Close the benchmark blob and unload its blobstore. */
static void blobBenchUnload(BlobBench *bench, int rc) {
	if (rc != 0 && bench->rc == 0) {
		bench->rc = rc;
	}
	if (bench->bs == NULL) {
		spdk_app_stop(bench->rc);
		return;
	}
	if (bench->channels != NULL) {
		uint32_t i;

		for (i = 0; i < bench->channelCount; i++) {
			if (bench->channels[i] != NULL) {
				spdk_bs_free_io_channel(bench->channels[i]);
			}
		}
		free(bench->channels);
		bench->channels = NULL;
	}
	spdk_bs_unload(bench->bs, blobBenchUnloadDone, bench);
}

/* Delete the temporary benchmark blob after all data I/O has completed. */
static void blobBenchDeleteDone(void *arg, int bserrno) {
	BlobBench *bench = arg;

	blobBenchUnload(bench, bserrno);
}

/* Close the temporary benchmark blob before deleting it. */
static void blobBenchCloseDone(void *arg, int bserrno) {
	BlobBench *bench = arg;

	if (bserrno != 0) {
		blobBenchUnload(bench, bserrno);
		return;
	}
	spdk_bs_delete_blob(bench->bs, bench->blobId, blobBenchDeleteDone, bench);
}

/* Begin asynchronous teardown once the last outstanding operation returns. */
static void blobBenchMaybeFinish(BlobBench *bench) {
	if (!bench->stopping || bench->outstanding != 0 || bench->finishedOps != 0) {
		return;
	}
	bench->finishedOps = 1;
	printf("A2 blob bdev=%s bs=%"PRIu32" rw=%s channels=%"PRIu32
	       " qd=%"PRIu32" qd_per_channel=%"PRIu32
	       " iops=%.2f avg_us=%.3f p50_us=%.3f p99_us=%.3f errors=%"PRIu64"\n",
	       bench->bdevName, bench->blockBytes,
	       getenv("TASK_BLOB_RW") != NULL ? getenv("TASK_BLOB_RW") : "randread",
	       bench->channelCount, bench->queueDepth,
	       (bench->queueDepth + bench->channelCount - 1U) / bench->channelCount,
	       (double)bench->completed / (double)bench->durationSeconds,
	       (double)bench->totalLatencyTicks * 1e6 /
	       (double)bench->ticksHz / (double)(bench->completed == 0 ? 1 : bench->completed),
	       blobBenchQuantileUs(bench, 50), blobBenchQuantileUs(bench, 99),
	       bench->errors);
	spdk_blob_close(bench->blob, blobBenchCloseDone, bench);
}

/* Stop issuing new work after the configured wall-clock interval. */
static void blobBenchStop(BlobBench *bench) {
	if (spdk_get_ticks() >= bench->deadlineTick) {
		bench->stopping = true;
	}
}

/* Submit one random blob I/O while the benchmark remains inside its deadline. */
static void blobBenchSubmit(BlobBenchOp *op) {
	BlobBench *bench = op->bench;
	uint64_t offset;
	const char *rw = getenv("TASK_BLOB_RW");

	if (bench->stopping) {
		return;
	}
	{
		uint64_t maxOffset = bench->rangeUnits > bench->ioUnits ?
			bench->rangeUnits - bench->ioUnits : 0;
		offset = maxOffset == 0 ? 0 :
			blobBenchNextRandom(bench) % (maxOffset + 1U);
	}
	op->startTick = spdk_get_ticks();
	bench->outstanding++;
	bench->submitted++;
	if (rw != NULL && strcasecmp(rw, "randwrite") == 0) {
		spdk_blob_io_write(bench->blob, bench->channels[op->channelIndex], op->buffer,
			offset, bench->ioUnits, blobBenchIoDone, op);
	} else {
		spdk_blob_io_read(bench->blob, bench->channels[op->channelIndex], op->buffer,
			offset, bench->ioUnits, blobBenchIoDone, op);
	}
}

/* Complete one child I/O and keep the queue depth full until the deadline. */
static void blobBenchIoDone(void *arg, int bserrno) {
	BlobBenchOp *op = arg;
	BlobBench *bench = op->bench;
	uint64_t now = spdk_get_ticks();

	if (bserrno != 0) {
		bench->errors++;
	}
	if (bench->outstanding != 0) {
		bench->outstanding--;
	}
	bench->completed++;
	if (now >= op->startTick) {
		blobBenchRecordLatency(bench, now - op->startTick);
	}
	blobBenchStop(bench);
	if (!bench->stopping) {
		blobBenchSubmit(op);
	}
	blobBenchMaybeFinish(bench);
}

/* Start the fixed-depth direct blob I/O workload. */
static void blobBenchStartIo(BlobBench *bench) {
	uint32_t i;

	bench->channels = calloc(bench->channelCount, sizeof(*bench->channels));
	bench->ops = calloc(bench->queueDepth, sizeof(*bench->ops));
	if (bench->channels == NULL || bench->ops == NULL) {
		blobBenchUnload(bench, -ENOMEM);
		return;
	}
	for (i = 0; i < bench->channelCount; i++) {
		bench->channels[i] = spdk_bs_alloc_io_channel(bench->bs);
		if (bench->channels[i] == NULL) {
			blobBenchUnload(bench, -ENOMEM);
			return;
		}
	}
	for (i = 0; i < bench->queueDepth; i++) {
		bench->ops[i].bench = bench;
		bench->ops[i].channelIndex = i % bench->channelCount;
		bench->ops[i].buffer = spdk_malloc(bench->blockBytes, 4096, NULL,
			SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		if (bench->ops[i].buffer == NULL) {
			blobBenchUnload(bench, -ENOMEM);
			return;
		}
		memset(bench->ops[i].buffer, 0xa5, bench->blockBytes);
	}
	bench->ticksHz = spdk_get_ticks_hz();
	bench->durationSeconds = blobBenchEnv("TASK_BLOB_SECONDS", 5);
	bench->deadlineTick = spdk_get_ticks() +
		bench->durationSeconds * bench->ticksHz;
	bench->randomState = UINT64_C(0x6a09e667f3bcc909);
	for (i = 0; i < bench->queueDepth; i++) {
		blobBenchSubmit(&bench->ops[i]);
	}
}

/* Finish blob resize and metadata sync before data requests begin. */
static void blobBenchSyncDone(void *arg, int bserrno) {
	BlobBench *bench = arg;

	if (bserrno != 0) {
		blobBenchUnload(bench, bserrno);
		return;
	}
	blobBenchStartIo(bench);
}

/* Resize the temporary blob to the requested bounded benchmark range. */
static void blobBenchResizeDone(void *arg, int bserrno) {
	BlobBench *bench = arg;

	if (bserrno != 0) {
		blobBenchUnload(bench, bserrno);
		return;
	}
	spdk_blob_sync_md(bench->blob, blobBenchSyncDone, bench);
}

/* Open the new blob and reserve enough clusters for random I/O. */
static void blobBenchOpenDone(void *arg, struct spdk_blob *blob, int bserrno) {
	BlobBench *bench = arg;
	uint64_t freeClusters;
	uint64_t wantedClusters;

	if (bserrno != 0 || blob == NULL) {
		blobBenchUnload(bench, bserrno != 0 ? bserrno : -EIO);
		return;
	}
	bench->blob = blob;
	freeClusters = spdk_bs_free_cluster_count(bench->bs);
	wantedClusters = (blobBenchEnv("TASK_BLOB_SIZE_MB", 64) << 20) /
		bench->clusterSize;
	if (wantedClusters == 0) {
		wantedClusters = 1;
	}
	if (wantedClusters > freeClusters) {
		wantedClusters = freeClusters;
	}
	if (wantedClusters == 0) {
		blobBenchUnload(bench, -ENOSPC);
		return;
	}
	bench->rangeUnits = wantedClusters * bench->clusterSize /
		bench->ioUnitSize;
	spdk_blob_resize(bench->blob, wantedClusters, blobBenchResizeDone, bench);
}

/* Capture the new blob id and open its data handle. */
static void blobBenchCreateDone(void *arg, spdk_blob_id blobId, int bserrno) {
	BlobBench *bench = arg;

	if (bserrno != 0 || blobId == SPDK_BLOBID_INVALID) {
		blobBenchUnload(bench, bserrno != 0 ? bserrno : -EIO);
		return;
	}
	bench->blobId = blobId;
	spdk_bs_open_blob(bench->bs, blobId, blobBenchOpenDone, bench);
}

/* Initialize blobstore state and create one temporary benchmark blob. */
static void blobBenchBsInitDone(void *arg, struct spdk_blob_store *bs,
		int bserrno) {
	BlobBench *bench = arg;
	uint32_t requestedBlock;

	if (bserrno != 0 || bs == NULL) {
		blobBenchUnload(bench, bserrno != 0 ? bserrno : -EIO);
		return;
	}
	bench->bs = bs;
	bench->ioUnitSize = spdk_bs_get_io_unit_size(bs);
	bench->clusterSize = spdk_bs_get_cluster_size(bs);
	requestedBlock = bench->blockBytes;
	if (bench->blockBytes < bench->ioUnitSize) {
		bench->blockBytes = (uint32_t)bench->ioUnitSize;
	}
	if (bench->blockBytes % bench->ioUnitSize != 0) {
		bench->blockBytes = (uint32_t)(((uint64_t)bench->blockBytes /
			bench->ioUnitSize + 1U) * bench->ioUnitSize);
	}
	bench->ioUnits = (uint32_t)(bench->blockBytes / bench->ioUnitSize);
	if (requestedBlock != bench->blockBytes) {
		SPDK_WARNLOG("rounded block size %"PRIu32" to blob io unit multiple %"PRIu32"\n",
			requestedBlock, bench->blockBytes);
	}
	spdk_bs_create_blob(bs, blobBenchCreateDone, bench);
}

/* Start blobstore on the configured bdev through the public blob-bdev bridge. */
static void blobBenchStart(void *arg) {
	BlobBench *bench = arg;
	struct spdk_bs_dev *bsDev = NULL;
	int rc;

	rc = spdk_bdev_create_bs_dev_ext(bench->bdevName, blobBenchBdevEvent,
		NULL, &bsDev);
	if (rc != 0) {
		blobBenchUnload(bench, rc);
		return;
	}
	spdk_bs_init(bsDev, NULL, blobBenchBsInitDone, bench);
}

int main(int argc, char **argv) {
	struct spdk_app_opts opts = {};
	BlobBench bench = {0};
	int rc;

	if (argc < 2) {
		fprintf(stderr, "usage: %s SPDK_JSON_CONFIG\n", argv[0]);
		return 2;
	}
	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "task90150022_blob_bench";
	opts.json_config_file = argv[1];
	opts.rpc_addr = NULL;
	bench.bdevName = getenv("TASK_BLOB_BDEV");
	bench.bdevName = bench.bdevName != NULL ? bench.bdevName : "Nvme0n1";
	bench.blockBytes = (uint32_t)blobBenchEnv("TASK_BLOB_BS", 4096);
	bench.queueDepth = (uint32_t)blobBenchEnv("TASK_BLOB_QD", 32);
	bench.channelCount = (uint32_t)blobBenchEnv("TASK_BLOB_CHANNELS", 1);
	if (bench.channelCount > 4) {
		bench.channelCount = 4;
	}
	if (bench.queueDepth > 64) {
		bench.queueDepth = 64;
	}
	rc = spdk_app_start(&opts, blobBenchStart, &bench);
	if (rc == 0 && bench.rc != 0) {
		rc = bench.rc;
	}
	spdk_app_fini();
	if (bench.ops != NULL) {
		uint32_t i;
		for (i = 0; i < bench.queueDepth; i++) {
			spdk_free(bench.ops[i].buffer);
		}
		free(bench.ops);
	}
	return rc;
}
