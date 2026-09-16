#include "core/io_planner.hpp"

#include <algorithm>

#include "common/trace.hpp"

namespace hfs {

IoPlanner::IoPlanner(Config config) : config_(config) {}

Result<IoPlanner::Plan> IoPlanner::plan(const ExtentMap &map, std::uint64_t offset,
                                        std::uint64_t length, bool write) const {
    (void)write;
    if (length == 0) {
        return makeError<Plan>(Status::InvalidArgument, "zero-length I/O");
    }
    if (config_.blockSize == 0 || config_.maxChildBytes < config_.blockSize ||
        config_.maxChildren == 0 || config_.maxChildren > Plan::kMaxChildren) {
        return makeError<Plan>(Status::InvalidArgument, "invalid planner config");
    }

    std::array<ExtentView, Plan::kMaxChildren> extents{};
    auto lookup = map.lookup(offset, length, extents);
    if (!lookup.ok()) {
        return makeError<Plan>(lookup.status(), lookup.message());
    }

    Plan plan;
    std::uint64_t cursor = offset;
    const std::uint64_t end = offset + length;
    const std::uint64_t blockSize = config_.blockSize;

    auto pushChild = [&](Child child) -> Result<void> {
        if (plan.count >= config_.maxChildren) {
            return makeError(Status::NoMemory, "I/O plan exceeds the child limit");
        }
        plan.children[plan.count++] = child;
        return Result<void>(Status::Ok);
    };

    for (std::size_t i = 0; i < lookup.value(); ++i) {
        const ExtentView &extent = extents[i];
        if (extent.logicalOffset != cursor) {
            return makeError<Plan>(Status::NotFound,
                                   "logical range is not fully allocated");
        }
        std::uint64_t remaining = extent.logicalOffset + extent.length - cursor;

        while (remaining > 0) {
            const std::uint64_t blockOffset = cursor % blockSize;
            const std::uint64_t physicalByte =
                extent.physicalByte + (cursor - extent.logicalOffset);
            if (blockOffset != 0) {
                const std::uint64_t span = std::min(blockSize - blockOffset, remaining);
                Child child;
                child.kind = Child::Kind::HeadRmw;
                child.logicalOffset = cursor;
                child.physicalBlock = (physicalByte - blockOffset) / blockSize;
                child.deviceId = extent.deviceId;
                child.bytes = static_cast<std::uint32_t>(span);
                child.spanBytes = static_cast<std::uint32_t>(blockSize);
                child.bufferOffset = static_cast<std::uint32_t>(cursor - offset);
                child.spanOffset = static_cast<std::uint32_t>(blockOffset);
                auto pushed = pushChild(child);
                if (!pushed.ok()) {
                    return makeError<Plan>(pushed.status(), pushed.message());
                }
                cursor += span;
                remaining -= span;
                continue;
            }
            if (remaining >= blockSize) {
                const std::uint64_t aligned = remaining - (remaining % blockSize);
                std::uint64_t chunk = std::min<std::uint64_t>(aligned, config_.maxChildBytes);
                chunk -= chunk % blockSize;
                if (chunk == 0) {
                    chunk = blockSize;
                }
                Child child;
                child.kind = Child::Kind::Direct;
                child.logicalOffset = cursor;
                child.physicalBlock = physicalByte / blockSize;
                child.deviceId = extent.deviceId;
                child.bytes = static_cast<std::uint32_t>(chunk);
                child.spanBytes = static_cast<std::uint32_t>(chunk);
                child.bufferOffset = static_cast<std::uint32_t>(cursor - offset);
                child.spanOffset = 0;
                auto pushed = pushChild(child);
                if (!pushed.ok()) {
                    return makeError<Plan>(pushed.status(), pushed.message());
                }
                cursor += chunk;
                remaining -= chunk;
                continue;
            }
            /* tail partial block */
            Child child;
            child.kind = Child::Kind::TailRmw;
            child.logicalOffset = cursor;
            child.physicalBlock = physicalByte / blockSize;
            child.deviceId = extent.deviceId;
            child.bytes = static_cast<std::uint32_t>(remaining);
            child.spanBytes = static_cast<std::uint32_t>(blockSize);
            child.bufferOffset = static_cast<std::uint32_t>(cursor - offset);
            child.spanOffset = 0;
            auto pushed = pushChild(child);
            if (!pushed.ok()) {
                return makeError<Plan>(pushed.status(), pushed.message());
            }
            cursor += remaining;
            remaining = 0;
        }
    }

    if (cursor != end) {
        return makeError<Plan>(Status::NotFound, "logical range is not fully allocated");
    }
    plan.coveredBytes = length;
    trace::event(trace::EventId::ExtentPlan, offset, length, plan.count,
                 write ? 1ULL : 0ULL);
    return plan;
}

} // namespace hfs
