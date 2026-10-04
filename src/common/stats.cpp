#include "common/stats.hpp"
#include "uuse/diagnostics.hpp"

namespace uuse::stats {

namespace {

Counters g_counters;
bool g_enabled = false;

} // namespace

Counters &counters() noexcept { return g_counters; }

bool enabled() noexcept { return g_enabled; }

void setEnabled(bool value) noexcept { g_enabled = value; }

} // namespace uuse::stats

namespace uuse::diagnostics {

Snapshot snapshot() noexcept {
    const auto &c = stats::counters();
    Snapshot value;
    value.frontendReadOps = c.frontendReadOps.load(std::memory_order_relaxed);
    value.frontendReadBytes = c.frontendReadBytes.load(std::memory_order_relaxed);
    value.frontendRequest4k = c.frontendRequest4k.load(std::memory_order_relaxed);
    value.frontendRequest8k = c.frontendRequest8k.load(std::memory_order_relaxed);
    value.frontendRequestOther = c.frontendRequestOther.load(std::memory_order_relaxed);
    value.frontendCacheHit = c.frontendCacheHit.load(std::memory_order_relaxed);
    value.frontendCacheMiss = c.frontendCacheMiss.load(std::memory_order_relaxed);
    value.frontendSubmitOps = c.frontendSubmitOps.load(std::memory_order_relaxed);
    value.directWorkerReadOps = c.directWorkerReadOps.load(std::memory_order_relaxed);
    value.directWorkerReadBytes = c.directWorkerReadBytes.load(std::memory_order_relaxed);
    value.directWorkerCompletionOps =
        c.directWorkerCompletionOps.load(std::memory_order_relaxed);
    value.plannerReadRequests = c.plannerReadRequests.load(std::memory_order_relaxed);
    value.plannerReadRequestBytes =
        c.plannerReadRequestBytes.load(std::memory_order_relaxed);
    value.plannerReadChildBytes = c.plannerReadChildBytes.load(std::memory_order_relaxed);
    value.plannerReadSpanBytes = c.plannerReadSpanBytes.load(std::memory_order_relaxed);
    value.plannerReadCoalescedChildren =
        c.plannerReadCoalescedChildren.load(std::memory_order_relaxed);
    value.plannerReadDirectChildren =
        c.plannerReadDirectChildren.load(std::memory_order_relaxed);
    value.plannerReadHeadRmwChildren =
        c.plannerReadHeadRmwChildren.load(std::memory_order_relaxed);
    value.plannerReadTailRmwChildren =
        c.plannerReadTailRmwChildren.load(std::memory_order_relaxed);
    value.plannerReadZeroChildren =
        c.plannerReadZeroChildren.load(std::memory_order_relaxed);
    value.plannerSpan4k = c.plannerSpan4k.load(std::memory_order_relaxed);
    value.plannerSpan8k = c.plannerSpan8k.load(std::memory_order_relaxed);
    value.plannerSpanOther = c.plannerSpanOther.load(std::memory_order_relaxed);
    value.dataReactorReadSubmit = c.dataReactorReadSubmit.load(std::memory_order_relaxed);
    value.dataReactorReadComplete = c.dataReactorReadComplete.load(std::memory_order_relaxed);
    value.dataReactorQueueDepthSum = c.dataReactorQueueDepthSum.load(std::memory_order_relaxed);
    value.dataReactorQueueDepthMax = c.dataReactorQueueDepthMax.load(std::memory_order_relaxed);
    value.bdevDataReadOps = c.bdevDataReadOps.load(std::memory_order_relaxed);
    value.bdevDataReadBytes = c.bdevDataReadBytes.load(std::memory_order_relaxed);
    value.bdevDataBlockBytes = c.bdevDataBlockBytes.load(std::memory_order_relaxed);
    value.bdevDataWriteOps = c.bdevDataWriteOps.load(std::memory_order_relaxed);
    value.bdevDataWriteBytes = c.bdevDataWriteBytes.load(std::memory_order_relaxed);
    value.completionCount = c.completionCount.load(std::memory_order_relaxed);
    value.completionWaitCount = c.completionWaitCount.load(std::memory_order_relaxed);
    value.completionWaitNs = c.completionWaitNs.load(std::memory_order_relaxed);
    value.frontendQueueWaitNs = c.frontendQueueWaitNs.load(std::memory_order_relaxed);
    value.frontendIoNs = c.frontendIoNs.load(std::memory_order_relaxed);
    value.readBounceChildren = c.readBounceChildren.load(std::memory_order_relaxed);
    value.readBounceBytes = c.readBounceBytes.load(std::memory_order_relaxed);
    value.readDirectUserChildren = c.readDirectUserChildren.load(std::memory_order_relaxed);
    value.readDirectUserBytes = c.readDirectUserBytes.load(std::memory_order_relaxed);
    value.readCopyOutOps = c.readCopyOutOps.load(std::memory_order_relaxed);
    value.readCopyOutBytes = c.readCopyOutBytes.load(std::memory_order_relaxed);
    value.readCopyOutNs = c.readCopyOutNs.load(std::memory_order_relaxed);
    value.snapshotHits = c.snapshotHits.load(std::memory_order_relaxed);
    value.snapshotBuilds = c.snapshotBuilds.load(std::memory_order_relaxed);
    return value;
}

bool profilingEnabled() noexcept { return stats::enabled(); }

void reset() noexcept {
    auto &c = stats::counters();
    c.frontendReadOps.store(0, std::memory_order_relaxed);
    c.frontendReadBytes.store(0, std::memory_order_relaxed);
    c.frontendRequest4k.store(0, std::memory_order_relaxed);
    c.frontendRequest8k.store(0, std::memory_order_relaxed);
    c.frontendRequestOther.store(0, std::memory_order_relaxed);
    c.frontendCacheHit.store(0, std::memory_order_relaxed);
    c.frontendCacheMiss.store(0, std::memory_order_relaxed);
    c.frontendSubmitOps.store(0, std::memory_order_relaxed);
    c.directWorkerReadOps.store(0, std::memory_order_relaxed);
    c.directWorkerReadBytes.store(0, std::memory_order_relaxed);
    c.directWorkerCompletionOps.store(0, std::memory_order_relaxed);
    c.plannerReadRequests.store(0, std::memory_order_relaxed);
    c.plannerReadRequestBytes.store(0, std::memory_order_relaxed);
    c.plannerReadChildBytes.store(0, std::memory_order_relaxed);
    c.plannerReadSpanBytes.store(0, std::memory_order_relaxed);
    c.plannerReadCoalescedChildren.store(0, std::memory_order_relaxed);
    c.plannerReadDirectChildren.store(0, std::memory_order_relaxed);
    c.plannerReadHeadRmwChildren.store(0, std::memory_order_relaxed);
    c.plannerReadTailRmwChildren.store(0, std::memory_order_relaxed);
    c.plannerReadZeroChildren.store(0, std::memory_order_relaxed);
    c.plannerSpan4k.store(0, std::memory_order_relaxed);
    c.plannerSpan8k.store(0, std::memory_order_relaxed);
    c.plannerSpanOther.store(0, std::memory_order_relaxed);
    c.dataReactorReadSubmit.store(0, std::memory_order_relaxed);
    c.dataReactorReadComplete.store(0, std::memory_order_relaxed);
    c.dataReactorQueueDepthSum.store(0, std::memory_order_relaxed);
    c.dataReactorQueueDepthMax.store(0, std::memory_order_relaxed);
    c.bdevDataReadOps.store(0, std::memory_order_relaxed);
    c.bdevDataReadBytes.store(0, std::memory_order_relaxed);
    c.bdevDataBlockBytes.store(0, std::memory_order_relaxed);
    c.bdevDataWriteOps.store(0, std::memory_order_relaxed);
    c.bdevDataWriteBytes.store(0, std::memory_order_relaxed);
    c.completionCount.store(0, std::memory_order_relaxed);
    c.completionWaitCount.store(0, std::memory_order_relaxed);
    c.completionWaitNs.store(0, std::memory_order_relaxed);
    c.frontendQueueWaitNs.store(0, std::memory_order_relaxed);
    c.frontendIoNs.store(0, std::memory_order_relaxed);
    c.readBounceChildren.store(0, std::memory_order_relaxed);
    c.readBounceBytes.store(0, std::memory_order_relaxed);
    c.readDirectUserChildren.store(0, std::memory_order_relaxed);
    c.readDirectUserBytes.store(0, std::memory_order_relaxed);
    c.readCopyOutOps.store(0, std::memory_order_relaxed);
    c.readCopyOutBytes.store(0, std::memory_order_relaxed);
    c.readCopyOutNs.store(0, std::memory_order_relaxed);
    c.snapshotHits.store(0, std::memory_order_relaxed);
    c.snapshotBuilds.store(0, std::memory_order_relaxed);
}

void recordFrontendRead(bool cacheHit, std::uint64_t requestBytes) noexcept {
    if (!stats::enabled()) return;
    auto &c = stats::counters();
    c.frontendReadOps.fetch_add(1, std::memory_order_relaxed);
    c.frontendReadBytes.fetch_add(requestBytes, std::memory_order_relaxed);
    if (requestBytes == 4096) {
        c.frontendRequest4k.fetch_add(1, std::memory_order_relaxed);
    } else if (requestBytes == 8192) {
        c.frontendRequest8k.fetch_add(1, std::memory_order_relaxed);
    } else {
        c.frontendRequestOther.fetch_add(1, std::memory_order_relaxed);
    }
    (cacheHit ? c.frontendCacheHit : c.frontendCacheMiss).fetch_add(
        1, std::memory_order_relaxed);
}

void recordDirectWorkerRead(std::uint64_t requestBytes) noexcept {
    if (!stats::enabled()) return;
    auto &c = stats::counters();
    c.directWorkerReadOps.fetch_add(1, std::memory_order_relaxed);
    c.directWorkerReadBytes.fetch_add(requestBytes, std::memory_order_relaxed);
}

void recordDirectWorkerCompletion() noexcept {
    if (!stats::enabled()) return;
    stats::counters().directWorkerCompletionOps.fetch_add(1, std::memory_order_relaxed);
}

void recordDataReadPlan(std::uint64_t requestBytes, std::uint64_t childBytes,
                        std::uint64_t spanBytes, std::uint64_t directChildren,
                        std::uint64_t coalescedChildren,
                        std::uint64_t headRmwChildren, std::uint64_t tailRmwChildren,
                        std::uint64_t zeroChildren, std::uint64_t span4kChildren,
                        std::uint64_t span8kChildren,
                        std::uint64_t spanOtherChildren) noexcept {
    if (!stats::enabled()) return;
    auto &c = stats::counters();
    c.plannerReadRequests.fetch_add(1, std::memory_order_relaxed);
    c.plannerReadRequestBytes.fetch_add(requestBytes, std::memory_order_relaxed);
    c.plannerReadChildBytes.fetch_add(childBytes, std::memory_order_relaxed);
    c.plannerReadSpanBytes.fetch_add(spanBytes, std::memory_order_relaxed);
    c.plannerReadCoalescedChildren.fetch_add(coalescedChildren, std::memory_order_relaxed);
    c.plannerReadDirectChildren.fetch_add(directChildren, std::memory_order_relaxed);
    c.plannerReadHeadRmwChildren.fetch_add(headRmwChildren, std::memory_order_relaxed);
    c.plannerReadTailRmwChildren.fetch_add(tailRmwChildren, std::memory_order_relaxed);
    c.plannerReadZeroChildren.fetch_add(zeroChildren, std::memory_order_relaxed);
    c.plannerSpan4k.fetch_add(span4kChildren, std::memory_order_relaxed);
    c.plannerSpan8k.fetch_add(span8kChildren, std::memory_order_relaxed);
    c.plannerSpanOther.fetch_add(spanOtherChildren, std::memory_order_relaxed);
}

void recordFrontendSubmit() noexcept {
    if (!stats::enabled()) return;
    stats::counters().frontendSubmitOps.fetch_add(1, std::memory_order_relaxed);
}

void recordCompletionWait() noexcept {
    if (!stats::enabled()) return;
    stats::counters().completionWaitCount.fetch_add(1, std::memory_order_relaxed);
}

void recordCompletionWaitNs(std::uint64_t nanoseconds) noexcept {
    if (!stats::enabled()) return;
    stats::counters().completionWaitNs.fetch_add(nanoseconds, std::memory_order_relaxed);
}

void recordCompletion() noexcept {
    if (!stats::enabled()) return;
    stats::counters().completionCount.fetch_add(1, std::memory_order_relaxed);
}

void recordFrontendQueueWaitNs(std::uint64_t nanoseconds) noexcept {
    if (!stats::enabled()) return;
    stats::counters().frontendQueueWaitNs.fetch_add(nanoseconds, std::memory_order_relaxed);
}

void recordFrontendIoNs(std::uint64_t nanoseconds) noexcept {
    if (!stats::enabled()) return;
    stats::counters().frontendIoNs.fetch_add(nanoseconds, std::memory_order_relaxed);
}

void recordReadBufferPlan(std::uint64_t bounceChildren, std::uint64_t bounceBytes,
                          std::uint64_t directUserChildren,
                          std::uint64_t directUserBytes) noexcept {
    if (!stats::enabled()) return;
    auto &c = stats::counters();
    c.readBounceChildren.fetch_add(bounceChildren, std::memory_order_relaxed);
    c.readBounceBytes.fetch_add(bounceBytes, std::memory_order_relaxed);
    c.readDirectUserChildren.fetch_add(directUserChildren, std::memory_order_relaxed);
    c.readDirectUserBytes.fetch_add(directUserBytes, std::memory_order_relaxed);
}

void recordReadCopyOut(std::uint64_t bytes, std::uint64_t nanoseconds) noexcept {
    if (!stats::enabled()) return;
    auto &c = stats::counters();
    c.readCopyOutOps.fetch_add(1, std::memory_order_relaxed);
    c.readCopyOutBytes.fetch_add(bytes, std::memory_order_relaxed);
    c.readCopyOutNs.fetch_add(nanoseconds, std::memory_order_relaxed);
}

void recordSnapshotHit() noexcept {
    if (!stats::enabled()) return;
    stats::counters().snapshotHits.fetch_add(1, std::memory_order_relaxed);
}

void recordSnapshotBuild() noexcept {
    if (!stats::enabled()) return;
    stats::counters().snapshotBuilds.fetch_add(1, std::memory_order_relaxed);
}

} // namespace uuse::diagnostics
