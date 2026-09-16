#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "common/trace.hpp"
#include "core/actor.hpp"
#include "core/completion_queue.hpp"
#include "core/frame_pool.hpp"
#include "core/operation.hpp"
#include "core/shard_ring.hpp"
#include "hfs/task.hpp"
#include "test_support.hpp"

namespace {

/* noinline + runtime value keeps the coroutine frame real (no elision). */
__attribute__((noinline)) hfs::Task<int> answer(int value) { co_return value; }

int gCompletions = 0;
void onOpComplete(hfs::Operation *operation) {
    (void)operation;
    ++gCompletions;
}

} // namespace

int main() {
    /* 1. frame pool: bounded classes, reuse, overflow accounting */
    {
        auto &pool = hfs::FramePool::instance();
        pool.setCapacity(64 * 1024);
        std::vector<void *> blocks;
        for (int i = 0; i < 1000; ++i) {
            void *block = pool.allocate(200);
            HFS_TEST_CHECK(block != nullptr);
            blocks.push_back(block);
        }
        const auto stats = pool.stats();
        HFS_TEST_CHECK(stats.overflowAllocations > 0);
        HFS_TEST_CHECK(stats.poolBytes <= stats.capacityBytes);
        for (void *block : blocks) {
            pool.deallocate(block, 200);
        }
        HFS_TEST_CHECK(pool.stats().freeBlocks[1] > 0);
        void *reused = pool.allocate(200);
        HFS_TEST_CHECK(reused != nullptr);
        pool.deallocate(reused, 200);
        HFS_TEST_CHECK(pool.hasBudget(256));
    }

    /* 2. Task frames come from the pool and are returned */
    {
        auto &pool = hfs::FramePool::instance();
        const auto before = pool.stats();
        {
            auto task = answer(42);
            auto result = task.wait();
            HFS_TEST_CHECK(result.ok() && result.value() == 42);
        }
        const auto after = pool.stats();
        HFS_TEST_CHECK((after.allocations + after.overflowAllocations) >
                       (before.allocations + before.overflowAllocations));
        HFS_TEST_CHECK((after.deallocations + after.overflowAllocations) >
                       (before.deallocations + before.overflowAllocations));
    }

    /* 3. operation lifecycle and single terminal transition */
    {
        hfs::Operation::setPoolCapacity(4);
        gCompletions = 0;
        auto *operation = hfs::Operation::create(11);
        HFS_TEST_CHECK(operation != nullptr);
        HFS_TEST_CHECK(operation->requestId == 11);
        operation->onComplete = onOpComplete;
        operation->addRef();
        operation->release();
        HFS_TEST_CHECK(operation->complete(0));
        HFS_TEST_CHECK(!operation->complete(0));
        HFS_TEST_CHECK(gCompletions == 1);
        operation->release();
        HFS_TEST_CHECK(hfs::Operation::liveCount() == 0);

        const std::size_t overflowBefore = hfs::Operation::poolOverflow();
        std::vector<hfs::Operation *> held;
        for (int i = 0; i < 8; ++i) {
            auto *created = hfs::Operation::create(100 + i);
            if (created != nullptr) {
                held.push_back(created);
            }
        }
        HFS_TEST_CHECK(held.size() == 4);
        HFS_TEST_CHECK(hfs::Operation::poolOverflow() > overflowBefore);
        for (auto *item : held) {
            item->release();
        }
        HFS_TEST_CHECK(hfs::Operation::liveCount() == 0);
    }

    /* 4. ring order, full/busy and doorbell dedup */
    {
        hfs::Operation::setPoolCapacity(32);
        hfs::ShardRing ring(8);
        std::vector<hfs::Operation *> ops;
        for (int i = 0; i < 8; ++i) {
            auto *operation = hfs::Operation::create(static_cast<std::uint64_t>(i));
            HFS_TEST_CHECK(operation != nullptr);
            ops.push_back(operation);
            auto reservation = ring.reserve();
            HFS_TEST_CHECK(reservation.valid());
            ring.publish(reservation, operation);
        }
        auto full = ring.reserve();
        HFS_TEST_CHECK(!full.valid());
        HFS_TEST_CHECK(ring.stats().busy == 1);
        HFS_TEST_CHECK(ring.stats().doorbells == 1); /* armed once for the burst */
        HFS_TEST_CHECK(ring.doorbellArmed());
        std::vector<hfs::Operation *> out(8);
        HFS_TEST_CHECK(ring.drain(out.data(), out.size()) == 8);
        for (int i = 0; i < 8; ++i) {
            HFS_TEST_CHECK(out[i] == ops[i]);
        }
        HFS_TEST_CHECK(ring.empty());
        ring.clearDoorbell();
        HFS_TEST_CHECK(!ring.doorbellArmed());
        for (auto *operation : ops) {
            operation->release();
        }
    }

    /* 5. multi-producer -> actor -> completion queue */
    {
        constexpr int kProducers = 4;
        constexpr int kPerProducer = 500;
        constexpr int kTotal = kProducers * kPerProducer;
        hfs::Operation::setPoolCapacity(4096);
        hfs::ShardRing ring(1024);
        hfs::CompletionQueue completions(2048);
        const std::size_t overflowBefore = hfs::Operation::poolOverflow();

        struct StepCtx {
            hfs::CompletionQueue *completions = nullptr;
        };
        StepCtx stepCtx{&completions};
        hfs::ShardActor actor(
            ring,
            [](void *raw, hfs::Operation *operation) {
                auto *ctx = static_cast<StepCtx *>(raw);
                hfs::CompletionQueue::Event event;
                event.requestId = operation->requestId;
                event.userCtx = operation->userCtx;
                event.status = 0;
                ctx->completions->push(event);
                operation->complete(0);
                operation->release();
            },
            &stepCtx, 64);

        hfs::trace::setEnabled(true);
        const std::uint64_t traceBefore = hfs::trace::recorded();
        std::atomic<bool> failure{false};
        std::vector<std::thread> producers;
        producers.reserve(kProducers);
        for (int producer = 0; producer < kProducers; ++producer) {
            producers.emplace_back([&ring, &failure, producer]() {
                for (int i = 0; i < kPerProducer; ++i) {
                    const std::uint64_t id =
                        static_cast<std::uint64_t>(producer * kPerProducer + i);
                    auto *operation = hfs::Operation::create(id);
                    if (operation == nullptr) {
                        failure.store(true);
                        return;
                    }
                    operation->userCtx =
                        reinterpret_cast<void *>(static_cast<std::uintptr_t>(id));
                    for (;;) {
                        auto reservation = ring.reserve();
                        if (reservation.valid()) {
                            ring.publish(reservation, operation);
                            break;
                        }
                        std::this_thread::yield();
                    }
                }
            });
        }

        std::vector<hfs::CompletionQueue::Event> events(64);
        std::vector<bool> seen(kTotal, false);
        std::size_t reaped = 0;
        while (reaped < static_cast<std::size_t>(kTotal) && !failure.load()) {
            actor.runOnce();
            const std::size_t count = completions.reap(events.data(), events.size());
            for (std::size_t i = 0; i < count; ++i) {
                const auto index = static_cast<std::uint64_t>(
                    reinterpret_cast<std::uintptr_t>(events[i].userCtx));
                if (index < seen.size()) {
                    seen[index] = true;
                }
                ++reaped;
            }
        }
        for (auto &thread : producers) {
            thread.join();
        }
        while (reaped < static_cast<std::size_t>(kTotal) && !failure.load()) {
            actor.runOnce();
            const std::size_t count = completions.reap(events.data(), events.size());
            for (std::size_t i = 0; i < count; ++i) {
                const auto index = static_cast<std::uint64_t>(
                    reinterpret_cast<std::uintptr_t>(events[i].userCtx));
                if (index < seen.size()) {
                    seen[index] = true;
                }
                ++reaped;
            }
        }

        HFS_TEST_CHECK(!failure.load());
        HFS_TEST_CHECK(reaped == static_cast<std::size_t>(kTotal));
        for (bool value : seen) {
            HFS_TEST_CHECK(value);
        }
        HFS_TEST_CHECK(ring.empty());
        HFS_TEST_CHECK(completions.empty());
        HFS_TEST_CHECK(completions.dropped() == 0);
        HFS_TEST_CHECK(hfs::Operation::liveCount() == 0);
        HFS_TEST_CHECK(hfs::Operation::poolOverflow() == overflowBefore);
        HFS_TEST_CHECK(hfs::trace::recorded() > traceBefore);
        hfs::trace::setEnabled(false);
    }

    /* 6. lost-wake window: publish after the actor cleared the doorbell */
    {
        hfs::Operation::setPoolCapacity(8);
        hfs::ShardRing ring(4);
        int processed = 0;
        hfs::ShardActor actor(
            ring,
            [](void *raw, hfs::Operation *operation) {
                ++(*static_cast<int *>(raw));
                operation->complete(0);
                operation->release();
            },
            &processed, 4);

        auto *first = hfs::Operation::create(1);
        HFS_TEST_CHECK(first != nullptr);
        auto reservation = ring.reserve();
        HFS_TEST_CHECK(reservation.valid());
        ring.publish(reservation, first);
        HFS_TEST_CHECK(actor.runOnce() == 1);
        HFS_TEST_CHECK(!ring.doorbellArmed());

        auto *second = hfs::Operation::create(2);
        HFS_TEST_CHECK(second != nullptr);
        reservation = ring.reserve();
        HFS_TEST_CHECK(reservation.valid());
        ring.publish(reservation, second);
        HFS_TEST_CHECK(ring.doorbellArmed());
        HFS_TEST_CHECK(actor.runOnce() == 1);
        HFS_TEST_CHECK(processed == 2);
        HFS_TEST_CHECK(hfs::Operation::liveCount() == 0);
    }

    std::printf("data_plane_test: ok\n");
    return 0;
}
