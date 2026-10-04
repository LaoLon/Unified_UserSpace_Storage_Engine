#include "core/io_planner.hpp"

#include <algorithm>
#include <limits>

#include "common/trace.hpp"

namespace uuse {

IoPlanner::IoPlanner(Config config) : config_(config) {}

Result<IoPlanner::Plan> IoPlanner::plan(const ExtentMap &map, std::uint64_t offset,
                                        std::uint64_t length, bool write,
                                        bool allowHoles,
                                        std::uint32_t blockSizeOverride) const {
    if (length == 0) {
        return makeError<Plan>(Status::InvalidArgument, "zero-length I/O");
    }
    if (offset > std::numeric_limits<std::uint64_t>::max() - length) {
        return makeError<Plan>(Status::InvalidArgument, "I/O range overflows");
    }
    const std::uint32_t blockSizeValue = blockSizeOverride == 0 ? config_.blockSize
                                                                  : blockSizeOverride;
    if (blockSizeValue == 0 || config_.maxChildBytes < blockSizeValue ||
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
    const std::uint64_t blockSize = blockSizeValue;
    const bool usePhysicalAlignment = blockSizeOverride != 0;

    auto pushChild = [&](Child child) -> Result<void> {
        if (usePhysicalAlignment && child.kind == Child::Kind::Direct && plan.count != 0) {
            Child &previous = plan.children[plan.count - 1];
            const std::uint64_t previousLogicalEnd =
                previous.logicalOffset + previous.bytes;
            const std::uint64_t previousPhysicalEnd =
                previous.physicalByte + previous.spanBytes;
            const std::uint64_t previousBufferEnd =
                static_cast<std::uint64_t>(previous.bufferOffset) + previous.bytes;
            const std::uint64_t combinedSpan =
                static_cast<std::uint64_t>(previous.spanBytes) + child.spanBytes;
            if (previous.kind == Child::Kind::Direct && previous.spanOffset == 0 &&
                child.spanOffset == 0 && previous.deviceId == child.deviceId &&
                previousLogicalEnd == child.logicalOffset &&
                previousPhysicalEnd == child.physicalByte &&
                previousBufferEnd == child.bufferOffset &&
                combinedSpan <= config_.maxChildBytes && combinedSpan <=
                                                   std::numeric_limits<std::uint32_t>::max()) {
                previous.bytes = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(previous.bytes) + child.bytes);
                previous.spanBytes = static_cast<std::uint32_t>(combinedSpan);
                ++plan.coalescedChildren;
                return Result<void>(Status::Ok);
            }
        }
        if (plan.count >= config_.maxChildren) {
            return makeError(Status::NoMemory, "I/O plan exceeds the child limit");
        }
        plan.children[plan.count++] = child;
        return Result<void>(Status::Ok);
    };

    for (std::size_t i = 0; i < lookup.value(); ++i) {
        const ExtentView &extent = extents[i];
        if (extent.logicalOffset != cursor && allowHoles && !write) {
            const std::uint64_t gap = extent.logicalOffset - cursor;
            Child child;
            child.kind = Child::Kind::Zero;
            child.logicalOffset = cursor;
            child.bytes = static_cast<std::uint32_t>(gap);
            child.spanBytes = static_cast<std::uint32_t>(gap);
            child.bufferOffset = static_cast<std::uint32_t>(cursor - offset);
            auto pushed = pushChild(child);
            if (!pushed.ok()) {
                return makeError<Plan>(pushed.status(), pushed.message());
            }
            cursor = extent.logicalOffset;
        }
        if (extent.logicalOffset != cursor) {
            return makeError<Plan>(Status::NotFound,
                                   "logical range is not fully allocated");
        }
        std::uint64_t remaining = extent.logicalOffset + extent.length - cursor;

        while (remaining > 0) {
            const std::uint64_t physicalByte =
                extent.physicalByte + (cursor - extent.logicalOffset);
            const std::uint64_t blockOffset =
                usePhysicalAlignment ? physicalByte % blockSize : cursor % blockSize;
            if (blockOffset != 0) {
                const std::uint64_t span = std::min(blockSize - blockOffset, remaining);
                Child child;
                child.kind = Child::Kind::HeadRmw;
                child.logicalOffset = cursor;
                child.physicalBlock = (physicalByte - blockOffset) / blockSize;
                child.physicalByte = physicalByte - blockOffset;
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
                child.physicalByte = physicalByte;
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
            child.physicalByte = physicalByte;
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
        if (allowHoles && !write) {
            Child child;
            child.kind = Child::Kind::Zero;
            child.logicalOffset = cursor;
            child.bytes = static_cast<std::uint32_t>(end - cursor);
            child.spanBytes = child.bytes;
            child.bufferOffset = static_cast<std::uint32_t>(cursor - offset);
            auto pushed = pushChild(child);
            if (!pushed.ok()) {
                return makeError<Plan>(pushed.status(), pushed.message());
            }
            cursor = end;
        } else {
            return makeError<Plan>(Status::NotFound,
                                   "logical range is not fully allocated");
        }
    }
    plan.coveredBytes = length;
    trace::event(trace::EventId::ExtentPlan, offset, length, plan.count,
                 write ? 1ULL : 0ULL);
    return plan;
}

} // namespace uuse
