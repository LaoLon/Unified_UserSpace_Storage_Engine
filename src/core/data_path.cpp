#include "core/data_path.hpp"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>

#include "common/trace.hpp"
#include "core/parent_completion.hpp"
#include "runtime/spdk_runtime.hpp"

namespace hfs {

namespace {

struct WaitState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    int status = 0;
};

void childCallback(void *ctx, int status) {
    auto *state = static_cast<WaitState *>(ctx);
    trace::event(trace::EventId::BdevComplete, static_cast<std::uint64_t>(status));
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->done = true;
        state->status = status;
    }
    state->cv.notify_all();
}

/* Wait for a child completion; on timeout report TimedOut but keep waiting so
 * a late SPDK completion never writes into a released buffer. */
int waitForChild(WaitState &state, std::uint32_t timeoutMs) {
    std::unique_lock<std::mutex> lock(state.mutex);
    if (state.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                          [&state]() { return state.done; })) {
        return state.status;
    }
    state.cv.wait(lock, [&state]() { return state.done; });
    return -ETIMEDOUT;
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
    deviceBlocksPerMeta_.assign(count, 1);
    for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t sector = runtime_.deviceBlockSize(i);
        if (sector == 0) {
            sector = 512;
        }
        deviceBlockBytes_[i] = sector;
        const std::uint32_t factor = plannerConfig_.blockSize / sector;
        deviceBlocksPerMeta_[i] = factor == 0 ? 1 : factor;
    }
}

Result<void> DataPath::read(const ExtentMap &map, std::span<std::byte> buffer,
                            std::uint64_t offset, std::uint64_t length) {
    return execute(map, buffer, {}, offset, length, false);
}

Result<void> DataPath::write(const ExtentMap &map, std::span<const std::byte> buffer,
                             std::uint64_t offset, std::uint64_t length) {
    return execute(map, {}, buffer, offset, length, true);
}

Result<void> DataPath::execute(const ExtentMap &map, std::span<std::byte> readBuffer,
                               std::span<const std::byte> writeBuffer, std::uint64_t offset,
                               std::uint64_t length, bool write) {
    auto planResult = planner_.plan(map, offset, length, write, !write);
    if (!planResult.ok()) {
        return makeError(planResult.status(), planResult.message());
    }
    const auto &plan = planResult.value();

    ParentCompletion parent;
    parent.init(plan.count, nullptr, nullptr);
    int firstError = 0;

    for (std::uint32_t i = 0; i < plan.count; ++i) {
        const auto &child = plan.children[i];
        if (firstError != 0) {
            parent.childDone(firstError, 0);
            continue;
        }
        if (child.kind == IoPlanner::Child::Kind::Zero) {
            std::memset(readBuffer.data() + child.bufferOffset, 0, child.bytes);
            stats_.zeroBytes += child.bytes;
            ++stats_.zeroChildren;
            parent.childDone(0, child.bytes);
            continue;
        }
        if (child.deviceId >= deviceBlockBytes_.size()) {
            firstError = static_cast<int>(Status::InvalidArgument);
            parent.childDone(firstError, 0);
            continue;
        }
        const std::uint32_t deviceBlockBytes = deviceBlockBytes_[child.deviceId];
        const std::uint32_t blocksPerMeta = deviceBlocksPerMeta_[child.deviceId];
        if (plannerConfig_.blockSize % deviceBlockBytes != 0) {
            firstError = static_cast<int>(Status::InvalidArgument);
            parent.childDone(firstError, 0);
            continue;
        }

        BufferLease lease;
        bool direct = false;
        std::span<std::byte> directSpan;
        if (child.kind == IoPlanner::Child::Kind::Direct) {
            /* Direct DMA is only safe for arena-owned (registered, contiguous)
             * buffers; foreign memory may be scattered or unpinned. */
            if (write) {
                auto slice = const_cast<std::byte *>(writeBuffer.data() + child.bufferOffset);
                if (arena_.owns(slice) && runtime_.isDmaCapable(slice, child.bytes)) {
                    directSpan = std::span<std::byte>(slice, child.bytes);
                    direct = true;
                }
            } else {
                directSpan = readBuffer.subspan(child.bufferOffset, child.bytes);
                if (arena_.owns(directSpan.data()) &&
                    runtime_.isDmaCapable(directSpan.data(), directSpan.size())) {
                    direct = true;
                }
            }
        }
        if (direct) {
            auto leaseResult = BufferLease::fromCaller(directSpan);
            if (!leaseResult.ok()) {
                firstError = static_cast<int>(Status::InvalidArgument);
                parent.childDone(firstError, 0);
                continue;
            }
            lease = std::move(leaseResult).value();
            ++stats_.directChildren;
        } else {
            auto leaseResult = BufferLease::fromArena(arena_, child.spanBytes,
                                                      config_.timeoutMs);
            if (!leaseResult.ok()) {
                firstError = static_cast<int>(leaseResult.status());
                parent.childDone(firstError, 0);
                continue;
            }
            lease = std::move(leaseResult).value();
            ++stats_.bounceChildren;
        }
        auto span = lease.span();
        const std::uint64_t deviceOffset = child.physicalBlock * blocksPerMeta;
        const std::uint64_t deviceBlocks = child.spanBytes / deviceBlockBytes;

        if (child.kind != IoPlanner::Child::Kind::Direct && write) {
            /* RMW: refresh the full block, merge the user bytes, then write. */
            WaitState readState;
            auto submit =
                runtime_.submitDataRead(config_.reactorIndex, child.deviceId,
                                        span.data(), deviceOffset, deviceBlocks,
                                        childCallback, &readState);
            if (!submit.ok()) {
                firstError = static_cast<int>(submit.status());
                parent.childDone(firstError, 0);
                continue;
            }
            ++stats_.submits;
            trace::event(trace::EventId::BdevSubmit, deviceOffset, deviceBlocks, 2);
            const int status = waitForChild(readState, config_.timeoutMs);
            ++stats_.completes;
            if (status != 0) {
                firstError = status;
                parent.childDone(status, 0);
                continue;
            }
            std::memcpy(span.data() + child.spanOffset,
                        writeBuffer.data() + child.bufferOffset, child.bytes);
            stats_.copyBytes += child.bytes;
            trace::event(trace::EventId::CopyIn, child.bytes);
        } else if (write && !direct) {
            std::memcpy(span.data(), writeBuffer.data() + child.bufferOffset, child.bytes);
            stats_.copyBytes += child.bytes;
            trace::event(trace::EventId::CopyIn, child.bytes);
        }

        WaitState state;
        Result<void> submit =
            write ? runtime_.submitDataWrite(config_.reactorIndex, child.deviceId,
                                             span.data(), deviceOffset, deviceBlocks,
                                             childCallback, &state)
                  : runtime_.submitDataRead(config_.reactorIndex, child.deviceId,
                                            span.data(), deviceOffset, deviceBlocks,
                                            childCallback, &state);
        if (!submit.ok()) {
            firstError = static_cast<int>(submit.status());
            parent.childDone(firstError, 0);
            continue;
        }
        ++stats_.submits;
        trace::event(trace::EventId::BdevSubmit, deviceOffset, deviceBlocks,
                     write ? 1ULL : 0ULL);
        const int status = waitForChild(state, config_.timeoutMs);
        ++stats_.completes;
        if (status != 0) {
            firstError = status;
            parent.childDone(status, 0);
            continue;
        }

        if (!write) {
            if (child.kind == IoPlanner::Child::Kind::Direct) {
                if (!direct) {
                    std::memcpy(readBuffer.data() + child.bufferOffset, span.data(),
                                child.bytes);
                    stats_.copyBytes += child.bytes;
                    trace::event(trace::EventId::CopyOut, child.bytes);
                }
            } else {
                std::memcpy(readBuffer.data() + child.bufferOffset,
                            span.data() + child.spanOffset, child.bytes);
                stats_.copyBytes += child.bytes;
                trace::event(trace::EventId::CopyOut, child.bytes);
            }
        }
        parent.childDone(0, child.bytes);
    }

    ++stats_.parentWaits;
    if (firstError != 0 || parent.firstError() != 0) {
        ++stats_.errors;
        return makeError(Status::IoError, "data path child failed");
    }
    if (!parent.done() || parent.completedBytes() != plan.coveredBytes) {
        ++stats_.errors;
        return makeError(Status::Corrupt, "parent completion accounting mismatch");
    }
    return Result<void>(Status::Ok);
}

DataPath::Stats DataPath::stats() const noexcept { return stats_; }

} // namespace hfs
