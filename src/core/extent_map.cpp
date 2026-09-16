#include "core/extent_map.hpp"

#include <algorithm>

namespace hfs {

void SyntheticExtentMap::add(std::uint64_t logicalOffset, std::uint64_t length,
                             std::uint64_t physicalByte, std::uint32_t deviceId) {
    extents_.push_back(ExtentView{logicalOffset, length, physicalByte, deviceId});
    std::sort(extents_.begin(), extents_.end(),
              [](const ExtentView &left, const ExtentView &right) {
                  return left.logicalOffset < right.logicalOffset;
              });
}

Result<std::size_t> SyntheticExtentMap::lookup(std::uint64_t offset, std::uint64_t length,
                                               std::span<ExtentView> out) const {
    std::size_t count = 0;
    const std::uint64_t end = offset + length;
    for (const auto &extent : extents_) {
        const std::uint64_t extentEnd = extent.logicalOffset + extent.length;
        if (extentEnd <= offset) {
            continue;
        }
        if (extent.logicalOffset >= end) {
            break;
        }
        if (count >= out.size()) {
            return makeError<std::size_t>(Status::NoMemory, "extent lookup buffer too small");
        }
        const std::uint64_t clippedStart = std::max(extent.logicalOffset, offset);
        const std::uint64_t clippedEnd = std::min(extentEnd, end);
        ExtentView view;
        view.logicalOffset = clippedStart;
        view.length = clippedEnd - clippedStart;
        view.physicalByte = extent.physicalByte + (clippedStart - extent.logicalOffset);
        view.deviceId = extent.deviceId;
        out[count++] = view;
    }
    return count;
}

std::size_t SyntheticExtentMap::size() const noexcept { return extents_.size(); }

void SyntheticExtentMap::clear() noexcept { extents_.clear(); }

} // namespace hfs
