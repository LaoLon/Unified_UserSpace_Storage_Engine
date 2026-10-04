#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

#include "common/trace.hpp"
#include "core/actor.hpp"
#include "core/completion_queue.hpp"
#include "core/dma_arena.hpp"
#include "core/frame_pool.hpp"
#include "core/operation.hpp"
#include "core/shard_ring.hpp"
#include "uuse/task.hpp"
#include "test_support.hpp"

namespace {

/* noinline + runtime value keeps the coroutine frame real (no elision). */
__attribute__((noinline)) uuse::Task<int> answer(int value) { co_return value; }

int gCompletions = 0;
void onOpComplete(uuse::Operation *operation) {
    (void)operation;
    ++gCompletions;
}

/* Counts value completions without retaining an Operation or frontend object. */
void onCompletionPublished(void *raw, const uuse::CompletionEvent &event) {
    auto *count = static_cast<int *>(raw);
    ++(*count);
    if (event.requestId == 0) {
        *count = -1;
    }
}

/* Resets externally owned bounded sidecar storage when its Operation is reaped. */
void resetSidecarProbe(void *raw) {
    auto *value = static_cast<std::uint32_t *>(raw);
    *value = 0;
}

void *testDmaAlloc(std::size_t size, void *) noexcept {
    return ::operator new(size, std::nothrow);
}

void testDmaFree(void *ptr, void *) noexcept { ::operator delete(ptr); }

} // namespace

int main() {
    /* 0. large contiguous allocations use the bounded arena path. */
    {
        uuse::DmaArena arena;
        arena.configure(3 * 1024 * 1024, testDmaAlloc, testDmaFree, nullptr);
        auto large = arena.acquire(2 * 1024 * 1024, 0);
        UUSE_TEST_CHECK(large.ok());
        UUSE_TEST_CHECK(arena.owns(large.value().data()));
        auto over = arena.acquire(2 * 1024 * 1024, 0);
        UUSE_TEST_CHECK(!over.ok() && over.status() == uuse::Status::Busy);
        arena.release(large.value());
        UUSE_TEST_CHECK(arena.stats().bytes == 0);
    }

    /* 0b. bounded arena wait wakes on release and times out otherwise. */
    {
        uuse::DmaArena arena;
        arena.configure(4096, testDmaAlloc, testDmaFree, nullptr);
        auto held = arena.acquire(4096, 0);
        UUSE_TEST_CHECK(held.ok());
        std::atomic<bool> waiterOk = false;
        std::thread waiter([&] {
            auto acquired = arena.acquire(4096, 200);
            waiterOk.store(acquired.ok());
            if (acquired.ok()) arena.release(acquired.value());
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        arena.release(held.value());
        waiter.join();
        UUSE_TEST_CHECK(waiterOk.load());

        auto heldAgain = arena.acquire(4096, 0);
        UUSE_TEST_CHECK(heldAgain.ok());
        auto timedOut = arena.acquire(4096, 10);
        UUSE_TEST_CHECK(!timedOut.ok() && timedOut.status() == uuse::Status::Busy);
        arena.release(heldAgain.value());
    }

    /* 1. frame pool: bounded classes, reuse, overflow accounting */
    {
        auto &pool = uuse::FramePool::instance();
        pool.setCapacity(64 * 1024);
        std::vector<void *> blocks;
        for (int i = 0; i < 1000; ++i) {
            void *block = pool.allocate(200);
            UUSE_TEST_CHECK(block != nullptr);
            blocks.push_back(block);
        }
        const auto stats = pool.stats();
        UUSE_TEST_CHECK(stats.overflowAllocations > 0);
        UUSE_TEST_CHECK(stats.poolBytes <= stats.capacityBytes);
        for (void *block : blocks) {
            pool.deallocate(block, 200);
        }
        UUSE_TEST_CHECK(pool.stats().freeBlocks[1] > 0);
        void *reused = pool.allocate(200);
        UUSE_TEST_CHECK(reused != nullptr);
        pool.deallocate(reused, 200);
        UUSE_TEST_CHECK(pool.hasBudget(256));
    }

    /* 2. Task frames come from the pool and are returned */
    {
        auto &pool = uuse::FramePool::instance();
        const auto before = pool.stats();
        {
            auto task = answer(42);
            auto result = task.wait();
            UUSE_TEST_CHECK(result.ok() && result.value() == 42);
        }
        const auto after = pool.stats();
        UUSE_TEST_CHECK((after.allocations + after.overflowAllocations) >
                       (before.allocations + before.overflowAllocations));
        UUSE_TEST_CHECK((after.deallocations + after.overflowAllocations) >
                       (before.deallocations + before.overflowAllocations));
    }

    /* 3. operation lifecycle and single terminal transition */
    {
        uuse::Operation::setPoolCapacity(4);
        gCompletions = 0;
        auto *operation = uuse::Operation::create(11);
        UUSE_TEST_CHECK(operation != nullptr);
        UUSE_TEST_CHECK(operation->requestId == 11);
        operation->onComplete = onOpComplete;
        operation->addRef();
        operation->release();
        UUSE_TEST_CHECK(operation->complete(0));
        UUSE_TEST_CHECK(!operation->complete(0));
        UUSE_TEST_CHECK(gCompletions == 1);
        operation->release();
        UUSE_TEST_CHECK(uuse::Operation::liveCount() == 0);

        /* Role references close independently while sharing one underlying count. */
        auto *roleOperation = uuse::Operation::create(12);
        UUSE_TEST_CHECK(roleOperation != nullptr);
        UUSE_TEST_CHECK(roleOperation->refCount() == 1);
        UUSE_TEST_CHECK(roleOperation->refCount(uuse::OperationRefRole::Legacy) == 1);
        UUSE_TEST_CHECK(roleOperation->tryAcquireRef(uuse::OperationRefRole::Actor));
        roleOperation->acquireRef(uuse::OperationRefRole::Scheduler);
        roleOperation->acquireRef(uuse::OperationRefRole::Backend);
        roleOperation->acquireRef(uuse::OperationRefRole::Parent);
        UUSE_TEST_CHECK(roleOperation->refCount() == 5);
        UUSE_TEST_CHECK(roleOperation->refCount(uuse::OperationRefRole::Actor) == 1);
        UUSE_TEST_CHECK(roleOperation->releaseRef(uuse::OperationRefRole::Parent));
        UUSE_TEST_CHECK(roleOperation->releaseRef(uuse::OperationRefRole::Backend));
        UUSE_TEST_CHECK(roleOperation->releaseRef(uuse::OperationRefRole::Scheduler));
        UUSE_TEST_CHECK(roleOperation->releaseRef(uuse::OperationRefRole::Actor));
        UUSE_TEST_CHECK(!roleOperation->releaseRef(uuse::OperationRefRole::Actor));
        UUSE_TEST_CHECK(roleOperation->refCount() == 1);
        roleOperation->release();
        UUSE_TEST_CHECK(uuse::Operation::liveCount() == 0);

        /* A frontend handle keeps a fast-completed operation alive until return. */
        int published = 0;
        auto *fast = uuse::Operation::create(13);
        UUSE_TEST_CHECK(fast != nullptr);
        fast->setCompletionSink({&published, onCompletionPublished});
        auto handle = fast->acquireFrontendHandle({7, 1});
        UUSE_TEST_CHECK(handle.valid());
        UUSE_TEST_CHECK(handle.requestId() == 13);
        UUSE_TEST_CHECK(handle.token().slot == 7);
        UUSE_TEST_CHECK(fast->refCount(uuse::OperationRefRole::FrontendHandle) == 1);
        fast->release(); /* Drop the creator reference before the frontend returns. */
        UUSE_TEST_CHECK(fast->completeTerminal(0));
        UUSE_TEST_CHECK(fast->isTerminal());
        UUSE_TEST_CHECK(!fast->isCompletionPublished());
        UUSE_TEST_CHECK(fast->publishCompletion());
        UUSE_TEST_CHECK(fast->state.load() == uuse::OperationState::CompletionPublished);
        UUSE_TEST_CHECK(published == 1);
        UUSE_TEST_CHECK(!fast->publishCompletion());
        UUSE_TEST_CHECK(handle.valid());
        handle.reset();
        UUSE_TEST_CHECK(uuse::Operation::liveCount() == 0);

        /* New preparation and drain states remain on the existing lifecycle object. */
        auto *lifecycle = uuse::Operation::create(14);
        UUSE_TEST_CHECK(lifecycle != nullptr);
        UUSE_TEST_CHECK(lifecycle->transition(uuse::OperationState::Created,
                                             uuse::OperationState::Enqueued));
        UUSE_TEST_CHECK(lifecycle->beginPreparing());
        UUSE_TEST_CHECK(lifecycle->state.load() == uuse::OperationState::Preparing);
        UUSE_TEST_CHECK(lifecycle->beginDraining());
        UUSE_TEST_CHECK(lifecycle->state.load() == uuse::OperationState::Draining);
        UUSE_TEST_CHECK(lifecycle->completeTerminal(0));
        UUSE_TEST_CHECK(lifecycle->state.load() == uuse::OperationState::Completed);
        UUSE_TEST_CHECK(lifecycle->publishCompletion());
        lifecycle->release();
        UUSE_TEST_CHECK(uuse::Operation::liveCount() == 0);

        /* Terminal transition and publication each reject a second caller. */
        auto *split = uuse::Operation::create(15);
        UUSE_TEST_CHECK(split != nullptr);
        UUSE_TEST_CHECK(split->completeTerminal(uuse::Status::TimedOut, -110, 128));
        UUSE_TEST_CHECK(!split->completeTerminal(uuse::Status::Ok));
        UUSE_TEST_CHECK(!split->isCompletionPublished());
        UUSE_TEST_CHECK(split->publishCompletion());
        UUSE_TEST_CHECK(!split->publishCompletion());
        split->release();
        UUSE_TEST_CHECK(uuse::Operation::liveCount() == 0);

        /* Pool reuse clears public state, sidecar state, hooks and role counters. */
        std::uint32_t sidecarProbe = 99;
        auto *dirty = uuse::Operation::create(16);
        UUSE_TEST_CHECK(dirty != nullptr);
        dirty->kind = uuse::OperationKind::Write;
        dirty->state.store(uuse::OperationState::Draining);
        dirty->fileId = 44;
        dirty->offset = 55;
        dirty->length = 66;
        dirty->shard = 3;
        dirty->userCtx = &sidecarProbe;
        dirty->onComplete = onOpComplete;
        dirty->status = -5;
        dirty->resultStatus = uuse::Status::IoError;
        dirty->backendErrno = -5;
        dirty->completedBytes = 77;
        dirty->retryCount = 8;
        dirty->schedulerNode = &sidecarProbe;
        dirty->dmaLease = &sidecarProbe;
        dirty->parent = &sidecarProbe;
        dirty->setCompletionToken({8, 9});
        dirty->setCompletionSink({&published, onCompletionPublished});
        dirty->asyncState().schedulerNode = &sidecarProbe;
        dirty->asyncState().parent = &sidecarProbe;
        dirty->asyncState().backendState = &sidecarProbe;
        dirty->asyncState().status = uuse::Status::IoError;
        dirty->asyncState().backendErrno = -6;
        dirty->asyncState().childCount = 2;
        dirty->asyncState().submittedChildren = 2;
        dirty->asyncState().drainedChildren = 1;
        dirty->asyncState().completionPublished = true;
        UUSE_TEST_CHECK(dirty->attachAsyncStorage(&sidecarProbe, sizeof(sidecarProbe),
                                                  resetSidecarProbe));
        dirty->release();
        UUSE_TEST_CHECK(sidecarProbe == 0);

        auto *reused = uuse::Operation::create(17);
        UUSE_TEST_CHECK(reused != nullptr);
        UUSE_TEST_CHECK(reused->requestId == 17);
        UUSE_TEST_CHECK(reused->kind == uuse::OperationKind::Other);
        UUSE_TEST_CHECK(reused->state.load() == uuse::OperationState::Created);
        UUSE_TEST_CHECK(!reused->isTerminal());
        UUSE_TEST_CHECK(!reused->isCompletionPublished());
        UUSE_TEST_CHECK(reused->fileId == 0 && reused->offset == 0 && reused->length == 0);
        UUSE_TEST_CHECK(reused->shard == 0 && reused->userCtx == nullptr);
        UUSE_TEST_CHECK(reused->onComplete == nullptr);
        UUSE_TEST_CHECK(reused->status == 0 && reused->resultStatus == uuse::Status::Ok);
        UUSE_TEST_CHECK(reused->backendErrno == 0 && reused->completedBytes == 0);
        UUSE_TEST_CHECK(reused->retryCount == 0 && reused->schedulerNode == nullptr);
        UUSE_TEST_CHECK(reused->dmaLease == nullptr && reused->parent == nullptr);
        UUSE_TEST_CHECK(!reused->completionToken().valid());
        UUSE_TEST_CHECK(reused->completionSink().context == nullptr);
        UUSE_TEST_CHECK(reused->completionSink().publish == nullptr);
        UUSE_TEST_CHECK(reused->asyncStorage().storage == nullptr);
        UUSE_TEST_CHECK(reused->asyncState().schedulerNode == nullptr);
        UUSE_TEST_CHECK(reused->asyncState().parent == nullptr);
        UUSE_TEST_CHECK(reused->asyncState().backendState == nullptr);
        UUSE_TEST_CHECK(reused->asyncState().childCount == 0);
        UUSE_TEST_CHECK(reused->asyncState().submittedChildren == 0);
        UUSE_TEST_CHECK(reused->asyncState().drainedChildren == 0);
        UUSE_TEST_CHECK(!reused->asyncState().completionPublished);
        UUSE_TEST_CHECK(reused->refCount() == 1);
        UUSE_TEST_CHECK(reused->refCount(uuse::OperationRefRole::Legacy) == 1);
        UUSE_TEST_CHECK(reused->refCount(uuse::OperationRefRole::Actor) == 0);
        reused->release();
        UUSE_TEST_CHECK(uuse::Operation::liveCount() == 0);

        const std::size_t overflowBefore = uuse::Operation::poolOverflow();
        std::vector<uuse::Operation *> held;
        for (int i = 0; i < 8; ++i) {
            auto *created = uuse::Operation::create(100 + i);
            if (created != nullptr) {
                held.push_back(created);
            }
        }
        UUSE_TEST_CHECK(held.size() == 4);
        UUSE_TEST_CHECK(uuse::Operation::poolOverflow() > overflowBefore);
        for (auto *item : held) {
            item->release();
        }
        UUSE_TEST_CHECK(uuse::Operation::liveCount() == 0);
    }

    /* 4. ring order, full/busy and doorbell dedup */
    {
        uuse::Operation::setPoolCapacity(32);
        uuse::ShardRing ring(8);
        std::vector<uuse::Operation *> ops;
        for (int i = 0; i < 8; ++i) {
            auto *operation = uuse::Operation::create(static_cast<std::uint64_t>(i));
            UUSE_TEST_CHECK(operation != nullptr);
            ops.push_back(operation);
            auto reservation = ring.reserve();
            UUSE_TEST_CHECK(reservation.valid());
            ring.publish(reservation, operation);
        }
        auto full = ring.reserve();
        UUSE_TEST_CHECK(!full.valid());
        UUSE_TEST_CHECK(ring.stats().busy == 1);
        UUSE_TEST_CHECK(ring.stats().doorbells == 1); /* armed once for the burst */
        UUSE_TEST_CHECK(ring.doorbellArmed());
        std::vector<uuse::Operation *> out(8);
        UUSE_TEST_CHECK(ring.drain(out.data(), out.size()) == 8);
        for (int i = 0; i < 8; ++i) {
            UUSE_TEST_CHECK(out[i] == ops[i]);
        }
        UUSE_TEST_CHECK(ring.empty());
        ring.clearDoorbell();
        UUSE_TEST_CHECK(!ring.doorbellArmed());
        for (auto *operation : ops) {
            operation->release();
        }
    }

    /* 5. multi-producer -> actor -> completion queue */
    {
        constexpr int kProducers = 4;
        constexpr int kPerProducer = 500;
        constexpr int kTotal = kProducers * kPerProducer;
        uuse::Operation::setPoolCapacity(4096);
        uuse::ShardRing ring(1024);
        uuse::CompletionQueue completions(2048);
        const std::size_t overflowBefore = uuse::Operation::poolOverflow();

        struct StepCtx {
            uuse::CompletionQueue *completions = nullptr;
        };
        StepCtx stepCtx{&completions};
        uuse::ShardActor actor(
            ring,
            [](void *raw, uuse::Operation *operation) {
                auto *ctx = static_cast<StepCtx *>(raw);
                uuse::CompletionQueue::Event event;
                event.requestId = operation->requestId;
                event.userCtx = operation->userCtx;
                event.status = 0;
                ctx->completions->push(event);
                operation->complete(0);
                operation->release();
            },
            &stepCtx, 64);

        uuse::trace::setEnabled(true);
        const std::uint64_t traceBefore = uuse::trace::recorded();
        std::atomic<bool> failure{false};
        std::vector<std::thread> producers;
        producers.reserve(kProducers);
        for (int producer = 0; producer < kProducers; ++producer) {
            producers.emplace_back([&ring, &failure, producer]() {
                for (int i = 0; i < kPerProducer; ++i) {
                    const std::uint64_t id =
                        static_cast<std::uint64_t>(producer * kPerProducer + i);
                    auto *operation = uuse::Operation::create(id);
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

        std::vector<uuse::CompletionQueue::Event> events(64);
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

        UUSE_TEST_CHECK(!failure.load());
        UUSE_TEST_CHECK(reaped == static_cast<std::size_t>(kTotal));
        for (bool value : seen) {
            UUSE_TEST_CHECK(value);
        }
        UUSE_TEST_CHECK(ring.empty());
        UUSE_TEST_CHECK(completions.empty());
        UUSE_TEST_CHECK(completions.dropped() == 0);
        UUSE_TEST_CHECK(uuse::Operation::liveCount() == 0);
        UUSE_TEST_CHECK(uuse::Operation::poolOverflow() == overflowBefore);
        UUSE_TEST_CHECK(uuse::trace::recorded() > traceBefore);
        uuse::trace::setEnabled(false);
    }

    /* 6. lost-wake window: publish after the actor cleared the doorbell */
    {
        uuse::Operation::setPoolCapacity(8);
        uuse::ShardRing ring(4);
        int processed = 0;
        uuse::ShardActor actor(
            ring,
            [](void *raw, uuse::Operation *operation) {
                ++(*static_cast<int *>(raw));
                operation->complete(0);
                operation->release();
            },
            &processed, 4);

        auto *first = uuse::Operation::create(1);
        UUSE_TEST_CHECK(first != nullptr);
        auto reservation = ring.reserve();
        UUSE_TEST_CHECK(reservation.valid());
        ring.publish(reservation, first);
        UUSE_TEST_CHECK(actor.runOnce() == 1);
        UUSE_TEST_CHECK(!ring.doorbellArmed());

        auto *second = uuse::Operation::create(2);
        UUSE_TEST_CHECK(second != nullptr);
        reservation = ring.reserve();
        UUSE_TEST_CHECK(reservation.valid());
        ring.publish(reservation, second);
        UUSE_TEST_CHECK(ring.doorbellArmed());
        UUSE_TEST_CHECK(actor.runOnce() == 1);
        UUSE_TEST_CHECK(processed == 2);
        UUSE_TEST_CHECK(uuse::Operation::liveCount() == 0);
    }

    /* 7. concurrent pool churn: the lock-free free list must never hand one
     * operation to two threads and must return to zero live count. */
    {
        constexpr int kThreads = 4;
        constexpr int kIterations = 20000;
        uuse::Operation::setPoolCapacity(64);
        std::atomic<bool> failure{false};
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int thread = 0; thread < kThreads; ++thread) {
            threads.emplace_back([&failure]() {
                for (int i = 0; i < kIterations; ++i) {
                    auto *operation = uuse::Operation::create(1);
                    if (operation == nullptr) {
                        failure.store(true);
                        continue;
                    }
                    if (operation->requestId != 1) {
                        failure.store(true);
                    }
                    operation->requestId = 0xDEADBEEFULL;
                    if (operation->requestId != 0xDEADBEEFULL) {
                        failure.store(true);
                    }
                    operation->release();
                }
            });
        }
        for (auto &thread : threads) {
            thread.join();
        }
        UUSE_TEST_CHECK(!failure.load());
        UUSE_TEST_CHECK(uuse::Operation::liveCount() == 0);
    }

    std::printf("data_plane_test: ok\n");
    return 0;
}
