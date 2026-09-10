#ifndef HFS_TRACE_H
#define HFS_TRACE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* Trace phases identify the observable stages of a request. */
typedef enum HfsTracePhase {
    HFS_TRACE_IO_ENTER = 1,
    HFS_TRACE_IO_EXIT,
    HFS_TRACE_BUFFER_CLASSIFY,
    HFS_TRACE_LEASE_WAIT_BEGIN,
    HFS_TRACE_LEASE_WAIT_END,
    HFS_TRACE_LEASE_ACQUIRE,
    HFS_TRACE_LEASE_RELEASE,
    HFS_TRACE_COPY_IN_BEGIN,
    HFS_TRACE_COPY_IN_END,
    HFS_TRACE_COPY_OUT_BEGIN,
    HFS_TRACE_COPY_OUT_END,
    HFS_TRACE_RANGE_BEGIN,
    HFS_TRACE_RANGE_ACQUIRED,
    HFS_TRACE_RANGE_RELEASE,
    HFS_TRACE_RUNTIME_ENQUEUE,
    HFS_TRACE_RUNTIME_DEQUEUE,
    HFS_TRACE_SPDK_SUBMIT,
    HFS_TRACE_SPDK_COMPLETE,
    HFS_TRACE_METADATA_PUBLISH,
    HFS_TRACE_PARENT_START,
    HFS_TRACE_CHILD_START,
    HFS_TRACE_CHILD_DONE,
    HFS_TRACE_PARENT_DONE
} HfsTracePhase;

/* Buffer kinds distinguish caller buffers from internal bounce buffers. */
typedef enum HfsTraceBufferKind {
    HFS_TRACE_BUFFER_NONE = 0,
    HFS_TRACE_BUFFER_CALLER,
    /* Reserved for traces written by the retired registered-DMA API. */
    HFS_TRACE_BUFFER_REGISTERED,
    HFS_TRACE_BUFFER_DMA_LEASE,
    HFS_TRACE_BUFFER_VECTOR,
    HFS_TRACE_BUFFER_RMW
} HfsTraceBufferKind;

/* Operations use a compact stable vocabulary for trace consumers. */
typedef enum HfsTraceOperation {
    HFS_TRACE_OPERATION_NONE = 0,
    HFS_TRACE_OPERATION_READ,
    HFS_TRACE_OPERATION_WRITE,
    HFS_TRACE_OPERATION_UNMAP,
    HFS_TRACE_OPERATION_WRITE_ZEROES,
    HFS_TRACE_OPERATION_VECTOR_READ,
    HFS_TRACE_OPERATION_VECTOR_WRITE
} HfsTraceOperation;

/* A fixed-size event keeps the hot path allocation-free. */
typedef struct HfsTraceEvent {
    uint64_t sequence;
    uint64_t timestampNs;
    uint64_t durationNs;
    uint64_t requestId;
    uint64_t parentId;
    uint64_t inodeId;
    uint64_t offset;
    uint64_t bytes;
    uint32_t units;
    uint32_t queueDepth;
    uint32_t reactorId;
    uint32_t rangeBucketCount;
    uint32_t poolClass;
    int32_t result;
    uint8_t phase;
    uint8_t operation;
    uint8_t bufferKind;
    uint8_t reserved;
} HfsTraceEvent;

extern _Atomic bool hfsTraceEnabledState;

/* Initialize and finalize the process-local trace recorder. */
int hfsTraceInit(void);
void hfsTraceFini(void);

/* Return a parent request id, or zero while tracing is disabled. */
uint64_t hfsTraceNextRequestId(void);

/* Return a monotonic timestamp in nanoseconds. */
uint64_t hfsTraceNowNs(void);

/* Record one event when tracing is enabled. */
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
                  int32_t result);

/* Fast branch used by hot paths before constructing event arguments. */
static inline bool hfsTraceEnabledFast(void)
{
    return atomic_load_explicit(&hfsTraceEnabledState, memory_order_relaxed);
}

#endif
