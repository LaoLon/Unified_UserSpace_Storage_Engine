#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

#include <spdk/env.h>

#include "core/data_path.hpp"
#include "core/dma_arena.hpp"
#include "core/extent_map.hpp"
#include "core/io_planner.hpp"
#include "hfs/config.hpp"
#include "hfs/core.hpp"
#include "runtime/spdk_runtime.hpp"
#include "test_support.hpp"

using namespace hfs;

namespace {

constexpr std::uint32_t kBlockSize = 4096;

void *dmaAlloc(std::size_t size, void *ctx) {
    (void)ctx;
    return spdk_dma_zmalloc(size, kBlockSize, nullptr);
}

void dmaFree(void *ptr, void *ctx) {
    (void)ctx;
    spdk_dma_free(ptr);
}

std::uint64_t checksum(std::span<const std::byte> bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::byte byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

void fillPattern(std::span<std::byte> bytes, std::uint8_t seed) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = std::byte{static_cast<std::uint8_t>(seed + i * 7)};
    }
}

} // namespace

int main() {
    const std::string lockPath =
        "/tmp/hfs2-datapath-" + std::to_string(::getpid()) + ".lock";
    auto parsed = parseConfig(validConfigText(lockPath), "data_path_device_test");
    HFS_TEST_CHECK(parsed.ok());

    {
        auto created = Engine::create(parsed.value());
        HFS_TEST_CHECK(created.ok());
        auto engine = std::move(created).value();
        HFS_TEST_CHECK(engine->format({true}).wait().value().ok());
        HFS_TEST_CHECK(engine->mount().wait().value().ok());

        const auto geometry = engine->deviceGeometries()[0];
        const std::uint32_t metaBlock = engine->config().metadataBlockSize;
        HFS_TEST_CHECK(metaBlock == kBlockSize);
        /* 2 MiB into the data region, 4 MiB synthetic extent. */
        const std::uint64_t physicalByte = (geometry.dataStart + 512) * metaBlock;

        SyntheticExtentMap map;
        map.add(0, 4ULL << 20, physicalByte);

        DmaArena arena;
        arena.configure(16ULL << 20, dmaAlloc, dmaFree, nullptr);

        SpdkRuntime &runtime = SpdkRuntime::instance();
        HFS_TEST_CHECK(runtime.active());
        HFS_TEST_CHECK(runtime.dataReactorCount() > 0);

        IoPlanner::Config plannerConfig;
        plannerConfig.blockSize = metaBlock;
        plannerConfig.maxChildBytes = 1U << 20;
        DataPath::Config pathConfig;
        pathConfig.deviceIndex = 0;
        pathConfig.reactorIndex = 0;
        pathConfig.timeoutMs = 10000;
        DataPath path(runtime, arena, plannerConfig, pathConfig);

        /* 1. zero-fill the extent so RMW reads see defined content */
        std::vector<std::byte> zeros(2ULL << 20, std::byte{0});
        HFS_TEST_CHECK(path.write(map, zeros, 0, zeros.size()).ok());
        HFS_TEST_CHECK(path.write(map, zeros, zeros.size(), zeros.size()).ok());

        /* 2. aligned write/read round-trip through bounce buffers */
        std::vector<std::byte> pattern(3 * metaBlock);
        fillPattern(pattern, 0x11);
        HFS_TEST_CHECK(path.write(map, pattern, 0, pattern.size()).ok());
        std::vector<std::byte> readBack(pattern.size(), std::byte{0xFF});
        HFS_TEST_CHECK(path.read(map, readBack, 0, readBack.size()).ok());
        HFS_TEST_CHECK(readBack == pattern);

        /* 3. unaligned write/read exercises head/tail RMW children */
        std::vector<std::byte> unaligned(5000);
        fillPattern(unaligned, 0x22);
        const std::uint64_t unalignedOffset = 3 * metaBlock - 1000;
        HFS_TEST_CHECK(path.write(map, unaligned, unalignedOffset, unaligned.size()).ok());
        std::vector<std::byte> unalignedRead(unaligned.size(), std::byte{0});
        HFS_TEST_CHECK(
            path.read(map, unalignedRead, unalignedOffset, unalignedRead.size()).ok());
        HFS_TEST_CHECK(unalignedRead == unaligned);

        std::vector<std::byte> window(3 * metaBlock, std::byte{0});
        HFS_TEST_CHECK(path.read(map, window, 2 * metaBlock, window.size()).ok());
        /* step 2 pattern still covers block 2; the unaligned write overlays it */
        std::vector<std::byte> expected(window.size(), std::byte{0});
        std::memcpy(expected.data(), pattern.data() + 2 * metaBlock, metaBlock);
        std::memcpy(expected.data() + (unalignedOffset - 2 * metaBlock), unaligned.data(),
                    unaligned.size());
        HFS_TEST_CHECK(window == expected);

        /* 3b. sparse read: unmapped ranges read as zeros */
        {
            const std::uint64_t sparseBase = physicalByte + (16ULL << 20);
            SyntheticExtentMap sparse;
            sparse.add(0, metaBlock, sparseBase);
            sparse.add(2 * metaBlock, metaBlock, sparseBase + 2 * metaBlock);
            std::vector<std::byte> firstBlock(metaBlock);
            std::vector<std::byte> secondBlock(metaBlock);
            fillPattern(firstBlock, 0x55);
            fillPattern(secondBlock, 0x66);
            HFS_TEST_CHECK(path.write(sparse, firstBlock, 0, firstBlock.size()).ok());
            HFS_TEST_CHECK(path.write(sparse, secondBlock, 2 * metaBlock,
                                      secondBlock.size()).ok());
            std::vector<std::byte> sparseRead(3 * metaBlock, std::byte{0xFF});
            const auto beforeSparse = path.stats();
            HFS_TEST_CHECK(path.read(sparse, sparseRead, 0, sparseRead.size()).ok());
            const auto afterSparse = path.stats();
            HFS_TEST_CHECK(afterSparse.zeroChildren > beforeSparse.zeroChildren);
            HFS_TEST_CHECK(afterSparse.zeroBytes - beforeSparse.zeroBytes == metaBlock);
            HFS_TEST_CHECK(std::memcmp(sparseRead.data(), firstBlock.data(), metaBlock) == 0);
            for (std::size_t i = metaBlock; i < 2 * metaBlock; ++i) {
                HFS_TEST_CHECK(sparseRead[i] == std::byte{0});
            }
            HFS_TEST_CHECK(
                std::memcmp(sparseRead.data() + 2 * metaBlock, secondBlock.data(),
                            metaBlock) == 0);
        }

        /* 4. DMA-capable caller buffer takes the direct path without copies */
        auto lease = BufferLease::fromArena(arena, 1U << 20, 1000);
        HFS_TEST_CHECK(lease.ok());
        auto leaseSpan = lease.value().span().first(1U << 20);
        HFS_TEST_CHECK(runtime.isDmaCapable(leaseSpan.data(), leaseSpan.size()));
        fillPattern(leaseSpan, 0x33);
        const auto beforeDirect = path.stats();
        HFS_TEST_CHECK(path.write(map, std::span<const std::byte>(leaseSpan.data(),
                                                                  leaseSpan.size()),
                                  0, leaseSpan.size()).ok());
        HFS_TEST_CHECK(path.read(map, leaseSpan, 0, leaseSpan.size()).ok());
        const auto afterDirect = path.stats();
        HFS_TEST_CHECK(afterDirect.directChildren > beforeDirect.directChildren);
        HFS_TEST_CHECK(afterDirect.copyBytes == beforeDirect.copyBytes);
        lease.value().reset();

        /* 5. large I/O is chunked into at most maxChildBytes children */
        std::vector<std::byte> large(3U << 20);
        fillPattern(large, 0x44);
        const auto beforeLarge = path.stats();
        HFS_TEST_CHECK(path.write(map, large, 0, large.size()).ok());
        const auto afterLarge = path.stats();
        HFS_TEST_CHECK(afterLarge.submits - beforeLarge.submits == 3);
        HFS_TEST_CHECK(afterLarge.completes - beforeLarge.completes == 3);
        std::vector<std::byte> largeBack(large.size(), std::byte{0});
        HFS_TEST_CHECK(path.read(map, largeBack, 0, largeBack.size()).ok());
        HFS_TEST_CHECK(checksum(largeBack) == checksum(large));

        /* 6. child accounting and error-free completion */
        const auto stats = path.stats();
        HFS_TEST_CHECK(stats.submits == stats.completes);
        HFS_TEST_CHECK(stats.bounceChildren > 0);
        HFS_TEST_CHECK(stats.copyBytes > 0);
        HFS_TEST_CHECK(stats.parentWaits > 0);
        HFS_TEST_CHECK(stats.errors == 0);
        const auto arenaStats = arena.stats();
        HFS_TEST_CHECK(arenaStats.acquires == arenaStats.releases);
        std::printf("datapath stats: submits=%llu direct=%llu bounce=%llu copy=%llu\n",
                    static_cast<unsigned long long>(stats.submits),
                    static_cast<unsigned long long>(stats.directChildren),
                    static_cast<unsigned long long>(stats.bounceChildren),
                    static_cast<unsigned long long>(stats.copyBytes));

        HFS_TEST_CHECK(engine->stop().wait().value().ok());
    }

    /* restore a clean formatted image for the other device tests */
    {
        auto created = Engine::create(parsed.value());
        HFS_TEST_CHECK(created.ok());
        auto engine = std::move(created).value();
        HFS_TEST_CHECK(engine->format({true}).wait().value().ok());
        HFS_TEST_CHECK(engine->stop().wait().value().ok());
    }

    ::unlink(lockPath.c_str());
    std::printf("data_path_device_test: ok\n");
    return 0;
}
