#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "core/dma_arena.hpp"
#include "core/extent_map.hpp"
#include "core/io_planner.hpp"
#include "core/parent_completion.hpp"
#include "test_support.hpp"

using namespace hfs;

namespace {

void *memAlloc(std::size_t size, void *ctx) {
    (void)ctx;
    return ::aligned_alloc(4096, size);
}

void memFree(void *ptr, void *ctx) {
    (void)ctx;
    ::free(ptr);
}

struct ParentProbe {
    std::atomic<int> calls{0};
    int status = 0;
    std::uint64_t bytes = 0;
};

void parentDone(void *ctx, int status, std::uint64_t bytes) {
    auto *probe = static_cast<ParentProbe *>(ctx);
    probe->calls.fetch_add(1, std::memory_order_relaxed);
    probe->status = status;
    probe->bytes = bytes;
}

IoPlanner::Config plannerConfig(std::uint32_t maxChildBytes = 1U << 20,
                                std::uint32_t maxChildren = 64) {
    IoPlanner::Config config;
    config.blockSize = 4096;
    config.maxChildBytes = maxChildBytes;
    config.maxChildren = maxChildren;
    return config;
}

} // namespace

int main() {
    /* 1. extent map clipping preserves byte-exact physical offsets */
    {
        SyntheticExtentMap map;
        map.add(0, 8192, 4096);
        map.add(8192, 4096, 8192);
        std::array<ExtentView, 4> views{};
        auto lookup = map.lookup(4096, 8192, views);
        HFS_TEST_CHECK(lookup.ok() && lookup.value() == 2);
        HFS_TEST_CHECK(views[0].logicalOffset == 4096 && views[0].length == 4096);
        HFS_TEST_CHECK(views[0].physicalByte == 4096 + 4096);
        HFS_TEST_CHECK(views[1].logicalOffset == 8192 && views[1].length == 4096);
        HFS_TEST_CHECK(views[1].physicalByte == 8192);
        auto outOfRange = map.lookup(16384, 4096, views);
        HFS_TEST_CHECK(outOfRange.ok() && outOfRange.value() == 0);
    }

    /* 2. aligned read plans one direct child with matching geometry */
    {
        SyntheticExtentMap map;
        map.add(0, 16384, 4096);
        IoPlanner planner(plannerConfig());
        auto planned = planner.plan(map, 0, 8192, false);
        HFS_TEST_CHECK(planned.ok());
        const auto &plan = planned.value();
        HFS_TEST_CHECK(plan.count == 1 && plan.coveredBytes == 8192);
        const auto &child = plan.children[0];
        HFS_TEST_CHECK(child.kind == IoPlanner::Child::Kind::Direct);
        HFS_TEST_CHECK(child.physicalBlock == 1 && child.spanBytes == 8192);
        HFS_TEST_CHECK(child.bufferOffset == 0 && child.spanOffset == 0);
    }

    /* 3. unaligned I/O splits into head/tail RMW children */
    {
        SyntheticExtentMap map;
        map.add(0, 16384, 4096);
        IoPlanner planner(plannerConfig());
        for (const bool write : {false, true}) {
            auto planned = planner.plan(map, 1000, 5000, write);
            HFS_TEST_CHECK(planned.ok());
            const auto &plan = planned.value();
            HFS_TEST_CHECK(plan.count == 2 && plan.coveredBytes == 5000);
            HFS_TEST_CHECK(plan.children[0].kind == IoPlanner::Child::Kind::HeadRmw);
            HFS_TEST_CHECK(plan.children[0].bytes == 3096 && plan.children[0].spanBytes == 4096);
            HFS_TEST_CHECK(plan.children[0].physicalBlock == 1 &&
                           plan.children[0].spanOffset == 1000);
            HFS_TEST_CHECK(plan.children[1].kind == IoPlanner::Child::Kind::TailRmw);
            HFS_TEST_CHECK(plan.children[1].bytes == 1904 && plan.children[1].spanOffset == 0);
            HFS_TEST_CHECK(plan.children[1].physicalBlock == 2);
            HFS_TEST_CHECK(plan.children[1].bufferOffset == 3096);
        }
    }

    /* 4. multi-extent plans keep per-child physical and buffer offsets */
    {
        SyntheticExtentMap map;
        map.add(0, 4096, 0);
        map.add(4096, 4096, 8192);
        IoPlanner planner(plannerConfig());
        auto planned = planner.plan(map, 0, 8192, false);
        HFS_TEST_CHECK(planned.ok());
        const auto &plan = planned.value();
        HFS_TEST_CHECK(plan.count == 2);
        HFS_TEST_CHECK(plan.children[0].physicalBlock == 0 && plan.children[0].bufferOffset == 0);
        HFS_TEST_CHECK(plan.children[1].physicalBlock == 2 &&
                       plan.children[1].bufferOffset == 4096);
    }

    /* 5. child chunking honors maxChildBytes and maxChildren */
    {
        SyntheticExtentMap map;
        map.add(0, 32768, 0);
        IoPlanner planner(plannerConfig(4096));
        auto planned = planner.plan(map, 0, 32768, false);
        HFS_TEST_CHECK(planned.ok());
        HFS_TEST_CHECK(planned.value().count == 8);
        for (std::uint32_t i = 0; i < planned.value().count; ++i) {
            HFS_TEST_CHECK(planned.value().children[i].spanBytes == 4096);
        }
        IoPlanner limited(plannerConfig(4096, 2));
        auto overflow = limited.plan(map, 0, 32768, false);
        HFS_TEST_CHECK(!overflow.ok());
        HFS_TEST_CHECK(overflow.status() == Status::NoMemory);
    }

    /* 6. holes and empty ranges are rejected */
    {
        SyntheticExtentMap map;
        map.add(0, 4096, 0);
        map.add(8192, 4096, 8192);
        IoPlanner planner(plannerConfig());
        auto hole = planner.plan(map, 0, 8192, false);
        HFS_TEST_CHECK(!hole.ok() && hole.status() == Status::NotFound);
        auto empty = planner.plan(map, 0, 0, false);
        HFS_TEST_CHECK(!empty.ok() && empty.status() == Status::InvalidArgument);
    }

    /* 7. DMA arena classes, backpressure, and reuse accounting */
    {
        DmaArena arena;
        arena.configure(8192, memAlloc, memFree, nullptr);
        auto first = arena.acquire(100, 0);
        HFS_TEST_CHECK(first.ok() && first.value().size() == 4096);
        auto second = arena.acquire(4096, 0);
        HFS_TEST_CHECK(second.ok());
        auto exhausted = arena.acquire(4096, 0);
        HFS_TEST_CHECK(!exhausted.ok() && exhausted.status() == Status::Busy);
        {
            auto lease = BufferLease::fromArena(arena, 4096, 0);
            HFS_TEST_CHECK(!lease.ok() && lease.status() == Status::Busy);
        }
        arena.release(second.value());
        auto reused = arena.acquire(4096, 0);
        HFS_TEST_CHECK(reused.ok());
        arena.release(first.value());
        arena.release(reused.value());
        const auto stats = arena.stats();
        HFS_TEST_CHECK(stats.acquires == 3 && stats.releases == 3);
        /* bytes tracks arena-retained memory (cap budget), not in-flight use */
        HFS_TEST_CHECK(stats.bytes == 8192 && stats.highWatermark == 8192);
        HFS_TEST_CHECK(stats.busy >= 2);

        auto oversized = arena.acquire(4U << 20, 0);
        HFS_TEST_CHECK(!oversized.ok() && oversized.status() == Status::InvalidArgument);
    }

    /* 8. BufferLease returns arena slices and ignores caller memory */
    {
        DmaArena arena;
        arena.configure(4096, memAlloc, memFree, nullptr);
        {
            auto lease = BufferLease::fromArena(arena, 2048, 0);
            HFS_TEST_CHECK(lease.ok() && lease.value().fromArena());
            HFS_TEST_CHECK(lease.value().span().size() == 4096);
            std::memset(lease.value().span().data(), 0x5A, 4096);
        }
        HFS_TEST_CHECK(arena.stats().releases == 1);

        std::array<std::byte, 4096> caller{};
        auto callerLease = BufferLease::fromCaller(caller);
        HFS_TEST_CHECK(callerLease.ok() && !callerLease.value().fromArena());
        HFS_TEST_CHECK(arena.stats().releases == 1);
    }

    /* 9. parent completion aggregates concurrently issued children */
    {
        ParentCompletion parent;
        ParentProbe probe;
        parent.init(4, parentDone, &probe);
        HFS_TEST_CHECK(parent.pending() == 4);
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&parent, i]() {
                if (i == 2) {
                    parent.childDone(-5, 0);
                } else {
                    parent.childDone(0, 1024);
                }
            });
        }
        for (auto &thread : threads) {
            thread.join();
        }
        HFS_TEST_CHECK(parent.done());
        HFS_TEST_CHECK(parent.pending() == 0);
        HFS_TEST_CHECK(parent.firstError() == -5);
        HFS_TEST_CHECK(parent.completedBytes() == 3072);
        HFS_TEST_CHECK(probe.calls.load() == 1 && probe.status == -5 && probe.bytes == 3072);
    }

    std::printf("io_planner_test: ok\n");
    return 0;
}
