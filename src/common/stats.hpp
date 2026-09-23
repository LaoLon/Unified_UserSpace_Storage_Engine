#pragma once

#include <atomic>
#include <cstdint>

namespace hfs::stats {

/* Always-cheap counters; enabled state is switched in the control phase. */
struct Counters {
    std::atomic<std::uint64_t> submit{0};
    std::atomic<std::uint64_t> complete{0};
    std::atomic<std::uint64_t> busyReject{0};
    std::atomic<std::uint64_t> traceDropped{0};
    /* Read-path counters used by the RocksDB root-cause benchmark. */
    std::atomic<std::uint64_t> frontendReadOps{0};
    std::atomic<std::uint64_t> frontendReadBytes{0};
    std::atomic<std::uint64_t> frontendRequest4k{0};
    std::atomic<std::uint64_t> frontendRequest8k{0};
    std::atomic<std::uint64_t> frontendRequestOther{0};
    std::atomic<std::uint64_t> frontendCacheHit{0};
    std::atomic<std::uint64_t> frontendCacheMiss{0};
    std::atomic<std::uint64_t> frontendSubmitOps{0};
    std::atomic<std::uint64_t> directWorkerReadOps{0};
    std::atomic<std::uint64_t> directWorkerReadBytes{0};
    std::atomic<std::uint64_t> directWorkerCompletionOps{0};
    std::atomic<std::uint64_t> plannerReadRequests{0};
    std::atomic<std::uint64_t> plannerReadRequestBytes{0};
    std::atomic<std::uint64_t> plannerReadChildBytes{0};
    std::atomic<std::uint64_t> plannerReadSpanBytes{0};
    std::atomic<std::uint64_t> plannerReadCoalescedChildren{0};
    std::atomic<std::uint64_t> plannerReadDirectChildren{0};
    std::atomic<std::uint64_t> plannerReadHeadRmwChildren{0};
    std::atomic<std::uint64_t> plannerReadTailRmwChildren{0};
    std::atomic<std::uint64_t> plannerReadZeroChildren{0};
    std::atomic<std::uint64_t> plannerSpan4k{0};
    std::atomic<std::uint64_t> plannerSpan8k{0};
    std::atomic<std::uint64_t> plannerSpanOther{0};
    std::atomic<std::uint64_t> dataReactorReadSubmit{0};
    std::atomic<std::uint64_t> dataReactorReadComplete{0};
    std::atomic<std::uint64_t> dataReactorQueueDepthSum{0};
    std::atomic<std::uint64_t> dataReactorQueueDepthMax{0};
    std::atomic<std::uint64_t> bdevDataReadOps{0};
    std::atomic<std::uint64_t> bdevDataReadBytes{0};
    std::atomic<std::uint64_t> bdevDataBlockBytes{0};
    std::atomic<std::uint64_t> bdevDataWriteOps{0};
    std::atomic<std::uint64_t> bdevDataWriteBytes{0};
    std::atomic<std::uint64_t> completionCount{0};
    std::atomic<std::uint64_t> completionWaitCount{0};
    std::atomic<std::uint64_t> completionWaitNs{0};
    std::atomic<std::uint64_t> frontendQueueWaitNs{0};
    std::atomic<std::uint64_t> frontendIoNs{0};
    std::atomic<std::uint64_t> readBounceChildren{0};
    std::atomic<std::uint64_t> readBounceBytes{0};
    std::atomic<std::uint64_t> readDirectUserChildren{0};
    std::atomic<std::uint64_t> readDirectUserBytes{0};
    std::atomic<std::uint64_t> readCopyOutOps{0};
    std::atomic<std::uint64_t> readCopyOutBytes{0};
    std::atomic<std::uint64_t> readCopyOutNs{0};
};

Counters &counters() noexcept;
bool enabled() noexcept;
void setEnabled(bool value) noexcept;

} // namespace hfs::stats
