#pragma once

#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

#include "core/dma_arena.hpp"
#include "core/extent_map.hpp"
#include "core/io_planner.hpp"
#include "uuse/types.hpp"

namespace uuse {

class SpdkRuntime;

/* Synchronous driver over the async data-reactor path (tests/tools). The real
 * frontends drive the same primitives asynchronously in later shards. */
class DataPath {
public:
    struct Config {
        std::size_t deviceIndex = 0;
        std::uint32_t reactorIndex = 0;
        std::uint32_t timeoutMs = 5000;
        /* Native sector size used to plan writes; 0 keeps the metadata-block
         * planner default. Sector planning avoids read-modify-write when a
         * partial write is already aligned to the device. */
        std::uint32_t nativeBlockSize = 0;
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
    /* knownEof is the file's logical size before this write; device blocks that
     * start at or beyond it need no read-modify-write (append fast path). */
    Result<void> write(const ExtentMap &map, std::span<const std::byte> buffer,
                       std::uint64_t offset, std::uint64_t length,
                       std::uint64_t knownEof);
    Stats stats() const noexcept;

private:
    Result<void> execute(const ExtentMap &map, std::span<std::byte> readBuffer,
                         std::span<const std::byte> writeBuffer, std::uint64_t offset,
                         std::uint64_t length, bool write, std::uint64_t knownEof);

    SpdkRuntime &runtime_;
    DmaArena &arena_;
    IoPlanner planner_;
    IoPlanner::Config plannerConfig_;
    Config config_;
    /* Round-robin requests across the engine-owned data reactors. */
    std::atomic<std::uint32_t> nextReactor_{0};
    Stats stats_;
    std::vector<std::uint32_t> deviceBlockBytes_;   /* per-device sector size */
};

} // namespace uuse
