#define _GNU_SOURCE

#include "hfs_trace.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define HFS_TRACE_RING_CAPACITY 16384U

/* Each thread owns one ring, so event recording does not take a global lock. */
typedef struct HfsTraceThread {
    uint64_t threadId;
    uint64_t writeSequence;
    uint64_t dropped;
    HfsTraceEvent events[HFS_TRACE_RING_CAPACITY];
    struct HfsTraceThread *next;
} HfsTraceThread;

_Atomic bool hfsTraceEnabledState = false;

static pthread_mutex_t g_traceLock = PTHREAD_MUTEX_INITIALIZER;
static HfsTraceThread *g_traceThreads;
static __thread HfsTraceThread *g_traceLocalThread;
static _Atomic uint64_t g_traceRequestId;
static char *g_tracePath;
static uint32_t g_traceSample = 1;
static bool g_traceInitialized;

/* Return a monotonic timestamp that is safe for duration measurements. */
uint64_t hfsTraceNowNs(void)
{
    struct timespec ts;

    if (!hfsTraceEnabledFast()) {
        return 0;
    }
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Parse a boolean environment variable without making tracing mandatory. */
static bool hfsTraceBool(const char *value)
{
    if (value == NULL) {
        return false;
    }
    return strcmp(value, "1") == 0 ||
           strcasecmp(value, "true") == 0 ||
           strcasecmp(value, "yes") == 0 ||
           strcasecmp(value, "on") == 0;
}

/* Parse the optional parent-id sampling interval. */
static uint32_t hfsTraceSampleEnv(const char *value)
{
    char *end = NULL;
    unsigned long sample;

    if (value == NULL || value[0] == '\0') {
        return 1;
    }
    errno = 0;
    sample = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || sample == 0 ||
        sample > UINT32_MAX) {
        return 1;
    }
    return (uint32_t)sample;
}

/* Return the Linux thread id used in exported trace records. */
static uint64_t hfsTraceThreadId(void)
{
    return (uint64_t)syscall(SYS_gettid);
}

/* Allocate the current thread's ring once and publish it for finalization. */
static HfsTraceThread *hfsTraceThreadGet(void)
{
    HfsTraceThread *thread;

    thread = g_traceLocalThread;
    if (thread != NULL) {
        return thread;
    }
    thread = calloc(1, sizeof(*thread));
    if (thread == NULL) {
        return NULL;
    }
    thread->threadId = hfsTraceThreadId();
    pthread_mutex_lock(&g_traceLock);
    thread->next = g_traceThreads;
    g_traceThreads = thread;
    pthread_mutex_unlock(&g_traceLock);
    g_traceLocalThread = thread;
    return thread;
}

/* Convert an enum value to a stable text token for offline analysis. */
static const char *hfsTracePhaseName(HfsTracePhase phase)
{
    switch (phase) {
    case HFS_TRACE_IO_ENTER: return "io_enter";
    case HFS_TRACE_IO_EXIT: return "io_exit";
    case HFS_TRACE_BUFFER_CLASSIFY: return "buffer_classify";
    case HFS_TRACE_LEASE_WAIT_BEGIN: return "lease_wait_begin";
    case HFS_TRACE_LEASE_WAIT_END: return "lease_wait_end";
    case HFS_TRACE_LEASE_ACQUIRE: return "lease_acquire";
    case HFS_TRACE_LEASE_RELEASE: return "lease_release";
    case HFS_TRACE_COPY_IN_BEGIN: return "copy_in_begin";
    case HFS_TRACE_COPY_IN_END: return "copy_in_end";
    case HFS_TRACE_COPY_OUT_BEGIN: return "copy_out_begin";
    case HFS_TRACE_COPY_OUT_END: return "copy_out_end";
    case HFS_TRACE_RANGE_BEGIN: return "range_begin";
    case HFS_TRACE_RANGE_ACQUIRED: return "range_acquired";
    case HFS_TRACE_RANGE_RELEASE: return "range_release";
    case HFS_TRACE_RUNTIME_ENQUEUE: return "runtime_enqueue";
    case HFS_TRACE_RUNTIME_DEQUEUE: return "runtime_dequeue";
    case HFS_TRACE_SPDK_SUBMIT: return "spdk_submit";
    case HFS_TRACE_SPDK_COMPLETE: return "spdk_complete";
    case HFS_TRACE_METADATA_PUBLISH: return "metadata_publish";
    case HFS_TRACE_PARENT_START: return "parent_start";
    case HFS_TRACE_CHILD_START: return "child_start";
    case HFS_TRACE_CHILD_DONE: return "child_done";
    case HFS_TRACE_PARENT_DONE: return "parent_done";
    case HFS_TRACE_LOCK_WAIT_BEGIN: return "lock_wait_begin";
    case HFS_TRACE_LOCK_ACQUIRED: return "lock_acquired";
    case HFS_TRACE_LOCK_RELEASE: return "lock_release";
    case HFS_TRACE_METADATA_BEGIN: return "metadata_begin";
    case HFS_TRACE_METADATA_END: return "metadata_end";
    case HFS_TRACE_FLUSH_BEGIN: return "flush_begin";
    case HFS_TRACE_FLUSH_END: return "flush_end";
    case HFS_TRACE_COMPLETION_WAIT_BEGIN: return "completion_wait_begin";
    case HFS_TRACE_COMPLETION_WAIT_END: return "completion_wait_end";
    case HFS_TRACE_COMPLETION_WAKE: return "completion_wake";
    case HFS_TRACE_THREAD_WAIT_BEGIN: return "thread_wait_begin";
    case HFS_TRACE_THREAD_WAIT_END: return "thread_wait_end";
    case HFS_TRACE_THREAD_WAKE: return "thread_wake";
    case HFS_TRACE_TICKET_ADMIT: return "ticket_admit";
    case HFS_TRACE_TICKET_DEP_WAIT: return "ticket_dependency_wait";
    case HFS_TRACE_TICKET_READY: return "ticket_ready";
    case HFS_TRACE_TICKET_WAKE: return "ticket_wake";
    case HFS_TRACE_PARENT_PROFILE: return "parent_profile";
    default: return "unknown";
    }
}

/* Convert an operation value to a stable text token for offline analysis. */
static const char *hfsTraceOperationName(HfsTraceOperation operation)
{
    switch (operation) {
    case HFS_TRACE_OPERATION_READ: return "read";
    case HFS_TRACE_OPERATION_WRITE: return "write";
    case HFS_TRACE_OPERATION_UNMAP: return "unmap";
    case HFS_TRACE_OPERATION_WRITE_ZEROES: return "write_zeroes";
    case HFS_TRACE_OPERATION_VECTOR_READ: return "vector_read";
    case HFS_TRACE_OPERATION_VECTOR_WRITE: return "vector_write";
    case HFS_TRACE_OPERATION_METADATA: return "metadata";
    case HFS_TRACE_OPERATION_FLUSH: return "flush";
    case HFS_TRACE_OPERATION_COMPLETION: return "completion";
    case HFS_TRACE_OPERATION_LOCK: return "lock";
    case HFS_TRACE_OPERATION_THREAD: return "thread";
    default: return "none";
    }
}

/* Convert a lock class to a stable text token for offline analysis. */
static const char *hfsTraceLockName(uint32_t lockClass)
{
    switch ((HfsTraceLockClass)lockClass) {
    case HFS_TRACE_LOCK_SYNC: return "sync";
    case HFS_TRACE_LOCK_RANGE_STRIPE: return "range_stripe";
    case HFS_TRACE_LOCK_METADATA: return "metadata";
    case HFS_TRACE_LOCK_RUNTIME: return "runtime";
    case HFS_TRACE_LOCK_FD: return "fd";
    case HFS_TRACE_LOCK_APPEND: return "append";
    case HFS_TRACE_LOCK_OTHER: return "other";
    default: return "none";
    }
}

/* Convert a wake reason to a stable text token for offline analysis. */
static const char *hfsTraceWakeName(uint32_t wakeReason)
{
    switch ((HfsTraceWakeReason)wakeReason) {
    case HFS_TRACE_WAKE_COMPLETION: return "completion";
    case HFS_TRACE_WAKE_DURABILITY: return "durability";
    case HFS_TRACE_WAKE_RUNTIME: return "runtime";
    case HFS_TRACE_WAKE_DMA_LEASE: return "dma_lease";
    default: return "none";
    }
}

/* Convert a metadata operation to a stable text token for offline analysis. */
static const char *hfsTraceMetadataName(uint32_t metadataOperation)
{
    switch ((HfsTraceMetadataOperation)metadataOperation) {
    case HFS_TRACE_META_OP_OPEN: return "open";
    case HFS_TRACE_META_OP_READ: return "read";
    case HFS_TRACE_META_OP_WRITE: return "write";
    case HFS_TRACE_META_OP_SYNC: return "sync";
    case HFS_TRACE_META_OP_RESIZE: return "resize";
    case HFS_TRACE_META_OP_CLOSE: return "close";
    case HFS_TRACE_META_OP_TRANSACTION: return "transaction";
    case HFS_TRACE_META_OP_FLUSH: return "flush";
    case HFS_TRACE_META_OP_PUBLISH: return "publish";
    default: return "none";
    }
}

/* Convert a buffer class to a stable text token for offline analysis. */
static const char *hfsTraceBufferName(HfsTraceBufferKind bufferKind)
{
    switch (bufferKind) {
    case HFS_TRACE_BUFFER_CALLER: return "caller";
    case HFS_TRACE_BUFFER_REGISTERED: return "registered";
    case HFS_TRACE_BUFFER_DMA_LEASE: return "dma_lease";
    case HFS_TRACE_BUFFER_VECTOR: return "vector";
    case HFS_TRACE_BUFFER_RMW: return "rmw";
    default: return "none";
    }
}

/* Allocate a monotonically increasing parent id when tracing is active. */
uint64_t hfsTraceNextRequestId(void)
{
    if (!hfsTraceEnabledFast()) {
        return 0;
    }
    return atomic_fetch_add_explicit(&g_traceRequestId, 1, memory_order_relaxed) + 1;
}

/* Record one fixed-size event with optional lock, wake and metadata details. */
void hfsTraceEmitDetail(HfsTracePhase phase,
                        HfsTraceOperation operation,
                        HfsTraceBufferKind bufferKind,
                        uint64_t requestId,
                        uint64_t parentId,
                        uint64_t inodeId,
                        uint64_t offset,
                        uint64_t bytes,
                        uint32_t units,
                        uint32_t queueDepth,
                        uint32_t reactorId,
                        uint32_t rangeBucketCount,
                        uint32_t poolClass,
                        uint32_t lockClass,
                        uint32_t lockId,
                        uint32_t wakeReason,
                        uint32_t metadataOperation,
                        uint64_t durationNs,
                        int32_t result)
{
    HfsTraceThread *thread;
    HfsTraceEvent *event;
    uint64_t sampleId;
    uint64_t sequence;

    if (!hfsTraceEnabledFast()) {
        return;
    }
    sampleId = parentId != 0 ? parentId : requestId;
    /* Drop uncorrelated events in sampled mode so the ring remains bounded. */
    if (g_traceSample > 1 &&
        (sampleId == 0 || (sampleId % g_traceSample) != 0)) {
        return;
    }
    thread = hfsTraceThreadGet();
    if (thread == NULL) {
        return;
    }
    sequence = thread->writeSequence++;
    if (sequence >= HFS_TRACE_RING_CAPACITY) {
        thread->dropped++;
    }
    event = &thread->events[sequence % HFS_TRACE_RING_CAPACITY];
    event->sequence = sequence;
    event->timestampNs = hfsTraceNowNs();
    event->durationNs = durationNs;
    event->requestId = requestId;
    event->parentId = parentId;
    event->inodeId = inodeId;
    event->offset = offset;
    event->bytes = bytes;
    event->units = units;
    event->queueDepth = queueDepth;
    event->reactorId = reactorId;
    event->rangeBucketCount = rangeBucketCount;
    event->poolClass = poolClass;
    event->lockClass = lockClass;
    event->lockId = lockId;
    event->wakeReason = wakeReason;
    event->metadataOperation = metadataOperation;
    event->result = result;
    event->phase = (uint8_t)phase;
    event->operation = (uint8_t)operation;
    event->bufferKind = (uint8_t)bufferKind;
    event->reserved = 0;
    event->profileChildCount = 0;
    event->profileValid = 0;
    memset(event->profileTicks, 0, sizeof(event->profileTicks));
}

/* Preserve the compact legacy call shape for ordinary data-path events. */
void hfsTraceEmit(HfsTracePhase phase,
                  HfsTraceOperation operation,
                  HfsTraceBufferKind bufferKind,
                  uint64_t requestId,
                  uint64_t parentId,
                  uint64_t inodeId,
                  uint64_t offset,
                  uint64_t bytes,
                  uint32_t units,
                  uint32_t queueDepth,
                  uint32_t reactorId,
                  uint32_t rangeBucketCount,
                  uint32_t poolClass,
                  uint64_t durationNs,
                  int32_t result)
{
    hfsTraceEmitDetail(phase, operation, bufferKind, requestId, parentId,
                       inodeId, offset, bytes, units, queueDepth, reactorId,
                       rangeBucketCount, poolClass, 0, 0, 0, 0,
                       durationNs, result);
}

/* Start timing one lock acquisition and publish its wait-begin event. */
void hfsTraceLockWaitBegin(HfsTraceLockToken *token,
                           HfsTraceLockClass lockClass,
                           uint32_t lockId,
                           HfsTraceOperation operation,
                           uint64_t parentId,
                           uint64_t inodeId,
                           uint64_t offset,
                           uint64_t bytes,
                           uint32_t rangeBucketCount)
{
    if (token == NULL) {
        return;
    }
    memset(token, 0, sizeof(*token));
    token->lockClass = (uint32_t)lockClass;
    token->lockId = lockId;
    token->operation = (uint8_t)operation;
    token->parentId = parentId;
    token->inodeId = inodeId;
    token->offset = offset;
    token->bytes = bytes;
    if (!hfsTraceEnabledFast()) {
        return;
    }
    token->waitStartNs = hfsTraceNowNs();
    hfsTraceEmitDetail(HFS_TRACE_LOCK_WAIT_BEGIN, operation,
                       HFS_TRACE_BUFFER_NONE, 0, parentId, inodeId,
                       offset, bytes, 0, 0, 0, rangeBucketCount, 0,
                       token->lockClass, lockId, 0, 0, 0, 0);
}

/* Finish lock acquisition timing and publish the wait duration. */
void hfsTraceLockAcquired(HfsTraceLockToken *token, int32_t result)
{
    uint64_t nowNs;
    uint64_t waitNs = 0;

    if (token == NULL || !hfsTraceEnabledFast()) {
        return;
    }
    nowNs = hfsTraceNowNs();
    if (token->waitStartNs != 0 && nowNs >= token->waitStartNs) {
        waitNs = nowNs - token->waitStartNs;
    }
    hfsTraceEmitDetail(HFS_TRACE_LOCK_ACQUIRED,
                       (HfsTraceOperation)token->operation,
                       HFS_TRACE_BUFFER_NONE, 0, token->parentId,
                       token->inodeId, token->offset, token->bytes, 0, 0, 0,
                       0, 0, token->lockClass, token->lockId, 0, 0,
                       waitNs, result);
    token->holdStartNs = nowNs;
    token->active = result == 0;
}

/* Publish lock hold duration when the protected section is released. */
void hfsTraceLockReleased(HfsTraceLockToken *token, int32_t result)
{
    uint64_t nowNs;
    uint64_t holdNs = 0;

    if (token == NULL || !hfsTraceEnabledFast() || !token->active) {
        return;
    }
    nowNs = hfsTraceNowNs();
    if (token->holdStartNs != 0 && nowNs >= token->holdStartNs) {
        holdNs = nowNs - token->holdStartNs;
    }
    hfsTraceEmitDetail(HFS_TRACE_LOCK_RELEASE,
                       (HfsTraceOperation)token->operation,
                       HFS_TRACE_BUFFER_NONE, 0, token->parentId,
                       token->inodeId, token->offset, token->bytes, 0, 0, 0,
                       0, 0, token->lockClass, token->lockId, 0, 0,
                       holdNs, result);
    token->active = false;
}

/* Start timing one condition-variable wait and publish its wait-begin event. */
void hfsTraceThreadWaitBegin(HfsTraceWaitToken *token,
                             HfsTraceOperation operation,
                             HfsTraceWakeReason wakeReason,
                             HfsTraceMetadataOperation metadataOperation,
                             uint64_t parentId,
                             uint64_t inodeId,
                             uint64_t offset,
                             uint64_t bytes,
                             uint32_t units,
                             uint32_t queueDepth,
                             uint32_t reactorId,
                             uint32_t poolClass)
{
    if (token == NULL) {
        return;
    }
    memset(token, 0, sizeof(*token));
    token->parentId = parentId;
    token->inodeId = inodeId;
    token->offset = offset;
    token->bytes = bytes;
    token->units = units;
    token->queueDepth = queueDepth;
    token->reactorId = reactorId;
    token->poolClass = poolClass;
    token->wakeReason = (uint32_t)wakeReason;
    token->metadataOperation = (uint32_t)metadataOperation;
    token->operation = (uint8_t)operation;
    if (!hfsTraceEnabledFast()) {
        return;
    }
    token->waitStartNs = hfsTraceNowNs();
    token->active = true;
    hfsTraceEmitDetail(HFS_TRACE_THREAD_WAIT_BEGIN, operation,
                       HFS_TRACE_BUFFER_NONE, 0, parentId, inodeId,
                       offset, bytes, units, queueDepth, reactorId, 0,
                       poolClass, 0, 0, token->wakeReason,
                       token->metadataOperation, 0, 0);
}

/* Finish one condition-variable wait and publish its measured duration. */
void hfsTraceThreadWaitEnd(HfsTraceWaitToken *token, int32_t result)
{
    uint64_t nowNs;
    uint64_t durationNs = 0;

    if (token == NULL || !token->active || !hfsTraceEnabledFast()) {
        return;
    }
    nowNs = hfsTraceNowNs();
    if (nowNs >= token->waitStartNs) {
        durationNs = nowNs - token->waitStartNs;
    }
    hfsTraceEmitDetail(HFS_TRACE_THREAD_WAIT_END,
                       (HfsTraceOperation)token->operation,
                       HFS_TRACE_BUFFER_NONE, 0, token->parentId,
                       token->inodeId, token->offset, token->bytes,
                       token->units, token->queueDepth, token->reactorId,
                       0, token->poolClass, 0, 0, token->wakeReason,
                       token->metadataOperation, durationNs, result);
    token->active = false;
}

/* Publish a condition-variable wake event without allocating or taking a lock. */
void hfsTraceThreadWake(HfsTraceOperation operation,
                        HfsTraceWakeReason wakeReason,
                        HfsTraceMetadataOperation metadataOperation,
                        uint64_t parentId,
                        uint64_t inodeId,
                        uint64_t offset,
                        uint64_t bytes,
                        uint32_t units,
                        uint32_t queueDepth,
                        uint32_t reactorId,
                        uint32_t poolClass,
                        int32_t result)
{
    if (!hfsTraceEnabledFast()) {
        return;
    }
    hfsTraceEmitDetail(HFS_TRACE_THREAD_WAKE, operation,
                       HFS_TRACE_BUFFER_NONE, 0, parentId, inodeId,
                       offset, bytes, units, queueDepth, reactorId, 0,
                       poolClass, 0, 0, (uint32_t)wakeReason,
                       (uint32_t)metadataOperation, 0, result);
}

/* Append one parent profile event without allocating a second trace record. */
void hfsTraceEmitProfile(uint64_t parentId,
                         uint64_t bytes,
                         uint32_t flags,
                         uint32_t childCount,
                         const uint64_t ticks[12],
                         bool monotonic,
                         int32_t result)
{
    HfsTraceThread *thread;
    HfsTraceEvent *event;
    uint64_t sampleId;
    uint64_t sequence;
    unsigned int i;

    if (!hfsTraceEnabledFast() || ticks == NULL) {
        return;
    }
    sampleId = parentId;
    /* Parent profiles use the same sampling rule as their child events. */
    if (g_traceSample > 1 &&
        (sampleId == 0 || (sampleId % g_traceSample) != 0)) {
        return;
    }
    thread = hfsTraceThreadGet();
    if (thread == NULL) {
        return;
    }
    sequence = thread->writeSequence++;
    if (sequence >= HFS_TRACE_RING_CAPACITY) {
        thread->dropped++;
    }
    event = &thread->events[sequence % HFS_TRACE_RING_CAPACITY];
    memset(event, 0, sizeof(*event));
    event->sequence = sequence;
    event->timestampNs = hfsTraceNowNs();
    event->requestId = parentId;
    event->parentId = parentId;
    event->bytes = bytes;
    event->profileChildCount = childCount;
    event->profileValid = monotonic ? 1U : 0U;
    event->phase = HFS_TRACE_PARENT_PROFILE;
    event->operation = (flags & 1U) != 0 ? HFS_TRACE_OPERATION_WRITE :
                       HFS_TRACE_OPERATION_READ;
    event->result = result;
    for (i = 0; i < 12U; i++) {
        event->profileTicks[i] = ticks[i];
    }
}

/* Write all per-thread rings as a tab-separated snapshot at shutdown. */
static void hfsTraceWriteSnapshot(void)
{
    FILE *file;
    HfsTraceThread *thread;

    if (g_tracePath == NULL) {
        return;
    }
    file = fopen(g_tracePath, "w");
    if (file == NULL) {
        return;
    }
    fprintf(file, "sequence\ttid\ttimestamp_ns\tphase\toperation\tbuffer_kind\trequest_id\tparent_id\tinode_id\toffset\tbytes\tunits\tqueue_depth\treactor\trange_buckets\tpool_class\tlock_class\tlock_id\twake_reason\tmetadata_op\tduration_ns\tresult\tprofile_child_count\tprofile_valid\tprofile_t0\tprofile_t1\tprofile_t2\tprofile_t3\tprofile_t4\tprofile_t5\tprofile_t6\tprofile_t7\tprofile_t8\tprofile_t9\tprofile_t10\tprofile_t11\n");
    pthread_mutex_lock(&g_traceLock);
    for (thread = g_traceThreads; thread != NULL; thread = thread->next) {
        uint64_t count = thread->writeSequence;
        uint64_t first = count > HFS_TRACE_RING_CAPACITY ?
                         count - HFS_TRACE_RING_CAPACITY : 0;
        uint64_t sequence;

        for (sequence = first; sequence < count; sequence++) {
            HfsTraceEvent *event = &thread->events[sequence % HFS_TRACE_RING_CAPACITY];
            if (event->sequence != sequence) {
                continue;
            }
            fprintf(file, "%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%s\t%s\t%s\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%u\t%u\t%u\t%u\t%u\t%s\t%u\t%s\t%s\t%" PRIu64 "\t%d\t%u\t%u\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\n",
                    event->sequence, thread->threadId, event->timestampNs,
                    hfsTracePhaseName((HfsTracePhase)event->phase),
                    hfsTraceOperationName((HfsTraceOperation)event->operation),
                    hfsTraceBufferName((HfsTraceBufferKind)event->bufferKind),
                    event->requestId, event->parentId, event->inodeId,
                    event->offset, event->bytes, event->units,
                    event->queueDepth, event->reactorId,
                    event->rangeBucketCount, event->poolClass,
                    hfsTraceLockName(event->lockClass), event->lockId,
                    hfsTraceWakeName(event->wakeReason),
                    hfsTraceMetadataName(event->metadataOperation),
                    event->durationNs, event->result, event->profileChildCount,
                    event->profileValid, event->profileTicks[0],
                    event->profileTicks[1], event->profileTicks[2],
                    event->profileTicks[3], event->profileTicks[4],
                    event->profileTicks[5], event->profileTicks[6],
                    event->profileTicks[7], event->profileTicks[8],
                    event->profileTicks[9], event->profileTicks[10],
                    event->profileTicks[11]);
        }
        fprintf(file, "# thread=%" PRIu64 " dropped=%" PRIu64 "\n",
                thread->threadId, thread->dropped);
    }
    pthread_mutex_unlock(&g_traceLock);
    fclose(file);
}

/* Initialize optional tracing without failing filesystem initialization. */
int hfsTraceInit(void)
{
    const char *path;
    const char *enabled;

    if (g_traceInitialized) {
        return 0;
    }
    path = getenv("HFS_TRACE_PATH");
    enabled = getenv("HFS_TRACE");
    g_traceSample = hfsTraceSampleEnv(getenv("HFS_TRACE_SAMPLE"));
    if (path != NULL && path[0] != '\0') {
        g_tracePath = strdup(path);
    } else if (hfsTraceBool(enabled)) {
        char fallback[128];
        snprintf(fallback, sizeof(fallback), "/var/tmp/hfs-trace-%ld.tsv",
                 (long)getpid());
        g_tracePath = strdup(fallback);
    }
    atomic_store_explicit(&g_traceRequestId, 0, memory_order_relaxed);
    atomic_store_explicit(&hfsTraceEnabledState, g_tracePath != NULL,
                          memory_order_release);
    g_traceInitialized = true;
    return 0;
}

/* Disable tracing and persist the final bounded snapshots. */
void hfsTraceFini(void)
{
    if (!g_traceInitialized) {
        return;
    }
    atomic_store_explicit(&hfsTraceEnabledState, false, memory_order_release);
    hfsTraceWriteSnapshot();
    g_traceInitialized = false;
}
