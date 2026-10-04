#include "metadata/inode_map.hpp"

#include <algorithm>
#include <limits>

namespace uuse::metadata {

void InodeExtentMap::configure(std::uint64_t extentBytes) noexcept {
    if (extentBytes != 0) {
        extentBytes_ = extentBytes;
    }
}

Result<void> InodeExtentMap::addExtents(std::span<const ExtentDescriptor> descriptors) {
    for (const ExtentDescriptor &descriptor : descriptors) {
        if (descriptor.count == 0) {
            return makeError(Status::InvalidArgument, "empty extent descriptor");
        }
        const std::uint64_t start = descriptor.logicalExtent;
        const std::uint64_t end = start + descriptor.count;
        /* Monotonic file growth appends at the end without a search. */
        if (!extents_.empty()) {
            const ExtentDescriptor &last = extents_.back();
            if (start == last.logicalExtent + last.count) {
                extents_.push_back(descriptor);
                continue;
            }
        }
        auto it = std::lower_bound(extents_.begin(), extents_.end(), start,
                                   [](const ExtentDescriptor &left, std::uint64_t value) {
                                       return left.logicalExtent < value;
                                   });
        if (it != extents_.begin()) {
            const ExtentDescriptor &previous = *std::prev(it);
            if (previous.logicalExtent + previous.count > start) {
                return makeError(Status::AlreadyExists, "extent overlaps existing mapping");
            }
        }
        if (it != extents_.end() && it->logicalExtent < end) {
            return makeError(Status::AlreadyExists, "extent overlaps existing mapping");
        }
        extents_.insert(it, descriptor);
    }
    return Result<void>(Status::Ok);
}

std::size_t InodeExtentMap::firstOverlapping(std::uint64_t logicalExtent) const noexcept {
    return static_cast<std::size_t>(
        std::lower_bound(extents_.begin(), extents_.end(), logicalExtent,
                         [](const ExtentDescriptor &descriptor, std::uint64_t extent) {
                             return descriptor.logicalExtent + descriptor.count <= extent;
                         }) -
        extents_.begin());
}

const ExtentDescriptor *InodeExtentMap::descriptorAt(std::size_t index) const noexcept {
    return index < extents_.size() ? &extents_[index] : nullptr;
}

/* Validates a batch without mutating the map so commitExtents can keep the
 * old all-or-nothing contract without copying the whole extent vector. */
Result<void> InodeExtentMap::validateInsert(
    std::span<const ExtentDescriptor> descriptors) const {
    for (std::size_t i = 0; i < descriptors.size(); ++i) {
        const ExtentDescriptor &descriptor = descriptors[i];
        if (descriptor.count == 0) {
            return makeError(Status::InvalidArgument, "empty extent descriptor");
        }
        const std::uint64_t start = descriptor.logicalExtent;
        const std::uint64_t end = start + descriptor.count;
        if (end < start) {
            return makeError(Status::InvalidArgument, "extent range overflows");
        }
        const std::size_t position = firstOverlapping(start);
        if (position < extents_.size() && extents_[position].logicalExtent < end) {
            return makeError(Status::AlreadyExists, "extent overlaps existing mapping");
        }
        for (std::size_t j = 0; j < i; ++j) {
            const ExtentDescriptor &other = descriptors[j];
            if (other.logicalExtent < end &&
                start < other.logicalExtent + other.count) {
                return makeError(Status::AlreadyExists, "extent batch overlaps itself");
            }
        }
    }
    return Result<void>(Status::Ok);
}

Result<void> InodeExtentMap::removeFrom(std::uint64_t logicalExtent) {
    while (!extents_.empty()) {
        ExtentDescriptor &last = extents_.back();
        if (last.logicalExtent >= logicalExtent) {
            extents_.pop_back();
            continue;
        }
        if (last.logicalExtent + last.count > logicalExtent) {
            last.count = logicalExtent - last.logicalExtent;
        }
        break;
    }
    return Result<void>(Status::Ok);
}

std::uint64_t InodeExtentMap::mappedExtents() const noexcept {
    std::uint64_t total = 0;
    for (const ExtentDescriptor &descriptor : extents_) {
        total += descriptor.count;
    }
    return total;
}

std::uint64_t InodeExtentMap::logicalExtents() const noexcept {
    if (extents_.empty()) {
        return 0;
    }
    const ExtentDescriptor &last = extents_.back();
    return last.logicalExtent + last.count;
}

bool InodeExtentMap::empty() const noexcept { return extents_.empty(); }

std::size_t InodeExtentMap::size() const noexcept { return extents_.size(); }

std::vector<ExtentDescriptor> InodeExtentMap::snapshot() const { return extents_; }

Result<void> InodeExtentMap::restore(std::span<const ExtentDescriptor> descriptors) {
    std::vector<ExtentDescriptor> restored(descriptors.begin(), descriptors.end());
    std::uint64_t previousEnd = 0;
    for (const ExtentDescriptor &descriptor : restored) {
        if (descriptor.count == 0 || descriptor.logicalExtent < previousEnd) {
            return makeError(Status::Corrupt, "invalid extent map snapshot");
        }
        previousEnd = descriptor.logicalExtent + descriptor.count;
    }
    extents_ = std::move(restored);
    return Result<void>(Status::Ok);
}

Result<std::size_t> InodeExtentMap::lookup(std::uint64_t offset, std::uint64_t length,
                                           std::span<ExtentView> out) const {
    if (length == 0 || offset > std::numeric_limits<std::uint64_t>::max() - length) {
        return makeError<std::size_t>(Status::InvalidArgument, "extent lookup range overflows");
    }
    const std::uint64_t end = offset + length;
    /* Descriptors are sorted and disjoint, so their end offsets are strictly
     * increasing: binary-search the first descriptor that can overlap instead
     * of scanning the whole map from the start (O(E) -> O(log E + k)). */
    const std::uint64_t offsetExtent = offset / extentBytes_;
    auto next = std::lower_bound(
        extents_.begin(), extents_.end(), offsetExtent,
        [](const ExtentDescriptor &descriptor, std::uint64_t extent) {
            return descriptor.logicalExtent + descriptor.count <= extent;
        });
    std::size_t count = 0;
    for (; next != extents_.end(); ++next) {
        const ExtentDescriptor &descriptor = *next;
        const std::uint64_t startByte = descriptor.logicalExtent * extentBytes_;
        if (startByte >= end) {
            break;
        }
        if (count >= out.size()) {
            return makeError<std::size_t>(Status::NoMemory, "extent lookup buffer too small");
        }
        const std::uint64_t endByte =
            (descriptor.logicalExtent + descriptor.count) * extentBytes_;
        const std::uint64_t clippedStart = std::max(startByte, offset);
        const std::uint64_t clippedEnd = std::min(endByte, end);
        ExtentView view;
        view.logicalOffset = clippedStart;
        view.length = clippedEnd - clippedStart;
        view.physicalByte =
            descriptor.physicalExtent * extentBytes_ + (clippedStart - startByte);
        view.deviceId = descriptor.deviceId;
        out[count++] = view;
    }
    return count;
}

} // namespace uuse::metadata
