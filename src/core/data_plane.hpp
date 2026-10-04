#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/data_path.hpp"
#include "core/dma_arena.hpp"
#include "uuse/types.hpp"
#include "metadata/inode_map.hpp"

namespace uuse {

class SpdkRuntime;

/* Engine-owned data plane: shared DMA arena plus file I/O helpers over the
 * per-inode extent map (Shard-77). */
class DataPlane {
public:
    DataPlane(SpdkRuntime &runtime, DmaArena &arena, IoPlanner::Config plannerConfig,
              DataPath::Config pathConfig);

    DmaArena &arena() noexcept { return arena_; }
    DataPath &path() noexcept { return path_; }

    /* Writes zeros over reserved extents (device-absolute descriptors). */
    Result<void> zeroExtents(std::span<const metadata::ExtentDescriptor> descriptors,
                             std::uint64_t extentBytes);
    /* Zeroes a byte range inside one physical extent without clobbering bytes
     * outside the range (used to zero only the uncovered extent tails). */
    Result<void> zeroRange(std::uint32_t deviceId, std::uint64_t physicalExtent,
                           std::uint64_t byteOffset, std::uint64_t bytes,
                           std::uint64_t extentBytes);
    Result<void> read(const metadata::InodeExtentMap &map, std::span<std::byte> buffer,
                      std::uint64_t offset, std::uint64_t length);
    /* knownEof enables the append fast path: device blocks at or beyond the
     * pre-write EOF are written without a refresh read. */
    Result<void> write(const metadata::InodeExtentMap &map,
                       std::span<const std::byte> buffer, std::uint64_t offset,
                       std::uint64_t length, std::uint64_t knownEof);

private:
    DmaArena &arena_;
    DataPath path_;
    std::vector<std::byte> zeroBuffer_;
};

} // namespace uuse
