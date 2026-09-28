#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include <spdk/env.h>
#include <spdk/memory.h>

#include "common/test_hooks.hpp"
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
        HFS_TEST_CHECK(path.write(map, zeros, 0, zeros.size(), ~0ULL).ok());
        HFS_TEST_CHECK(path.write(map, zeros, zeros.size(), zeros.size(), ~0ULL).ok());

        /* 2. aligned write/read round-trip through bounce buffers */
        std::vector<std::byte> pattern(3 * metaBlock);
        fillPattern(pattern, 0x11);
        HFS_TEST_CHECK(path.write(map, pattern, 0, pattern.size(), ~0ULL).ok());
        std::vector<std::byte> readBack(pattern.size(), std::byte{0xFF});
        HFS_TEST_CHECK(path.read(map, readBack, 0, readBack.size()).ok());
        HFS_TEST_CHECK(readBack == pattern);

        /* 3. unaligned write/read exercises head/tail RMW children */
        std::vector<std::byte> unaligned(5000);
        fillPattern(unaligned, 0x22);
        const std::uint64_t unalignedOffset = 3 * metaBlock - 1000;
        test_hooks::setDataIoDelay(test_hooks::DataIoDelayKind::Write, 25);
        const auto delayedWriteStart = std::chrono::steady_clock::now();
        const auto delayedWrite =
            path.write(map, unaligned, unalignedOffset, unaligned.size(), ~0ULL);
        const auto delayedWriteElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - delayedWriteStart);
        test_hooks::setDataIoDelay(test_hooks::DataIoDelayKind::Write, 0);
        HFS_TEST_CHECK(delayedWrite.ok());
        /* The second RMW phase must be fully reaped before DataPath returns. */
        HFS_TEST_CHECK(delayedWriteElapsed.count() >= 20);
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
            HFS_TEST_CHECK(path.write(sparse, firstBlock, 0, firstBlock.size(), ~0ULL).ok());
            HFS_TEST_CHECK(path.write(sparse, secondBlock, 2 * metaBlock,
                                      secondBlock.size(), ~0ULL).ok());
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

        /* 3c. append fast path: blocks at or beyond the pre-write EOF skip the
         * refresh read, so two children cost two submits instead of three. */
        {
            const std::uint64_t appendBase = physicalByte + (32ULL << 20);
            SyntheticExtentMap appendMap;
            appendMap.add(0, 2 * metaBlock, appendBase);
            std::vector<std::byte> appendBytes(metaBlock + 100);
            fillPattern(appendBytes, 0x77);
            const auto beforeAppend = path.stats();
            HFS_TEST_CHECK(
                path.write(appendMap, appendBytes, 0, appendBytes.size(), 0).ok());
            const auto afterAppend = path.stats();
            HFS_TEST_CHECK(afterAppend.submits - beforeAppend.submits == 2);
            HFS_TEST_CHECK(afterAppend.completes - beforeAppend.completes == 2);
            std::vector<std::byte> appendRead(appendBytes.size(), std::byte{0});
            HFS_TEST_CHECK(path.read(appendMap, appendRead, 0, appendRead.size()).ok());
            HFS_TEST_CHECK(appendRead == appendBytes);
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
                                  0, leaseSpan.size(), ~0ULL).ok());
        HFS_TEST_CHECK(path.read(map, leaseSpan, 0, leaseSpan.size()).ok());
        const auto afterDirect = path.stats();
        HFS_TEST_CHECK(afterDirect.directChildren > beforeDirect.directChildren);
        HFS_TEST_CHECK(afterDirect.copyBytes == beforeDirect.copyBytes);
        lease.value().reset();

        /* 4b. capability probe: can plain (non-hugepage) heap memory be
         * registered for device DMA under the active env driver? */
        {
            void *heap = nullptr;
            if (::posix_memalign(&heap, metaBlock, metaBlock) == 0 && heap != nullptr) {
                std::memset(heap, 0, metaBlock);
                const int mlockRc = ::mlock(heap, metaBlock);
                const int registerRc = spdk_mem_register(heap, metaBlock);
                std::uint64_t translated = metaBlock;
                const std::uint64_t physical = spdk_vtophys(heap, &translated);
                const bool dmaCapable = physical != SPDK_VTOPHYS_ERROR;
                std::printf("dma-register probe: mlock=%d register=%d vtophys_ok=%d\n",
                            mlockRc, registerRc, dmaCapable ? 1 : 0);
                if (registerRc == 0 && dmaCapable) {
                    const std::uint64_t probeOffset = (4ULL << 20) - metaBlock;
                    std::vector<std::byte> probePattern(metaBlock);
                    fillPattern(probePattern, 0x9B);
                    std::span<std::byte> heapSpan(static_cast<std::byte *>(heap), metaBlock);
                    HFS_TEST_CHECK(path.write(map, std::span<const std::byte>(probePattern),
                                              probeOffset, metaBlock, ~0ULL)
                                       .ok());
                    const auto beforeHeap = path.stats();
                    HFS_TEST_CHECK(path.read(map, heapSpan, probeOffset, metaBlock).ok());
                    const auto afterHeap = path.stats();
                    std::printf("dma-register probe: direct_delta=%llu copy_delta=%llu\n",
                                static_cast<unsigned long long>(afterHeap.directChildren -
                                                                beforeHeap.directChildren),
                                static_cast<unsigned long long>(afterHeap.copyBytes -
                                                                beforeHeap.copyBytes));
                    HFS_TEST_CHECK(std::memcmp(heap, probePattern.data(), metaBlock) == 0);
                }
                if (registerRc == 0) {
                    spdk_mem_unregister(heap, metaBlock);
                }
                if (mlockRc == 0) {
                    ::munlock(heap, metaBlock);
                }
                ::free(heap);
            }
        }

        /* 5. large I/O is chunked into at most maxChildBytes children */
        std::vector<std::byte> large(3U << 20);
        fillPattern(large, 0x44);
        const auto beforeLarge = path.stats();
        HFS_TEST_CHECK(path.write(map, large, 0, large.size(), ~0ULL).ok());
        const auto afterLarge = path.stats();
        HFS_TEST_CHECK(afterLarge.submits - beforeLarge.submits == 3);
        HFS_TEST_CHECK(afterLarge.completes - beforeLarge.completes == 3);
        std::vector<std::byte> largeBack(large.size(), std::byte{0});
        HFS_TEST_CHECK(path.read(map, largeBack, 0, largeBack.size()).ok());
        HFS_TEST_CHECK(checksum(largeBack) == checksum(large));

        /* A late child completion must become TimedOut at the parent boundary
         * while still draining before the buffers are released. */
        DataPath::Config timeoutConfig = pathConfig;
        timeoutConfig.timeoutMs = 1;
        DataPath timeoutPath(runtime, arena, plannerConfig, timeoutConfig);
        std::vector<std::byte> timeoutRead(metaBlock, std::byte{0});
        test_hooks::setDataIoDelay(test_hooks::DataIoDelayKind::Generic, 25);
        auto timeoutResult = timeoutPath.read(map, timeoutRead, 0, timeoutRead.size());
        test_hooks::setDataIoDelay(test_hooks::DataIoDelayKind::Generic, 0);
        HFS_TEST_CHECK(!timeoutResult.ok() && timeoutResult.status() == Status::TimedOut);
        HFS_TEST_CHECK(timeoutPath.stats().submits == timeoutPath.stats().completes);

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
