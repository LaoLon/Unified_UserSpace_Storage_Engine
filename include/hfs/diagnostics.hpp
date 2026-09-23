#pragma once

#include <cstdint>

#include "hfs/export.hpp"

namespace hfs::diagnostics {

/* A stable, non-atomic snapshot for one benchmark interval. */
struct HFS_API Snapshot {
    std::uint64_t frontendReadOps = 0;
    std::uint64_t frontendReadBytes = 0;
    std::uint64_t frontendRequest4k = 0;
    std::uint64_t frontendRequest8k = 0;
    std::uint64_t frontendRequestOther = 0;
    std::uint64_t frontendCacheHit = 0;
    std::uint64_t frontendCacheMiss = 0;
    std::uint64_t frontendSubmitOps = 0;
    std::uint64_t directWorkerReadOps = 0;
    std::uint64_t directWorkerReadBytes = 0;
    std::uint64_t directWorkerCompletionOps = 0;
    std::uint64_t plannerReadRequests = 0;
    std::uint64_t plannerReadRequestBytes = 0;
    std::uint64_t plannerReadChildBytes = 0;
    std::uint64_t plannerReadSpanBytes = 0;
    std::uint64_t plannerReadCoalescedChildren = 0;
    std::uint64_t plannerReadDirectChildren = 0;
    std::uint64_t plannerReadHeadRmwChildren = 0;
    std::uint64_t plannerReadTailRmwChildren = 0;
    std::uint64_t plannerReadZeroChildren = 0;
    std::uint64_t plannerSpan4k = 0;
    std::uint64_t plannerSpan8k = 0;
    std::uint64_t plannerSpanOther = 0;
    std::uint64_t dataReactorReadSubmit = 0;
    std::uint64_t dataReactorReadComplete = 0;
    std::uint64_t dataReactorQueueDepthSum = 0;
    std::uint64_t dataReactorQueueDepthMax = 0;
    std::uint64_t bdevDataReadOps = 0;
    std::uint64_t bdevDataReadBytes = 0;
    std::uint64_t bdevDataBlockBytes = 0;
    std::uint64_t bdevDataWriteOps = 0;
    std::uint64_t bdevDataWriteBytes = 0;
    std::uint64_t completionCount = 0;
    std::uint64_t completionWaitCount = 0;
    std::uint64_t completionWaitNs = 0;
    std::uint64_t frontendQueueWaitNs = 0;
    std::uint64_t frontendIoNs = 0;
    std::uint64_t readBounceChildren = 0;
    std::uint64_t readBounceBytes = 0;
    std::uint64_t readDirectUserChildren = 0;
    std::uint64_t readDirectUserBytes = 0;
    std::uint64_t readCopyOutOps = 0;
    std::uint64_t readCopyOutBytes = 0;
    std::uint64_t readCopyOutNs = 0;
};

/* Reads the process-wide counters without stopping the I/O path. */
HFS_API Snapshot snapshot() noexcept;

/* Returns whether expensive latency sampling is enabled for this run. */
HFS_API bool profilingEnabled() noexcept;

/* Clears only the lightweight read-path counters used by this benchmark. */
HFS_API void reset() noexcept;

/* Records adapter-side events without exposing internal counter storage. */
HFS_API void recordFrontendRead(bool cacheHit, std::uint64_t requestBytes) noexcept;
HFS_API void recordDirectWorkerRead(std::uint64_t requestBytes) noexcept;
HFS_API void recordDirectWorkerCompletion() noexcept;
HFS_API void recordDataReadPlan(std::uint64_t requestBytes, std::uint64_t childBytes,
                                std::uint64_t spanBytes, std::uint64_t directChildren,
                                std::uint64_t coalescedChildren,
                                std::uint64_t headRmwChildren,
                                std::uint64_t tailRmwChildren,
                                std::uint64_t zeroChildren, std::uint64_t span4kChildren,
                                std::uint64_t span8kChildren,
                                std::uint64_t spanOtherChildren) noexcept;
HFS_API void recordFrontendSubmit() noexcept;
HFS_API void recordCompletionWait() noexcept;
HFS_API void recordCompletionWaitNs(std::uint64_t nanoseconds) noexcept;
HFS_API void recordCompletion() noexcept;
HFS_API void recordFrontendQueueWaitNs(std::uint64_t nanoseconds) noexcept;
HFS_API void recordFrontendIoNs(std::uint64_t nanoseconds) noexcept;
HFS_API void recordReadBufferPlan(std::uint64_t bounceChildren,
                                  std::uint64_t bounceBytes,
                                  std::uint64_t directUserChildren,
                                  std::uint64_t directUserBytes) noexcept;
HFS_API void recordReadCopyOut(std::uint64_t bytes, std::uint64_t nanoseconds) noexcept;

} // namespace hfs::diagnostics
