#include "spdk/stdinc.h"

#include <inttypes.h>

#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"

typedef struct BdevBench BdevBench;

typedef struct BdevBenchOp {
	BdevBench *bench;
	void *buffer;
	uint64_t startTick;
	bool waiting;
	struct spdk_bdev_io_wait_entry waitEntry;
} BdevBenchOp;

/* Ignore unsupported bdev events during the bounded standalone benchmark. */
static void bdevBenchEvent(enum spdk_bdev_event_type type,
		struct spdk_bdev *bdev, void *eventCtx);

struct BdevBench {
	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *channel;
	const char *bdevName;
	uint32_t blockBytes;
	uint32_t queueDepth;
	uint64_t durationSeconds;
	uint64_t rangeBytes;
	uint64_t deadlineTick;
	uint64_t ticksHz;
	uint64_t randomState;
	uint64_t submitted;
	uint64_t completed;
	uint64_t errors;
	uint64_t outstanding;
	uint64_t latencyBuckets[64];
	uint64_t totalLatencyTicks;
	bool stopping;
	bool finished;
	BdevBenchOp *ops;
};

/* Return a positive integer environment setting or a deterministic default. */
static uint64_t bdevBenchEnv(const char *name, uint64_t fallback) {
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
static uint64_t bdevBenchNextRandom(BdevBench *bench) {
	uint64_t value = bench->randomState;

	value ^= value << 13;
	value ^= value >> 7;
	value ^= value << 17;
	bench->randomState = value;
	return value;
}

/* Record a completed bdev I/O in a logarithmic latency histogram. */
static void bdevBenchRecordLatency(BdevBench *bench, uint64_t ticks) {
	unsigned int bucket = ticks == 0 ? 0 : 63U - (unsigned int)__builtin_clzll(ticks);

	if (bucket >= 64U) {
		bucket = 63U;
	}
	bench->latencyBuckets[bucket]++;
	bench->totalLatencyTicks += ticks;
}

/* Convert one histogram percentile to microseconds using SPDK ticks. */
static double bdevBenchQuantileUs(const BdevBench *bench, unsigned int percentile) {
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

/* Stop the app after all outstanding raw bdev requests have completed. */
static void bdevBenchMaybeFinish(BdevBench *bench) {
	if (!bench->stopping || bench->outstanding != 0 || bench->finished) {
		return;
	}
	bench->finished = true;
	printf("A1 bdev=%s bs=%"PRIu32" rw=%s qd=%"PRIu32
	       " iops=%.2f avg_us=%.3f p50_us=%.3f p99_us=%.3f errors=%"PRIu64"\n",
	       bench->bdevName, bench->blockBytes,
	       getenv("TASK_BDEV_RW") != NULL ? getenv("TASK_BDEV_RW") : "randread",
	       bench->queueDepth,
	       (double)bench->completed / (double)bench->durationSeconds,
	       (double)bench->totalLatencyTicks * 1e6 /
	       (double)bench->ticksHz /
	       (double)(bench->completed == 0 ? 1 : bench->completed),
	       bdevBenchQuantileUs(bench, 50), bdevBenchQuantileUs(bench, 99),
	       bench->errors);
	if (bench->channel != NULL) {
		spdk_put_io_channel(bench->channel);
		bench->channel = NULL;
	}
	if (bench->desc != NULL) {
		spdk_bdev_close(bench->desc);
		bench->desc = NULL;
	}
	spdk_app_stop(bench->errors == 0 ? 0 : -EIO);
}

/* Submit one random aligned bdev I/O while the benchmark remains active. */
static int bdevBenchSubmit(BdevBenchOp *op);

/* Handle one completed raw bdev request and refill the fixed queue depth. */
static void bdevBenchIoDone(struct spdk_bdev_io *bdevIo, bool success, void *arg) {
	BdevBenchOp *op = arg;
	BdevBench *bench = op->bench;
	uint64_t now = spdk_get_ticks();

	if (!success) {
		bench->errors++;
	}
	spdk_bdev_free_io(bdevIo);
	if (bench->outstanding != 0) {
		bench->outstanding--;
	}
	bench->completed++;
	if (now >= op->startTick) {
		bdevBenchRecordLatency(bench, now - op->startTick);
	}
	if (spdk_get_ticks() >= bench->deadlineTick) {
		bench->stopping = true;
	}
	if (!bench->stopping) {
		(void)bdevBenchSubmit(op);
	}
	bdevBenchMaybeFinish(bench);
}

/* Retry a request after SPDK signals that bdev resources are available. */
static void bdevBenchIoWait(void *arg) {
	BdevBenchOp *op = arg;

	op->waiting = false;
	(void)bdevBenchSubmit(op);
}

/* Submit one request and queue it when the bdev temporarily runs out of I/O objects. */
static int bdevBenchSubmit(BdevBenchOp *op) {
	BdevBench *bench = op->bench;
	uint64_t rangeBlocks = bench->rangeBytes / bench->blockBytes;
	uint64_t offsetBlocks = rangeBlocks > 1 ?
		bdevBenchNextRandom(bench) % (rangeBlocks - 1U) : 0;
	uint64_t offsetBytes = offsetBlocks * bench->blockBytes;
	const char *rw = getenv("TASK_BDEV_RW");
	int rc;

	if (bench->stopping || op->waiting) {
		return 0;
	}
	op->startTick = spdk_get_ticks();
	if (rw != NULL && strcasecmp(rw, "randwrite") == 0) {
		rc = spdk_bdev_write(bench->desc, bench->channel, op->buffer,
			offsetBytes, bench->blockBytes, bdevBenchIoDone, op);
	} else {
		rc = spdk_bdev_read(bench->desc, bench->channel, op->buffer,
			offsetBytes, bench->blockBytes, bdevBenchIoDone, op);
	}
	if (rc == -ENOMEM) {
		op->waiting = true;
		op->waitEntry.bdev = bench->bdev;
		op->waitEntry.cb_fn = bdevBenchIoWait;
		op->waitEntry.cb_arg = op;
		return spdk_bdev_queue_io_wait(bench->bdev, bench->channel,
			&op->waitEntry);
	}
	if (rc != 0) {
		bench->errors++;
		bench->stopping = true;
		bdevBenchMaybeFinish(bench);
		return rc;
	}
	bench->outstanding++;
	bench->submitted++;
	return 0;
}

/* Begin the fixed-depth raw bdev workload after the bdev is opened. */
static void bdevBenchStartIo(BdevBench *bench) {
	uint32_t i;
	uint32_t align = spdk_bdev_get_buf_align(bench->bdev);

	bench->channel = spdk_bdev_get_io_channel(bench->desc);
	bench->ops = calloc(bench->queueDepth, sizeof(*bench->ops));
	if (bench->channel == NULL || bench->ops == NULL) {
		bench->errors++;
		bench->stopping = true;
		bdevBenchMaybeFinish(bench);
		return;
	}
	for (i = 0; i < bench->queueDepth; i++) {
		bench->ops[i].bench = bench;
		bench->ops[i].buffer = spdk_dma_zmalloc(bench->blockBytes, align, NULL);
		if (bench->ops[i].buffer == NULL) {
			bench->errors++;
			bench->stopping = true;
			return;
		}
		memset(bench->ops[i].buffer, 0x5a, bench->blockBytes);
	}
	bench->ticksHz = spdk_get_ticks_hz();
	bench->durationSeconds = bdevBenchEnv("TASK_BDEV_SECONDS", 5);
	bench->deadlineTick = spdk_get_ticks() +
		bench->durationSeconds * bench->ticksHz;
	bench->randomState = UINT64_C(0x510e527fade682d1);
	for (i = 0; i < bench->queueDepth; i++) {
		(void)bdevBenchSubmit(&bench->ops[i]);
	}
}

/* Open the configured bdev and normalize the request to its block size. */
static void bdevBenchStart(void *arg) {
	BdevBench *bench = arg;
	uint32_t bdevBlockSize;
	uint64_t requestedBytes = bench->blockBytes;
	uint64_t sizeMb = bdevBenchEnv("TASK_BDEV_SIZE_MB", 64);
	int rc;

	rc = spdk_bdev_open_ext(bench->bdevName, true, bdevBenchEvent, NULL, &bench->desc);
	if (rc != 0) {
		SPDK_ERRLOG("failed to open bdev %s: %d\n", bench->bdevName, rc);
		spdk_app_stop(rc);
		return;
	}
	bench->bdev = spdk_bdev_desc_get_bdev(bench->desc);
	bdevBlockSize = spdk_bdev_get_block_size(bench->bdev);
	if (bench->blockBytes < bdevBlockSize) {
		bench->blockBytes = bdevBlockSize;
	}
	if (bench->blockBytes % bdevBlockSize != 0) {
		bench->blockBytes = (bench->blockBytes / bdevBlockSize + 1U) * bdevBlockSize;
	}
	if (requestedBytes != bench->blockBytes) {
		SPDK_WARNLOG("rounded block size %"PRIu64" to bdev block multiple %"PRIu32"\n",
			requestedBytes, bench->blockBytes);
	}
	bench->rangeBytes = sizeMb << 20;
	if (bench->rangeBytes > spdk_bdev_get_num_blocks(bench->bdev) *
			(uint64_t)bdevBlockSize) {
		bench->rangeBytes = spdk_bdev_get_num_blocks(bench->bdev) *
			(uint64_t)bdevBlockSize;
	}
	bench->rangeBytes -= bench->rangeBytes % bench->blockBytes;
	if (bench->rangeBytes < bench->blockBytes) {
		SPDK_ERRLOG("bdev is too small for benchmark range\n");
		spdk_bdev_close(bench->desc);
		bench->desc = NULL;
		spdk_app_stop(-ENOSPC);
		return;
	}
	bdevBenchStartIo(bench);
}

/* Ignore unsupported bdev events during the bounded standalone benchmark. */
static void bdevBenchEvent(enum spdk_bdev_event_type type,
		struct spdk_bdev *bdev, void *eventCtx) {
	(void)type;
	(void)bdev;
	(void)eventCtx;
}

int main(int argc, char **argv) {
	struct spdk_app_opts opts = {};
	BdevBench bench = {};
	const char *bdevName;
	int rc;

	if (argc < 2) {
		fprintf(stderr, "usage: %s SPDK_JSON_CONFIG\n", argv[0]);
		return 2;
	}
	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "task90150022_bdev_bench";
	opts.json_config_file = argv[1];
	opts.rpc_addr = NULL;
	bdevName = getenv("TASK_BDEV_BDEV");
	bench.bdevName = bdevName != NULL ? bdevName : "Nvme0n1";
	bench.blockBytes = (uint32_t)bdevBenchEnv("TASK_BDEV_BS", 4096);
	bench.queueDepth = (uint32_t)bdevBenchEnv("TASK_BDEV_QD", 32);
	if (bench.queueDepth > 64) {
		bench.queueDepth = 64;
	}
	rc = spdk_app_start(&opts, bdevBenchStart, &bench);
	spdk_app_fini();
	if (bench.ops != NULL) {
		uint32_t i;
		for (i = 0; i < bench.queueDepth; i++) {
			spdk_dma_free(bench.ops[i].buffer);
		}
		free(bench.ops);
	}
	return rc;
}
