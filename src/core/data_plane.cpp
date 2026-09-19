#include "core/data_plane.hpp"

#include "core/extent_map.hpp"

namespace hfs {

DataPlane::DataPlane(SpdkRuntime &runtime, DmaArena &arena,
                     IoPlanner::Config plannerConfig, DataPath::Config pathConfig)
    : arena_(arena), path_(runtime, arena, plannerConfig, pathConfig) {}

Result<void> DataPlane::zeroExtents(
    std::span<const metadata::ExtentDescriptor> descriptors, std::uint64_t extentBytes) {
    if (extentBytes == 0) {
        return makeError(Status::InvalidArgument, "invalid extent size");
    }
    if (zeroBuffer_.size() < extentBytes) {
        zeroBuffer_.assign(static_cast<std::size_t>(extentBytes), std::byte{0});
    }
    const std::span<const std::byte> zeros(zeroBuffer_.data(),
                                           static_cast<std::size_t>(extentBytes));
    for (const metadata::ExtentDescriptor &descriptor : descriptors) {
        SyntheticExtentMap map;
        for (std::uint64_t i = 0; i < descriptor.count; ++i) {
            map.add(i * extentBytes, extentBytes,
                    (descriptor.physicalExtent + i) * extentBytes, descriptor.deviceId);
        }
        for (std::uint64_t i = 0; i < descriptor.count; ++i) {
            auto written = path_.write(map, zeros, i * extentBytes, extentBytes, 0);
            if (!written.ok()) {
                return written;
            }
        }
    }
    return Result<void>(Status::Ok);
}

/* Zeroes one byte range inside a single physical extent. Partial edge blocks
 * take the RMW path so bytes outside the range are preserved. */
Result<void> DataPlane::zeroRange(std::uint32_t deviceId, std::uint64_t physicalExtent,
                                  std::uint64_t byteOffset, std::uint64_t bytes,
                                  std::uint64_t extentBytes) {
    if (bytes == 0) {
        return Result<void>(Status::Ok);
    }
    if (extentBytes == 0 || byteOffset > extentBytes || bytes > extentBytes - byteOffset) {
        return makeError(Status::InvalidArgument, "zero range exceeds the extent");
    }
    if (zeroBuffer_.size() < extentBytes) {
        zeroBuffer_.assign(static_cast<std::size_t>(extentBytes), std::byte{0});
    }
    SyntheticExtentMap map;
    map.add(byteOffset, bytes, physicalExtent * extentBytes + byteOffset, deviceId);
    const std::span<const std::byte> zeros(zeroBuffer_.data(),
                                           static_cast<std::size_t>(bytes));
    /* knownEof = ~0 disables the append fast path: the zero range must merge
     * with existing bytes instead of zero-filling whole blocks. */
    return path_.write(map, zeros, byteOffset, bytes, ~0ULL);
}

Result<void> DataPlane::read(const metadata::InodeExtentMap &map, std::span<std::byte> buffer,
                             std::uint64_t offset, std::uint64_t length) {
    return path_.read(map, buffer, offset, length);
}

Result<void> DataPlane::write(const metadata::InodeExtentMap &map,
                              std::span<const std::byte> buffer, std::uint64_t offset,
                              std::uint64_t length, std::uint64_t knownEof) {
    return path_.write(map, buffer, offset, length, knownEof);
}

} // namespace hfs
