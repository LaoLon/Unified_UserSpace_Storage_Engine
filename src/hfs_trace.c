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

#define HFS_TRACE_RING_CAPACITY 4096U

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

/* Record one fixed-size event into the calling thread's ring. */
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
    HfsTraceThread *thread;
    HfsTraceEvent *event;
    uint64_t sampleId;
    uint64_t sequence;

    if (!hfsTraceEnabledFast()) {
        return;
    }
    sampleId = parentId != 0 ? parentId : requestId;
    if (g_traceSample > 1 && sampleId != 0 && (sampleId % g_traceSample) != 0) {
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
    event->result = result;
    event->phase = (uint8_t)phase;
    event->operation = (uint8_t)operation;
    event->bufferKind = (uint8_t)bufferKind;
    event->reserved = 0;
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
    fprintf(file, "sequence\ttid\ttimestamp_ns\tphase\toperation\tbuffer_kind\trequest_id\tparent_id\tinode_id\toffset\tbytes\tunits\tqueue_depth\treactor\trange_buckets\tpool_class\tduration_ns\tresult\n");
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
            fprintf(file, "%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%s\t%s\t%s\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%u\t%u\t%u\t%u\t%u\t%" PRIu64 "\t%d\n",
                    event->sequence, thread->threadId, event->timestampNs,
                    hfsTracePhaseName((HfsTracePhase)event->phase),
                    hfsTraceOperationName((HfsTraceOperation)event->operation),
                    hfsTraceBufferName((HfsTraceBufferKind)event->bufferKind),
                    event->requestId, event->parentId, event->inodeId,
                    event->offset, event->bytes, event->units,
                    event->queueDepth, event->reactorId,
                    event->rangeBucketCount, event->poolClass,
                    event->durationNs, event->result);
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
