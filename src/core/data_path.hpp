#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "core/dma_arena.hpp"
#include "core/extent_map.hpp"
#include "core/io_planner.hpp"
#include "hfs/types.hpp"

namespace hfs {

class SpdkRuntime;

/* Synchronous driver over the async data-reactor path (tests/tools). The real
 * frontends drive the same primitives asynchronously in later shards. */
class DataPath {
public:
    struct Config {
        std::size_t deviceIndex = 0;
        std::uint32_t reactorIndex = 0;
        std::uint32_t timeoutMs = 5000;
    };

    struct Stats {
        std::uint64_t submits = 0;
        std::uint64_t completes = 0;
        std::uint64_t directChildren = 0;
        std::uint64_t bounceChildren = 0;
        std::uint64_t zeroChildren = 0;
        std::uint64_t copyBytes = 0;
        std::uint64_t zeroBytes = 0;
        std::uint64_t parentWaits = 0;
        std::uint64_t errors = 0;
    };

    DataPath(SpdkRuntime &runtime, DmaArena &arena, IoPlanner::Config plannerConfig,
             Config config);

    Result<void> read(const ExtentMap &map, std::span<std::byte> buffer,
                      std::uint64_t offset, std::uint64_t length);
    Result<void> write(const ExtentMap &map, std::span<const std::byte> buffer,
                       std::uint64_t offset, std::uint64_t length);
    Stats stats() const noexcept;

private:
    Result<void> execute(const ExtentMap &map, std::span<std::byte> readBuffer,
                         std::span<const std::byte> writeBuffer, std::uint64_t offset,
                         std::uint64_t length, bool write);

    SpdkRuntime &runtime_;
    DmaArena &arena_;
    IoPlanner planner_;
    IoPlanner::Config plannerConfig_;
    Config config_;
    Stats stats_;
    std::vector<std::uint32_t> deviceBlockBytes_;   /* per-device sector size */
    std::vector<std::uint32_t> deviceBlocksPerMeta_; /* sectors per metadata block */
};

} // namespace hfs
