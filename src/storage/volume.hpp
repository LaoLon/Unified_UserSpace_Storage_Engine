#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "hfs/config.hpp"
#include "hfs/superblock.hpp"
#include "hfs/types.hpp"

namespace hfs {

class SpdkRuntime;

/* Volume-level metadata recovered from (or written by) the superblocks. */
struct VolumeInfo {
    std::array<std::uint8_t, 16> uuid{};
    std::uint64_t generation = 0;
    std::uint32_t metadataBlockSize = 0;
    std::uint32_t replicaCount = 1;
    bool cleanShutdown = true;
    std::vector<DeviceGeometry> devices;
};

/* Destructive format of the configured device set; confirm must be true. */
Result<VolumeInfo> formatVolume(SpdkRuntime &runtime, const Config &config, bool confirm);

/* Read and validate superblock A/B on every configured device. */
Result<VolumeInfo> mountVolume(SpdkRuntime &runtime, const Config &config);

/* Publish the clean-shutdown state to every configured device. */
Result<void> updateCleanShutdown(SpdkRuntime &runtime, const Config &config, bool clean);

} // namespace hfs
