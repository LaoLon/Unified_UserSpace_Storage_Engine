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
    HFS_TRACE_BACKEND_TB0,
    HFS_TRACE_BACKEND_TB1,
    HFS_TRACE_BACKEND_TB2,
    HFS_TRACE_METADATA_PUBLISH,
    HFS_TRACE_PARENT_START,
    HFS_TRACE_CHILD_START,
    HFS_TRACE_CHILD_DONE,
    HFS_TRACE_PARENT_DONE,
    HFS_TRACE_LOCK_WAIT_BEGIN,
    HFS_TRACE_LOCK_ACQUIRED,
    HFS_TRACE_LOCK_RELEASE,
    HFS_TRACE_METADATA_BEGIN,
    HFS_TRACE_METADATA_END,
    HFS_TRACE_FLUSH_BEGIN,
    HFS_TRACE_FLUSH_END,
    HFS_TRACE_COMPLETION_WAIT_BEGIN,
    HFS_TRACE_COMPLETION_WAIT_END,
    HFS_TRACE_COMPLETION_WAKE,
    HFS_TRACE_THREAD_WAIT_BEGIN,
    HFS_TRACE_THREAD_WAIT_END,
    HFS_TRACE_THREAD_WAKE,
    HFS_TRACE_TICKET_ADMIT,
    HFS_TRACE_TICKET_DEP_WAIT,
    HFS_TRACE_TICKET_READY,
    HFS_TRACE_TICKET_WAKE,
    HFS_TRACE_PARENT_PROFILE
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
    HFS_TRACE_OPERATION_VECTOR_WRITE,
    HFS_TRACE_OPERATION_METADATA,
    HFS_TRACE_OPERATION_FLUSH,
    HFS_TRACE_OPERATION_COMPLETION,
    HFS_TRACE_OPERATION_LOCK,
    HFS_TRACE_OPERATION_THREAD
} HfsTraceOperation;

/* Lock classes identify the synchronization object behind a lock event. */
typedef enum HfsTraceLockClass {
    HFS_TRACE_LOCK_NONE = 0,
    HFS_TRACE_LOCK_SYNC,
    HFS_TRACE_LOCK_RANGE_STRIPE,
    HFS_TRACE_LOCK_METADATA,
    HFS_TRACE_LOCK_RUNTIME,
    HFS_TRACE_LOCK_FD,
    HFS_TRACE_LOCK_APPEND,
    HFS_TRACE_LOCK_OTHER
} HfsTraceLockClass;

/* Wake reasons distinguish futex completion from durability coordination. */
typedef enum HfsTraceWakeReason {
    HFS_TRACE_WAKE_NONE = 0,
    HFS_TRACE_WAKE_COMPLETION,
    HFS_TRACE_WAKE_DURABILITY,
    HFS_TRACE_WAKE_RUNTIME,
    HFS_TRACE_WAKE_DMA_LEASE
} HfsTraceWakeReason;

/* Metadata sub-operations identify the request that caused a metadata event. */
typedef enum HfsTraceMetadataOperation {
    HFS_TRACE_META_OP_NONE = 0,
    HFS_TRACE_META_OP_OPEN,
    HFS_TRACE_META_OP_READ,
    HFS_TRACE_META_OP_WRITE,
    HFS_TRACE_META_OP_SYNC,
    HFS_TRACE_META_OP_RESIZE,
    HFS_TRACE_META_OP_CLOSE,
    HFS_TRACE_META_OP_TRANSACTION,
    HFS_TRACE_META_OP_FLUSH,
    HFS_TRACE_META_OP_PUBLISH
} HfsTraceMetadataOperation;

/* State carried between lock acquisition and release on one calling thread. */
typedef struct HfsTraceLockToken {
    uintptr_t lockKey;
    uint64_t waitStartNs;
    uint64_t holdStartNs;
    uint64_t parentId;
    uint64_t inodeId;
    uint64_t offset;
    uint64_t bytes;
    uint32_t lockClass;
    uint32_t lockId;
    uint8_t operation;
    bool active;
} HfsTraceLockToken;

/* State carried between a condition-variable wait and its wake result. */
typedef struct HfsTraceWaitToken {
    uint64_t waitStartNs;
    uint64_t parentId;
    uint64_t inodeId;
    uint64_t offset;
    uint64_t bytes;
    uint32_t units;
    uint32_t queueDepth;
    uint32_t reactorId;
    uint32_t poolClass;
    uint32_t wakeReason;
    uint32_t metadataOperation;
    uint8_t operation;
    bool active;
} HfsTraceWaitToken;

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
    uint32_t cpu;
    uint32_t rangeBucketCount;
    uint32_t poolClass;
    uint32_t lockClass;
    uint32_t lockId;
    uint32_t wakeReason;
    uint32_t metadataOperation;
    int32_t result;
    uint8_t phase;
    uint8_t operation;
    uint8_t bufferKind;
    uint8_t reserved;
    uint32_t profileChildCount;
    uint32_t profileValid;
    uint64_t profileTicks[12];
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

/* Record an event with lock, wake and metadata detail fields. */
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
                        int32_t result);

/* Emit a lock wait/acquire/release sequence with duration accounting. */
void hfsTraceLockWaitBegin(HfsTraceLockToken *token,
                           HfsTraceLockClass lockClass,
                           uint32_t lockId,
                           HfsTraceOperation operation,
                           uint64_t parentId,
                           uint64_t inodeId,
                           uint64_t offset,
                           uint64_t bytes,
                           uint32_t rangeBucketCount);
void hfsTraceLockAcquired(HfsTraceLockToken *token, int32_t result);
void hfsTraceLockReleased(HfsTraceLockToken *token, int32_t result);

/* Emit a condition-variable wait begin/end pair with one duration token. */
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
                             uint32_t poolClass);
void hfsTraceThreadWaitEnd(HfsTraceWaitToken *token, int32_t result);
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
                        int32_t result);

/* Record one parent T0..T11 profile row for offline stage attribution. */
void hfsTraceEmitProfile(uint64_t parentId,
                         uint64_t bytes,
                         uint32_t flags,
                         uint32_t childCount,
                         const uint64_t ticks[12],
                         bool monotonic,
                         int32_t result);

/* Fast branch used by hot paths before constructing event arguments. */
static inline bool hfsTraceEnabledFast(void)
{
    return atomic_load_explicit(&hfsTraceEnabledState, memory_order_relaxed);
}

#endif
