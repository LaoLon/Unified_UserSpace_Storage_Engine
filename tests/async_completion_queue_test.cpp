#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "core/async_completion_queue.hpp"
#include "core/operation.hpp"
#include "test_support.hpp"

namespace {

/* Builds a value-only completion event for a shard producer. */
hfs::CompletionEvent makeEvent(std::uint64_t requestId, std::uint32_t shard) {
    hfs::CompletionEvent event;
    event.requestId = requestId;
    event.token = hfs::CompletionToken{static_cast<std::uint32_t>(requestId),
                                       static_cast<std::uint32_t>(requestId + 1)};
    event.status = shard == 0 ? hfs::Status::Ok : hfs::Status::IoError;
    event.backendErrno = shard == 0 ? 0 : 5;
    event.completedBytes = 4096 + shard;
    return event;
}

} // namespace

int main() {
    /* 1. IoHandle owns exactly one intrusive frontend reference and moves it. */
    {
        hfs::Operation::setPoolCapacity(8);
        auto *operation = hfs::Operation::create(77);
        HFS_TEST_CHECK(operation != nullptr);
        HFS_TEST_CHECK(hfs::Operation::liveCount() == 1);
        const hfs::CompletionToken token{3, 1};
        auto handle = operation->acquireFrontendHandle(token);
        operation->release();
        HFS_TEST_CHECK(handle.valid());
        HFS_TEST_CHECK(handle.requestId() == 77);
        HFS_TEST_CHECK(handle.token().slot == token.slot &&
                       handle.token().generation == token.generation);

        hfs::IoHandle moved(std::move(handle));
        HFS_TEST_CHECK(!handle.valid());
        HFS_TEST_CHECK(moved.valid());
        hfs::IoHandle assigned;
        assigned = std::move(moved);
        HFS_TEST_CHECK(!moved.valid());
        assigned.reset();
        HFS_TEST_CHECK(!assigned.valid());
        HFS_TEST_CHECK(hfs::Operation::liveCount() == 0);

        auto *retainedOperation = hfs::Operation::create(78);
        HFS_TEST_CHECK(retainedOperation != nullptr);
        auto retained = retainedOperation->acquireFrontendHandle(token);
        retainedOperation->release();
        HFS_TEST_CHECK(retained.valid());
        retained.reset();
        HFS_TEST_CHECK(hfs::Operation::liveCount() == 0);
    }

    /* 2. Capacity is guaranteed per shard and overflow is observable. */
    {
        hfs::AsyncCompletionQueue queue(4, 8);
        HFS_TEST_CHECK(queue.valid());
        HFS_TEST_CHECK(queue.totalCapacity() == 32);
        for (std::uint64_t index = 0; index < 8; ++index) {
            HFS_TEST_CHECK(queue.push(2, makeEvent(index, 2)));
        }
        HFS_TEST_CHECK(!queue.push(2, makeEvent(8, 2)));
        HFS_TEST_CHECK(queue.shardDiagnostics(2).overflow == 1);
        HFS_TEST_CHECK(queue.diagnostics().overflow == 1);

        std::vector<hfs::CompletionEvent> output(8);
        HFS_TEST_CHECK(queue.drain(output, output.size()) == 8);
        HFS_TEST_CHECK(queue.shardDiagnostics(2).size == 0);
    }

    /* 3. External frontends can disable per-client eventfd creation. */
    {
        hfs::AsyncCompletionQueue queue;
        HFS_TEST_CHECK(queue.initialize(1, 4, false));
        HFS_TEST_CHECK(!queue.diagnostics().notifier.eventFd);
        HFS_TEST_CHECK(queue.push(0, makeEvent(900, 0)));
        std::vector<hfs::CompletionEvent> output(1);
        HFS_TEST_CHECK(queue.drain(output, output.size()) == 1);
    }

    /* 4. Four shard producers feed independent SPSC rings and one consumer drains them. */
    {
        constexpr std::size_t shardCount = 4;
        constexpr std::size_t perShard = 300;
        constexpr std::size_t capacity = 512;
        hfs::AsyncCompletionQueue queue(shardCount, capacity);
        HFS_TEST_CHECK(queue.valid());
        std::atomic<bool> failed{false};
        std::vector<std::thread> producers;
        producers.reserve(shardCount);
        for (std::size_t shard = 0; shard < shardCount; ++shard) {
            producers.emplace_back([&queue, &failed, shard] {
                for (std::size_t index = 0; index < perShard; ++index) {
                    const std::uint64_t requestId =
                        static_cast<std::uint64_t>(shard * perShard + index);
                    while (!queue.push(shard, makeEvent(requestId,
                                                         static_cast<std::uint32_t>(shard)))) {
                        if (queue.diagnostics().invalidShard != 0) {
                            failed.store(true, std::memory_order_release);
                            return;
                        }
                        std::this_thread::yield();
                    }
                }
            });
        }

        const std::size_t total = shardCount * perShard;
        std::size_t received = 0;
        std::vector<hfs::CompletionEvent> output(37);
        std::vector<bool> seen(total, false);
        while (received < total && !failed.load(std::memory_order_acquire)) {
            const std::size_t count = queue.drain(output, output.size());
            for (std::size_t index = 0; index < count; ++index) {
                const auto requestId = output[index].requestId;
                HFS_TEST_CHECK(requestId < total);
                HFS_TEST_CHECK(!seen[static_cast<std::size_t>(requestId)]);
                seen[static_cast<std::size_t>(requestId)] = true;
                ++received;
            }
            if (count == 0) {
                std::this_thread::yield();
            }
        }
        for (auto &producer : producers) {
            producer.join();
        }
        while (received < total) {
            const std::size_t count = queue.drain(output, output.size());
            HFS_TEST_CHECK(count != 0);
            for (std::size_t index = 0; index < count; ++index) {
                const auto requestId = output[index].requestId;
                HFS_TEST_CHECK(requestId < total);
                HFS_TEST_CHECK(!seen[static_cast<std::size_t>(requestId)]);
                seen[static_cast<std::size_t>(requestId)] = true;
                ++received;
            }
        }
        HFS_TEST_CHECK(!failed.load(std::memory_order_acquire));
        HFS_TEST_CHECK(received == total);
        HFS_TEST_CHECK(queue.diagnostics().overflow == 0);
        HFS_TEST_CHECK(queue.diagnostics().drained == total);
        for (bool value : seen) {
            HFS_TEST_CHECK(value);
        }
    }

    /* 5. Poll validates min/max, is nonblocking for zero timeout, and preserves events. */
    {
        hfs::AsyncCompletionQueue queue(2, 4);
        std::vector<hfs::CompletionEvent> output(2);
        auto invalid = queue.poll(output, 2, 1, std::chrono::microseconds(0));
        HFS_TEST_CHECK(invalid.status == hfs::Status::InvalidArgument);
        auto empty = queue.poll(output, 1, 1, std::chrono::microseconds(0));
        HFS_TEST_CHECK(empty.status == hfs::Status::Ok && empty.count == 0);
        HFS_TEST_CHECK(queue.diagnostics().drained == 0);

        HFS_TEST_CHECK(queue.push(1, makeEvent(991, 1)));
        auto immediate = queue.poll(output, 1, 1, std::chrono::microseconds(0));
        HFS_TEST_CHECK(immediate.status == hfs::Status::Ok && immediate.count == 1);
        HFS_TEST_CHECK(output[0].requestId == 991);
    }

    /* 6. A new producer still wakes after the previous poll drained to empty. */
    {
        hfs::AsyncCompletionQueue queue(1, 4);
        std::vector<hfs::CompletionEvent> output(1);
        HFS_TEST_CHECK(queue.push(0, makeEvent(1000, 0)));
        auto first = queue.poll(output, 1, 1, std::chrono::microseconds(0));
        HFS_TEST_CHECK(first.status == hfs::Status::Ok && first.count == 1);

        std::atomic<bool> producerFailed{false};
        std::thread producer([&queue, &producerFailed] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!queue.push(0, makeEvent(1001, 0))) {
                producerFailed.store(true, std::memory_order_release);
            }
        });
        auto second = queue.poll(output, 1, 1, std::chrono::milliseconds(250));
        producer.join();
        HFS_TEST_CHECK(!producerFailed.load(std::memory_order_acquire));
        HFS_TEST_CHECK(second.status == hfs::Status::Ok && second.count == 1);
        HFS_TEST_CHECK(output[0].requestId == 1001);
    }

    /* 7. Timed poll wakes from a producer and leaves in-flight state untouched on timeout. */
    {
        hfs::AsyncCompletionQueue queue(1, 4);
        std::vector<hfs::CompletionEvent> output(1);
        const auto start = std::chrono::steady_clock::now();
        auto timeout = queue.poll(output, 1, 1, std::chrono::milliseconds(20));
        const auto elapsed = std::chrono::steady_clock::now() - start;
        HFS_TEST_CHECK(timeout.status == hfs::Status::Ok && timeout.count == 0);
        HFS_TEST_CHECK(elapsed >= std::chrono::milliseconds(10));

        std::atomic<bool> producerFailed{false};
        std::thread producer([&queue, &producerFailed] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!queue.push(0, makeEvent(1001, 0))) {
                producerFailed.store(true, std::memory_order_release);
            }
        });
        const auto wakeStart = std::chrono::steady_clock::now();
        auto woken = queue.poll(output, 1, 1, std::chrono::milliseconds(250));
        const auto wakeElapsed = std::chrono::steady_clock::now() - wakeStart;
        producer.join();
        HFS_TEST_CHECK(!producerFailed.load(std::memory_order_acquire));
        HFS_TEST_CHECK(woken.status == hfs::Status::Ok && woken.count == 1);
        HFS_TEST_CHECK(output[0].requestId == 1001);
        HFS_TEST_CHECK(wakeElapsed < std::chrono::milliseconds(200));
        HFS_TEST_CHECK(queue.diagnostics().drained == 1);
    }

    /* 8. Rearming clears stale wake state without losing a queued event. */
    {
        hfs::AsyncCompletionRing ring(4);
        HFS_TEST_CHECK(ring.push(makeEvent(2000, 0)));
        HFS_TEST_CHECK(ring.rearmIfNonEmpty());
        std::vector<hfs::CompletionEvent> output(1);
        HFS_TEST_CHECK(ring.drain(output) == 1);
        HFS_TEST_CHECK(!ring.rearmIfNonEmpty());
        HFS_TEST_CHECK(ring.push(makeEvent(2001, 0)));
        HFS_TEST_CHECK(ring.rearmIfNonEmpty());
        HFS_TEST_CHECK(ring.drain(output) == 1);
    }

    hfs::CompletionNotifier notifier;
    HFS_TEST_CHECK(notifier.initialize());
    std::printf("async_completion_queue_test: ok (eventfd=%s)\n",
                 notifier.usingEventFd() ? "yes" : "no");
    return 0;
}
