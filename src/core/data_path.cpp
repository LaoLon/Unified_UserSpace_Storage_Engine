#include "core/data_path.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

#include "common/trace.hpp"
#include "common/test_hooks.hpp"
#include "core/parent_completion.hpp"
#include "uuse/diagnostics.hpp"
#include "runtime/spdk_runtime.hpp"

namespace uuse {

namespace {

/* One completion latch shared by every child of a parent request: the sync
 * path pays one condvar handshake per batch instead of one per child. */
struct ParentLatch {
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t pending = 0;
    int firstError = 0;
};

/* Keeps one planned child and its DMA storage alive across submit/reap. */
struct ChildExecution {
    BufferLease lease;
    std::span<std::byte> ioBuffer{};
    int status = 0;
    bool direct = false;
    bool submitted = false;
    bool rmwRead = false;
    bool secondSubmitted = false;
};

/* Callback context: one child slot plus the shared parent latch. */
struct ChildCallbackContext {
    ParentLatch *latch = nullptr;
    ChildExecution *child = nullptr;
    bool writePhase = false;
};

void childCallback(void *ctx, int status) {
    auto *context = static_cast<ChildCallbackContext *>(ctx);
    trace::event(trace::EventId::BdevComplete, static_cast<std::uint64_t>(status));
    /* Test-only delays make completion and RMW lifetime boundaries
     * deterministic; the cached hook keeps device completions getenv-free. */
    const test_hooks::DataIoDelaySetting delay =
        test_hooks::effectiveDataIoDelay(context->writePhase);
    if (delay.present) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay.milliseconds));
    }
    ParentLatch *latch = context->latch;
    {
        std::lock_guard<std::mutex> lock(latch->mutex);
        context->child->status = status;
        if (status != 0 && latch->firstError == 0) {
            latch->firstError = status;
        }
        if (latch->pending != 0) {
            --latch->pending;
        }
        /* Notify under the lock: the waiter destroys the stack latch as soon as
         * it observes pending == 0, so the condvar must not be touched after
         * the lock is released (Shard-118 TSAN). */
        latch->cv.notify_all();
    }
}

/* Arms the latch before submission so a fast completion cannot be lost. */
void armLatch(ParentLatch &latch, std::size_t count) {
    std::lock_guard<std::mutex> lock(latch.mutex);
    latch.pending = count;
}

/* Releases one armed slot for a child that was skipped or failed to submit. */
void cancelLatchSlot(ParentLatch &latch) {
    std::lock_guard<std::mutex> lock(latch.mutex);
    if (latch.pending != 0) {
        --latch.pending;
    }
}

/* Waits for every armed child; on timeout it reports TimedOut but keeps
 * waiting so a late SPDK completion never writes into a released buffer. */
int waitForLatch(ParentLatch &latch, std::uint32_t timeoutMs) {
    std::unique_lock<std::mutex> lock(latch.mutex);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    if (latch.cv.wait_until(lock, deadline, [&latch] { return latch.pending == 0; })) {
        return latch.firstError;
    }
    latch.cv.wait(lock, [&latch] { return latch.pending == 0; });
    return -ETIMEDOUT;
}

/* Preserve the first child status and backend errno at the parent boundary. */
Result<void> childFailureResult(int error) {
    if (error == -ETIMEDOUT) {
        return makeError(Status::TimedOut, "data path child timed out");
    }
    if (error == -ECANCELED) {
        return makeError(Status::Cancelled, "data path child cancelled");
    }
    if (error == -EAGAIN) {
        return makeError(Status::Busy, "data path child temporarily unavailable");
    }
    if (error == -ENOSPC) {
        return makeError(Status::NoSpace, "data path child has no space");
    }
    if (error > 0 && error <= static_cast<int>(Status::InvalidState)) {
        const auto status = static_cast<Status>(error);
        return makeError(status, std::string("data path child failed: ") +
                                   statusName(status));
    }
    if (error < 0) {
        const int backendErrno = -error;
        return makeError(Status::IoError,
                         "data path child failed: errno=" +
                             std::to_string(backendErrno) + " (" +
                             std::strerror(backendErrno) + ")");
    }
    return makeError(Status::IoError,
                     "data path child failed: status=" + std::to_string(error));
}

} // namespace

DataPath::DataPath(SpdkRuntime &runtime, DmaArena &arena, IoPlanner::Config plannerConfig,
                   Config config)
    : runtime_(runtime),
      arena_(arena),
      planner_(plannerConfig),
      plannerConfig_(plannerConfig),
      config_(config) {
    const std::size_t deviceCount = runtime_.deviceCount();
    const std::size_t count = deviceCount == 0 ? 1 : deviceCount;
    deviceBlockBytes_.assign(count, 512);
    for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t sector = runtime_.deviceBlockSize(i);
        if (sector == 0) {
            sector = 512;
        }
        deviceBlockBytes_[i] = sector;
    }
}

Result<void> DataPath::read(const ExtentMap &map, std::span<std::byte> buffer,
                            std::uint64_t offset, std::uint64_t length) {
    return execute(map, buffer, {}, offset, length, false, 0);
}

Result<void> DataPath::write(const ExtentMap &map, std::span<const std::byte> buffer,
                             std::uint64_t offset, std::uint64_t length,
                             std::uint64_t knownEof) {
    return execute(map, {}, buffer, offset, length, true, knownEof);
}

Result<void> DataPath::execute(const ExtentMap &map, std::span<std::byte> readBuffer,
                               std::span<const std::byte> writeBuffer, std::uint64_t offset,
                               std::uint64_t length, bool write, std::uint64_t knownEof) {
    auto planResult = planner_.plan(map, offset, length, write, !write,
                                    config_.nativeBlockSize);
    if (!planResult.ok()) {
        return makeError(planResult.status(), planResult.message());
    }
    const auto &plan = planResult.value();
    if (!write && diagnostics::profilingEnabled()) {
        std::uint64_t childBytes = 0;
        std::uint64_t spanBytes = 0;
        std::uint64_t directChildren = 0;
        std::uint64_t headRmwChildren = 0;
        std::uint64_t tailRmwChildren = 0;
        std::uint64_t zeroChildren = 0;
        std::uint64_t span4kChildren = 0;
        std::uint64_t span8kChildren = 0;
        std::uint64_t spanOtherChildren = 0;
        for (std::uint32_t index = 0; index < plan.count; ++index) {
            const auto &child = plan.children[index];
            childBytes += child.bytes;
            spanBytes += child.spanBytes;
            if (child.spanBytes == 4096) {
                ++span4kChildren;
            } else if (child.spanBytes == 8192) {
                ++span8kChildren;
            } else {
                ++spanOtherChildren;
            }
            switch (child.kind) {
            case IoPlanner::Child::Kind::Direct:
                ++directChildren;
                break;
            case IoPlanner::Child::Kind::HeadRmw:
                ++headRmwChildren;
                break;
            case IoPlanner::Child::Kind::TailRmw:
                ++tailRmwChildren;
                break;
            case IoPlanner::Child::Kind::Zero:
                ++zeroChildren;
                break;
            }
        }
        diagnostics::recordDataReadPlan(length, childBytes, spanBytes, directChildren,
                                        plan.coalescedChildren,
                                        headRmwChildren, tailRmwChildren, zeroChildren,
                                        span4kChildren, span8kChildren, spanOtherChildren);
    }
    ParentCompletion parent;
    parent.init(plan.count, nullptr, nullptr);
    int firstError = 0;

    /* Admit every DMA lease before the first device submission. This makes
     * resource shortage fail as one parent instead of partially publishing
     * children and then discovering that a later child cannot be prepared. */
    std::array<ChildExecution, IoPlanner::Plan::kMaxChildren> executions{};
    std::array<std::size_t, IoPlanner::Plan::kMaxChildren> leaseSizes{};
    std::array<std::uint32_t, IoPlanner::Plan::kMaxChildren> leaseIndices{};
    std::array<BufferLease, IoPlanner::Plan::kMaxChildren> leases{};
    std::array<ChildCallbackContext, IoPlanner::Plan::kMaxChildren> callbackContexts{};
    leaseIndices.fill(std::numeric_limits<std::uint32_t>::max());
    std::size_t leaseCount = 0;
    for (std::uint32_t i = 0; i < plan.count; ++i) {
        const auto &child = plan.children[i];
        if (child.kind == IoPlanner::Child::Kind::Zero) {
            continue;
        }
        if (child.deviceId >= deviceBlockBytes_.size()) {
            firstError = static_cast<int>(Status::InvalidArgument);
            continue;
        }
        const std::uint32_t deviceBlockBytes = deviceBlockBytes_[child.deviceId];
        if (plannerConfig_.blockSize % deviceBlockBytes != 0 || child.spanBytes == 0 ||
            child.physicalByte % deviceBlockBytes != 0 ||
            child.spanBytes % deviceBlockBytes != 0 ||
            static_cast<std::uint64_t>(child.bufferOffset) + child.bytes >
                (write ? writeBuffer.size() : readBuffer.size())) {
            firstError = static_cast<int>(Status::InvalidArgument);
            continue;
        }

        std::span<std::byte> directSpan;
        if (child.kind == IoPlanner::Child::Kind::Direct) {
            /* Direct DMA is only safe for arena-owned, contiguous buffers. */
            if (write) {
                auto *slice = const_cast<std::byte *>(writeBuffer.data() + child.bufferOffset);
                if (arena_.ownsRange(slice, child.bytes) &&
                    runtime_.isDmaCapable(slice, child.bytes)) {
                    directSpan = std::span<std::byte>(slice, child.bytes);
                }
            } else {
                directSpan = readBuffer.subspan(child.bufferOffset, child.bytes);
                if (!arena_.ownsRange(directSpan.data(), directSpan.size()) ||
                    !runtime_.isDmaCapable(directSpan.data(), directSpan.size())) {
                    directSpan = {};
                }
            }
        }
        if (!directSpan.empty()) {
            executions[i].direct = true;
            executions[i].ioBuffer = directSpan;
            ++stats_.directChildren;
        } else {
            leaseIndices[i] = static_cast<std::uint32_t>(leaseCount);
            leaseSizes[leaseCount++] = child.spanBytes;
            ++stats_.bounceChildren;
        }
    }
    if (firstError == 0 && leaseCount != 0) {
        auto acquired = arena_.tryAcquireBatch(
            std::span<const std::size_t>(leaseSizes.data(), leaseCount),
            std::span<BufferLease>(leases.data(), leaseCount));
        if (!acquired.ok()) {
            firstError = static_cast<int>(acquired.status());
        } else {
            for (std::uint32_t i = 0; i < plan.count; ++i) {
                if (leaseIndices[i] == std::numeric_limits<std::uint32_t>::max()) {
                    continue;
                }
                executions[i].lease = std::move(leases[leaseIndices[i]]);
                executions[i].ioBuffer = executions[i].lease.span();
            }
        }
    }

    /* Phase 1: arm the shared latch, submit every ready child, then wait once
     * for the whole batch instead of one condvar handshake per child. */
    ParentLatch latch;
    armLatch(latch, plan.count);
    for (std::uint32_t i = 0; i < plan.count; ++i) {
        const auto &child = plan.children[i];
        if (child.kind == IoPlanner::Child::Kind::Zero) {
            if (!write) {
                std::memset(readBuffer.data() + child.bufferOffset, 0, child.bytes);
            }
            stats_.zeroBytes += child.bytes;
            ++stats_.zeroChildren;
            parent.childDone(0, child.bytes);
            cancelLatchSlot(latch);
            continue;
        }
        if (firstError != 0 || executions[i].ioBuffer.empty()) {
            parent.childDone(firstError == 0 ? static_cast<int>(Status::NoMemory) : firstError,
                             0);
            cancelLatchSlot(latch);
            continue;
        }

        const std::uint32_t deviceBlockBytes = deviceBlockBytes_[child.deviceId];
        const std::uint64_t deviceOffset = child.physicalByte / deviceBlockBytes;
        const std::uint64_t deviceBlocks = child.spanBytes / deviceBlockBytes;
        const std::uint64_t spanLogicalStart =
            child.logicalOffset >= child.spanOffset ? child.logicalOffset - child.spanOffset
                                                    : 0;
        const bool needsRmw = write && child.kind != IoPlanner::Child::Kind::Direct &&
                              !executions[i].direct && spanLogicalStart < knownEof;
        executions[i].rmwRead = needsRmw;
        if (write && !executions[i].direct) {
            if (!needsRmw) {
                std::memset(executions[i].ioBuffer.data(), 0, child.spanBytes);
                std::memcpy(executions[i].ioBuffer.data() + child.spanOffset,
                            writeBuffer.data() + child.bufferOffset, child.bytes);
                stats_.copyBytes += child.bytes;
                trace::event(trace::EventId::CopyIn, child.bytes);
            }
        }

        const std::uint32_t reactorIndex =
            runtime_.dataReactorCount() == 0
                ? config_.reactorIndex
                : runtime_.dataReactorForDevice(child.deviceId);
        callbackContexts[i] = ChildCallbackContext{&latch, &executions[i],
                                                   write && !needsRmw};
        Result<void> submit =
            needsRmw || !write
                ? runtime_.submitDataRead(reactorIndex, child.deviceId,
                                          executions[i].ioBuffer.data(), deviceOffset,
                                          deviceBlocks, childCallback, &callbackContexts[i])
                : runtime_.submitDataWrite(reactorIndex, child.deviceId,
                                           executions[i].ioBuffer.data(), deviceOffset,
                                           deviceBlocks, childCallback, &callbackContexts[i]);
        if (!submit.ok()) {
            if (firstError == 0) firstError = static_cast<int>(submit.status());
            executions[i].status = static_cast<int>(submit.status());
            parent.childDone(static_cast<int>(submit.status()), 0);
            cancelLatchSlot(latch);
            continue;
        }
        executions[i].submitted = true;
        ++stats_.submits;
        trace::event(trace::EventId::BdevSubmit, deviceOffset, deviceBlocks,
                     needsRmw ? 2ULL : (write ? 1ULL : 0ULL));
    }
    if (waitForLatch(latch, config_.timeoutMs) == -ETIMEDOUT && firstError == 0) {
        firstError = -ETIMEDOUT;
    }

    for (std::uint32_t i = 0; i < plan.count; ++i) {
        if (!executions[i].submitted) continue;
        executions[i].submitted = false;
        ++stats_.completes;
        const auto &child = plan.children[i];
        if (executions[i].status != 0) {
            if (firstError == 0) firstError = executions[i].status;
            parent.childDone(executions[i].status, 0);
            continue;
        }
        if (executions[i].rmwRead) {
            std::memcpy(executions[i].ioBuffer.data() + child.spanOffset,
                        writeBuffer.data() + child.bufferOffset, child.bytes);
            stats_.copyBytes += child.bytes;
            trace::event(trace::EventId::CopyIn, child.bytes);
            continue;
        }
        if (!write && !executions[i].direct) {
            std::memcpy(readBuffer.data() + child.bufferOffset,
                        executions[i].ioBuffer.data() + child.spanOffset, child.bytes);
            stats_.copyBytes += child.bytes;
            trace::event(trace::EventId::CopyOut, child.bytes);
        }
        parent.childDone(0, child.bytes);
    }

    /* Phase 2: RMW writes are submitted and reaped as one armed batch. */
    armLatch(latch, plan.count);
    for (std::uint32_t i = 0; i < plan.count; ++i) {
        if (!executions[i].rmwRead || executions[i].status != 0) {
            cancelLatchSlot(latch);
            continue;
        }
        const auto &child = plan.children[i];
        const std::uint32_t deviceBlockBytes = deviceBlockBytes_[child.deviceId];
        const std::uint64_t rmwOffset = child.physicalByte / deviceBlockBytes;
        const std::uint32_t reactorIndex =
            runtime_.dataReactorCount() == 0
                ? config_.reactorIndex
                : runtime_.dataReactorForDevice(child.deviceId);
        callbackContexts[i] = ChildCallbackContext{&latch, &executions[i], true};
        const Result<void> submit = runtime_.submitDataWrite(
            reactorIndex, child.deviceId, executions[i].ioBuffer.data(), rmwOffset,
            child.spanBytes / deviceBlockBytes, childCallback, &callbackContexts[i]);
        if (!submit.ok()) {
            if (firstError == 0) firstError = static_cast<int>(submit.status());
            executions[i].status = static_cast<int>(submit.status());
            parent.childDone(static_cast<int>(submit.status()), 0);
            cancelLatchSlot(latch);
            continue;
        }
        executions[i].secondSubmitted = true;
        ++stats_.submits;
        trace::event(trace::EventId::BdevSubmit, rmwOffset,
                     child.spanBytes / deviceBlockBytes, 1ULL);
    }
    if (waitForLatch(latch, config_.timeoutMs) == -ETIMEDOUT && firstError == 0) {
        firstError = -ETIMEDOUT;
    }
    for (std::uint32_t i = 0; i < plan.count; ++i) {
        if (!executions[i].secondSubmitted) continue;
        ++stats_.completes;
        if (executions[i].status != 0) {
            if (firstError == 0) firstError = executions[i].status;
            parent.childDone(executions[i].status, 0);
        } else {
            parent.childDone(0, plan.children[i].bytes);
        }
    }

    ++stats_.parentWaits;
    const int childError = firstError != 0 ? firstError : parent.firstError();
    if (childError != 0) {
        ++stats_.errors;
        return childFailureResult(childError);
    }
    if (!parent.done() || parent.completedBytes() != plan.coveredBytes) {
        ++stats_.errors;
        return makeError(Status::Corrupt, "parent completion accounting mismatch");
    }
    return Result<void>(Status::Ok);
}

DataPath::Stats DataPath::stats() const noexcept { return stats_; }

} // namespace uuse
