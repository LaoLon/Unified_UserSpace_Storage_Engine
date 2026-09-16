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
            auto written = path_.write(map, zeros, i * extentBytes, extentBytes);
            if (!written.ok()) {
                return written;
            }
        }
    }
    return Result<void>(Status::Ok);
}

Result<void> DataPlane::read(const metadata::InodeExtentMap &map, std::span<std::byte> buffer,
                             std::uint64_t offset, std::uint64_t length) {
    return path_.read(map, buffer, offset, length);
}

Result<void> DataPlane::write(const metadata::InodeExtentMap &map,
                              std::span<const std::byte> buffer, std::uint64_t offset,
                              std::uint64_t length) {
    return path_.write(map, buffer, offset, length);
}

} // namespace hfs
